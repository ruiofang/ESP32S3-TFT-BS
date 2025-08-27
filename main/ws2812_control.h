#ifndef WS2812_CONTROL_H
#define WS2812_CONTROL_H

#include "esp_err.h"
#include <stdint.h>

// WS2812配置
#define WS2812_GPIO_PIN         19          // WS2812数据引脚
#define WS2812_LED_COUNT        160          // LED数量，可根据实际情况调整
#define WS2812_RMT_CHANNEL      0           // RMT通道

// 灯光效果模式
typedef enum {
    WS2812_MODE_OFF = 0,        // 关闭
    WS2812_MODE_STATIC,         // 静态颜色
    WS2812_MODE_RAINBOW,        // 彩虹效果
    WS2812_MODE_BREATHING,      // 呼吸灯效果
    WS2812_MODE_RUNNING,        // 跑马灯效果
    WS2812_MODE_FLASH,          // 闪烁效果
    WS2812_MODE_WAVE,           // 波浪效果
    WS2812_MODE_AUTO_CYCLE,     // 自动循环模式
    WS2812_MODE_MAX
} ws2812_mode_t;

// 颜色结构体
typedef struct {
    uint8_t r;
    uint8_t g;
    uint8_t b;
} rgb_color_t;

// 灯光控制参数
typedef struct {
    ws2812_mode_t mode;         // 当前模式
    rgb_color_t color;          // 静态颜色
    uint32_t speed;             // 效果速度 (ms)
    uint8_t brightness;         // 亮度 (0-255)
} ws2812_config_t;

/**
 * @brief 初始化WS2812
 * @return ESP_OK成功，其他值失败
 */
esp_err_t ws2812_init(void);

/**
 * @brief 设置灯光模式
 * @param mode 灯光模式
 * @return ESP_OK成功，其他值失败
 */
esp_err_t ws2812_set_mode(ws2812_mode_t mode);

/**
 * @brief 设置静态颜色
 * @param r 红色分量 (0-255)
 * @param g 绿色分量 (0-255)
 * @param b 蓝色分量 (0-255)
 * @return ESP_OK成功，其他值失败
 */
esp_err_t ws2812_set_color(uint8_t r, uint8_t g, uint8_t b);

/**
 * @brief 设置亮度
 * @param brightness 亮度 (0-255)
 * @return ESP_OK成功，其他值失败
 */
esp_err_t ws2812_set_brightness(uint8_t brightness);

/**
 * @brief 设置效果速度
 * @param speed 速度，单位ms
 * @return ESP_OK成功，其他值失败
 */
esp_err_t ws2812_set_speed(uint32_t speed);

/**
 * @brief 设置自动循环模式每个效果的持续时间
 * @param duration 持续时间，单位ms (1000-60000)
 * @return ESP_OK成功，其他值失败
 */
esp_err_t ws2812_set_cycle_duration(uint32_t duration);

/**
 * @brief 获取当前配置
 * @return 当前WS2812配置
 */
ws2812_config_t ws2812_get_config(void);

/**
 * @brief WS2812控制任务
 * @param pvParameters 任务参数
 */
void ws2812_task(void *pvParameters);

/**
 * @brief 处理串口命令
 * @param command 命令字符串
 * @return ESP_OK成功，其他值失败
 */
esp_err_t ws2812_handle_uart_command(const char *command);

#endif // WS2812_CONTROL_H
