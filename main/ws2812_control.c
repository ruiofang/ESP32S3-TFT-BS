#include "ws2812_control.h"
#include "driver/rmt_tx.h"
#include "led_strip.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "Lib/cJSON/cJSON.h"
#include "nvs_flash.h"
#include "nvs.h"
#include <string.h>
#include <stdlib.h>
#include <math.h>

static const char *TAG = "WS2812_CONTROL";

// NVS存储相关定义
#define WS2812_NVS_NAMESPACE "ws2812_cfg"
#define WS2812_NVS_KEY_PREFIX "ch_"
#define WS2812_CONFIG_VERSION 1

// GPIO引脚定义
static const int ws2812_gpio_pins[WS2812_CHANNEL_COUNT] = WS2812_GPIO_PINS;

// 全局变量 - 多通道支持
static led_strip_handle_t led_strips[WS2812_CHANNEL_COUNT] = {NULL};
static ws2812_channel_t channels[WS2812_CHANNEL_COUNT];
static SemaphoreHandle_t ws2812_mutex = NULL;
static bool task_running = false;

// 自动循环模式相关变量 - 每个通道独立
static uint32_t auto_cycle_timers[WS2812_CHANNEL_COUNT] = {0};
static uint32_t auto_cycle_durations[WS2812_CHANNEL_COUNT] = {8000, 8000, 8000, 8000};
static ws2812_mode_t auto_cycle_modes[] = {
    WS2812_MODE_RAINBOW,
    WS2812_MODE_BREATHING,
    WS2812_MODE_RUNNING,
    WS2812_MODE_WAVE,
    WS2812_MODE_FLASH
};
static const int auto_cycle_count = sizeof(auto_cycle_modes) / sizeof(auto_cycle_modes[0]);
static int current_auto_mode_indices[WS2812_CHANNEL_COUNT] = {0};

// HSV转RGB函数
static rgb_color_t hsv_to_rgb(uint16_t h, uint8_t s, uint8_t v) {
    rgb_color_t rgb;
    uint8_t region, remainder, p, q, t;

    if (s == 0) {
        rgb.r = rgb.g = rgb.b = v;
        return rgb;
    }

    region = h / 43;
    remainder = (h - (region * 43)) * 6;

    p = (v * (255 - s)) >> 8;
    q = (v * (255 - ((s * remainder) >> 8))) >> 8;
    t = (v * (255 - ((s * (255 - remainder)) >> 8))) >> 8;

    switch (region) {
        case 0:
            rgb.r = v; rgb.g = t; rgb.b = p;
            break;
        case 1:
            rgb.r = q; rgb.g = v; rgb.b = p;
            break;
        case 2:
            rgb.r = p; rgb.g = v; rgb.b = t;
            break;
        case 3:
            rgb.r = p; rgb.g = q; rgb.b = v;
            break;
        case 4:
            rgb.r = t; rgb.g = p; rgb.b = v;
            break;
        default:
            rgb.r = v; rgb.g = p; rgb.b = q;
            break;
    }

    return rgb;
}

// 应用亮度调节
static rgb_color_t apply_brightness(rgb_color_t color, uint8_t brightness) {
    rgb_color_t result;
    result.r = (color.r * brightness) / 255;
    result.g = (color.g * brightness) / 255;
    result.b = (color.b * brightness) / 255;
    return result;
}

esp_err_t ws2812_init(void) {
    ESP_LOGI(TAG, "Initializing WS2812 multi-channel system (%d channels)", WS2812_CHANNEL_COUNT);
    
    // 创建互斥锁
    ws2812_mutex = xSemaphoreCreateMutex();
    if (ws2812_mutex == NULL) {
        ESP_LOGE(TAG, "Failed to create mutex");
        return ESP_FAIL;
    }
    
    // 初始化每个通道
    for (int ch = 0; ch < WS2812_CHANNEL_COUNT; ch++) {
        // 先设置默认配置（将在load_config中覆盖）
        channels[ch].channel_id = ch;
        channels[ch].enabled = true;
        channels[ch].config.mode = WS2812_MODE_AUTO_CYCLE;
        channels[ch].config.color = (rgb_color_t){255, 255, 255};
        channels[ch].config.speed = 150;
        channels[ch].config.brightness = 180;
        
        ESP_LOGI(TAG, "Initializing channel %d on GPIO %d", ch, ws2812_gpio_pins[ch]);
        
        // 配置LED strip - 使用简化的配置
        led_strip_config_t strip_config = {
            .strip_gpio_num = ws2812_gpio_pins[ch],
            .max_leds = WS2812_LED_COUNT,
            .led_pixel_format = LED_PIXEL_FORMAT_GRB,
            .led_model = LED_MODEL_WS2812,
            .flags.invert_out = false,
        };
        
        led_strip_rmt_config_t rmt_config = {
            .clk_src = RMT_CLK_SRC_DEFAULT,
            .resolution_hz = 10000000,
            .flags.with_dma = false,
        };
        
        esp_err_t ret = led_strip_new_rmt_device(&strip_config, &rmt_config, &led_strips[ch]);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to create LED strip for channel %d (GPIO %d): %s", 
                     ch, ws2812_gpio_pins[ch], esp_err_to_name(ret));
            channels[ch].enabled = false; // 禁用失败的通道
            led_strips[ch] = NULL;
            continue;
        }
        
        // 清空所有LED
        ret = led_strip_clear(led_strips[ch]);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "Warning: Failed to clear LED strip for channel %d: %s", 
                     ch, esp_err_to_name(ret));
        }
        
        ESP_LOGI(TAG, "Channel %d initialized successfully on GPIO %d with %d LEDs", 
                 ch, ws2812_gpio_pins[ch], WS2812_LED_COUNT);
    }
    
    ESP_LOGI(TAG, "WS2812 multi-channel system initialized successfully");
    
    // 测试所有通道 - 短暂点亮白色
    ESP_LOGI(TAG, "Testing all channels with white light for 2 seconds...");
    for (int ch = 0; ch < WS2812_CHANNEL_COUNT; ch++) {
        if (led_strips[ch]) {
            for (int i = 0; i < WS2812_LED_COUNT; i++) {
                led_strip_set_pixel(led_strips[ch], i, 50, 50, 50); // 低亮度白光
            }
            led_strip_refresh(led_strips[ch]);
            ESP_LOGI(TAG, "Channel %d test light activated", ch);
        } else {
            ESP_LOGE(TAG, "Channel %d LED strip is NULL - initialization failed", ch);
        }
    }
    
    // 等待2秒后关闭测试灯
    vTaskDelay(pdMS_TO_TICKS(2000));
    for (int ch = 0; ch < WS2812_CHANNEL_COUNT; ch++) {
        if (led_strips[ch]) {
            led_strip_clear(led_strips[ch]);
            led_strip_refresh(led_strips[ch]);
        }
    }
    ESP_LOGI(TAG, "Channel test completed, returning to normal operation");
    
    // 加载保存的配置
    ESP_LOGI(TAG, "Loading saved WS2812 configuration...");
    esp_err_t load_ret = ws2812_load_config();
    if (load_ret == ESP_OK) {
        ESP_LOGI(TAG, "Configuration loaded successfully");
    } else {
        ESP_LOGW(TAG, "Failed to load configuration: %s, using defaults", esp_err_to_name(load_ret));
    }
    
    return ESP_OK;
}

esp_err_t ws2812_set_mode(uint8_t channel_id, ws2812_mode_t mode) {
    if (mode >= WS2812_MODE_MAX) {
        return ESP_ERR_INVALID_ARG;
    }
    
    if (xSemaphoreTake(ws2812_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        if (channel_id == WS2812_BROADCAST_ID) {
            // 广播到所有通道
            for (int ch = 0; ch < WS2812_CHANNEL_COUNT; ch++) {
                channels[ch].config.mode = mode;
            }
            ESP_LOGI(TAG, "Mode set to %d for all channels", mode);
        } else if (channel_id < WS2812_CHANNEL_COUNT) {
            // 设置指定通道
            channels[channel_id].config.mode = mode;
            ESP_LOGI(TAG, "Mode set to %d for channel %d", mode, channel_id);
        } else {
            xSemaphoreGive(ws2812_mutex);
            return ESP_ERR_INVALID_ARG;
        }
        xSemaphoreGive(ws2812_mutex);
        
        // 自动保存配置
        esp_err_t save_ret = ws2812_save_config();
        if (save_ret != ESP_OK) {
            ESP_LOGW(TAG, "Failed to save config after mode change: %s", esp_err_to_name(save_ret));
        }
        
        return ESP_OK;
    }
    return ESP_ERR_TIMEOUT;
}

esp_err_t ws2812_set_color(uint8_t channel_id, uint8_t r, uint8_t g, uint8_t b) {
    if (xSemaphoreTake(ws2812_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        if (channel_id == WS2812_BROADCAST_ID) {
            // 广播到所有通道
            for (int ch = 0; ch < WS2812_CHANNEL_COUNT; ch++) {
                channels[ch].config.color.r = r;
                channels[ch].config.color.g = g;
                channels[ch].config.color.b = b;
            }
            ESP_LOGI(TAG, "Color set to RGB(%d,%d,%d) for all channels", r, g, b);
        } else if (channel_id < WS2812_CHANNEL_COUNT) {
            // 设置指定通道
            channels[channel_id].config.color.r = r;
            channels[channel_id].config.color.g = g;
            channels[channel_id].config.color.b = b;
            ESP_LOGI(TAG, "Color set to RGB(%d,%d,%d) for channel %d", r, g, b, channel_id);
        } else {
            xSemaphoreGive(ws2812_mutex);
            return ESP_ERR_INVALID_ARG;
        }
        xSemaphoreGive(ws2812_mutex);
        
        // 自动保存配置
        esp_err_t save_ret = ws2812_save_config();
        if (save_ret != ESP_OK) {
            ESP_LOGW(TAG, "Failed to save config after color change: %s", esp_err_to_name(save_ret));
        }
        
        return ESP_OK;
    }
    return ESP_ERR_TIMEOUT;
}

esp_err_t ws2812_set_brightness(uint8_t channel_id, uint8_t brightness) {
    if (xSemaphoreTake(ws2812_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        if (channel_id == WS2812_BROADCAST_ID) {
            // 广播到所有通道
            for (int ch = 0; ch < WS2812_CHANNEL_COUNT; ch++) {
                channels[ch].config.brightness = brightness;
            }
            ESP_LOGI(TAG, "Brightness set to %d for all channels", brightness);
        } else if (channel_id < WS2812_CHANNEL_COUNT) {
            // 设置指定通道
            channels[channel_id].config.brightness = brightness;
            ESP_LOGI(TAG, "Brightness set to %d for channel %d", brightness, channel_id);
        } else {
            xSemaphoreGive(ws2812_mutex);
            return ESP_ERR_INVALID_ARG;
        }
        xSemaphoreGive(ws2812_mutex);
        
        // 自动保存配置
        esp_err_t save_ret = ws2812_save_config();
        if (save_ret != ESP_OK) {
            ESP_LOGW(TAG, "Failed to save config after brightness change: %s", esp_err_to_name(save_ret));
        }
        
        return ESP_OK;
    }
    return ESP_ERR_TIMEOUT;
}

esp_err_t ws2812_set_speed(uint8_t channel_id, uint32_t speed) {
    if (speed < 1) speed = 1; // 最小1ms
    if (speed > 10000) speed = 10000; // 最大10秒
    
    if (xSemaphoreTake(ws2812_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        if (channel_id == WS2812_BROADCAST_ID) {
            // 广播到所有通道
            for (int ch = 0; ch < WS2812_CHANNEL_COUNT; ch++) {
                channels[ch].config.speed = speed;
            }
            ESP_LOGI(TAG, "Speed set to %ld ms for all channels", speed);
        } else if (channel_id < WS2812_CHANNEL_COUNT) {
            // 设置指定通道
            channels[channel_id].config.speed = speed;
            ESP_LOGI(TAG, "Speed set to %ld ms for channel %d", speed, channel_id);
        } else {
            xSemaphoreGive(ws2812_mutex);
            return ESP_ERR_INVALID_ARG;
        }
        xSemaphoreGive(ws2812_mutex);
        
        // 自动保存配置
        esp_err_t save_ret = ws2812_save_config();
        if (save_ret != ESP_OK) {
            ESP_LOGW(TAG, "Failed to save config after speed change: %s", esp_err_to_name(save_ret));
        }
        
        return ESP_OK;
    }
    return ESP_ERR_TIMEOUT;
}

esp_err_t ws2812_set_channel_enabled(uint8_t channel_id, bool enabled) {
    if (xSemaphoreTake(ws2812_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        if (channel_id == WS2812_BROADCAST_ID) {
            // 广播到所有通道
            for (int ch = 0; ch < WS2812_CHANNEL_COUNT; ch++) {
                channels[ch].enabled = enabled;
                if (!enabled && led_strips[ch]) {
                    led_strip_clear(led_strips[ch]);
                    led_strip_refresh(led_strips[ch]);
                }
            }
            ESP_LOGI(TAG, "All channels %s", enabled ? "enabled" : "disabled");
        } else if (channel_id < WS2812_CHANNEL_COUNT) {
            // 设置指定通道
            channels[channel_id].enabled = enabled;
            if (!enabled && led_strips[channel_id]) {
                led_strip_clear(led_strips[channel_id]);
                led_strip_refresh(led_strips[channel_id]);
            }
            ESP_LOGI(TAG, "Channel %d %s", channel_id, enabled ? "enabled" : "disabled");
        } else {
            xSemaphoreGive(ws2812_mutex);
            return ESP_ERR_INVALID_ARG;
        }
        xSemaphoreGive(ws2812_mutex);
        
        // 自动保存配置
        esp_err_t save_ret = ws2812_save_config();
        if (save_ret != ESP_OK) {
            ESP_LOGW(TAG, "Failed to save config after enable/disable change: %s", esp_err_to_name(save_ret));
        }
        
        return ESP_OK;
    }
    return ESP_ERR_TIMEOUT;
}

ws2812_channel_t ws2812_get_channel_config(uint8_t channel_id) {
    ws2812_channel_t config = {0};
    if (channel_id < WS2812_CHANNEL_COUNT) {
        if (xSemaphoreTake(ws2812_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
            config = channels[channel_id];
            xSemaphoreGive(ws2812_mutex);
        }
    }
    return config;
}

esp_err_t ws2812_get_all_configs(ws2812_channel_t configs[WS2812_CHANNEL_COUNT]) {
    if (configs == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    
    if (xSemaphoreTake(ws2812_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        for (int ch = 0; ch < WS2812_CHANNEL_COUNT; ch++) {
            configs[ch] = channels[ch];
        }
        xSemaphoreGive(ws2812_mutex);
        return ESP_OK;
    }
    return ESP_ERR_TIMEOUT;
}

esp_err_t ws2812_set_cycle_duration(uint8_t channel_id, uint32_t duration) {
    if (duration < 1000) duration = 1000;   // 最小1秒
    if (duration > 60000) duration = 60000; // 最大60秒
    
    if (xSemaphoreTake(ws2812_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        if (channel_id == WS2812_BROADCAST_ID) {
            // 广播到所有通道
            for (int ch = 0; ch < WS2812_CHANNEL_COUNT; ch++) {
                auto_cycle_durations[ch] = duration;
                auto_cycle_timers[ch] = 0; // 重置计时器
            }
            ESP_LOGI(TAG, "Auto-cycle duration set to %ld ms for all channels", duration);
        } else if (channel_id < WS2812_CHANNEL_COUNT) {
            // 设置指定通道
            auto_cycle_durations[channel_id] = duration;
            auto_cycle_timers[channel_id] = 0; // 重置计时器
            ESP_LOGI(TAG, "Auto-cycle duration set to %ld ms for channel %d", duration, channel_id);
        } else {
            xSemaphoreGive(ws2812_mutex);
            return ESP_ERR_INVALID_ARG;
        }
        xSemaphoreGive(ws2812_mutex);
        
        // 自动保存配置
        esp_err_t save_ret = ws2812_save_config();
        if (save_ret != ESP_OK) {
            ESP_LOGW(TAG, "Failed to save config after cycle duration change: %s", esp_err_to_name(save_ret));
        }
        
        return ESP_OK;
    }
    return ESP_ERR_TIMEOUT;
}

void ws2812_task(void *pvParameters) {
    static uint32_t counters[WS2812_CHANNEL_COUNT] = {0};
    static int directions[WS2812_CHANNEL_COUNT] = {1, 1, 1, 1};
    static uint32_t breath_values[WS2812_CHANNEL_COUNT] = {0};
    
    task_running = true;
    ESP_LOGI(TAG, "WS2812 multi-channel task started");
    
    while (task_running) {
        // 处理每个通道
        for (int ch = 0; ch < WS2812_CHANNEL_COUNT; ch++) {
            if (!led_strips[ch] || !channels[ch].enabled) {
                if (!led_strips[ch]) {
                    ESP_LOGD(TAG, "Channel %d: LED strip not initialized", ch);
                }
                if (!channels[ch].enabled) {
                    ESP_LOGD(TAG, "Channel %d: Channel disabled", ch);
                }
                continue; // 跳过未初始化或禁用的通道
            }
            
            ESP_LOGV(TAG, "Processing channel %d on GPIO %d", ch, ws2812_gpio_pins[ch]);
            
            ws2812_config_t current_config = channels[ch].config;
            ws2812_mode_t effective_mode = current_config.mode;
            
            // 处理自动循环模式
            if (current_config.mode == WS2812_MODE_AUTO_CYCLE) {
                auto_cycle_timers[ch] += current_config.speed;
                
                // 检查是否需要切换到下一个效果
                if (auto_cycle_timers[ch] >= auto_cycle_durations[ch]) {
                    current_auto_mode_indices[ch] = (current_auto_mode_indices[ch] + 1) % auto_cycle_count;
                    auto_cycle_timers[ch] = 0;
                    
                    // 为不同效果设置不同颜色
                    rgb_color_t cycle_colors[] = {
                        {255, 255, 255}, // 彩虹效果 - 白色（实际显示彩虹）
                        {255, 100, 50},  // 呼吸灯效果 - 橙色
                        {0, 255, 150},   // 跑马灯效果 - 青绿色
                        {150, 0, 255},   // 波浪效果 - 紫色
                        {255, 50, 50}    // 闪烁效果 - 红色
                    };
                    
                    // 更新配置中的颜色
                    if (xSemaphoreTake(ws2812_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
                        channels[ch].config.color = cycle_colors[current_auto_mode_indices[ch]];
                        xSemaphoreGive(ws2812_mutex);
                    }
                    
                    ESP_LOGI(TAG, "Channel %d auto-cycle switched to mode %d", ch, auto_cycle_modes[current_auto_mode_indices[ch]]);
                }
                
                effective_mode = auto_cycle_modes[current_auto_mode_indices[ch]];
            }
            
            switch (effective_mode) {
                case WS2812_MODE_OFF:
                    // 关闭所有LED
                    led_strip_clear(led_strips[ch]);
                    break;
                    
                case WS2812_MODE_STATIC:
                    // 静态颜色
                    {
                        rgb_color_t color = apply_brightness(current_config.color, current_config.brightness);
                        for (int i = 0; i < WS2812_LED_COUNT; i++) {
                            led_strip_set_pixel(led_strips[ch], i, color.r, color.g, color.b);
                        }
                    }
                    break;
                    
                case WS2812_MODE_RAINBOW:
                    // 彩虹效果
                    for (int i = 0; i < WS2812_LED_COUNT; i++) {
                        uint16_t hue = (counters[ch] + i * 255 / WS2812_LED_COUNT) % 256;
                        rgb_color_t color = hsv_to_rgb(hue, 255, 255);
                        color = apply_brightness(color, current_config.brightness);
                        led_strip_set_pixel(led_strips[ch], i, color.r, color.g, color.b);
                    }
                    counters[ch] = (counters[ch] + 5) % 256;
                    break;
                    
                case WS2812_MODE_BREATHING:
                    // 呼吸灯效果 - 速度和颜色可调
                    {
                        // 根据speed参数调整呼吸速度，speed越小，呼吸越快
                        int breath_step = (1000 / current_config.speed) * 2 + 1; // 最小步进为1
                        if (breath_step > 20) breath_step = 20; // 限制最大步进
                        
                        uint8_t breath_brightness = (uint8_t)(128 + 127 * sin(breath_values[ch] * M_PI / 180));
                        breath_brightness = (breath_brightness * current_config.brightness) / 255;
                        rgb_color_t color = apply_brightness(current_config.color, breath_brightness);
                        
                        for (int i = 0; i < WS2812_LED_COUNT; i++) {
                            led_strip_set_pixel(led_strips[ch], i, color.r, color.g, color.b);
                        }
                        breath_values[ch] = (breath_values[ch] + breath_step) % 360;
                    }
                    break;
                    
                case WS2812_MODE_RUNNING:
                    // 跑马灯效果 - 速度和颜色可调
                    led_strip_clear(led_strips[ch]);
                    {
                        rgb_color_t color = apply_brightness(current_config.color, current_config.brightness);
                        
                        // 可配置跑马灯长度和间距
                        int tail_length = 5; // 拖尾长度
                        for (int i = 0; i < tail_length; i++) {
                            int pos = (counters[ch] - i + WS2812_LED_COUNT) % WS2812_LED_COUNT;
                            // 创建渐变拖尾效果
                            uint8_t tail_brightness = (uint8_t)((tail_length - i) * 255 / tail_length);
                            tail_brightness = (tail_brightness * current_config.brightness) / 255;
                            rgb_color_t tail_color = apply_brightness(current_config.color, tail_brightness);
                            led_strip_set_pixel(led_strips[ch], pos, tail_color.r, tail_color.g, tail_color.b);
                        }
                        
                        // 根据speed参数调整移动速度
                        static uint32_t last_move_time[WS2812_CHANNEL_COUNT] = {0};
                        uint32_t current_time = xTaskGetTickCount() * portTICK_PERIOD_MS;
                        if (current_time - last_move_time[ch] >= current_config.speed) {
                            counters[ch] = (counters[ch] + directions[ch] + WS2812_LED_COUNT) % WS2812_LED_COUNT;
                            last_move_time[ch] = current_time;
                        }
                    }
                    break;
                    
                case WS2812_MODE_FLASH:
                    // 闪烁效果 - 速度和颜色可调
                    {
                        static uint32_t last_flash_time[WS2812_CHANNEL_COUNT] = {0};
                        uint32_t current_time = xTaskGetTickCount() * portTICK_PERIOD_MS;
                        
                        // 根据speed参数确定闪烁周期
                        uint32_t flash_period = current_config.speed * 2; // 完整闪烁周期
                        uint32_t time_in_cycle = (current_time - last_flash_time[ch]) % flash_period;
                        
                        if (time_in_cycle < current_config.speed) {
                            // 亮起阶段 - 使用配置的颜色
                            rgb_color_t color = apply_brightness(current_config.color, current_config.brightness);
                            for (int i = 0; i < WS2812_LED_COUNT; i++) {
                                led_strip_set_pixel(led_strips[ch], i, color.r, color.g, color.b);
                            }
                        } else {
                            // 熄灭阶段
                            led_strip_clear(led_strips[ch]);
                        }
                        
                        // 重置计时器以避免溢出
                        if (time_in_cycle == 0) {
                            last_flash_time[ch] = current_time;
                        }
                    }
                    break;
                    
                case WS2812_MODE_WAVE:
                    // 波浪效果
                    for (int i = 0; i < WS2812_LED_COUNT; i++) {
                        uint8_t wave_brightness = (uint8_t)(128 + 127 * sin((counters[ch] + i * 20) * M_PI / 180));
                        wave_brightness = (wave_brightness * current_config.brightness) / 255;
                        rgb_color_t color = apply_brightness(current_config.color, wave_brightness);
                        led_strip_set_pixel(led_strips[ch], i, color.r, color.g, color.b);
                    }
                    counters[ch] = (counters[ch] + 10) % 360;
                    break;
                    
                default:
                    led_strip_clear(led_strips[ch]);
                    break;
            }
            
            // 刷新LED显示
            led_strip_refresh(led_strips[ch]);
        }
        
        // 使用固定的任务延时，不影响各效果的独立速度控制
        vTaskDelay(pdMS_TO_TICKS(20)); // 50Hz 刷新率
    }
    
    ESP_LOGI(TAG, "WS2812 multi-channel task ended");
    vTaskDelete(NULL);
}

esp_err_t ws2812_handle_json_command(const char *json_command) {
    if (json_command == NULL || strlen(json_command) == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    
    ESP_LOGI(TAG, "Processing JSON command: %s", json_command);
    
    cJSON *json = cJSON_Parse(json_command);
    if (json == NULL) {
        ESP_LOGE(TAG, "Invalid JSON format");
        return ESP_ERR_INVALID_ARG;
    }
    
    esp_err_t ret = ESP_OK;
    
    // 检查是否是状态查询
    cJSON *action = cJSON_GetObjectItem(json, "action");
    if (action && cJSON_IsString(action)) {
        if (strcmp(action->valuestring, "status") == 0) {
            // 返回所有通道状态
            ESP_LOGI(TAG, "=== WS2812 Multi-Channel Status ===");
            for (int ch = 0; ch < WS2812_CHANNEL_COUNT; ch++) {
                ws2812_channel_t config = ws2812_get_channel_config(ch);
                ESP_LOGI(TAG, "Channel %d (GPIO %d): %s, Mode:%d, RGB(%d,%d,%d), Brightness:%d, Speed:%ld", 
                         ch, ws2812_gpio_pins[ch], config.enabled ? "Enabled" : "Disabled",
                         config.config.mode, config.config.color.r, config.config.color.g, 
                         config.config.color.b, config.config.brightness, config.config.speed);
            }
            cJSON_Delete(json);
            return ESP_OK;
        }
    }
    
    // 获取通道ID
    cJSON *channel_json = cJSON_GetObjectItem(json, "channel");
    if (!channel_json || !cJSON_IsNumber(channel_json)) {
        ESP_LOGE(TAG, "Missing or invalid channel parameter");
        cJSON_Delete(json);
        return ESP_ERR_INVALID_ARG;
    }
    
    uint8_t channel_id = (uint8_t)channel_json->valueint;
    if (channel_id != WS2812_BROADCAST_ID && channel_id >= WS2812_CHANNEL_COUNT) {
        ESP_LOGE(TAG, "Invalid channel ID: %d", channel_id);
        cJSON_Delete(json);
        return ESP_ERR_INVALID_ARG;
    }
    
    // 处理各种命令
    cJSON *mode_json = cJSON_GetObjectItem(json, "mode");
    if (mode_json && cJSON_IsNumber(mode_json)) {
        int mode = mode_json->valueint;
        if (mode >= 0 && mode < WS2812_MODE_MAX) {
            ret = ws2812_set_mode(channel_id, (ws2812_mode_t)mode);
            if (ret != ESP_OK) goto cleanup;
        } else {
            ESP_LOGE(TAG, "Invalid mode: %d", mode);
            ret = ESP_ERR_INVALID_ARG;
            goto cleanup;
        }
    }
    
    cJSON *color_json = cJSON_GetObjectItem(json, "color");
    if (color_json && cJSON_IsObject(color_json)) {
        cJSON *r_json = cJSON_GetObjectItem(color_json, "r");
        cJSON *g_json = cJSON_GetObjectItem(color_json, "g");
        cJSON *b_json = cJSON_GetObjectItem(color_json, "b");
        
        if (r_json && g_json && b_json && 
            cJSON_IsNumber(r_json) && cJSON_IsNumber(g_json) && cJSON_IsNumber(b_json)) {
            int r = r_json->valueint;
            int g = g_json->valueint;
            int b = b_json->valueint;
            
            if (r >= 0 && r <= 255 && g >= 0 && g <= 255 && b >= 0 && b <= 255) {
                ret = ws2812_set_color(channel_id, (uint8_t)r, (uint8_t)g, (uint8_t)b);
                if (ret != ESP_OK) goto cleanup;
            } else {
                ESP_LOGE(TAG, "Invalid color values: RGB(%d,%d,%d)", r, g, b);
                ret = ESP_ERR_INVALID_ARG;
                goto cleanup;
            }
        }
    }
    
    cJSON *brightness_json = cJSON_GetObjectItem(json, "brightness");
    if (brightness_json && cJSON_IsNumber(brightness_json)) {
        int brightness = brightness_json->valueint;
        if (brightness >= 0 && brightness <= 255) {
            ret = ws2812_set_brightness(channel_id, (uint8_t)brightness);
            if (ret != ESP_OK) goto cleanup;
        } else {
            ESP_LOGE(TAG, "Invalid brightness: %d", brightness);
            ret = ESP_ERR_INVALID_ARG;
            goto cleanup;
        }
    }
    
    cJSON *speed_json = cJSON_GetObjectItem(json, "speed");
    if (speed_json && cJSON_IsNumber(speed_json)) {
        int speed = speed_json->valueint;
        if (speed >= 1 && speed <= 10000) {
            ret = ws2812_set_speed(channel_id, (uint32_t)speed);
            if (ret != ESP_OK) goto cleanup;
        } else {
            ESP_LOGE(TAG, "Invalid speed: %d", speed);
            ret = ESP_ERR_INVALID_ARG;
            goto cleanup;
        }
    }
    
    cJSON *enabled_json = cJSON_GetObjectItem(json, "enabled");
    if (enabled_json && cJSON_IsBool(enabled_json)) {
        bool enabled = cJSON_IsTrue(enabled_json);
        ret = ws2812_set_channel_enabled(channel_id, enabled);
        if (ret != ESP_OK) goto cleanup;
    }
    
    cJSON *cycle_duration_json = cJSON_GetObjectItem(json, "cycle_duration");
    if (cycle_duration_json && cJSON_IsNumber(cycle_duration_json)) {
        int duration = cycle_duration_json->valueint;
        if (duration >= 1000 && duration <= 60000) {
            ret = ws2812_set_cycle_duration(channel_id, (uint32_t)duration);
            if (ret != ESP_OK) goto cleanup;
        } else {
            ESP_LOGE(TAG, "Invalid cycle duration: %d", duration);
            ret = ESP_ERR_INVALID_ARG;
            goto cleanup;
        }
    }
    
cleanup:
    cJSON_Delete(json);
    return ret;
}

esp_err_t ws2812_test_all_channels(void) {
    ESP_LOGI(TAG, "=== WS2812 Multi-Channel Test ===");
    
    // 测试每个通道独立点亮
    rgb_color_t test_colors[] = {
        {255, 0, 0},    // 红色 - 通道0
        {0, 255, 0},    // 绿色 - 通道1  
        {0, 0, 255},    // 蓝色 - 通道2
        {255, 255, 0}   // 黄色 - 通道3
    };
    
    for (int ch = 0; ch < WS2812_CHANNEL_COUNT; ch++) {
        if (!led_strips[ch] || !channels[ch].enabled) {
            ESP_LOGW(TAG, "Channel %d (GPIO %d): Not available", ch, ws2812_gpio_pins[ch]);
            continue;
        }
        
        ESP_LOGI(TAG, "Testing Channel %d (GPIO %d) with color RGB(%d,%d,%d)", 
                 ch, ws2812_gpio_pins[ch], 
                 test_colors[ch].r, test_colors[ch].g, test_colors[ch].b);
        
        // 点亮当前通道
        for (int i = 0; i < WS2812_LED_COUNT; i++) {
            led_strip_set_pixel(led_strips[ch], i, 
                              test_colors[ch].r / 4,  // 降低亮度以节省电流
                              test_colors[ch].g / 4, 
                              test_colors[ch].b / 4);
        }
        led_strip_refresh(led_strips[ch]);
        
        // 保持2秒
        vTaskDelay(pdMS_TO_TICKS(2000));
        
        // 关闭当前通道
        led_strip_clear(led_strips[ch]);
        led_strip_refresh(led_strips[ch]);
        
        ESP_LOGI(TAG, "Channel %d test completed", ch);
    }
    
    ESP_LOGI(TAG, "=== All Channel Test Completed ===");
    return ESP_OK;
}

/**
 * @brief 保存WS2812参数到NVS
 */
esp_err_t ws2812_save_config(void) {
    nvs_handle_t nvs_handle;
    esp_err_t ret;
    
    // 打开NVS
    ret = nvs_open(WS2812_NVS_NAMESPACE, NVS_READWRITE, &nvs_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open NVS handle for saving: %s", esp_err_to_name(ret));
        return ret;
    }
    
    if (xSemaphoreTake(ws2812_mutex, pdMS_TO_TICKS(1000)) == pdTRUE) {
        // 保存配置版本号
        ret = nvs_set_u32(nvs_handle, "version", WS2812_CONFIG_VERSION);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to save config version: %s", esp_err_to_name(ret));
            goto cleanup;
        }
        
        // 保存每个通道的配置
        for (int ch = 0; ch < WS2812_CHANNEL_COUNT; ch++) {
            char key[32];
            
            // 保存通道配置结构体
            snprintf(key, sizeof(key), "%s%d", WS2812_NVS_KEY_PREFIX, ch);
            ret = nvs_set_blob(nvs_handle, key, &channels[ch], sizeof(ws2812_channel_t));
            if (ret != ESP_OK) {
                ESP_LOGE(TAG, "Failed to save channel %d config: %s", ch, esp_err_to_name(ret));
                goto cleanup;
            }
            
            // 保存自动循环持续时间
            snprintf(key, sizeof(key), "cycle_dur_%d", ch);
            ret = nvs_set_u32(nvs_handle, key, auto_cycle_durations[ch]);
            if (ret != ESP_OK) {
                ESP_LOGE(TAG, "Failed to save channel %d cycle duration: %s", ch, esp_err_to_name(ret));
                goto cleanup;
            }
        }
        
        // 提交写入
        ret = nvs_commit(nvs_handle);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to commit NVS data: %s", esp_err_to_name(ret));
            goto cleanup;
        }
        
        ESP_LOGI(TAG, "WS2812 configuration saved successfully");
        
cleanup:
        xSemaphoreGive(ws2812_mutex);
    } else {
        ESP_LOGE(TAG, "Failed to acquire mutex for saving config");
        ret = ESP_ERR_TIMEOUT;
    }
    
    nvs_close(nvs_handle);
    return ret;
}

/**
 * @brief 从NVS加载WS2812参数
 */
esp_err_t ws2812_load_config(void) {
    nvs_handle_t nvs_handle;
    esp_err_t ret;
    uint32_t version = 0;
    
    // 打开NVS
    ret = nvs_open(WS2812_NVS_NAMESPACE, NVS_READONLY, &nvs_handle);
    if (ret != ESP_OK) {
        if (ret == ESP_ERR_NVS_NOT_FOUND) {
            ESP_LOGI(TAG, "No saved WS2812 config found, using defaults");
            return ws2812_reset_config(); // 使用默认配置
        }
        ESP_LOGE(TAG, "Failed to open NVS handle for loading: %s", esp_err_to_name(ret));
        return ret;
    }
    
    // 检查配置版本
    ret = nvs_get_u32(nvs_handle, "version", &version);
    if (ret != ESP_OK || version != WS2812_CONFIG_VERSION) {
        ESP_LOGW(TAG, "Config version mismatch or not found, using defaults");
        nvs_close(nvs_handle);
        return ws2812_reset_config();
    }
    
    if (xSemaphoreTake(ws2812_mutex, pdMS_TO_TICKS(1000)) == pdTRUE) {
        bool load_success = true;
        
        // 加载每个通道的配置
        for (int ch = 0; ch < WS2812_CHANNEL_COUNT; ch++) {
            char key[32];
            size_t required_size = sizeof(ws2812_channel_t);
            
            // 加载通道配置
            snprintf(key, sizeof(key), "%s%d", WS2812_NVS_KEY_PREFIX, ch);
            ret = nvs_get_blob(nvs_handle, key, &channels[ch], &required_size);
            if (ret != ESP_OK) {
                ESP_LOGW(TAG, "Failed to load channel %d config: %s, using default", 
                         ch, esp_err_to_name(ret));
                // 使用默认配置
                channels[ch].channel_id = ch;
                channels[ch].enabled = true;
                channels[ch].config.mode = WS2812_MODE_AUTO_CYCLE;
                channels[ch].config.color = (rgb_color_t){255, 255, 255};
                channels[ch].config.speed = 150;
                channels[ch].config.brightness = 180;
                load_success = false;
            }
            
            // 加载自动循环持续时间
            snprintf(key, sizeof(key), "cycle_dur_%d", ch);
            ret = nvs_get_u32(nvs_handle, key, &auto_cycle_durations[ch]);
            if (ret != ESP_OK) {
                ESP_LOGW(TAG, "Failed to load channel %d cycle duration, using default", ch);
                auto_cycle_durations[ch] = 8000; // 默认8秒
            }
            
            // 验证加载的数据有效性
            if (channels[ch].config.mode >= WS2812_MODE_MAX) {
                ESP_LOGW(TAG, "Invalid mode for channel %d, resetting to AUTO_CYCLE", ch);
                channels[ch].config.mode = WS2812_MODE_AUTO_CYCLE;
                load_success = false;
            }
            
            if (channels[ch].config.brightness > 255) {
                ESP_LOGW(TAG, "Invalid brightness for channel %d, resetting to 180", ch);
                channels[ch].config.brightness = 180;
                load_success = false;
            }
            
            if (auto_cycle_durations[ch] < 1000 || auto_cycle_durations[ch] > 60000) {
                ESP_LOGW(TAG, "Invalid cycle duration for channel %d, resetting to 8000ms", ch);
                auto_cycle_durations[ch] = 8000;
                load_success = false;
            }
        }
        
        if (load_success) {
            ESP_LOGI(TAG, "WS2812 configuration loaded successfully");
        } else {
            ESP_LOGW(TAG, "Some configuration data was invalid, using mixed default/loaded values");
        }
        
        // 重置自动循环计时器
        for (int ch = 0; ch < WS2812_CHANNEL_COUNT; ch++) {
            auto_cycle_timers[ch] = 0;
            current_auto_mode_indices[ch] = 0;
        }
        
        xSemaphoreGive(ws2812_mutex);
    } else {
        ESP_LOGE(TAG, "Failed to acquire mutex for loading config");
        ret = ESP_ERR_TIMEOUT;
    }
    
    nvs_close(nvs_handle);
    return ret;
}

/**
 * @brief 重置WS2812配置为默认值
 */
esp_err_t ws2812_reset_config(void) {
    if (xSemaphoreTake(ws2812_mutex, pdMS_TO_TICKS(1000)) == pdTRUE) {
        // 初始化每个通道的默认配置
        for (int ch = 0; ch < WS2812_CHANNEL_COUNT; ch++) {
            channels[ch].channel_id = ch;
            channels[ch].enabled = true;
            channels[ch].config.mode = WS2812_MODE_AUTO_CYCLE;
            channels[ch].config.color = (rgb_color_t){255, 255, 255};
            channels[ch].config.speed = 150;
            channels[ch].config.brightness = 180;
            
            // 重置自动循环相关参数
            auto_cycle_durations[ch] = 8000;
            auto_cycle_timers[ch] = 0;
            current_auto_mode_indices[ch] = 0;
        }
        
        ESP_LOGI(TAG, "WS2812 configuration reset to defaults");
        xSemaphoreGive(ws2812_mutex);
        return ESP_OK;
    }
    
    ESP_LOGE(TAG, "Failed to acquire mutex for resetting config");
    return ESP_ERR_TIMEOUT;
}
