#ifndef WS2812_CONTROL_H
#define WS2812_CONTROL_H

#include "esp_err.h"
#include <stdint.h>
#include <stdbool.h>

// WS2812配置 - 4通道支持
#define WS2812_CHANNEL_COUNT    4           // 支持4个通道
#define WS2812_GPIO_PINS        {18, 19, 20, 21}  // 4个通道的GPIO引脚
#define WS2812_LED_COUNT_DEFAULT 100       // 默认每个通道的LED数量
#define WS2812_MAX_LED_COUNT    300         // 每个通道最大LED数量
#define WS2812_BROADCAST_ID     255         // 广播ID，控制所有通道
#define WS2812_BATTERY_CHANNEL_DISABLED 255 // 表示未启用电量显示通道

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
    WS2812_MODE_BATTERY,        // 电量显示模式
    WS2812_MODE_MUSIC_RHYTHM,   // 音乐律动模式
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

// 电量显示配置
typedef struct {
    uint8_t battery_channel;    // 显示电量的通道ID (0-3, 255表示禁用)
    bool show_charging_effect;  // 是否显示充电特效
    uint8_t background_brightness; // 背景LED亮度 (0-255)
} ws2812_battery_config_t;

// 通道配置结构体
typedef struct {
    uint8_t channel_id;         // 通道ID (0-3)
    bool enabled;               // 通道是否启用
    uint16_t led_count;         // 当前通道LED数量 (1-300)
    ws2812_config_t config;     // 通道配置
} ws2812_channel_t;

/**
 * @brief 初始化WS2812多通道系统
 * @return ESP_OK成功，其他值失败
 */
esp_err_t ws2812_init(void);

/**
 * @brief 设置指定通道的灯光模式
 * @param channel_id 通道ID (0-3, 255表示广播到所有通道)
 * @param mode 灯光模式
 * @return ESP_OK成功，其他值失败
 */
esp_err_t ws2812_set_mode(uint8_t channel_id, ws2812_mode_t mode);

/**
 * @brief 设置指定通道的静态颜色
 * @param channel_id 通道ID (0-3, 255表示广播到所有通道)
 * @param r 红色分量 (0-255)
 * @param g 绿色分量 (0-255)
 * @param b 蓝色分量 (0-255)
 * @return ESP_OK成功，其他值失败
 */
esp_err_t ws2812_set_color(uint8_t channel_id, uint8_t r, uint8_t g, uint8_t b);

/**
 * @brief 设置指定通道的亮度
 * @param channel_id 通道ID (0-3, 255表示广播到所有通道)
 * @param brightness 亮度 (0-255)
 * @return ESP_OK成功，其他值失败
 */
esp_err_t ws2812_set_brightness(uint8_t channel_id, uint8_t brightness);

/**
 * @brief 设置指定通道的效果速度
 * @param channel_id 通道ID (0-3, 255表示广播到所有通道)
 * @param speed 速度，单位ms
 * @return ESP_OK成功，其他值失败
 */
esp_err_t ws2812_set_speed(uint8_t channel_id, uint32_t speed);

/**
 * @brief 启用或禁用指定通道
 * @param channel_id 通道ID (0-3, 255表示广播到所有通道)
 * @param enabled true启用，false禁用
 * @return ESP_OK成功，其他值失败
 */
esp_err_t ws2812_set_channel_enabled(uint8_t channel_id, bool enabled);

/**
 * @brief 设置自动循环模式每个效果的持续时间
 * @param channel_id 通道ID (0-3, 255表示广播到所有通道)
 * @param duration 持续时间，单位ms (1000-60000)
 * @return ESP_OK成功，其他值失败
 */
esp_err_t ws2812_set_cycle_duration(uint8_t channel_id, uint32_t duration);

/**
 * @brief 获取指定通道的自动循环模式持续时间
 * @param channel_id 通道ID (0-3)
 * @return 持续时间，单位ms
 */
uint32_t ws2812_get_cycle_duration(uint8_t channel_id);

/**
 * @brief 获取指定通道的当前配置
 * @param channel_id 通道ID (0-3)
 * @return 指定通道的WS2812配置，如果通道无效返回默认配置
 */
ws2812_channel_t ws2812_get_channel_config(uint8_t channel_id);

/**
 * @brief 获取所有通道的配置
 * @param configs 用于存储配置的数组，必须至少有WS2812_CHANNEL_COUNT个元素
 * @return ESP_OK成功，其他值失败
 */
esp_err_t ws2812_get_all_configs(ws2812_channel_t configs[WS2812_CHANNEL_COUNT]);

/**
 * @brief WS2812控制任务
 * @param pvParameters 任务参数
 */
void ws2812_task(void *pvParameters);

/**
 * @brief 处理JSON格式的控制命令
 * @param json_command JSON格式的命令字符串
 * @return ESP_OK成功，其他值失败
 * 
 * JSON命令格式示例:
 * - 控制单个通道: {"channel": 0, "mode": 1, "color": {"r": 255, "g": 0, "b": 0}, "brightness": 128, "speed": 100}
 * - 广播控制: {"channel": 255, "mode": 2, "brightness": 200}
 * - 启用/禁用通道: {"channel": 1, "enabled": false}
 * - 查询状态: {"action": "status"}
 */
esp_err_t ws2812_handle_json_command(const char *json_command);

/**
 * @brief 测试所有通道功能
 * @return ESP_OK成功，其他值失败
 */
esp_err_t ws2812_test_all_channels(void);

/**
 * @brief 保存WS2812参数到NVS
 * @return ESP_OK成功，其他值失败
 */
esp_err_t ws2812_save_config(void);

/**
 * @brief 从NVS加载WS2812参数
 * @return ESP_OK成功，其他值失败
 */
esp_err_t ws2812_load_config(void);

/**
 * @brief 重置WS2812配置为默认值
 * @return ESP_OK成功，其他值失败
 */
esp_err_t ws2812_reset_config(void);

/**
 * @brief 设置指定通道的LED数量
 * @param channel_id 通道ID (0-3, 255表示广播到所有通道)
 * @param led_count LED数量 (1-300)
 * @return ESP_OK成功，其他值失败
 */
esp_err_t ws2812_set_led_count(uint8_t channel_id, uint16_t led_count);

/**
 * @brief 设置电量显示配置
 * @param battery_channel 显示电量的通道ID (0-3, 255表示禁用)
 * @param show_charging_effect 是否显示充电特效
 * @param background_brightness 背景LED亮度 (0-255)
 * @return ESP_OK成功，其他值失败
 */
esp_err_t ws2812_set_battery_display(uint8_t battery_channel, bool show_charging_effect, uint8_t background_brightness);

/**
 * @brief 设置指定通道的电量显示模式
 * @param channel_id 通道ID (0-3)
 * @param enable_battery_mode 是否启用电量显示模式
 * @param background_brightness 背景LED亮度 (0-255，0使用默认值10)
 * @return ESP_OK成功，其他值失败
 */
esp_err_t ws2812_set_channel_battery_mode(uint8_t channel_id, bool enable_battery_mode, uint8_t background_brightness);

/**
 * @brief 更新电量显示
 * @param battery_percentage 电量百分比 (0-100)
 * @param is_charging 是否正在充电
 * @return ESP_OK成功，其他值失败
 */
esp_err_t ws2812_update_battery_display(int battery_percentage, bool is_charging);

/**
 * @brief 获取电量显示配置
 * @return 电量显示配置结构体
 */
ws2812_battery_config_t ws2812_get_battery_config(void);

#endif // WS2812_CONTROL_H
