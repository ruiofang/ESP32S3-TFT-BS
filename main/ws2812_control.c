#include "ws2812_control.h"
#include "driver/rmt_tx.h"
#include "led_strip.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <string.h>
#include <stdlib.h>
#include <math.h>

static const char *TAG = "WS2812_CONTROL";

// 全局变量
static led_strip_handle_t led_strip = NULL;
static ws2812_config_t ws2812_config = {
    .mode = WS2812_MODE_AUTO_CYCLE,  // 默认启动自动循环模式
    .color = {255, 255, 255},
    .speed = 150,                    // 适中的速度
    .brightness = 180                // 较亮的亮度
};
static SemaphoreHandle_t ws2812_mutex = NULL;
static bool task_running = false;

// 自动循环模式相关变量
static uint32_t auto_cycle_timer = 0;
static uint32_t auto_cycle_duration = 8000;  // 每个效果持续8秒，可动态调整
static ws2812_mode_t auto_cycle_modes[] = {
    WS2812_MODE_RAINBOW,
    WS2812_MODE_BREATHING,
    WS2812_MODE_RUNNING,
    WS2812_MODE_WAVE,
    WS2812_MODE_FLASH
};
static const int auto_cycle_count = sizeof(auto_cycle_modes) / sizeof(auto_cycle_modes[0]);
static int current_auto_mode_index = 0;

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
    ESP_LOGI(TAG, "Initializing WS2812 on GPIO %d", WS2812_GPIO_PIN);
    
    // 创建RMT通道配置
    rmt_tx_channel_config_t tx_chan_config = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .gpio_num = WS2812_GPIO_PIN,
        .mem_block_symbols = 64,
        .resolution_hz = 10000000, // 10MHz resolution, 1 tick = 0.1us (led strip needs a high resolution)
        .trans_queue_depth = 4,
    };
    
    rmt_channel_handle_t led_chan = NULL;
    ESP_ERROR_CHECK(rmt_new_tx_channel(&tx_chan_config, &led_chan));
    
    // 配置LED strip
    led_strip_config_t strip_config = {
        .strip_gpio_num = WS2812_GPIO_PIN,
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
    
    ESP_ERROR_CHECK(led_strip_new_rmt_device(&strip_config, &rmt_config, &led_strip));
    
    // 创建互斥锁
    ws2812_mutex = xSemaphoreCreateMutex();
    if (ws2812_mutex == NULL) {
        ESP_LOGE(TAG, "Failed to create mutex");
        return ESP_FAIL;
    }
    
    // 清空所有LED
    ESP_ERROR_CHECK(led_strip_clear(led_strip));
    
    ESP_LOGI(TAG, "WS2812 initialized successfully with %d LEDs", WS2812_LED_COUNT);
    return ESP_OK;
}

esp_err_t ws2812_set_mode(ws2812_mode_t mode) {
    if (mode >= WS2812_MODE_MAX) {
        return ESP_ERR_INVALID_ARG;
    }
    
    if (xSemaphoreTake(ws2812_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        ws2812_config.mode = mode;
        xSemaphoreGive(ws2812_mutex);
        ESP_LOGI(TAG, "Mode set to %d", mode);
        return ESP_OK;
    }
    return ESP_ERR_TIMEOUT;
}

esp_err_t ws2812_set_color(uint8_t r, uint8_t g, uint8_t b) {
    if (xSemaphoreTake(ws2812_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        ws2812_config.color.r = r;
        ws2812_config.color.g = g;
        ws2812_config.color.b = b;
        xSemaphoreGive(ws2812_mutex);
        ESP_LOGI(TAG, "Color set to RGB(%d,%d,%d)", r, g, b);
        return ESP_OK;
    }
    return ESP_ERR_TIMEOUT;
}

esp_err_t ws2812_set_brightness(uint8_t brightness) {
    if (xSemaphoreTake(ws2812_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        ws2812_config.brightness = brightness;
        xSemaphoreGive(ws2812_mutex);
        ESP_LOGI(TAG, "Brightness set to %d", brightness);
        return ESP_OK;
    }
    return ESP_ERR_TIMEOUT;
}

esp_err_t ws2812_set_speed(uint32_t speed) {
    if (speed < 10) speed = 10; // 最小10ms
    if (speed > 10000) speed = 10000; // 最大10秒
    
    if (xSemaphoreTake(ws2812_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        ws2812_config.speed = speed;
        xSemaphoreGive(ws2812_mutex);
        ESP_LOGI(TAG, "Speed set to %ld ms", speed);
        return ESP_OK;
    }
    return ESP_ERR_TIMEOUT;
}

ws2812_config_t ws2812_get_config(void) {
    ws2812_config_t config;
    if (xSemaphoreTake(ws2812_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        config = ws2812_config;
        xSemaphoreGive(ws2812_mutex);
    }
    return config;
}

esp_err_t ws2812_set_cycle_duration(uint32_t duration) {
    if (duration < 1000) duration = 1000;   // 最小1秒
    if (duration > 60000) duration = 60000; // 最大60秒
    
    if (xSemaphoreTake(ws2812_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        auto_cycle_duration = duration;
        auto_cycle_timer = 0; // 重置计时器
        xSemaphoreGive(ws2812_mutex);
        ESP_LOGI(TAG, "Auto-cycle duration set to %ld ms", duration);
        return ESP_OK;
    }
    return ESP_ERR_TIMEOUT;
}

void ws2812_task(void *pvParameters) {
    static uint32_t counter = 0;
    static int direction = 1;
    static uint32_t breath_value = 0;
    
    task_running = true;
    ESP_LOGI(TAG, "WS2812 task started with auto-cycle mode");
    
    while (task_running) {
        ws2812_config_t current_config = ws2812_get_config();
        ws2812_mode_t effective_mode = current_config.mode;
        
        // 处理自动循环模式
        if (current_config.mode == WS2812_MODE_AUTO_CYCLE) {
            auto_cycle_timer += current_config.speed;
            
            // 检查是否需要切换到下一个效果
            if (auto_cycle_timer >= auto_cycle_duration) {
                current_auto_mode_index = (current_auto_mode_index + 1) % auto_cycle_count;
                auto_cycle_timer = 0;
                
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
                    ws2812_config.color = cycle_colors[current_auto_mode_index];
                    xSemaphoreGive(ws2812_mutex);
                }
                
                ESP_LOGI(TAG, "Auto-cycle switched to mode %d", auto_cycle_modes[current_auto_mode_index]);
            }
            
            effective_mode = auto_cycle_modes[current_auto_mode_index];
        }
        
        switch (effective_mode) {
            case WS2812_MODE_OFF:
                // 关闭所有LED
                led_strip_clear(led_strip);
                break;
                
            case WS2812_MODE_STATIC:
                // 静态颜色
                {
                    rgb_color_t color = apply_brightness(current_config.color, current_config.brightness);
                    for (int i = 0; i < WS2812_LED_COUNT; i++) {
                        led_strip_set_pixel(led_strip, i, color.r, color.g, color.b);
                    }
                }
                break;
                
            case WS2812_MODE_RAINBOW:
                // 彩虹效果
                for (int i = 0; i < WS2812_LED_COUNT; i++) {
                    uint16_t hue = (counter + i * 255 / WS2812_LED_COUNT) % 256;
                    rgb_color_t color = hsv_to_rgb(hue, 255, 255);
                    color = apply_brightness(color, current_config.brightness);
                    led_strip_set_pixel(led_strip, i, color.r, color.g, color.b);
                }
                counter = (counter + 5) % 256;
                break;
                
            case WS2812_MODE_BREATHING:
                // 呼吸灯效果
                {
                    uint8_t breath_brightness = (uint8_t)(128 + 127 * sin(breath_value * M_PI / 180));
                    breath_brightness = (breath_brightness * current_config.brightness) / 255;
                    rgb_color_t color = apply_brightness(current_config.color, breath_brightness);
                    
                    for (int i = 0; i < WS2812_LED_COUNT; i++) {
                        led_strip_set_pixel(led_strip, i, color.r, color.g, color.b);
                    }
                    breath_value = (breath_value + 10) % 360;
                }
                break;
                
            case WS2812_MODE_RUNNING:
                // 跑马灯效果
                led_strip_clear(led_strip);
                {
                    rgb_color_t color = apply_brightness(current_config.color, current_config.brightness);
                    for (int i = 0; i < 3; i++) {
                        int pos = (counter + i) % WS2812_LED_COUNT;
                        led_strip_set_pixel(led_strip, pos, color.r, color.g, color.b);
                    }
                    counter = (counter + direction) % WS2812_LED_COUNT;
                }
                break;
                
            case WS2812_MODE_FLASH:
                // 闪烁效果
                if ((counter / 10) % 2) {
                    rgb_color_t color = apply_brightness(current_config.color, current_config.brightness);
                    for (int i = 0; i < WS2812_LED_COUNT; i++) {
                        led_strip_set_pixel(led_strip, i, color.r, color.g, color.b);
                    }
                } else {
                    led_strip_clear(led_strip);
                }
                counter++;
                break;
                
            case WS2812_MODE_WAVE:
                // 波浪效果
                for (int i = 0; i < WS2812_LED_COUNT; i++) {
                    uint8_t wave_brightness = (uint8_t)(128 + 127 * sin((counter + i * 20) * M_PI / 180));
                    wave_brightness = (wave_brightness * current_config.brightness) / 255;
                    rgb_color_t color = apply_brightness(current_config.color, wave_brightness);
                    led_strip_set_pixel(led_strip, i, color.r, color.g, color.b);
                }
                counter = (counter + 10) % 360;
                break;
                
            default:
                led_strip_clear(led_strip);
                break;
        }
        
        // 刷新LED显示
        led_strip_refresh(led_strip);
        
        // 延时
        vTaskDelay(pdMS_TO_TICKS(current_config.speed));
    }
    
    ESP_LOGI(TAG, "WS2812 task ended");
    vTaskDelete(NULL);
}

esp_err_t ws2812_handle_uart_command(const char *command) {
    if (command == NULL || strlen(command) == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    
    ESP_LOGI(TAG, "Processing command: %s", command);
    
    // 命令格式解析
    if (strncmp(command, "MODE:", 5) == 0) {
        // 设置模式: MODE:0-6
        int mode = atoi(command + 5);
        if (mode >= 0 && mode < WS2812_MODE_MAX) {
            return ws2812_set_mode((ws2812_mode_t)mode);
        }
        return ESP_ERR_INVALID_ARG;
    }
    else if (strncmp(command, "COLOR:", 6) == 0) {
        // 设置颜色: COLOR:255,255,255
        int r, g, b;
        if (sscanf(command + 6, "%d,%d,%d", &r, &g, &b) == 3) {
            if (r >= 0 && r <= 255 && g >= 0 && g <= 255 && b >= 0 && b <= 255) {
                return ws2812_set_color((uint8_t)r, (uint8_t)g, (uint8_t)b);
            }
        }
        return ESP_ERR_INVALID_ARG;
    }
    else if (strncmp(command, "BRIGHTNESS:", 11) == 0) {
        // 设置亮度: BRIGHTNESS:128
        int brightness = atoi(command + 11);
        if (brightness >= 0 && brightness <= 255) {
            return ws2812_set_brightness((uint8_t)brightness);
        }
        return ESP_ERR_INVALID_ARG;
    }
    else if (strncmp(command, "SPEED:", 6) == 0) {
        // 设置速度: SPEED:100
        int speed = atoi(command + 6);
        if (speed >= 10 && speed <= 10000) {
            return ws2812_set_speed((uint32_t)speed);
        }
        return ESP_ERR_INVALID_ARG;
    }
    else if (strncmp(command, "CYCLE_DURATION:", 15) == 0) {
        // 设置自动循环持续时间: CYCLE_DURATION:8000
        int duration = atoi(command + 15);
        if (duration >= 1000 && duration <= 60000) {
            return ws2812_set_cycle_duration((uint32_t)duration);
        }
        return ESP_ERR_INVALID_ARG;
    }
    else if (strcmp(command, "STATUS") == 0) {
        // 查询状态
        ws2812_config_t config = ws2812_get_config();
        ESP_LOGI(TAG, "Current Status - Mode:%d, Color:RGB(%d,%d,%d), Brightness:%d, Speed:%ld", 
                 config.mode, config.color.r, config.color.g, config.color.b, 
                 config.brightness, config.speed);
        return ESP_OK;
    }
    else if (strcmp(command, "HELP") == 0) {
        // 帮助信息
        ESP_LOGI(TAG, "WS2812 Commands:");
        ESP_LOGI(TAG, "MODE:0-7 (0:OFF, 1:STATIC, 2:RAINBOW, 3:BREATHING, 4:RUNNING, 5:FLASH, 6:WAVE, 7:AUTO_CYCLE)");
        ESP_LOGI(TAG, "COLOR:r,g,b (0-255 for each)");
        ESP_LOGI(TAG, "BRIGHTNESS:0-255");
        ESP_LOGI(TAG, "SPEED:10-10000 (milliseconds)");
        ESP_LOGI(TAG, "CYCLE_DURATION:1000-60000 (ms, for auto-cycle mode)");
        ESP_LOGI(TAG, "STATUS - Show current settings");
        ESP_LOGI(TAG, "HELP - Show this help");
        ESP_LOGI(TAG, "Auto-cycle mode automatically switches between effects");
        return ESP_OK;
    }
    
    return ESP_ERR_NOT_SUPPORTED;
}
