#include "ws2812_control.h"
#include "driver/rmt_tx.h"
#include "led_strip.h"
#include "esp_log.h"
#include "esp_task_wdt.h"  // 添加看门狗头文件
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "Lib/cJSON/cJSON.h"
#include "nvs_flash.h"
#include "nvs.h"
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include "i2s_mic.h"

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
// 广播主色/亮度：当对所有通道设置相同模式/颜色/亮度时，保持广播一致性
static rgb_color_t broadcast_master_color = {255, 255, 255};
static uint8_t broadcast_master_brightness = 180;

// 电量显示相关变量
static ws2812_battery_config_t battery_config = {
    .battery_channel = WS2812_BATTERY_CHANNEL_DISABLED,
    .show_charging_effect = true,
    .background_brightness = 10, // 默认背景亮度为10
};
static int current_battery_percentage = 50;
static bool current_is_charging = false;
static bool battery_display_needs_refresh = false; // 电量显示刷新标志

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

// 音乐律动模式相关变量
static int32_t music_volume = 0;
static uint32_t last_beat_time = 0;

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

// 音乐律动模式应用函数
static void ws2812_apply_music_rhythm(int ch, rgb_color_t color, uint8_t brightness) {
    rgb_color_t final_color = apply_brightness(color, brightness);
    for (int i = 0; i < channels[ch].led_count; i++) {
        led_strip_set_pixel(led_strips[ch], i, final_color.r, final_color.g, final_color.b);
    }
    // 音乐模式需要在处理函数内刷新显示
    if (led_strips[ch]) {
        led_strip_refresh(led_strips[ch]);
    }
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
        channels[ch].led_count = WS2812_LED_COUNT_DEFAULT;
        channels[ch].config.mode = WS2812_MODE_AUTO_CYCLE;
        channels[ch].config.color = (rgb_color_t){255, 255, 255};
        channels[ch].config.speed = 150;
        channels[ch].config.brightness = 180;
        
        ESP_LOGI(TAG, "Initializing channel %d on GPIO %d", ch, ws2812_gpio_pins[ch]);
        
        // 配置LED strip - 使用可配置的LED数量
        led_strip_config_t strip_config = {
            .strip_gpio_num = ws2812_gpio_pins[ch],
            .max_leds = WS2812_MAX_LED_COUNT,  // 使用最大值，实际使用数量在运行时控制
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
                 ch, ws2812_gpio_pins[ch], channels[ch].led_count);
    }
    
    ESP_LOGI(TAG, "WS2812 multi-channel system initialized successfully");
    
    // 测试所有通道 - 短暂点亮白色
    ESP_LOGI(TAG, "Testing all channels with white light for 2 seconds...");
    for (int ch = 0; ch < WS2812_CHANNEL_COUNT; ch++) {
        if (led_strips[ch]) {
            for (int i = 0; i < channels[ch].led_count; i++) {
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
            // 广播到所有通道 — 同步主色与亮度
            for (int ch = 0; ch < WS2812_CHANNEL_COUNT; ch++) {
                channels[ch].config.mode = mode;
                channels[ch].config.color = broadcast_master_color;
                channels[ch].config.brightness = broadcast_master_brightness;
            }
            ESP_LOGI(TAG, "Mode set to %d for all channels (broadcast)", mode);
        } else if (channel_id < WS2812_CHANNEL_COUNT) {
            // 设置指定通道
            channels[channel_id].config.mode = mode;

            // 如果多个通道使用相同模式，尝试与它们同步颜色/亮度
            for (int ch = 0; ch < WS2812_CHANNEL_COUNT; ch++) {
                if (ch == channel_id) continue;
                if (channels[ch].config.mode == mode) {
                    // 同步色/亮度到新设置的通道，优先使用已有通道的色/亮度
                    channels[channel_id].config.color = channels[ch].config.color;
                    channels[channel_id].config.brightness = channels[ch].config.brightness;
                    break;
                }
            }
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
            // 广播到所有通道，并更新广播主色
            broadcast_master_color.r = r;
            broadcast_master_color.g = g;
            broadcast_master_color.b = b;
            for (int ch = 0; ch < WS2812_CHANNEL_COUNT; ch++) {
                channels[ch].config.color = broadcast_master_color;
            }
            ESP_LOGI(TAG, "Color set to RGB(%d,%d,%d) for all channels (broadcast)", r, g, b);
        } else if (channel_id < WS2812_CHANNEL_COUNT) {
            // 设置指定通道
            channels[channel_id].config.color.r = r;
            channels[channel_id].config.color.g = g;
            channels[channel_id].config.color.b = b;

            // 同步使用相同模式的其他通道（保持一致性）
            for (int ch = 0; ch < WS2812_CHANNEL_COUNT; ch++) {
                if (ch == channel_id) continue;
                if (channels[ch].config.mode == channels[channel_id].config.mode) {
                    channels[ch].config.color = channels[channel_id].config.color;
                }
            }

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
            // 广播到所有通道，并更新广播主亮度
            broadcast_master_brightness = brightness;
            for (int ch = 0; ch < WS2812_CHANNEL_COUNT; ch++) {
                channels[ch].config.brightness = brightness;
            }
            ESP_LOGI(TAG, "Brightness set to %d for all channels (broadcast)", brightness);
        } else if (channel_id < WS2812_CHANNEL_COUNT) {
            // 设置指定通道
            channels[channel_id].config.brightness = brightness;

            // 同步相同模式的其他通道亮度
            for (int ch = 0; ch < WS2812_CHANNEL_COUNT; ch++) {
                if (ch == channel_id) continue;
                if (channels[ch].config.mode == channels[channel_id].config.mode) {
                    channels[ch].config.brightness = brightness;
                }
            }

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

esp_err_t ws2812_set_cycle_duration(uint8_t channel_id, uint32_t duration) {
    if (channel_id != WS2812_BROADCAST_ID && channel_id >= WS2812_CHANNEL_COUNT) {
        return ESP_ERR_INVALID_ARG;
    }
    
    if (duration < 1000 || duration > 60000) {
        return ESP_ERR_INVALID_ARG;
    }
    
    if (channel_id == WS2812_BROADCAST_ID) {
        for (int ch = 0; ch < WS2812_CHANNEL_COUNT; ch++) {
            auto_cycle_durations[ch] = duration;
        }
    } else {
        auto_cycle_durations[channel_id] = duration;
    }
    
    return ESP_OK;
}

uint32_t ws2812_get_cycle_duration(uint8_t channel_id) {
    if (channel_id >= WS2812_CHANNEL_COUNT) {
        return 8000; // Default value
    }
    return auto_cycle_durations[channel_id];
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

esp_err_t ws2812_set_led_count(uint8_t channel_id, uint16_t led_count) {
    if (led_count < 1) led_count = 1;
    if (led_count > WS2812_MAX_LED_COUNT) led_count = WS2812_MAX_LED_COUNT;
    
    if (xSemaphoreTake(ws2812_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        if (channel_id == WS2812_BROADCAST_ID) {
            // 广播到所有通道
            bool has_battery_channel = false;
            for (int ch = 0; ch < WS2812_CHANNEL_COUNT; ch++) {
                channels[ch].led_count = led_count;
                // 清空所有LED
                if (led_strips[ch]) {
                    led_strip_clear(led_strips[ch]);
                    led_strip_refresh(led_strips[ch]);
                }
                // 检查是否有电量显示通道
                if (channels[ch].config.mode == WS2812_MODE_BATTERY) {
                    has_battery_channel = true;
                }
            }
            
            // 如果有电量显示通道，触发刷新
            if (has_battery_channel) {
                battery_display_needs_refresh = true;
            }
            
            ESP_LOGI(TAG, "LED count set to %d for all channels", led_count);
        } else if (channel_id < WS2812_CHANNEL_COUNT) {
            // 设置指定通道
            channels[channel_id].led_count = led_count;
            // 清空指定通道的所有LED
            if (led_strips[channel_id]) {
                led_strip_clear(led_strips[channel_id]);
                led_strip_refresh(led_strips[channel_id]);
            }
            
            // 如果是电量显示通道，触发刷新以恢复显示
            if (channels[channel_id].config.mode == WS2812_MODE_BATTERY) {
                battery_display_needs_refresh = true;
            }
            
            ESP_LOGI(TAG, "LED count set to %d for channel %d", led_count, channel_id);
        } else {
            xSemaphoreGive(ws2812_mutex);
            return ESP_ERR_INVALID_ARG;
        }
        xSemaphoreGive(ws2812_mutex);
        
        // 自动保存配置
        esp_err_t save_ret = ws2812_save_config();
        if (save_ret != ESP_OK) {
            ESP_LOGW(TAG, "Failed to save config after LED count change: %s", esp_err_to_name(save_ret));
        }
        
        return ESP_OK;
    }
    return ESP_ERR_TIMEOUT;
}

esp_err_t ws2812_set_battery_display(uint8_t battery_channel, bool show_charging_effect, uint8_t background_brightness) {
    if (battery_channel != WS2812_BATTERY_CHANNEL_DISABLED && battery_channel >= WS2812_CHANNEL_COUNT) {
        return ESP_ERR_INVALID_ARG;
    }
    
    if (xSemaphoreTake(ws2812_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        battery_config.battery_channel = battery_channel;
        battery_config.show_charging_effect = show_charging_effect;
        battery_config.background_brightness = background_brightness;
        
        // 如果启用了电量显示，将对应通道设置为电量模式
        if (battery_channel != WS2812_BATTERY_CHANNEL_DISABLED) {
            channels[battery_channel].config.mode = WS2812_MODE_BATTERY;
            battery_display_needs_refresh = true; // 设置刷新标志
        }
        
        xSemaphoreGive(ws2812_mutex);
        
        ESP_LOGI(TAG, "Battery display config: channel=%d, charging_effect=%s, bg_brightness=%d",
                 battery_channel, show_charging_effect ? "true" : "false", background_brightness);
        
        // 自动保存配置
        esp_err_t save_ret = ws2812_save_config();
        if (save_ret != ESP_OK) {
            ESP_LOGW(TAG, "Failed to save config after battery display change: %s", esp_err_to_name(save_ret));
        }
        
        return ESP_OK;
    }
    return ESP_ERR_TIMEOUT;
}

esp_err_t ws2812_set_channel_battery_mode(uint8_t channel_id, bool enable_battery_mode, uint8_t background_brightness) {
    if (channel_id >= WS2812_CHANNEL_COUNT) {
        return ESP_ERR_INVALID_ARG;
    }
    
    if (xSemaphoreTake(ws2812_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        if (enable_battery_mode) {
            // 启用指定通道的电量显示模式
            channels[channel_id].config.mode = WS2812_MODE_BATTERY;
            
            // 更新电量显示配置，设置当前通道为电量显示通道
            battery_config.battery_channel = channel_id;
            battery_config.show_charging_effect = true;  // 默认启用充电特效
            battery_config.background_brightness = (background_brightness > 0) ? background_brightness : 10;
            battery_display_needs_refresh = true;
            
            ESP_LOGI(TAG, "Channel %d set to battery display mode, bg_brightness=%d", 
                     channel_id, battery_config.background_brightness);
        } else {
            // 禁用指定通道的电量显示模式
            if (battery_config.battery_channel == channel_id) {
                // 如果当前是电量显示通道，禁用全局电量显示
                battery_config.battery_channel = WS2812_BATTERY_CHANNEL_DISABLED;
            }
            
            // 将通道恢复为静态颜色模式
            channels[channel_id].config.mode = WS2812_MODE_STATIC;
            channels[channel_id].config.color = (rgb_color_t){255, 255, 255};  // 默认白色
            channels[channel_id].config.brightness = 128;  // 默认亮度
            
            ESP_LOGI(TAG, "Channel %d disabled battery display mode, restored to static mode", channel_id);
        }
        
        xSemaphoreGive(ws2812_mutex);
        
        // 自动保存配置
        esp_err_t save_ret = ws2812_save_config();
        if (save_ret != ESP_OK) {
            ESP_LOGW(TAG, "Failed to save config after channel battery mode change: %s", esp_err_to_name(save_ret));
        }
        
        return ESP_OK;
    }
    return ESP_ERR_TIMEOUT;
}

esp_err_t ws2812_update_battery_display(int battery_percentage, bool is_charging) {
    if (battery_percentage < 0) battery_percentage = 0;
    if (battery_percentage > 100) battery_percentage = 100;
    
    if (xSemaphoreTake(ws2812_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        // 防抖逻辑：只在数值真正变化时才更新，减少无效刷新
        if (current_battery_percentage != battery_percentage || current_is_charging != is_charging) {
            ESP_LOGI(TAG, "Battery display update: %d%% -> %d%%, charging: %s -> %s", 
                     current_battery_percentage, battery_percentage,
                     current_is_charging ? "true" : "false",
                     is_charging ? "true" : "false");
            current_battery_percentage = battery_percentage;
            current_is_charging = is_charging;
            battery_display_needs_refresh = true; // 设置刷新标志
        }
        xSemaphoreGive(ws2812_mutex);
        return ESP_OK;
    }
    return ESP_ERR_TIMEOUT;
}

ws2812_battery_config_t ws2812_get_battery_config(void) {
    ws2812_battery_config_t config = {0};
    if (xSemaphoreTake(ws2812_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        config = battery_config;
        xSemaphoreGive(ws2812_mutex);
    }
    return config;
}

void ws2812_task(void *pvParameters) {
    static uint32_t counters[WS2812_CHANNEL_COUNT] = {0};
    static int directions[WS2812_CHANNEL_COUNT] = {1, 1, 1, 1};
    static uint32_t breath_values[WS2812_CHANNEL_COUNT] = {0};
    static rgb_color_t rhythm_color = {255, 255, 255}; // 保持律动颜色
    
    task_running = true;
    ESP_LOGI(TAG, "WS2812 multi-channel task started");
    
    while (task_running) {
        // 检查是否有通道处于音乐律动模式
        bool need_mic = false;
        for (int i = 0; i < WS2812_CHANNEL_COUNT; i++) {
            if (channels[i].enabled && channels[i].config.mode == WS2812_MODE_MUSIC_RHYTHM) {
                need_mic = true;
                break;
            }
        }
        
        int32_t current_volume = 0;
        bool is_beat = false;
        
        if (need_mic) {
            current_volume = get_mic_volume();
            music_volume = current_volume; // Update global debug var
            
            // Global beat detection
            if (current_volume > 800) { 
                uint32_t current_time = xTaskGetTickCount() * portTICK_PERIOD_MS;
                if (current_time - last_beat_time > 150) // 节拍间隔
                {
                    is_beat = true;
                    last_beat_time = current_time;
                }
            }
            
            if (is_beat) {
                uint16_t hue = rand() % 360;
                rhythm_color = hsv_to_rgb(hue, 255, 255);
            }
        }
        
        // Calculate base brightness from volume
        uint8_t base_brightness = (uint8_t)(current_volume * 10 / 100);
        if (current_volume > 50 && base_brightness < 10) {
            base_brightness = 10;
        }

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
                        for (int i = 0; i < channels[ch].led_count; i++) {
                            led_strip_set_pixel(led_strips[ch], i, color.r, color.g, color.b);
                        }
                    }
                    break;
                    
                case WS2812_MODE_RAINBOW:
                    // 彩虹效果
                    for (int i = 0; i < channels[ch].led_count; i++) {
                        uint16_t hue = (counters[ch] + i * 255 / channels[ch].led_count) % 256;
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
                        
                        for (int i = 0; i < channels[ch].led_count; i++) {
                            led_strip_set_pixel(led_strips[ch], i, color.r, color.g, color.b);
                        }
                        breath_values[ch] = (breath_values[ch] + breath_step) % 360;
                    }
                    break;
                    
                case WS2812_MODE_RUNNING:
                    // 跑马灯效果 - 速度和颜色可调
                    led_strip_clear(led_strips[ch]);
                    {
                        // 可配置跑马灯长度和间距
                        int tail_length = 5; // 拖尾长度
                        for (int i = 0; i < tail_length; i++) {
                            int pos = (counters[ch] - i + channels[ch].led_count) % channels[ch].led_count;
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
                            counters[ch] = (counters[ch] + directions[ch] + channels[ch].led_count) % channels[ch].led_count;
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
                            for (int i = 0; i < channels[ch].led_count; i++) {
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
                    for (int i = 0; i < channels[ch].led_count; i++) {
                        uint8_t wave_brightness = (uint8_t)(128 + 127 * sin((counters[ch] + i * 20) * M_PI / 180));
                        wave_brightness = (wave_brightness * current_config.brightness) / 255;
                        rgb_color_t color = apply_brightness(current_config.color, wave_brightness);
                        led_strip_set_pixel(led_strips[ch], i, color.r, color.g, color.b);
                    }
                    counters[ch] = (counters[ch] + 10) % 360;
                    break;
                
                case WS2812_MODE_MUSIC_RHYTHM:
                    {
                        if (is_beat) {
                            channels[ch].config.color = rhythm_color;
                        }
                        
                        uint8_t ch_brightness = base_brightness;
                        if (ch_brightness > channels[ch].config.brightness) {
                            ch_brightness = channels[ch].config.brightness;
                        }
                        
                        ws2812_apply_music_rhythm(ch, rhythm_color, ch_brightness);
                    }
                    break;

                case WS2812_MODE_BATTERY:
                    // 电量显示模式 - 只在命令刷新时更新显示
                    {
                        static uint32_t charging_animation_counter = 0;
                        bool should_refresh = false;
                        
                        // 检查是否需要刷新显示
                        if (battery_display_needs_refresh) {
                            should_refresh = true;
                            battery_display_needs_refresh = false;
                        }
                        
                        // 充电跑马灯动画需要适中频率更新以保持流畅效果
                        if (current_is_charging && battery_config.show_charging_effect) {
                            charging_animation_counter++;
                            if (charging_animation_counter >= 4) { // 每200ms更新一次跑马灯，保持适中速度
                                charging_animation_counter = 0;
                                should_refresh = true;
                            }
                        } else {
                            // 非充电时的呼吸灯动画需要更频繁更新以提高动感
                            static uint32_t breathing_counter = 0;
                            breathing_counter++;
                            if (breathing_counter >= 2) { // 每100ms更新一次呼吸灯，更快的呼吸节奏
                                breathing_counter = 0;
                                should_refresh = true;
                            }
                        }
                        
                        // 只在需要时刷新LED显示
                        if (should_refresh) {
                            // 计算电量显示的LED数量 - 使用整数运算和四舍五入
                            int battery_leds = (channels[ch].led_count * current_battery_percentage + 50) / 100;
                            if (battery_leds > channels[ch].led_count) battery_leds = channels[ch].led_count;
                            if (battery_leds < 0) battery_leds = 0;
                            
                            // 设置背景LED（低亮度白色）
                            rgb_color_t bg_color = {battery_config.background_brightness, 
                                                  battery_config.background_brightness, 
                                                  battery_config.background_brightness};
                            
                            // 先设置所有LED为背景颜色
                            for (int i = 0; i < channels[ch].led_count; i++) {
                                led_strip_set_pixel(led_strips[ch], i, bg_color.r, bg_color.g, bg_color.b);
                            }
                            
                            // 设置电量LED（绿色）
                            if (current_is_charging && battery_config.show_charging_effect) {
                                // 充电时的跑马灯效果
                                static uint32_t runner_position = 0;
                                runner_position = (runner_position + 1) % (battery_leds * 2); // 跑马灯位置循环
                                
                                // 先设置所有电量LED为暗绿色基础色
                                for (int i = 0; i < battery_leds; i++) {
                                    led_strip_set_pixel(led_strips[ch], i, 0, 80, 0); // 暗绿色基础
                                }
                                
                                // 跑马灯主体：3个LED的亮点从左到右移动
                                int runner_center = runner_position % battery_leds;
                                
                                // 设置跑马灯的LED（中心LED最亮，两侧渐暗）
                                for (int offset = -1; offset <= 1; offset++) {
                                    int led_pos = runner_center + offset;
                                    
                                    // 处理边界循环
                                    if (led_pos < 0) led_pos += battery_leds;
                                    if (led_pos >= battery_leds) led_pos -= battery_leds;
                                    
                                    // 根据距离中心的位置设置亮度
                                    uint8_t brightness_factor = 0;
                                    if (offset == 0) {
                                        brightness_factor = 255; // 中心LED最亮
                                    } else {
                                        brightness_factor = 120; // 侧边LED稍暗
                                    }
                                    
                                    // 设置跑马灯颜色：根据电量状态选择颜色
                                    uint8_t red, green, blue;
                                    if (current_battery_percentage <= 30) {
                                        // 低电量充电：红色跑马灯
                                        red = brightness_factor;
                                        green = brightness_factor / 4;
                                        blue = 0;
                                    } else if (current_battery_percentage <= 50) {
                                        // 中等电量充电：橙色跑马灯
                                        red = brightness_factor;
                                        green = brightness_factor / 2;
                                        blue = 0;
                                    } else {
                                        // 正常电量充电：绿色跑马灯
                                        red = brightness_factor / 8;
                                        green = brightness_factor;
                                        blue = brightness_factor / 4;
                                    }
                                    led_strip_set_pixel(led_strips[ch], led_pos, red, green, blue);
                                }
                                
                                // 添加尾迹效果：在跑马灯后面留下逐渐减弱的光点
                                for (int trail = 1; trail <= 3; trail++) {
                                    int trail_pos = runner_center - trail;
                                    if (trail_pos < 0) trail_pos += battery_leds;
                                    
                                    uint8_t trail_brightness = 150 / (trail + 1); // 尾迹亮度递减
                                    
                                    // 尾迹颜色与跑马灯主体颜色保持一致
                                    uint8_t trail_red, trail_green, trail_blue;
                                    if (current_battery_percentage <= 30) {
                                        // 低电量：红色尾迹
                                        trail_red = trail_brightness;
                                        trail_green = trail_brightness / 4;
                                        trail_blue = 0;
                                    } else if (current_battery_percentage <= 50) {
                                        // 中等电量：橙色尾迹
                                        trail_red = trail_brightness;
                                        trail_green = trail_brightness / 2;
                                        trail_blue = 0;
                                    } else {
                                        // 正常电量：绿色尾迹
                                        trail_red = 0;
                                        trail_green = trail_brightness;
                                        trail_blue = trail_brightness / 6;
                                    }
                                    
                                    led_strip_set_pixel(led_strips[ch], trail_pos, 
                                                       trail_red, trail_green, trail_blue);
                                }
                            } else {
                                // 非充电时的快速呼吸灯显示 - 根据电量选择颜色
                                static uint32_t breath_phase = 0;
                                breath_phase = (breath_phase + 1) % 60; // 缩短呼吸循环周期，更快呼吸
                                
                                // 计算呼吸效果的亮度因子 (0.2 - 1.0)，更大的亮度变化范围
                                float breath_factor = 0.2f + 0.8f * ((sin(breath_phase * M_PI / 30) + 1.0f) / 2.0f);
                                
                                for (int i = 0; i < battery_leds; i++) {
                                    uint8_t base_red, base_green, base_blue;
                                    
                                    // 根据电量状态选择基础颜色
                                    if (current_battery_percentage <= 30) {
                                        // 低电量警示：红色呼吸
                                        base_red = 255;
                                        base_green = 0;
                                        base_blue = 0;
                                    } else if (current_battery_percentage <= 50) {
                                        // 中等电量：橙色呼吸
                                        base_red = 255;
                                        base_green = 100;
                                        base_blue = 0;
                                    } else {
                                        // 正常电量：绿色呼吸
                                        base_red = 0;
                                        base_green = 255;
                                        base_blue = 0;
                                    }
                                    
                                    // 应用呼吸效果
                                    uint8_t breath_red = (uint8_t)(base_red * breath_factor);
                                    uint8_t breath_green = (uint8_t)(base_green * breath_factor);
                                    uint8_t breath_blue = (uint8_t)(base_blue * breath_factor);
                                    
                                    led_strip_set_pixel(led_strips[ch], i, breath_red, breath_green, breath_blue);
                                }
                            }
                            
                            // 刷新LED显示
                            led_strip_refresh(led_strips[ch]);
                        }
                        // 电量模式下跳过通用的LED刷新
                        continue;
                    }
                    break;
                    
                default:
                    led_strip_clear(led_strips[ch]);
                    break;
            }
            
            // 刷新LED显示
            if (effective_mode == WS2812_MODE_MUSIC_RHYTHM) {
                // 音乐模式下，由其自己的处理函数刷新
            } else {
                led_strip_refresh(led_strips[ch]);
            }
        }
        
        // 降低刷新频率以减少闪烁，特别是对电量显示模式
        vTaskDelay(pdMS_TO_TICKS(50)); // 20Hz 刷新率，减少闪烁
    }
    
    ESP_LOGI(TAG, "WS2812 multi-channel task ended");
    vTaskDelete(NULL);
}

esp_err_t ws2812_handle_json_command(const char *json_command) {
    if (json_command == NULL || strlen(json_command) == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    
    ESP_LOGI(TAG, "Processing JSON command: %s", json_command);
    
    // 重置看门狗以防止JSON处理超时
    // esp_task_wdt_reset();
    
    cJSON *json = cJSON_Parse(json_command);
    if (json == NULL) {
        ESP_LOGE(TAG, "Invalid JSON format");
        return ESP_ERR_INVALID_ARG;
    }
    
    // esp_task_wdt_reset(); // JSON解析后重置
    
    esp_err_t ret = ESP_OK;
    
    // 检查是否是状态查询或特殊命令
    cJSON *action = cJSON_GetObjectItem(json, "action");
    if (action && cJSON_IsString(action)) {
        if (strcmp(action->valuestring, "status") == 0) {
            // 返回所有通道状态
            ESP_LOGI(TAG, "=== WS2812 Multi-Channel Status ===");
            // esp_task_wdt_reset();
            for (int ch = 0; ch < WS2812_CHANNEL_COUNT; ch++) {
                ws2812_channel_t config = ws2812_get_channel_config(ch);
                ESP_LOGI(TAG, "Channel %d (GPIO %d): %s, Mode:%d, RGB(%d,%d,%d), Brightness:%d, Speed:%ld", 
                         ch, ws2812_gpio_pins[ch], config.enabled ? "Enabled" : "Disabled",
                         config.config.mode, config.config.color.r, config.config.color.g, 
                         config.config.color.b, config.config.brightness, config.config.speed);
                // esp_task_wdt_reset(); // 每次循环重置看门狗
            }
            cJSON_Delete(json);
            return ESP_OK;
        } else if (strcmp(action->valuestring, "save_config") == 0) {
            ESP_LOGI(TAG, "Executing save_config command");
            esp_err_t err = ws2812_save_config();
            cJSON_Delete(json);
            return err;
        } else if (strcmp(action->valuestring, "load_config") == 0) {
            ESP_LOGI(TAG, "Executing load_config command");
            esp_err_t err = ws2812_load_config();
            cJSON_Delete(json);
            return err;
        } else if (strcmp(action->valuestring, "test_all_channels") == 0) {
            ESP_LOGI(TAG, "Executing test_all_channels command");
            esp_err_t err = ws2812_test_all_channels();
            cJSON_Delete(json);
            return err;
        } else if (strcmp(action->valuestring, "ws2812_control") != 0) {
            // 不是WS2812控制命令，直接返回错误
            ESP_LOGE(TAG, "Invalid action: %s", action->valuestring);
            cJSON_Delete(json);
            return ESP_ERR_INVALID_ARG;
        }
    }
    
    // esp_task_wdt_reset(); // 命令处理前重置
    
    // 首先检查所有的JSON字段
    cJSON *battery_display_json = cJSON_GetObjectItem(json, "battery_display");
    cJSON *battery_status_json = cJSON_GetObjectItem(json, "battery_status");
    cJSON *battery_channel_json = cJSON_GetObjectItem(json, "battery_channel");
    cJSON *channel_json = cJSON_GetObjectItem(json, "channel");
    
    // 检查是否为电量相关命令（这些不需要channel字段）
    bool is_battery_command = (battery_display_json != NULL || 
                              battery_status_json != NULL || 
                              battery_channel_json != NULL);
    
    // 获取通道ID和验证
    uint8_t channel_id = 0;
    bool has_channel = false;
    
    if (channel_json && cJSON_IsNumber(channel_json)) {
        channel_id = (uint8_t)channel_json->valueint;
        if (channel_id != WS2812_BROADCAST_ID && channel_id >= WS2812_CHANNEL_COUNT) {
            ESP_LOGE(TAG, "Invalid channel ID: %d", channel_id);
            cJSON_Delete(json);
            return ESP_ERR_INVALID_ARG;
        }
        has_channel = true;
    } else if (!is_battery_command) {
        // 对于非电量命令，必须要有有效的channel参数
        ESP_LOGE(TAG, "Missing or invalid channel parameter for non-battery command");
        cJSON_Delete(json);
        return ESP_ERR_INVALID_ARG;
    }
    
    // 处理需要通道ID的命令
    cJSON *mode_json = cJSON_GetObjectItem(json, "mode");
    if (mode_json && cJSON_IsNumber(mode_json)) {
        if (!has_channel) {
            ESP_LOGE(TAG, "Mode command requires channel parameter");
            ret = ESP_ERR_INVALID_ARG;
            goto cleanup;
        }
        int mode = mode_json->valueint;
        if (mode >= 0 && mode < WS2812_MODE_MAX) {
            // esp_task_wdt_reset(); // 模式设置前重置看门狗
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
        if (!has_channel) {
            ESP_LOGE(TAG, "Color command requires channel parameter");
            ret = ESP_ERR_INVALID_ARG;
            goto cleanup;
        }
        // esp_task_wdt_reset(); // 颜色处理前重置看门狗
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
                // esp_task_wdt_reset(); // 颜色设置后重置看门狗
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
        if (!has_channel) {
            ESP_LOGE(TAG, "Brightness command requires channel parameter");
            ret = ESP_ERR_INVALID_ARG;
            goto cleanup;
        }
        // esp_task_wdt_reset(); // 亮度处理前重置看门狗
        int brightness = brightness_json->valueint;
        if (brightness >= 0 && brightness <= 255) {
            ret = ws2812_set_brightness(channel_id, (uint8_t)brightness);
            // esp_task_wdt_reset(); // 亮度设置后重置看门狗
            if (ret != ESP_OK) goto cleanup;
        } else {
            ESP_LOGE(TAG, "Invalid brightness: %d", brightness);
            ret = ESP_ERR_INVALID_ARG;
            goto cleanup;
        }
    }
    
    cJSON *speed_json = cJSON_GetObjectItem(json, "speed");
    if (speed_json && cJSON_IsNumber(speed_json)) {
        if (!has_channel) {
            ESP_LOGE(TAG, "Speed command requires channel parameter");
            ret = ESP_ERR_INVALID_ARG;
            goto cleanup;
        }
        // esp_task_wdt_reset(); // 速度处理前重置看门狗
        int speed = speed_json->valueint;
        if (speed >= 1 && speed <= 10000) {
            ret = ws2812_set_speed(channel_id, (uint32_t)speed);
            // esp_task_wdt_reset(); // 速度设置后重置看门狗
            if (ret != ESP_OK) goto cleanup;
        } else {
            ESP_LOGE(TAG, "Invalid speed: %d", speed);
            ret = ESP_ERR_INVALID_ARG;
            goto cleanup;
        }
    }
    
    cJSON *enabled_json = cJSON_GetObjectItem(json, "enabled");
    if (enabled_json && cJSON_IsBool(enabled_json)) {
        if (!has_channel) {
            ESP_LOGE(TAG, "Enabled command requires channel parameter");
            ret = ESP_ERR_INVALID_ARG;
            goto cleanup;
        }
        // esp_task_wdt_reset(); // 启用/禁用处理前重置看门狗
        bool enabled = cJSON_IsTrue(enabled_json);
        ret = ws2812_set_channel_enabled(channel_id, enabled);
        // esp_task_wdt_reset(); // 启用/禁用设置后重置看门狗
        if (ret != ESP_OK) goto cleanup;
    }
    
    cJSON *cycle_duration_json = cJSON_GetObjectItem(json, "cycle_duration");
    if (cycle_duration_json && cJSON_IsNumber(cycle_duration_json)) {
        if (!has_channel) {
            ESP_LOGE(TAG, "Cycle duration command requires channel parameter");
            ret = ESP_ERR_INVALID_ARG;
            goto cleanup;
        }
        // esp_task_wdt_reset(); // 循环持续时间处理前重置看门狗
        int duration = cycle_duration_json->valueint;
        if (duration >= 1000 && duration <= 60000) {
            ret = ws2812_set_cycle_duration(channel_id, (uint32_t)duration);
            // esp_task_wdt_reset(); // 循环持续时间设置后重置看门狗
            if (ret != ESP_OK) goto cleanup;
        } else {
            ESP_LOGE(TAG, "Invalid cycle duration: %d", duration);
            ret = ESP_ERR_INVALID_ARG;
            goto cleanup;
        }
    }
    
    // LED数量设置
    cJSON *led_count_json = cJSON_GetObjectItem(json, "led_count");
    if (led_count_json && cJSON_IsNumber(led_count_json)) {
        if (!has_channel) {
            ESP_LOGE(TAG, "LED count command requires channel parameter");
            ret = ESP_ERR_INVALID_ARG;
            goto cleanup;
        }
        // esp_task_wdt_reset(); // LED数量处理前重置看门狗
        int led_count = led_count_json->valueint;
        if (led_count >= 1 && led_count <= WS2812_MAX_LED_COUNT) {
            ret = ws2812_set_led_count(channel_id, (uint16_t)led_count);
            // esp_task_wdt_reset(); // LED数量设置后重置看门狗
            if (ret != ESP_OK) goto cleanup;
        } else {
            ESP_LOGE(TAG, "Invalid LED count: %d", led_count);
            ret = ESP_ERR_INVALID_ARG;
            goto cleanup;
        }
    }
    
    // 单通道电量显示模式设置
    cJSON *channel_battery_mode_json = cJSON_GetObjectItem(json, "channel_battery_mode");
    if (channel_battery_mode_json && cJSON_IsBool(channel_battery_mode_json)) {
        if (!has_channel) {
            ESP_LOGE(TAG, "Channel battery mode command requires channel parameter");
            ret = ESP_ERR_INVALID_ARG;
            goto cleanup;
        }
        // esp_task_wdt_reset(); // 电量模式处理前重置看门狗
        
        bool enable_battery_mode = cJSON_IsTrue(channel_battery_mode_json);
        uint8_t bg_brightness = 10;  // 默认背景亮度
        
        // 检查是否有背景亮度参数
        cJSON *bg_brightness_json = cJSON_GetObjectItem(json, "background_brightness");
        if (bg_brightness_json && cJSON_IsNumber(bg_brightness_json)) {
            int brightness = bg_brightness_json->valueint;
            if (brightness >= 0 && brightness <= 255) {
                bg_brightness = (uint8_t)brightness;
            }
        }
        
        ret = ws2812_set_channel_battery_mode(channel_id, enable_battery_mode, bg_brightness);
        // esp_task_wdt_reset(); // 电量模式设置后重置看门狗
        if (ret != ESP_OK) goto cleanup;
        
        ESP_LOGI(TAG, "Channel %d battery mode set to: %s, bg_brightness=%d", 
                 channel_id, enable_battery_mode ? "enabled" : "disabled", bg_brightness);
    }
    
    // 简化电量显示通道配置（与LCD兼容）
    if (battery_channel_json && cJSON_IsNumber(battery_channel_json)) {
        // esp_task_wdt_reset();
        int ch = battery_channel_json->valueint;
        uint8_t battery_channel = WS2812_BATTERY_CHANNEL_DISABLED;
        
        if (ch >= 0 && ch < WS2812_CHANNEL_COUNT) {
            battery_channel = (uint8_t)ch;
        } else if (ch == 255) {
            battery_channel = WS2812_BATTERY_CHANNEL_DISABLED;
        }
        
        // 使用默认设置：启用充电动画，背景亮度20
        ret = ws2812_set_battery_display(battery_channel, true, 10);
        // esp_task_wdt_reset();
        if (ret != ESP_OK) goto cleanup;
        
        ESP_LOGI(TAG, "Battery display channel set to: %d", battery_channel);
    }
    
    // 原有的详细电量显示配置（向后兼容）
    // battery_display_json already defined above
    if (battery_display_json && cJSON_IsObject(battery_display_json)) {
        // esp_task_wdt_reset(); // 电量显示处理前重置看门狗
        
        cJSON *battery_channel_json = cJSON_GetObjectItem(battery_display_json, "channel");
        cJSON *show_charging_json = cJSON_GetObjectItem(battery_display_json, "show_charging_effect");
        cJSON *bg_brightness_json = cJSON_GetObjectItem(battery_display_json, "background_brightness");
        
        uint8_t battery_channel = WS2812_BATTERY_CHANNEL_DISABLED;
        bool show_charging_effect = true;
        uint8_t background_brightness = 20;
        
        if (battery_channel_json && cJSON_IsNumber(battery_channel_json)) {
            int ch = battery_channel_json->valueint;
            if (ch >= 0 && ch < WS2812_CHANNEL_COUNT) {
                battery_channel = (uint8_t)ch;
            } else if (ch == 255) {
                battery_channel = WS2812_BATTERY_CHANNEL_DISABLED;
            }
        }
        
        if (show_charging_json && cJSON_IsBool(show_charging_json)) {
            show_charging_effect = cJSON_IsTrue(show_charging_json);
        }
        
        if (bg_brightness_json && cJSON_IsNumber(bg_brightness_json)) {
            int brightness = bg_brightness_json->valueint;
            if (brightness >= 0 && brightness <= 255) {
                background_brightness = (uint8_t)brightness;
            }
        }
        
        ret = ws2812_set_battery_display(battery_channel, show_charging_effect, background_brightness);
        // esp_task_wdt_reset(); // 电量显示设置后重置看门狗
        if (ret != ESP_OK) goto cleanup;
    }
    
    // 电量状态更新
    // battery_status_json already defined above
    if (battery_status_json && cJSON_IsObject(battery_status_json)) {
        // esp_task_wdt_reset(); // 电量状态处理前重置看门狗
        
        cJSON *percentage_json = cJSON_GetObjectItem(battery_status_json, "percentage");
        cJSON *charging_json = cJSON_GetObjectItem(battery_status_json, "charging");
        
        if (percentage_json && cJSON_IsNumber(percentage_json)) {
            int percentage = percentage_json->valueint;
            bool is_charging = false;
            
            if (charging_json && cJSON_IsBool(charging_json)) {
                is_charging = cJSON_IsTrue(charging_json);
            }
            
            ret = ws2812_update_battery_display(percentage, is_charging);
            // esp_task_wdt_reset(); // 电量状态设置后重置看门狗
            if (ret != ESP_OK) goto cleanup;
        }
    }
    
cleanup:
    // esp_task_wdt_reset(); // 函数结束前最后一次重置看门狗
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
        for (int i = 0; i < channels[ch].led_count; i++) {
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
        
        // 保存电量显示配置
        ret = nvs_set_blob(nvs_handle, "battery_config", &battery_config, sizeof(ws2812_battery_config_t));
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to save battery config: %s", esp_err_to_name(ret));
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
        
        // 加载电量显示配置
        size_t battery_config_size = sizeof(ws2812_battery_config_t);
        ret = nvs_get_blob(nvs_handle, "battery_config", &battery_config, &battery_config_size);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "Failed to load battery config, using defaults: %s", esp_err_to_name(ret));
            // 使用默认电量显示配置
            battery_config.battery_channel = 255;  // 禁用
            battery_config.show_charging_effect = true;
            battery_config.background_brightness = 20;
            load_success = false;
        }
        
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
                channels[ch].led_count = WS2812_LED_COUNT_DEFAULT;  // 默认LED数量
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
            
            // brightness字段是uint8_t类型，范围自然限制在0-255，无需检查上限
            
            if (channels[ch].led_count < 1 || channels[ch].led_count > WS2812_MAX_LED_COUNT) {
                ESP_LOGW(TAG, "Invalid LED count for channel %d, resetting to default", ch);
                channels[ch].led_count = WS2812_LED_COUNT_DEFAULT;
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
        // 重置电量显示配置
        battery_config.battery_channel = 255;  // 禁用
        battery_config.show_charging_effect = true;
        battery_config.background_brightness = 20;
        
        // 初始化每个通道的默认配置
        for (int ch = 0; ch < WS2812_CHANNEL_COUNT; ch++) {
            channels[ch].channel_id = ch;
            channels[ch].enabled = true;
            channels[ch].led_count = WS2812_LED_COUNT_DEFAULT;
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
