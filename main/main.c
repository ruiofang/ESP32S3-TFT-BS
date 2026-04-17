#include "nvs_flash.h"
#include "lvgl_demo.h"
#include "esp_vfs.h"
#include "esp_spiffs.h"
#include "led.h"
#include "lcd.h"
#include "lcd_init.h"
#include "ws2812_control.h"
#include "web_server.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "driver/uart.h"
#include "driver/gpio.h"
#include "esp_timer.h"
#include "esp_task_wdt.h"
#include "lvgl.h"
#include "Lib/cJSON/cJSON.h"
#include "i2s_mic.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include "battery_control.h"

#define TAG "BATTERY_MONITOR"

// LCD颜色定义 (RGB565格式)
#define LCD_COLOR_RED     0xF800
#define LCD_COLOR_GREEN   0x07E0
#define LCD_COLOR_BLUE    0x001F
#define LCD_COLOR_YELLOW  0xFFE0
#define LCD_COLOR_PURPLE  0xF81F
#define LCD_COLOR_CYAN    0x07FF
#define LCD_COLOR_WHITE   0xFFFF
#define LCD_COLOR_BLACK   0x0000

// 功能模式选择 (通过宏定义控制)
// 注: RS485 电池查询不再使用编译期开关，运行时通过模式切换 (JSON / RS485-1 / RS485-2) 决定
#define ENABLE_JSON_PASSIVE_MODE    1  // 1=启用JSON被动控制, 0=禁用
// 电池监控任务使能开关（为避免WDT问题可禁用）
#define ENABLE_BATTERY_MONITOR_TASK 0  // 1=启用电池监控任务, 0=禁用

// UART1 配置 (统一串口)
#define UART1_TXD_PIN       16
#define UART1_RXD_PIN       17
// 编译期默认波特率 (启动时 apply_battery_read_mode() 会按运行时模式重新设置)
#define UART1_BAUD_RATE     115200
#define UART_BUF_SIZE       2048  // 缓冲区大小

// 电池监控周期定义 (仅用于任务延时)
#define BATTERY_UPDATE_PERIOD 1000  // 更新周期 (ms)

// 电池查询配置
#define BATTERY_QUERY_PERIOD 3000   // 电池查询周期 (ms)
#define BATTERY_TIMEOUT_MS   500    // 电池响应超时 (ms)

// ---- BOOT 按键 & 运行时读取模式 ----
#define BOOT_BUTTON_GPIO            0       // ESP32-S3 BOOT 按键
#define BOOT_BUTTON_POLL_MS         30      // 去抖/轮询周期
#define BOOT_BUTTON_DEBOUNCE_MS     50      // 稳定时间

typedef enum {
    BATT_READ_MODE_JSON = 0,     // JSON 被动模式 (UART 115200)
    BATT_READ_MODE_RS485_1 = 1,    // RS485 电池协议 (DD/77 起止) 主动查询 (UART 9600)
    BATT_READ_MODE_RS485_2 = 2,  // RS485-2 电池协议 (0xAA 帧头, 小端) 主动查询 (UART 9600)
    BATT_READ_MODE_MAX
} battery_read_mode_t;

static battery_read_mode_t g_battery_read_mode = BATT_READ_MODE_JSON;

static inline const char *battery_read_mode_name(battery_read_mode_t m)
{
    switch (m) {
        case BATT_READ_MODE_JSON:    return "JSON";
        case BATT_READ_MODE_RS485_1: return "RS485-1";
        case BATT_READ_MODE_RS485_2: return "RS485-2";
        default: return "?";
    }
}

// ---- RS485-2 协议 (参考 doc/通讯协议-20250113-客户(蓝牙 485协议).pdf) ----
// 帧格式: 帧头(0xAA) + 命令(1B) + 数据长度(1B, <100) + 数据(LEN B) + 校验(2B, 小端, CMD+LEN+DATA 累加和)
#define BATT2_FRAME_HEAD             0xAA
#define BATT2_CMD_HANDSHAKE          0x00
#define BATT2_CMD_BATTERY_INFO       0x21
#define BATT2_QUERY_PERIOD_MS        2000    // 主动查询周期
#define BATT2_RESP_TIMEOUT_MS        800     // 单次应答超时 (协议规定 1s)
#define BATT2_MAX_DATA_LEN           100

// 电池通信协议定义
#define BATTERY_FRAME_START  0xDD   // 起始位
#define BATTERY_FRAME_END    0x77   // 结束位
#define BATTERY_READ_CMD     0xA5   // 读取命令
#define BATTERY_WRITE_CMD    0x5A   // 写入命令
#define BATTERY_CMD_INFO     0x03   // 基本信息命令
#define BATTERY_CMD_VOLTAGE  0x04   // 单体电压命令

// 充电动画相关定义
#define CHARGING_ANIM_PERIOD 500    // 充电动画周期 (ms) - 降低频率
#define CHARGING_ANIM_STEPS 10      // 动画步骤数
#define BATTERY_FULL_THRESHOLD 98   // 电池充满阈值 (%)

// 全局变量
static float g_battery_voltage = 24.0f;  // 当前电池电压
static int g_battery_percentage = 50;   // 当前电池百分比
static bool g_charging_status = false;  // 充电状态

// RS485电池数据
typedef struct {
    float pack_voltage;      // 总电压 (V)
    float pack_current;      // 电流 (A), 正数充电，负数放电
    uint16_t remain_capacity; // 剩余容量 (10mAh)
    uint16_t full_capacity;   // 标称容量 (10mAh)
    uint8_t soc;            // 剩余容量百分比 (%)
    uint8_t battery_strings; // 电池串数
    uint16_t protect_status; // 保护状态
    uint8_t fet_status;     // MOS管状态
    uint8_t temp_count;     // 温度探头个数
    float temperatures[8];   // 温度值 (℃)
    bool data_valid;        // 数据有效标志
    uint32_t last_update;   // 最后更新时间
} battery_data_t;

static battery_data_t g_battery_data = {0};
static bool g_battery_auto_query = true;  // 自动查询使能


// 外部控制状态
static bool battery_display_override = false;  // 外部控制电量显示标志
static int external_battery_value = 50;        // 外部设置的电量值
static bool charging_status_override = false;  // 外部控制充电状态标志
static bool external_charging_status = false;  // 外部设置的充电状态

// 外部电压控制
static bool voltage_override = false;          // 外部控制电压标志
static float external_voltage_value = 24.0f;   // 外部设置的电压值

static bool ui_update_pending = false;         // UI更新待处理标志
static TaskHandle_t lvgl_task_handle = NULL;   // LVGL任务句柄

// UI更新通知函数
static void notify_ui_update_needed(void) {
    ui_update_pending = true;
    if (lvgl_task_handle != NULL) {
        xTaskNotifyGive(lvgl_task_handle);  // 立即唤醒LVGL任务
    }
}

// 异步处理队列
#define UART_QUEUE_SIZE 10
#define UART_CMD_MAX_LEN 512

typedef struct {
    char command[UART_CMD_MAX_LEN];
    size_t length;
} uart_command_t;

static QueueHandle_t uart_command_queue = NULL;
static bool nvs_save_pending = false;          // NVS保存待处理标志 (已停用，保留以兼容)
static esp_timer_handle_t nvs_save_timer = NULL;

// ---- 电池信号丢失检测 ----
#define BATTERY_SIGNAL_TIMEOUT_MS   15000   // 超过此时间无外部电量/充电/电压数据 -> 视为信号丢失
#define BATTERY_NO_SIGNAL_BLINK_MS  500     // 丢失态红闪周期 (ms)
static TickType_t g_last_signal_tick = 0;  // 上次收到外部电池数据的 tick, 0 表示从未收到
static bool g_no_signal_blink_on = false;  // 红闪当前相位
static esp_timer_handle_t no_signal_blink_timer = NULL;

static inline bool is_battery_signal_lost(void)
{
    if (g_last_signal_tick == 0) return true;  // 启动后从未收到数据
    return (xTaskGetTickCount() - g_last_signal_tick) > pdMS_TO_TICKS(BATTERY_SIGNAL_TIMEOUT_MS);
}

static inline void mark_battery_signal_alive(void)
{
    g_last_signal_tick = xTaskGetTickCount();
    if (g_last_signal_tick == 0) g_last_signal_tick = 1; // 避免与"从未收到"标记冲突
}

// 电池状态缓存（避免每次重新创建JSON）
static char cached_battery_json[256] = "";
static uint32_t last_battery_json_update = 0;

// 充电动画相关变量
static bool charging_animation_enabled = true;  // 充电动画开关
static uint32_t charging_animation_step = 0;    // 动画步骤计数器
static bool charging_animation_direction = true; // 动画方向（true为增加，false为减少）
static esp_timer_handle_t charging_animation_timer = NULL; // 充电动画定时器
static int charging_animation_range = 15;        // 充电动画范围（百分比）

// 函数声明
static void start_charging_animation(void);
static void stop_charging_animation(void);
static esp_err_t save_battery_state_to_nvs(void);

/**
 * @brief 获取有效的充电状态（考虑外部控制）
 */
static bool get_effective_charging_status(void)
{
    return charging_status_override ? external_charging_status : g_charging_status;
}

/**
 * @brief NVS延时保存定时器回调 (已停用 — 电池数据不再持久化以减少 Flash 磨损)
 */
static void nvs_save_timer_callback(void *arg)
{
    (void)arg;
    // 不再执行保存
}

/**
 * @brief 请求延时保存到NVS (已停用 — 电池状态不存储)
 */
static void schedule_nvs_save(void)
{
    // 电池电量/充电/电压等状态为易失数据，不写入 NVS。
    // 保留空函数以兼容既有调用点，避免大面积改动。
}

/**
 * @brief 无信号红色闪烁定时器回调
 *        每 BATTERY_NO_SIGNAL_BLINK_MS 触发一次；仅当信号丢失时驱动闪烁，
 *        信号恢复后立即关闭红灯并刷新 UI。
 */
static void no_signal_blink_timer_callback(void *arg)
{
    (void)arg;
    static bool was_lost = false;
    bool lost = is_battery_signal_lost();

    if (lost) {
        g_no_signal_blink_on = !g_no_signal_blink_on;
        // 只让"电量显示通道"红闪；其它通道保持当前效果
        if (g_no_signal_blink_on) {
            ws2812_set_no_signal_override(true, 255, 0, 0);
        } else {
            ws2812_set_no_signal_override(true, 0, 0, 0);
        }
        // 通知 LCD 刷新
        ui_update_pending = true;
        if (lvgl_task_handle != NULL) {
            xTaskNotifyGive(lvgl_task_handle);
        }
        was_lost = true;
    } else if (was_lost) {
        // 刚刚从无信号恢复：关闭覆盖，让 ws2812_task 按原模式继续渲染
        ws2812_set_no_signal_override(false, 0, 0, 0);
        g_no_signal_blink_on = false;
        ui_update_pending = true;
        if (lvgl_task_handle != NULL) {
            xTaskNotifyGive(lvgl_task_handle);
        }
        was_lost = false;
    }
}









/**
 * @brief 计算带充电动画效果的电量显示值
 */
static int get_animated_battery_percentage(int base_percentage)
{
    // 统一充电判断逻辑：被动控制 OR RS485检测到充电
    bool is_charging_display = (charging_status_override && external_charging_status);

    // 如果RS485检测到充电，也启用充电动画
    if (g_battery_data.data_valid && g_battery_data.pack_current > 0.05f) {
        is_charging_display = true;
    }

    if (!is_charging_display || !charging_animation_enabled) {
        return base_percentage;
    }
    
    // 计算动画增量 (0 到 charging_animation_range)
    float animation_progress = (float)charging_animation_step / (CHARGING_ANIM_STEPS - 1);
    
    // 如果是往下的方向，反转进度
    if (!charging_animation_direction) {
        animation_progress = 1.0f - animation_progress;
    }
    
    // 应用动画增量
    int animation_increment = (int)(charging_animation_range * animation_progress);
    int animated_percentage = base_percentage + animation_increment;
    
    // 确保不超过100%
    if (animated_percentage > 100) {
        animated_percentage = 100;
    }
    
    return animated_percentage;
}

/**
 * @brief 充电动画定时器回调函数
 */
static void charging_animation_timer_callback(void *arg)
{
    // 统一充电判断逻辑：被动控制 OR RS485检测到充电
    bool is_charging_display = (charging_status_override && external_charging_status);

    // 如果RS485检测到充电，也启用充电动画
    if (g_battery_data.data_valid && g_battery_data.pack_current > 0.05f) {
        is_charging_display = true;
    }

    if (!charging_animation_enabled || !is_charging_display) {
        return;
    }
    
    // 更新动画步骤 - 进度条增长动画
    if (charging_animation_direction) {
        charging_animation_step++;
        if (charging_animation_step >= CHARGING_ANIM_STEPS) {
            charging_animation_direction = false;
            charging_animation_step = CHARGING_ANIM_STEPS - 1;
        }
    } else {
        if (charging_animation_step > 0) {
            charging_animation_step--;
        } else {
            charging_animation_direction = true;
        }
    }
    
    // 设置UI更新标志
    ui_update_pending = true;
}

/**
 * @brief 启动充电动画
 */
static void start_charging_animation(void)
{
    if (charging_animation_timer == NULL) {
        const esp_timer_create_args_t charging_timer_args = {
            .callback = &charging_animation_timer_callback,
            .name = "charging_anim"
        };
        ESP_ERROR_CHECK(esp_timer_create(&charging_timer_args, &charging_animation_timer));
    }
    
    if (!esp_timer_is_active(charging_animation_timer)) {
        ESP_ERROR_CHECK(esp_timer_start_periodic(charging_animation_timer, CHARGING_ANIM_PERIOD * 1000));
        ESP_LOGI(TAG, "Charging animation started");
    }
}

/**
 * @brief 停止充电动画
 */
static void stop_charging_animation(void)
{
    if (charging_animation_timer != NULL && esp_timer_is_active(charging_animation_timer)) {
        ESP_ERROR_CHECK(esp_timer_stop(charging_animation_timer));
        charging_animation_step = 0;
        ESP_LOGI(TAG, "Charging animation stopped");
    }
}



/**
 * @brief 后台处理任务（处理所有耗时操作）
 */
static void background_processing_task(void *pvParameters)
{
    TickType_t last_ws2812_update = 0;
    TickType_t last_animation_check = 0;
    
    while (1) {
        TickType_t current_time = xTaskGetTickCount();
        
        // UI更新标志由LVGL任务专门处理，这里不处理
        
        // 定期处理WS2812更新（避免过于频繁）
        if (current_time - last_ws2812_update > pdMS_TO_TICKS(50)) {  // 最多20Hz更新
            // 信号丢失时由 no_signal_blink_timer 独占 WS2812 (红闪)，这里跳过
            if (!is_battery_signal_lost() &&
                (battery_display_override || charging_status_override)) {
                ws2812_update_battery_display(external_battery_value, external_charging_status);
            }
            last_ws2812_update = current_time;
        }
        
        // 定期检查充电动画状态
        if (current_time - last_animation_check > pdMS_TO_TICKS(100)) {  // 每100ms检查一次
            bool should_animate = charging_status_override && external_charging_status && charging_animation_enabled;
            
            if (should_animate && !esp_timer_is_active(charging_animation_timer)) {
                start_charging_animation();
            } else if (!should_animate && esp_timer_is_active(charging_animation_timer)) {
                stop_charging_animation();
            }
            
            last_animation_check = current_time;
        }
        
        // 处理队列中的复杂命令（如果有的话）
        uart_command_t cmd;
        if (uart_command_queue != NULL && xQueueReceive(uart_command_queue, &cmd, 0) == pdTRUE) {
            // 处理复杂的WS2812命令等
            if (strncmp(cmd.command, "JSON:", 5) == 0) {
                ws2812_handle_json_command(cmd.command + 5);
            }
        }
        
        // 适当休眠，避免占用过多CPU
        vTaskDelay(pdMS_TO_TICKS(10));  // 100Hz处理频率
    }
}

// NVS存储键名
#define NVS_NAMESPACE "battery_display"
#define NVS_KEY_OVERRIDE "override"
#define NVS_KEY_VALUE "ext_value"
#define NVS_KEY_VOLTAGE "voltage"
#define NVS_KEY_PERCENTAGE "percentage"
#define NVS_KEY_CHARGING "charging"
#define NVS_KEY_CHARGING_OVERRIDE "chg_override"
#define NVS_KEY_EXT_CHARGING "ext_charging"
#define NVS_KEY_VOLTAGE_OVERRIDE "volt_override"
#define NVS_KEY_EXT_VOLTAGE "ext_voltage"

// LVGL 对象
static lv_obj_t *battery_bar;          // 电池条
static lv_obj_t *battery_label;        // 电池百分比标签
static lv_obj_t *info_label;           // 信息标签
static lv_obj_t *mode_label;           // 电池读取模式标签 (BOOT 键切换)

// 函数声明
static void update_battery_ui(void);
static void apply_battery_read_mode(battery_read_mode_t mode);
static void boot_button_task(void *pvParameters);
static void rs485_1_query_task(void *pvParameters);
static void rs485_2_query_task(void *pvParameters);
void set_external_battery_level(int level);
void set_external_battery_percentage(int percentage);
void set_external_charging_status(bool charging);
void restore_auto_battery_mode(void);
void restore_auto_charging_mode(void);
void set_external_voltage(float voltage);
void restore_auto_voltage_mode(void);
float get_battery_voltage(void);
int get_battery_percentage(void);
bool is_charging(void);



/**
 * @brief UART1 初始化 (统一串口配置)
 */
static void uart1_init(void)
{
    const uart_config_t uart_config = {
        .baud_rate = UART1_BAUD_RATE,
        .data_bits = UART_DATA_8_BITS,
        .parity    = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

    // 使用更大的 RX ring（4x 缓冲），避免高波特率或突发数据下 ring-full 卡死
    ESP_ERROR_CHECK(uart_driver_install(UART_NUM_1, UART_BUF_SIZE * 4, UART_BUF_SIZE, 0, NULL, 0));
    ESP_ERROR_CHECK(uart_param_config(UART_NUM_1, &uart_config));

    // 配置普通UART模式
    ESP_ERROR_CHECK(uart_set_pin(UART_NUM_1, UART1_TXD_PIN, UART1_RXD_PIN, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
    // 清空可能存在的遗留数据
    uart_flush_input(UART_NUM_1);
    ESP_LOGI(TAG, "UART1 initialized - TX:%d, RX:%d, Baud:%d", 
             UART1_TXD_PIN, UART1_RXD_PIN, UART1_BAUD_RATE);

    ESP_LOGI(TAG, "RS485 Battery Query: runtime-selectable (mode switches baudrate)");
#if ENABLE_JSON_PASSIVE_MODE
    ESP_LOGI(TAG, "JSON Passive Control: Enabled");
#endif
}

/**
 * @brief 计算校验和
 */
static uint16_t calculate_checksum(const uint8_t* data, size_t len)
{
    uint16_t sum = 0;
    for (size_t i = 0; i < len; i++) {
        sum += data[i];
    }
    return (~sum + 1);  // 取反加1
}

/**
 * @brief 发送电池查询命令
 */
static bool send_battery_query(uint8_t cmd)
{
    uint8_t frame[8];
    size_t frame_len = 0;
    
    // 构建查询帧
    frame[frame_len++] = BATTERY_FRAME_START;  // 起始位
    frame[frame_len++] = BATTERY_READ_CMD;     // 读取命令
    frame[frame_len++] = cmd;                  // 命令码
    frame[frame_len++] = 0x00;                 // 数据长度为0
    
    // 计算校验和（数据长度+命令码）
    uint16_t checksum = calculate_checksum(&frame[2], 2);
    frame[frame_len++] = (checksum >> 8) & 0xFF;  // 校验高字节
    frame[frame_len++] = checksum & 0xFF;          // 校验低字节
    frame[frame_len++] = BATTERY_FRAME_END;        // 结束位
    
    // 发送数据
    int bytes_sent = uart_write_bytes(UART_NUM_1, frame, frame_len);
    if (bytes_sent != frame_len) {
        ESP_LOGE(TAG, "Battery query send failed, expected %d bytes, sent %d", frame_len, bytes_sent);
        return false;
    }
    
    ESP_LOGD(TAG, "Sent battery query cmd 0x%02X", cmd);
    return true;
}

/**
 * @brief 解析电池基本信息响应
 */
static bool parse_battery_info_response(const uint8_t* data, size_t len, battery_data_t* battery_data)
{
    if (len < 7) {  // 最小帧长度
        ESP_LOGE(TAG, "Battery response too short: %d", len);
        return false;
    }
    
    // 验证帧头尾
    if (data[0] != BATTERY_FRAME_START || data[len-1] != BATTERY_FRAME_END) {
        ESP_LOGE(TAG, "Invalid frame header/footer");
        return false;
    }
    
    // 检查命令码和状态
    if (data[1] != BATTERY_CMD_INFO || data[2] != 0x00) {
        ESP_LOGE(TAG, "Invalid response cmd or status: 0x%02X, 0x%02X", data[1], data[2]);
        return false;
    }
    
    uint8_t data_len = data[3];
    if (len < data_len + 6) {  // 数据长度 + 帧头(4) + 校验(2)
        ESP_LOGE(TAG, "Data length mismatch");
        return false;
    }
    
    // 解析数据（按协议文档格式）
    const uint8_t* payload = &data[4];
    size_t pos = 0;
    
    if (data_len >= 2) {
        // 总电压 (2字节, 单位10mV)
        battery_data->pack_voltage = ((payload[pos] << 8) | payload[pos+1]) / 100.0f;  // 转换为V
        pos += 2;
    }
    
    if (data_len >= 4) {
        // 电流 (2字节, 单位10mA, 带符号)
        int16_t current_raw = (payload[pos] << 8) | payload[pos+1];
        battery_data->pack_current = current_raw / 100.0f;  // 转换为A
        pos += 2;
    }
    
    if (data_len >= 6) {
        // 剩余容量 (2字节, 单位10mAh)
        battery_data->remain_capacity = (payload[pos] << 8) | payload[pos+1];
        pos += 2;
    }
    
    if (data_len >= 8) {
        // 标称容量 (2字节, 单位10mAh)
        battery_data->full_capacity = (payload[pos] << 8) | payload[pos+1];
        pos += 2;
    }
    
    // 跳过循环次数和生产日期
    if (data_len >= 12) pos += 4;
    
    // 跳过均衡状态
    if (data_len >= 16) pos += 4;
    
    if (data_len >= 18) {
        // 保护状态 (2字节)
        battery_data->protect_status = (payload[pos] << 8) | payload[pos+1];
        pos += 2;
    }
    
    if (data_len >= 19) {
        // 软件版本 (1字节) - 跳过
        pos += 1;
    }
    
    if (data_len >= 20) {
        // SOC (1字节)
        battery_data->soc = payload[pos];
        pos += 1;
    }
    
    if (data_len >= 21) {
        // FET控制状态 (1字节)
        battery_data->fet_status = payload[pos];
        pos += 1;
    }
    
    if (data_len >= 22) {
        // 电池串数 (1字节)
        battery_data->battery_strings = payload[pos];
        pos += 1;
    }
    
    if (data_len >= 23) {
        // NTC个数 (1字节)
        battery_data->temp_count = payload[pos];
        pos += 1;
        
        // 解析温度值
        for (int i = 0; i < battery_data->temp_count && i < 8 && pos + 1 < data_len; i++) {
            uint16_t temp_raw = (payload[pos] << 8) | payload[pos+1];
            battery_data->temperatures[i] = (temp_raw - 2731) / 10.0f;  // 转换为摄氏度
            pos += 2;
        }
    }
    
    battery_data->data_valid = true;
    battery_data->last_update = xTaskGetTickCount();
    
    ESP_LOGI(TAG, "RS485 Battery Parsed: %.2fV, %.2fA, %d%%, %dmAh/%dmAh, %d串", 
             battery_data->pack_voltage, battery_data->pack_current, 
             battery_data->soc, battery_data->remain_capacity * 10, 
             battery_data->full_capacity * 10, battery_data->battery_strings);
    
    // 添加详细的数据调试
    ESP_LOGI(TAG, "RS485 Raw data - SOC: 0x%02X (%d%%), Voltage: %.2fV, Current: %.2fA", 
             battery_data->soc, battery_data->soc, battery_data->pack_voltage, battery_data->pack_current);
    
    return true;
}

/**
 * @brief 读取电池响应数据
 */
static bool read_battery_response(uint8_t expected_cmd, battery_data_t* battery_data)
{
    uint8_t buffer[128];
    int bytes_read = uart_read_bytes(UART_NUM_1, buffer, sizeof(buffer), pdMS_TO_TICKS(BATTERY_TIMEOUT_MS));
    
    if (bytes_read <= 0) {
        ESP_LOGW(TAG, "No battery response received");
        return false;
    }
    
    ESP_LOGD(TAG, "Received %d bytes from battery", bytes_read);
    
    // 解析响应
    if (expected_cmd == BATTERY_CMD_INFO) {
        return parse_battery_info_response(buffer, bytes_read, battery_data);
    }
    
    return false;
}

/**
 * @brief 查询电池基本信息
 */
static bool query_battery_info(void)
{
    if (!send_battery_query(BATTERY_CMD_INFO)) {
        return false;
    }
    
    return read_battery_response(BATTERY_CMD_INFO, &g_battery_data);
}

/**
 * @brief 更新全局电池状态（从RS485数据）
 */
static void update_battery_status_from_rs485(void)
{
    if (!g_battery_data.data_valid) {
        return;
    }
    
    // 更新电压 - RS485数据优先级最高
    g_battery_voltage = g_battery_data.pack_voltage;
    if (voltage_override) {
        external_voltage_value = g_battery_data.pack_voltage;
        ESP_LOGI(TAG, "RS485 voltage override: %.2fV", g_battery_data.pack_voltage);
    }
    
    // 更新电量百分比 - RS485数据强制更新显示
    g_battery_percentage = g_battery_data.soc;
    external_battery_value = g_battery_data.soc;
    
    // 如果之前有手动设置，现在切换到RS485模式
    if (battery_display_override) {
        battery_display_override = false;  // 取消手动覆盖，使用RS485数据
        ESP_LOGI(TAG, "Switched from manual to RS485 battery display: %d%%", g_battery_data.soc);
    }
    
    // 更新充电状态（根据电流判断） - RS485数据优先
    // 根据485协议：充电电流为正，放电电流为负
    // 设置合理的阈值避免误判
    bool is_charging_now = false;
    bool is_discharging_now = false;
    
    if (g_battery_data.pack_current > 0.05f) {  // 大于0.05A认为是充电
        is_charging_now = true;
        ESP_LOGD(TAG, "Battery charging detected: %.3fA", g_battery_data.pack_current);
    } else if (g_battery_data.pack_current < -0.05f) {  // 小于-0.05A认为是放电
        is_discharging_now = true;
        ESP_LOGD(TAG, "Battery discharging detected: %.3fA", g_battery_data.pack_current);
    } else {
        // -0.05A ≤ 电流 ≤ 0.05A 认为是静置状态
        ESP_LOGD(TAG, "Battery idle state: %.3fA", g_battery_data.pack_current);
    }
    
    g_charging_status = is_charging_now;
    external_charging_status = is_charging_now;
    
    // 如果之前有手动设置充电状态，现在切换到RS485模式
    if (charging_status_override) {
        charging_status_override = false;  // 取消手动覆盖，使用RS485数据
        ESP_LOGI(TAG, "Switched from manual to RS485 charging status: %s (current: %.3fA)", 
                is_charging_now ? "charging" : (is_discharging_now ? "discharging" : "idle"), 
                g_battery_data.pack_current);
    }
    
    // 根据电流状态更新日志
    static bool last_charging_state = false;
    if (is_charging_now != last_charging_state) {
        if (is_charging_now) {
            ESP_LOGI(TAG, "🔋 Charging started: %.3fA", g_battery_data.pack_current);
        } else if (is_discharging_now) {
            ESP_LOGI(TAG, "⚡ Discharging started: %.3fA", g_battery_data.pack_current);
        } else {
            ESP_LOGI(TAG, "🔌 Battery idle: %.3fA", g_battery_data.pack_current);
        }
        last_charging_state = is_charging_now;
    }
    
    // 充电状态变化时管理动画
    if (charging_animation_enabled) {
        if (g_charging_status) {
            start_charging_animation();
            ESP_LOGI(TAG, "Started charging animation (current: %.2fA)", g_battery_data.pack_current);
        } else {
            stop_charging_animation();
            ESP_LOGI(TAG, "Stopped charging animation (current: %.2fA)", g_battery_data.pack_current);
        }
    }
    
    // 触发UI更新
    notify_ui_update_needed();
}







/**
 * @brief 保存电池显示状态到NVS
 */
static esp_err_t save_battery_state_to_nvs(void)
{
    nvs_handle_t nvs_handle;
    esp_err_t ret = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Error opening NVS handle: %s", esp_err_to_name(ret));
        return ret;
    }

    // 保存显示覆盖状态
    ret = nvs_set_u8(nvs_handle, NVS_KEY_OVERRIDE, battery_display_override ? 1 : 0);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Error saving override state: %s", esp_err_to_name(ret));
        nvs_close(nvs_handle);
        return ret;
    }

    // 保存外部设置的电量值
    ret = nvs_set_u8(nvs_handle, NVS_KEY_VALUE, (uint8_t)external_battery_value);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Error saving external battery value: %s", esp_err_to_name(ret));
        nvs_close(nvs_handle);
        return ret;
    }

    // 保存当前电池状态
    ret = nvs_set_blob(nvs_handle, NVS_KEY_VOLTAGE, &g_battery_voltage, sizeof(float));
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Error saving voltage: %s", esp_err_to_name(ret));
        nvs_close(nvs_handle);
        return ret;
    }

    ret = nvs_set_u8(nvs_handle, NVS_KEY_PERCENTAGE, (uint8_t)external_battery_value);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Error saving percentage: %s", esp_err_to_name(ret));
        nvs_close(nvs_handle);
        return ret;
    }

    ret = nvs_set_u8(nvs_handle, NVS_KEY_CHARGING, g_charging_status ? 1 : 0);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Error saving charging status: %s", esp_err_to_name(ret));
        nvs_close(nvs_handle);
        return ret;
    }

    // 保存充电状态覆盖设置
    ret = nvs_set_u8(nvs_handle, NVS_KEY_CHARGING_OVERRIDE, charging_status_override ? 1 : 0);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Error saving charging override state: %s", esp_err_to_name(ret));
        nvs_close(nvs_handle);
        return ret;
    }

    // 保存外部设置的充电状态
    ret = nvs_set_u8(nvs_handle, NVS_KEY_EXT_CHARGING, external_charging_status ? 1 : 0);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Error saving external charging status: %s", esp_err_to_name(ret));
        nvs_close(nvs_handle);
        return ret;
    }

    // 保存电压覆盖状态
    ret = nvs_set_u8(nvs_handle, NVS_KEY_VOLTAGE_OVERRIDE, voltage_override ? 1 : 0);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Error saving voltage override state: %s", esp_err_to_name(ret));
        nvs_close(nvs_handle);
        return ret;
    }

    // 保存外部设置的电压值
    ret = nvs_set_blob(nvs_handle, NVS_KEY_EXT_VOLTAGE, &external_voltage_value, sizeof(float));
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Error saving external voltage value: %s", esp_err_to_name(ret));
        nvs_close(nvs_handle);
        return ret;
    }

    // 提交更改
    ret = nvs_commit(nvs_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Error committing NVS: %s", esp_err_to_name(ret));
    } else {
        ESP_LOGI(TAG, "Battery state saved to NVS successfully");
    }

    nvs_close(nvs_handle);
    return ret;
}

/**
 * @brief 从NVS加载电池显示状态
 */
static esp_err_t load_battery_state_from_nvs(void)
{
    nvs_handle_t nvs_handle;
    esp_err_t ret = nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs_handle);
    if (ret != ESP_OK) {
        ESP_LOGI(TAG, "No saved battery state found, using defaults");
        return ESP_OK; // 不是错误，只是没有保存的状态
    }

    uint8_t temp_u8;
    size_t required_size;

    // 加载显示覆盖状态
    ret = nvs_get_u8(nvs_handle, NVS_KEY_OVERRIDE, &temp_u8);
    if (ret == ESP_OK) {
        battery_display_override = (temp_u8 != 0);
    }

    // 加载外部设置的电量值
    ret = nvs_get_u8(nvs_handle, NVS_KEY_VALUE, &temp_u8);
    if (ret == ESP_OK) {
        external_battery_value = (int)temp_u8;
    }

    // 加载电池电压
    required_size = sizeof(float);
    ret = nvs_get_blob(nvs_handle, NVS_KEY_VOLTAGE, &g_battery_voltage, &required_size);
    if (ret != ESP_OK) {
        g_battery_voltage = 24.0f; // 默认值适配24V系统
    }

    // 加载电池百分比到外部控制值
    ret = nvs_get_u8(nvs_handle, NVS_KEY_PERCENTAGE, &temp_u8);
    if (ret == ESP_OK) {
        external_battery_value = (int)temp_u8;
        // 同时更新g_battery_percentage以保持兼容性
        g_battery_percentage = (int)temp_u8;
    }

    // 加载充电状态
    ret = nvs_get_u8(nvs_handle, NVS_KEY_CHARGING, &temp_u8);
    if (ret == ESP_OK) {
        g_charging_status = (temp_u8 != 0);
    }

    // 加载充电状态覆盖设置
    ret = nvs_get_u8(nvs_handle, NVS_KEY_CHARGING_OVERRIDE, &temp_u8);
    if (ret == ESP_OK) {
        charging_status_override = (temp_u8 != 0);
    }

    // 加载外部设置的充电状态
    ret = nvs_get_u8(nvs_handle, NVS_KEY_EXT_CHARGING, &temp_u8);
    if (ret == ESP_OK) {
        external_charging_status = (temp_u8 != 0);
    }

    // 加载电压覆盖状态
    ret = nvs_get_u8(nvs_handle, NVS_KEY_VOLTAGE_OVERRIDE, &temp_u8);
    if (ret == ESP_OK) {
        voltage_override = (temp_u8 != 0);
    }

    // 加载外部设置的电压值
    required_size = sizeof(float);
    ret = nvs_get_blob(nvs_handle, NVS_KEY_EXT_VOLTAGE, &external_voltage_value, &required_size);
    if (ret != ESP_OK) {
        external_voltage_value = 24.0f; // 默认值适配24V系统
    }

    nvs_close(nvs_handle);
    
    ESP_LOGI(TAG, "Battery state loaded from NVS: Override=%s, ExtValue=%d%%, ChargingOverride=%s, ExtCharging=%s, VoltageOverride=%s, ExtVoltage=%.2fV, Voltage=%.2fV, Percentage=%d%%, Charging=%s",
             battery_display_override ? "true" : "false",
             external_battery_value,
             charging_status_override ? "true" : "false",
             external_charging_status ? "true" : "false",
             voltage_override ? "true" : "false",
             external_voltage_value,
             g_battery_voltage,
             g_battery_percentage,
             g_charging_status ? "true" : "false");
    
    return ESP_OK;
}

/**
 * @brief 生成电池状态JSON字符串
 */
char* create_battery_status_json(void)
{
    cJSON *json = cJSON_CreateObject();
    if (json == NULL) {
        return NULL;
    }

    // 直接添加电池基本信息到根级别（取消battery对象）
    cJSON_AddNumberToObject(json, "voltage", (double)get_battery_voltage());
    cJSON_AddNumberToObject(json, "percentage", get_battery_percentage());
    
    // 获取有效的充电状态
    bool effective_charging = get_effective_charging_status();
    cJSON_AddStringToObject(json, "charging_status", effective_charging ? "CHARGING" : "DISCHARGING");
    
    // 添加电量和充电状态控制信息
    int display_percentage = external_battery_value;  // 始终使用external_battery_value
    cJSON_AddBoolToObject(json, "is_full", display_percentage >= BATTERY_FULL_THRESHOLD);

    // 添加显示控制信息
    cJSON *display = cJSON_CreateObject();
    cJSON_AddStringToObject(display, "mode", battery_display_override ? "external" : "auto");
    if (battery_display_override) {
        cJSON_AddNumberToObject(display, "external_value", external_battery_value);
    }
    cJSON_AddNumberToObject(display, "current_display", display_percentage);
    cJSON_AddItemToObject(json, "display", display);

    // 添加充电状态控制信息
    cJSON *charging_control = cJSON_CreateObject();
    cJSON_AddStringToObject(charging_control, "mode", charging_status_override ? "external" : "auto");
    cJSON_AddBoolToObject(charging_control, "auto_charging", g_charging_status);
    if (charging_status_override) {
        cJSON_AddBoolToObject(charging_control, "external_charging", external_charging_status);
    }
    cJSON_AddBoolToObject(charging_control, "effective_charging", effective_charging);
    cJSON_AddItemToObject(json, "charging_control", charging_control);

    // 添加电压控制信息
    cJSON *voltage_control = cJSON_CreateObject();
    cJSON_AddStringToObject(voltage_control, "mode", voltage_override ? "external" : "auto");
    cJSON_AddNumberToObject(voltage_control, "current_voltage", (double)g_battery_voltage);
    if (voltage_override) {
        cJSON_AddNumberToObject(voltage_control, "external_voltage", (double)external_voltage_value);
    }
    cJSON_AddStringToObject(voltage_control, "note", "Voltage is for monitoring only and does not affect battery percentage");
    cJSON_AddItemToObject(json, "voltage_control", voltage_control);

    // 添加充电动画信息
    cJSON *animation = cJSON_CreateObject();
    cJSON_AddBoolToObject(animation, "enabled", charging_animation_enabled);
    cJSON_AddBoolToObject(animation, "active", effective_charging && charging_animation_enabled);
    cJSON_AddNumberToObject(animation, "step", charging_animation_step);
    if (effective_charging && display_percentage >= BATTERY_FULL_THRESHOLD) {
        cJSON_AddStringToObject(animation, "effect", "full_battery");
    } else if (effective_charging) {
        cJSON_AddStringToObject(animation, "effect", "charging");
    } else {
        cJSON_AddStringToObject(animation, "effect", "static");
    }
    cJSON_AddItemToObject(json, "animation", animation);

    // 添加时间戳
    cJSON_AddNumberToObject(json, "timestamp", (double)(esp_timer_get_time() / 1000000));

    // 添加状态
    cJSON_AddStringToObject(json, "status", "success");

    char *json_string = cJSON_Print(json);
    cJSON_Delete(json);
    
    return json_string;
}
static float simulate_battery_voltage(void)
{
    static float voltage = 24.0f;
    static float direction = 0.1f;
    static bool prev_charging_status = false;
    
    voltage += direction;
    
    // 简单的电压范围限制（仅用于模拟，不影响百分比计算）
    if (voltage >= 29.4f) {  // 原 BATTERY_MAX_VOLTAGE
        voltage = 29.4f;
        direction = -0.1f;
        g_charging_status = false;
    } else if (voltage <= 18.0f) {  // 原 BATTERY_MIN_VOLTAGE
        voltage = 18.0f;
        direction = 0.1f;
        g_charging_status = true;
    }
    
    // 检查充电状态变化，控制动画
    if (g_charging_status != prev_charging_status) {
        if (g_charging_status) {
            ESP_LOGI(TAG, "Charging started - Starting charging animation");
            start_charging_animation();
        } else {
            ESP_LOGI(TAG, "Charging stopped - Stopping charging animation");
            stop_charging_animation();
        }
        prev_charging_status = g_charging_status;
    }
    
    return voltage;
}

/**
 * @brief 获取充电动画颜色
 */
__attribute__((unused))
static lv_color_t get_charging_animation_color(int percentage, uint32_t animation_step)
{
    // 计算动画亮度变化 (70% - 100%)
    float brightness_factor = 0.7f + 0.3f * (float)(animation_step % CHARGING_ANIM_STEPS) / (CHARGING_ANIM_STEPS - 1);
    
    // 如果是向下的动画，反转亮度
    if (animation_step >= CHARGING_ANIM_STEPS) {
        brightness_factor = 1.0f - brightness_factor + 0.7f;
    }
    
    // 根据电量级别选择基础颜色
    uint32_t color_hex;
    if (percentage >= BATTERY_FULL_THRESHOLD) {
        color_hex = 0x00FF00;  // 绿色
    } else if (percentage >= 60) {
        color_hex = 0x00FFAA;  // 青绿色
    } else if (percentage >= 30) {
        color_hex = 0xFFAA00;  // 橙色
    } else {
        color_hex = 0xFF4400;  // 红橙色
    }
    
    // 应用亮度变化
    uint8_t r = (uint8_t)(((color_hex >> 16) & 0xFF) * brightness_factor);
    uint8_t g = (uint8_t)(((color_hex >> 8) & 0xFF) * brightness_factor);
    uint8_t b = (uint8_t)((color_hex & 0xFF) * brightness_factor);
    
    return lv_color_make(r, g, b);
}

/**
 * @brief 获取充满电特殊效果颜色
 */
__attribute__((unused))
static lv_color_t get_full_battery_color(uint32_t animation_step)
{
    // 充满电时的彩虹渐变效果
    uint8_t phase = animation_step % (CHARGING_ANIM_STEPS * 3);
    
    if (phase < CHARGING_ANIM_STEPS) {
        // 绿色到青色
        uint8_t intensity = (255 * phase) / CHARGING_ANIM_STEPS;
        return lv_color_make(0, 255, intensity);
    } else if (phase < CHARGING_ANIM_STEPS * 2) {
        // 青色到蓝色
        uint8_t intensity = 255 - (255 * (phase - CHARGING_ANIM_STEPS)) / CHARGING_ANIM_STEPS;
        return lv_color_make(0, intensity, 255);
    } else {
        // 蓝色到绿色
        uint8_t intensity = (255 * (phase - CHARGING_ANIM_STEPS * 2)) / CHARGING_ANIM_STEPS;
        return lv_color_make(0, intensity, 255 - intensity);
    }
}

/* ============================================================
 * RS485-2 电池协议 (0xAA 帧头, 小端, 累加和校验)
 * 协议来源: doc/通讯协议-20250113-客户(蓝牙 485协议).pdf
 * ============================================================ */

static uint16_t batt2_checksum(const uint8_t *p, size_t len)
{
    uint16_t s = 0;
    while (len--) s += *p++;
    return s;
}

// 发送一条无数据的查询指令: 0xAA CMD 0x00 SUM_L SUM_H
static bool batt2_send_query(uint8_t cmd)
{
    uint8_t frame[5];
    frame[0] = BATT2_FRAME_HEAD;
    frame[1] = cmd;
    frame[2] = 0x00;
    uint16_t sum = batt2_checksum(&frame[1], 2);   // CMD + LEN
    frame[3] = (uint8_t)(sum & 0xFF);
    frame[4] = (uint8_t)((sum >> 8) & 0xFF);
    int n = uart_write_bytes(UART_NUM_1, frame, sizeof(frame));
    return n == (int)sizeof(frame);
}

// 读取一帧应答; 成功时将 payload 拷贝到 data_out (容量 cap), out_len 返回实际长度
static bool batt2_read_response(uint8_t expected_cmd,
                                uint8_t *data_out, size_t cap, uint8_t *out_len)
{
    // 先逐字节同步到帧头 0xAA (最多扫描 32 字节)
    uint8_t b = 0;
    int scan = 0;
    while (scan < 32) {
        int n = uart_read_bytes(UART_NUM_1, &b, 1, pdMS_TO_TICKS(BATT2_RESP_TIMEOUT_MS));
        if (n != 1) return false;
        if (b == BATT2_FRAME_HEAD) break;
        scan++;
    }
    if (b != BATT2_FRAME_HEAD) return false;

    // 读 CMD + LEN
    uint8_t hdr[2];
    int n = uart_read_bytes(UART_NUM_1, hdr, 2, pdMS_TO_TICKS(BATT2_RESP_TIMEOUT_MS));
    if (n != 2) return false;
    uint8_t cmd = hdr[0];
    uint8_t len = hdr[1];
    if (cmd != expected_cmd) {
        ESP_LOGW(TAG, "RS485-2: cmd mismatch got 0x%02X want 0x%02X", cmd, expected_cmd);
        return false;
    }
    if (len > BATT2_MAX_DATA_LEN) return false;

    uint8_t buf[BATT2_MAX_DATA_LEN + 2];
    size_t need = (size_t)len + 2; // payload + 2B checksum
    n = uart_read_bytes(UART_NUM_1, buf, need, pdMS_TO_TICKS(BATT2_RESP_TIMEOUT_MS));
    if (n != (int)need) return false;

    // 校验: sum(CMD, LEN, DATA), 小端比较
    uint16_t sum = (uint16_t)cmd + (uint16_t)len;
    for (uint8_t i = 0; i < len; i++) sum += buf[i];
    uint16_t recv = (uint16_t)buf[len] | ((uint16_t)buf[len + 1] << 8);
    if (sum != recv) {
        ESP_LOGW(TAG, "RS485-2: checksum err calc=0x%04X recv=0x%04X", sum, recv);
        return false;
    }

    if (data_out && cap > 0) {
        size_t copy = len < cap ? len : cap;
        memcpy(data_out, buf, copy);
    }
    if (out_len) *out_len = len;
    return true;
}

// 解析 0x21 电池信息 (26 字节, 小端)
// [0..3]voltage(mV) [4..7]current(mA,有符号) [8]SOC% [9]SOH%
// [10..13]remain(mAh) [14..17]full(mAh) [18..19]cycles
// [20..23]temp1..4 [24]MOS_temp [25]env_temp
static void batt2_parse_info(const uint8_t *d, uint8_t len)
{
    if (len < 26) {
        ESP_LOGW(TAG, "RS485-2: info len=%u < 26", len);
        return;
    }
    uint32_t mv = (uint32_t)d[0] | ((uint32_t)d[1] << 8) |
                  ((uint32_t)d[2] << 16) | ((uint32_t)d[3] << 24);
    int32_t  ma = (int32_t)((uint32_t)d[4] | ((uint32_t)d[5] << 8) |
                            ((uint32_t)d[6] << 16) | ((uint32_t)d[7] << 24));
    uint8_t soc = d[8];
    uint8_t soh = d[9];
    uint32_t remain_mah = (uint32_t)d[10] | ((uint32_t)d[11] << 8) |
                         ((uint32_t)d[12] << 16) | ((uint32_t)d[13] << 24);
    uint32_t full_mah   = (uint32_t)d[14] | ((uint32_t)d[15] << 8) |
                         ((uint32_t)d[16] << 16) | ((uint32_t)d[17] << 24);
    uint16_t cycles     = (uint16_t)d[18] | ((uint16_t)d[19] << 8);

    float voltage_v = (float)mv / 1000.0f;
    float current_a = (float)ma / 1000.0f;

    // 喂入现有显示/WS2812/信号存活管线
    set_external_voltage(voltage_v);
    if (soc <= 100) set_external_battery_level(soc);
    set_external_charging_status(current_a > 0.05f);

    ESP_LOGI(TAG, "RS485-2: %.3fV %.3fA SOC=%u%% SOH=%u%% %u/%u mAh cycles=%u",
             voltage_v, current_a, soc, soh,
             (unsigned)remain_mah, (unsigned)full_mah, (unsigned)cycles);
}

/**
 * @brief RS485-1 主动查询任务
 *        仅当当前模式为 BATT_READ_MODE_RS485_1 时才持有 UART 进行收发；
 *        其它模式下进入长睡眠，不干扰 UART 通道。
 */
static void rs485_1_query_task(void *pvParameters)
{
    (void)pvParameters;
    uint32_t fail_count = 0;

    while (1) {
        if (g_battery_read_mode != BATT_READ_MODE_RS485_1) {
            fail_count = 0;
            vTaskDelay(pdMS_TO_TICKS(200));
            continue;
        }

        uart_flush_input(UART_NUM_1);
        if (query_battery_info()) {
            update_battery_status_from_rs485();
            fail_count = 0;
            ui_update_pending = true;
        } else {
            fail_count++;
            ESP_LOGW(TAG, "RS485-1: query failed (%u)", (unsigned)fail_count);
            // 连续多次失败时，标记数据过期
            if (fail_count >= 3) {
                g_battery_data.data_valid = false;
                ui_update_pending = true;
            }
        }

        vTaskDelay(pdMS_TO_TICKS(BATTERY_QUERY_PERIOD));
    }
}

/**
 * @brief RS485-2 主动查询任务
 *        仅当当前模式为 BATT_READ_MODE_RS485_2 时才持有 UART 进行收发；
 *        其它模式下进入长睡眠，不干扰 UART 通道。
 */
static void rs485_2_query_task(void *pvParameters)
{
    (void)pvParameters;
    bool handshaked = false;
    uint32_t fail_count = 0;

    while (1) {
        if (g_battery_read_mode != BATT_READ_MODE_RS485_2) {
            handshaked = false;
            fail_count = 0;
            vTaskDelay(pdMS_TO_TICKS(200));
            continue;
        }

        // 首次进入或多次失败后重新握手, 避免 BMS 处于休眠
        if (!handshaked || fail_count >= 3) {
            uart_flush_input(UART_NUM_1);
            batt2_send_query(BATT2_CMD_HANDSHAKE);
            uint8_t tmp[8]; uint8_t tl = 0;
            (void)batt2_read_response(BATT2_CMD_HANDSHAKE, tmp, sizeof(tmp), &tl);
            handshaked = true;
            fail_count = 0;
            vTaskDelay(pdMS_TO_TICKS(50));
        }

        uart_flush_input(UART_NUM_1);
        bool ok = false;
        if (batt2_send_query(BATT2_CMD_BATTERY_INFO)) {
            uint8_t data[BATT2_MAX_DATA_LEN]; uint8_t rl = 0;
            if (batt2_read_response(BATT2_CMD_BATTERY_INFO, data, sizeof(data), &rl)) {
                batt2_parse_info(data, rl);
                ok = true;
            }
        }
        if (!ok) {
            fail_count++;
            ESP_LOGW(TAG, "RS485-2: query failed (%u)", (unsigned)fail_count);
        } else {
            fail_count = 0;
        }

        vTaskDelay(pdMS_TO_TICKS(BATT2_QUERY_PERIOD_MS));
    }
}

/**
 * @brief 应用指定的电池读取模式：同步 UART 波特率并通知 UI 刷新
 */
/* ============================================================
 * 读取模式持久化 (独立 NVS 命名空间, 只存一个 u8, 对 Flash 压力极小)
 * ============================================================ */
#define MODE_NVS_NAMESPACE   "batt_mode"
#define MODE_NVS_KEY         "mode"

static void save_battery_read_mode_to_nvs(battery_read_mode_t mode)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(MODE_NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "save mode: nvs_open failed: %s", esp_err_to_name(err));
        return;
    }
    uint8_t cur = 0xFF;
    nvs_get_u8(h, MODE_NVS_KEY, &cur);
    if (cur != (uint8_t)mode) {
        err = nvs_set_u8(h, MODE_NVS_KEY, (uint8_t)mode);
        if (err == ESP_OK) err = nvs_commit(h);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "save mode: write failed: %s", esp_err_to_name(err));
        } else {
            ESP_LOGI(TAG, "Battery read mode saved to NVS: %s",
                     battery_read_mode_name(mode));
        }
    }
    nvs_close(h);
}

static battery_read_mode_t load_battery_read_mode_from_nvs(void)
{
    nvs_handle_t h;
    if (nvs_open(MODE_NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) {
        return BATT_READ_MODE_JSON;
    }
    uint8_t v = 0;
    battery_read_mode_t m = BATT_READ_MODE_JSON;
    if (nvs_get_u8(h, MODE_NVS_KEY, &v) == ESP_OK && v < BATT_READ_MODE_MAX) {
        m = (battery_read_mode_t)v;
    }
    nvs_close(h);
    return m;
}

/**
 * @brief 应用指定的电池读取模式：同步 UART 波特率并通知 UI 刷新
 */
static void apply_battery_read_mode(battery_read_mode_t mode)
{
    if (mode >= BATT_READ_MODE_MAX) mode = BATT_READ_MODE_JSON;
    g_battery_read_mode = mode;

    uint32_t baud = 115200;   // JSON 默认
    if (mode == BATT_READ_MODE_RS485_1 || mode == BATT_READ_MODE_RS485_2) baud = 9600;

    // 等待当前 TX 发送完成，避免在半个字节期间改波特率
    uart_wait_tx_done(UART_NUM_1, pdMS_TO_TICKS(100));

    // 切换波特率并刷新缓冲，避免残留数据
    esp_err_t err = uart_set_baudrate(UART_NUM_1, baud);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "uart_set_baudrate(%u) failed: %s", (unsigned)baud, esp_err_to_name(err));
    }
    uart_flush_input(UART_NUM_1);

    // 持久化 (仅在值变化时真正写 Flash)
    save_battery_read_mode_to_nvs(mode);

    ESP_LOGI(TAG, "Battery read mode -> %s (UART1 baud=%u)",
             battery_read_mode_name(mode), (unsigned)baud);

    // 通知 LVGL 任务立刻刷新模式标签
    ui_update_pending = true;
    if (lvgl_task_handle != NULL) {
        xTaskNotifyGive(lvgl_task_handle);
    }
}

/**
 * @brief BOOT 键轮询任务：下降沿 + 去抖，触发模式循环切换
 */
static void boot_button_task(void *pvParameters)
{
    (void)pvParameters;

    gpio_config_t io_cfg = {
        .pin_bit_mask = 1ULL << BOOT_BUTTON_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io_cfg);

    bool last_level = true;            // 按下为低
    TickType_t last_change = xTaskGetTickCount();
    bool pending_release = false;      // 防止长按重复触发

    while (1) {
        bool level = gpio_get_level(BOOT_BUTTON_GPIO) ? true : false;
        TickType_t now = xTaskGetTickCount();

        if (level != last_level) {
            last_change = now;
            last_level = level;
        } else if ((now - last_change) >= pdMS_TO_TICKS(BOOT_BUTTON_DEBOUNCE_MS)) {
            if (!level && !pending_release) {
                // 稳定在低 -> 按下事件（仅触发一次，直到松开）
                battery_read_mode_t next = (battery_read_mode_t)((g_battery_read_mode + 1) % BATT_READ_MODE_MAX);
                apply_battery_read_mode(next);
                pending_release = true;
            } else if (level && pending_release) {
                pending_release = false;
            }
        }

        vTaskDelay(pdMS_TO_TICKS(BOOT_BUTTON_POLL_MS));
    }
}

/**
 * @brief 创建简洁的电池电量条界面（横屏布局 428x142）
 */
static void create_battery_ui(void)
{
    // 创建主屏幕
    lv_obj_t *scr = lv_scr_act();
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x000000), 0);  // 黑色背景
    
    // // 标题标签 - 居中显示
    // title_label = lv_label_create(scr);
    // lv_label_set_text(title_label, "BATTERY LEVEL");
    // lv_obj_set_style_text_color(title_label, lv_color_hex(0xFFFFFF), 0);
    // lv_obj_set_style_text_font(title_label, &lv_font_montserrat_18, 0);
    // lv_obj_align(title_label, LV_ALIGN_TOP_MID, 0, 15);
    
    // 创建电量条容器 - 简洁的白色边框
    lv_obj_t *battery_container = lv_obj_create(scr);
    lv_obj_set_size(battery_container, 380, 30);  // 横长的电量条
    lv_obj_set_style_bg_color(battery_container, lv_color_hex(0x000000), 0);  // 透明背景
    lv_obj_set_style_bg_opa(battery_container, 0, 0);  // 完全透明
    lv_obj_set_style_border_color(battery_container, lv_color_hex(0xFFFFFF), 0);  // 白色边框
    lv_obj_set_style_border_width(battery_container, 2, 0);
    lv_obj_set_style_radius(battery_container, 15, 0);  // 圆角
    lv_obj_set_style_pad_all(battery_container, 3, 0);  // 内边距
    lv_obj_align(battery_container, LV_ALIGN_CENTER, 0, -5);
    
    // 获取正确的初始显示值
    int initial_percentage = external_battery_value;  // 始终使用external_battery_value
    ESP_LOGI(TAG, "Creating UI with initial percentage: %d%% (Override: %s)", 
             initial_percentage, battery_display_override ? "true" : "false");
    
    // 电量条 - 白色填充
    battery_bar = lv_bar_create(battery_container);
    lv_obj_set_size(battery_bar, 374, 24);  // 填满容器内部
    lv_obj_center(battery_bar);
    lv_bar_set_range(battery_bar, 0, 100);
    lv_bar_set_value(battery_bar, initial_percentage, LV_ANIM_OFF);
    
    // 设置电量条样式 - 白色填充，透明背景
    lv_obj_set_style_bg_color(battery_bar, lv_color_hex(0x000000), LV_PART_MAIN);  // 背景透明
    lv_obj_set_style_bg_opa(battery_bar, 0, LV_PART_MAIN);
    lv_obj_set_style_bg_color(battery_bar, lv_color_hex(0xFFFFFF), LV_PART_INDICATOR);  // 白色填充
    lv_obj_set_style_radius(battery_bar, 12, LV_PART_MAIN);
    lv_obj_set_style_radius(battery_bar, 12, LV_PART_INDICATOR);
    
    // 电量百分比标签 - 显示在电量条中央
    battery_label = lv_label_create(scr);
    lv_label_set_text_fmt(battery_label, "%d%%", initial_percentage);
    lv_obj_set_style_text_color(battery_label, lv_color_hex(0x000000), 0);  // 黑色字体，在白色条上可见
    lv_obj_set_style_text_font(battery_label, &lv_font_montserrat_16, 0);
    lv_obj_align(battery_label, LV_ALIGN_CENTER, 0, -5);
    
    // // 信息标签 - 显示控制模式
    // info_label = lv_label_create(scr);
    // if (battery_display_override) {
    //     lv_label_set_text_fmt(info_label, "JSON Mode | %d%%", initial_percentage);
    //     lv_obj_set_style_text_color(info_label, lv_color_hex(0x00FF00), 0);  // 绿色表示外部控制
    // } else {
    //     lv_label_set_text(info_label, "Auto Mode");
    //     lv_obj_set_style_text_color(info_label, lv_color_hex(0x888888), 0);  // 灰色表示自动模式
    // }
    // lv_obj_set_style_text_font(info_label, &lv_font_montserrat_12, 0);
    // lv_obj_align(info_label, LV_ALIGN_BOTTOM_MID, 0, -15);

    // 电池读取模式标签 - 屏幕左上角，BOOT 键切换
    mode_label = lv_label_create(scr);
    lv_label_set_text_fmt(mode_label, "MODE: %s", battery_read_mode_name(g_battery_read_mode));
    lv_obj_set_style_text_color(mode_label, lv_color_hex(0xAAAAAA), 0);
    lv_obj_set_style_text_font(mode_label, &lv_font_montserrat_14, 0);
    lv_obj_align(mode_label, LV_ALIGN_TOP_LEFT, 8, 6);

    ESP_LOGI(TAG, "Simplified battery UI created successfully (428x142) with %d%%", initial_percentage);
}

/**
 * @brief 更新电池UI显示
 */
static void update_battery_ui(void)
{
    // 始终刷新模式标签（BOOT 键切换后立即可见）
    if (mode_label) {
        lv_label_set_text_fmt(mode_label, "MODE: %s", battery_read_mode_name(g_battery_read_mode));
        lv_obj_invalidate(mode_label);
    }

    // ---- 信号丢失优先：显示红色闪烁的报警画面 ----
    if (is_battery_signal_lost()) {
        bool blink_on = g_no_signal_blink_on;
        if (battery_bar) {
            // 进度条满格红色 / 熄灭 交替，形成闪烁
            lv_bar_set_value(battery_bar, 100, LV_ANIM_OFF);
            lv_obj_set_style_bg_color(battery_bar,
                blink_on ? lv_color_hex(0xFF0000) : lv_color_hex(0x000000),
                LV_PART_INDICATOR);
        }
        if (battery_label) {
            lv_label_set_text(battery_label, blink_on ? "NO SIGNAL" : "");
            lv_obj_set_style_text_color(battery_label, lv_color_hex(0xFFFFFF), 0);
            lv_obj_invalidate(battery_label);
        }
        if (info_label) {
            lv_label_set_text(info_label, "Battery data lost - check UART/host");
            lv_obj_set_style_text_color(info_label, lv_color_hex(0xFF0000), 0);
            lv_obj_invalidate(info_label);
        }
        return;
    }

    // 获取要显示的电量值和充电状态
    int base_percentage = external_battery_value;  // 始终使用external_battery_value
    // 应用充电动画效果
    int display_percentage = get_animated_battery_percentage(base_percentage);
    
    // 确保数值在有效范围内
    if (display_percentage < 0) display_percentage = 0;
    if (display_percentage > 100) display_percentage = 100;
    
    // 更新进度条
    if (battery_bar) {
        lv_bar_set_value(battery_bar, display_percentage, LV_ANIM_OFF);
        
        // 统一充电判断逻辑：被动控制 OR RS485检测到充电
        bool is_charging_display = (charging_status_override && external_charging_status);

        if (g_battery_data.data_valid && g_battery_data.pack_current > 0.05f) {
            is_charging_display = true;
        }

        lv_color_t bar_color;
        if (is_charging_display) {
            // 充电显示模式 - 使用统一的充电颜色（动画通过进度条长度实现）
            if (display_percentage >= BATTERY_FULL_THRESHOLD) {
                // 充满电时使用绿色
                bar_color = lv_color_hex(0x00FF00);
            } else {
                // 充电中可以选择以下颜色之一：
                bar_color = lv_color_hex(0x00CED1);  // 深青色 - 科技感强
                // bar_color = lv_color_hex(0x1E90FF);  // 道奇蓝 - 活力感
                // bar_color = lv_color_hex(0x32CD32);  // 酸橙绿 - 充电活跃感
                // bar_color = lv_color_hex(0xFF6B35);  // 活力橙 - 温暖充电感
                // bar_color = lv_color_hex(0x00BFFF);  // 深天蓝 - 清新感
            }
        } else {
            // 普通电量显示模式 - 根据电量设置合适的颜色
            if (display_percentage >= BATTERY_FULL_THRESHOLD) {
                bar_color = lv_color_hex(0x00FF00);  // 充满电绿色
            } else if (display_percentage < 20) {
                bar_color = lv_color_hex(0xFF0000);  // 低电量红色，确保白字可见
            } else {
                bar_color = lv_color_hex(0xFFFFFF);  // 正常白色，保持简约
            }
        }
        
        lv_obj_set_style_bg_color(battery_bar, bar_color, LV_PART_INDICATOR);
    }
    
    if (battery_label) {
        // 统一充电判断逻辑：被动控制 OR RS485检测到充电
        bool is_charging_display = (charging_status_override && external_charging_status);
        if (g_battery_data.data_valid && g_battery_data.pack_current > 0.05f) {
            is_charging_display = true;
        }

        if (is_charging_display) {
            // 充电显示（动画和字体统一）
            lv_label_set_text_fmt(battery_label, "%d%% [charging]", display_percentage);
            lv_obj_set_style_text_color(battery_label, lv_color_hex(0x808080), 0);  // 灰色
        } else {
            // 正常显示 - 使用字符串缓冲区格式化电压
            char voltage_str[32];
            snprintf(voltage_str, sizeof(voltage_str), "%d%% %.1fV", display_percentage, (double)get_battery_voltage());
            lv_label_set_text(battery_label, voltage_str);
            lv_obj_set_style_text_color(battery_label, lv_color_hex(0x808080), 0);  // 灰色
        }
        
        lv_obj_invalidate(battery_label);
    }
    
    if (info_label) {
        // 根据控制指令区分显示模式信息
        bool is_charging_display = charging_status_override && external_charging_status;

        if (g_battery_data.data_valid) {
            // 显示RS485电池信息，根据电流正负显示状态
            char info_text[128];
            char status_text[32];
            
            // 根据电流判断状态
            if (g_battery_data.pack_current > 0.05f) {
                snprintf(status_text, sizeof(status_text), "充电中");
                lv_obj_set_style_text_color(info_label, lv_color_hex(0xFFAA00), 0);  // 蓝色表示充电
            } else if (g_battery_data.pack_current < -0.05f) {
                snprintf(status_text, sizeof(status_text), "放电中");
                lv_obj_set_style_text_color(info_label, lv_color_hex(0x888888), 0);  // 橙色表示放电
            } else {
                snprintf(status_text, sizeof(status_text), "静置");
                lv_obj_set_style_text_color(info_label, lv_color_hex(0x888888), 0);  // 灰色表示静置
            }
            
            if (is_charging_display) {
                // 手动充电显示模式
                snprintf(info_text, sizeof(info_text), "RS485: %.3fA %s %d串 %.1f℃", 
                        g_battery_data.pack_current, status_text, g_battery_data.battery_strings,
                        g_battery_data.temp_count > 0 ? g_battery_data.temperatures[0] : 0.0f);
            } else {
                // 普通显示模式
                snprintf(info_text, sizeof(info_text), "RS485: %.3fA %s %d串 %.1f℃ %dmAh", 
                        g_battery_data.pack_current, status_text, g_battery_data.battery_strings,
                        g_battery_data.temp_count > 0 ? g_battery_data.temperatures[0] : 0.0f,
                        g_battery_data.remain_capacity * 10);
            }
            lv_label_set_text(info_label, info_text);
        } else {
            // RS485数据无效时的显示
            if (battery_display_override || charging_status_override) {
                // 外部控制模式
                char mode_text[128];
                if (is_charging_display) {
                    snprintf(mode_text, sizeof(mode_text), "手动充电模式 | 电量:%d%% | 动画:%s", 
                            display_percentage, charging_animation_enabled ? "开" : "关");
                    lv_obj_set_style_text_color(info_label, lv_color_hex(0xFFAA00), 0);  // 橙色表示充电模式
                } else {
                    snprintf(mode_text, sizeof(mode_text), "手动模式 | 电量:%d%% | RS485断开", display_percentage);
                    lv_obj_set_style_text_color(info_label, lv_color_hex(0x888888), 0);  // 灰色表示普通模式
                }
                lv_label_set_text(info_label, mode_text);
            } else {
                // 完全自动模式
                lv_label_set_text(info_label, "自动模式 | RS485通信断开 | JSON控制可用");
                lv_obj_set_style_text_color(info_label, lv_color_hex(0xFF0000), 0);  // 红色表示通信异常
            }
        }
        
        // 强制重新绘制标签
        lv_obj_invalidate(info_label);
    }
    
    // ESP_LOGI(TAG, "UI update completed");  // 减少日志输出
}



void set_external_battery_level(int level)
{
    if (level >= 0 && level <= 100) {
        // 标记收到外部数据 (即使值未变化也表示通信正常)
        mark_battery_signal_alive();

        // 检查参数是否真正发生变化
        bool battery_changed = (external_battery_value != level);
        bool override_changed = !battery_display_override;
        
        if (battery_changed || override_changed) {
            external_battery_value = level;
            battery_display_override = true;
            
            // 通知LVGL任务立即处理UI更新
            notify_ui_update_needed();
            
            // 只有参数真正改变时才保存到NVS
            schedule_nvs_save();
            
            // 异步更新WS2812电量显示
            ws2812_update_battery_display(external_battery_value, external_charging_status);
        }
        // 如果参数没有变化，跳过所有操作，提高性能
    } else {
        ESP_LOGW(TAG, "Invalid battery level: %d (must be 0-100)", level);
    }
}

/**
 * @brief 设置外部充电状态
 */
void set_external_charging_status(bool charging)
{
    // 标记收到外部数据
    mark_battery_signal_alive();

    // 检查参数是否真正发生变化
    bool charging_changed = (external_charging_status != charging);
    bool override_changed = !charging_status_override;
    
    if (charging_changed || override_changed) {
        external_charging_status = charging;
        charging_status_override = true;
        
        // 根据充电显示模式控制动画（只有在充电显示模式下才启用动画）
        bool is_charging_display = charging_status_override && external_charging_status;
        if (is_charging_display) {
            start_charging_animation();
        } else {
            stop_charging_animation();
        }
        
        // 通知LVGL任务立即处理UI更新
        notify_ui_update_needed();
        
        // 只有参数真正改变时才保存到NVS
        schedule_nvs_save();
        
        // 异步更新WS2812电量显示
        ws2812_update_battery_display(external_battery_value, external_charging_status);
    }
    // 如果参数没有变化，跳过所有操作
}

/**
 * @brief 恢复自动充电状态模式
 */
void restore_auto_charging_mode(void)
{
    // 只有在当前为手动模式时才进行切换
    if (charging_status_override) {
        charging_status_override = false;
        
        // 恢复自动模式后，不再是充电显示模式，停止动画
        stop_charging_animation();
        
        // 通知LVGL任务立即处理UI更新
        notify_ui_update_needed();
        
        // 只有真正改变时才保存到NVS
        schedule_nvs_save();
    }
    // 如果已经是自动模式，跳过所有操作
}

/**
 * @brief 获取电池详细信息 (RS485)
 */
char* get_battery_detailed_info(void)
{
    cJSON *json = cJSON_CreateObject();
    if (!json) {
        return NULL;
    }
    
    // 基本状态信息
    cJSON_AddNumberToObject(json, "voltage", get_battery_voltage());
    cJSON_AddNumberToObject(json, "percentage", get_battery_percentage());
    cJSON_AddBoolToObject(json, "charging", get_effective_charging_status());
    
    // 模式信息
    cJSON_AddBoolToObject(json, "rs485_enabled", true);
    cJSON_AddBoolToObject(json, "json_enabled", ENABLE_JSON_PASSIVE_MODE);

    // RS485通信状态
    cJSON_AddBoolToObject(json, "rs485_connected", g_battery_data.data_valid);
    cJSON_AddBoolToObject(json, "auto_query", g_battery_auto_query);
    
    if (g_battery_data.data_valid) {
        // 详细的RS485电池数据
        cJSON *battery_detail = cJSON_CreateObject();
        cJSON_AddNumberToObject(battery_detail, "pack_voltage", g_battery_data.pack_voltage);
        cJSON_AddNumberToObject(battery_detail, "pack_current", g_battery_data.pack_current);
        cJSON_AddNumberToObject(battery_detail, "soc", g_battery_data.soc);
        cJSON_AddNumberToObject(battery_detail, "remain_capacity_mah", g_battery_data.remain_capacity * 10);
        cJSON_AddNumberToObject(battery_detail, "full_capacity_mah", g_battery_data.full_capacity * 10);
        cJSON_AddNumberToObject(battery_detail, "battery_strings", g_battery_data.battery_strings);
        cJSON_AddNumberToObject(battery_detail, "protect_status", g_battery_data.protect_status);
        cJSON_AddNumberToObject(battery_detail, "fet_status", g_battery_data.fet_status);
        
        // 温度信息
        if (g_battery_data.temp_count > 0) {
            cJSON *temperatures = cJSON_CreateArray();
            for (int i = 0; i < g_battery_data.temp_count && i < 8; i++) {
                cJSON_AddItemToArray(temperatures, cJSON_CreateNumber(g_battery_data.temperatures[i]));
            }
            cJSON_AddItemToObject(battery_detail, "temperatures", temperatures);
        }
        
        cJSON_AddItemToObject(json, "rs485_data", battery_detail);
    }

    char *json_string = cJSON_Print(json);
    cJSON_Delete(json);
    
    return json_string;
}

/**
 * @brief 启用/禁用电池自动查询
 */
void set_battery_auto_query(bool enable)
{
    g_battery_auto_query = enable;
    ESP_LOGI(TAG, "Battery auto query %s", enable ? "enabled" : "disabled");
}

/**
 * @brief 手动触发电池查询
 */
bool trigger_battery_query(void)
{
    ESP_LOGI(TAG, "Manual battery query triggered");

    if (query_battery_info()) {
        update_battery_status_from_rs485();
        ESP_LOGI(TAG, "Manual battery query successful");
        return true;
    } else {
        ESP_LOGW(TAG, "Manual battery query failed");
        return false;
    }
}

/**
 * @brief 恢复自动电池显示模式
 */
void restore_auto_battery_mode(void)
{
    // 只有在当前为手动模式时才进行切换
    if (battery_display_override) {
        battery_display_override = false;
        
        // 通知LVGL任务立即处理UI更新
        notify_ui_update_needed();
        
        // 只有真正改变时才保存到NVS
        schedule_nvs_save();
    }
    // 如果已经是自动模式，跳过所有操作
}

/**
 * @brief 设置外部电池电量百分比（新函数名）
 */
void set_external_battery_percentage(int percentage)
{
    // 调用现有的函数，保持兼容性
    set_external_battery_level(percentage);
}

/**
 * @brief 获取当前电池电压
 */
float get_battery_voltage(void)
{
    // 如果有外部设置的电压值，优先返回外部值
    if (voltage_override) {
        return external_voltage_value;
    }
    return g_battery_voltage;
}

/**
 * @brief 获取当前电池电量百分比
 */
int get_battery_percentage(void)
{
    // 如果有外部设置的电量值，优先返回外部值
    if (battery_display_override) {
        return external_battery_value;
    }
    // 当恢复自动模式时，返回最后一次设置的外部值作为当前显示
    // 这比返回一个可能过时的g_battery_percentage更合理
    return external_battery_value;
}

/**
 * @brief 获取当前充电状态
 */
bool is_charging(void)
{
    return get_effective_charging_status();
}

/**
 * @brief 设置外部电压（仅记录电压值，不影响电量百分比显示）
 */
void set_external_voltage(float voltage)
{
    // 标记收到外部数据
    mark_battery_signal_alive();

    // 检查电压值是否真正发生变化（使用小的容差避免浮点精度问题）
    bool voltage_changed = (fabs(external_voltage_value - voltage) > 0.01f);
    bool override_changed = !voltage_override;
    
    if (voltage_changed || override_changed) {
        external_voltage_value = voltage;
        voltage_override = true;
        
        // 只有参数真正改变时才保存到NVS
        schedule_nvs_save();
    }
    // 如果电压值没有明显变化，跳过NVS保存
}

/**
 * @brief 恢复自动电压模式
 */
void restore_auto_voltage_mode(void)
{
    // 只有在当前为手动模式时才进行切换
    if (voltage_override) {
        voltage_override = false;
        
        // 将刷新交由 LVGL 任务，避免跨任务调用
        ui_update_pending = true;
        
        // 只有真正改变时才保存到NVS
        schedule_nvs_save();
    }
    // 如果已经是自动模式，跳过所有操作
}

/**
 * @brief LVGL时基回调函数
 */
static void increase_lvgl_tick(void *arg)
{
    lv_tick_inc(1);
}

/**
 * @brief 电池监控任务
 */
static void battery_monitor_task(void *pvParameters)
{
    ESP_LOGI(TAG, "Battery monitor task started (RS485 Auto Query Mode)");

    TickType_t last_wake_time = xTaskGetTickCount();
    TickType_t last_query_time = 0;

    while (1) {
        // RS485电池查询（如果启用自动查询）
        if (g_battery_auto_query) {
            TickType_t current_time = xTaskGetTickCount();
            if (current_time - last_query_time >= pdMS_TO_TICKS(BATTERY_QUERY_PERIOD)) {
                ESP_LOGD(TAG, "Querying battery info via RS485...");
                
                if (query_battery_info()) {
                    update_battery_status_from_rs485();
                    ESP_LOGD(TAG, "Battery query successful");
                } else {
                    ESP_LOGW(TAG, "Battery query failed");
                    // 查询失败时标记数据无效
                    if (xTaskGetTickCount() - g_battery_data.last_update > pdMS_TO_TICKS(10000)) {
                        g_battery_data.data_valid = false;
                        ESP_LOGW(TAG, "Battery data expired, marked as invalid");
                    }
                }
                
                last_query_time = current_time;
            }
        }
        
        // 备用电压模拟（当RS485数据无效时）
        if (!g_battery_data.data_valid) {
            if (voltage_override) {
                // 使用外部设置的电压值
                g_battery_voltage = external_voltage_value;
            } else {
                // 使用模拟电压 (在实际应用中，这里应该是从ADC读取)
                g_battery_voltage = simulate_battery_voltage();
            }
        }

        // 等待下一次更新
        vTaskDelayUntil(&last_wake_time, pdMS_TO_TICKS(BATTERY_UPDATE_PERIOD));
    }
}

/**
 * @brief UART命令处理任务
 */
static void uart_command_task(void *pvParameters)
{
#if ENABLE_JSON_PASSIVE_MODE
    ESP_LOGI(TAG, "UART command task started (JSON Control Mode)");
#else
    ESP_LOGI(TAG, "UART command task started (RS485 Only Mode)");
#endif
    
    uint8_t data[UART_BUF_SIZE];
    
#if ENABLE_JSON_PASSIVE_MODE
    char command_buffer[1024]; // 增加缓冲区大小以支持较长的JSON
    int command_pos = 0;
    int json_brace_count = 0;
    bool in_json = false;
    TickType_t parse_last_activity = xTaskGetTickCount();
    const TickType_t PARSE_TIMEOUT_TICKS = pdMS_TO_TICKS(2000); // 解析空闲超时 2s
#endif
    // RX 持续无数据计数，用于探测并恢复驱动异常
    uint32_t idle_cycles = 0;
    
    while (1) {
        // RS485 / RS485-2 主动查询模式下，UART 由查询任务独占，避免并发读导致数据被瓜分
        if (g_battery_read_mode != BATT_READ_MODE_JSON) {
#if ENABLE_JSON_PASSIVE_MODE
            in_json = false;
            command_pos = 0;
            json_brace_count = 0;
            parse_last_activity = xTaskGetTickCount();
#endif
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        int len = uart_read_bytes(UART_NUM_1, data, UART_BUF_SIZE - 1, pdMS_TO_TICKS(100));

        if (len < 0) {
            // 驱动错误（FIFO 溢出/buffer full 等）-> 刷新 RX 恢复
            ESP_LOGW(TAG, "uart_read_bytes error %d, flushing UART1", len);
            uart_flush_input(UART_NUM_1);
#if ENABLE_JSON_PASSIVE_MODE
            in_json = false;
            command_pos = 0;
            json_brace_count = 0;
            parse_last_activity = xTaskGetTickCount();
#endif
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }

        if (len > 0) {
            idle_cycles = 0;
#if ENABLE_JSON_PASSIVE_MODE
            data[len] = '\0';
            parse_last_activity = xTaskGetTickCount();
            
            for (int i = 0; i < len; i++) {
                char c = (char)data[i];
                
                // 处理JSON检测逻辑
                if (c == '{') {
                    if (!in_json) {
                        // 开始新的JSON命令
                        in_json = true;
                        json_brace_count = 1;
                        command_pos = 0;
                        command_buffer[command_pos++] = c;
                    } else {
                        // JSON内部的嵌套大括号
                        json_brace_count++;
                        if (command_pos < sizeof(command_buffer) - 1) {
                            command_buffer[command_pos++] = c;
                        }
                    }
                } else if (c == '}' && in_json) {
                    if (command_pos < sizeof(command_buffer) - 1) {
                        command_buffer[command_pos++] = c;
                    }
                    json_brace_count--;
                    
                    if (json_brace_count <= 0) {
                        // 完整的JSON命令接收完毕
                        command_buffer[command_pos] = '\0';

                        // 投递到后台处理队列（非阻塞），保持 UART 任务流畅
                        if (uart_command_queue != NULL) {
                            uart_command_t qcmd;
                            // 加 "JSON:" 前缀交给 background_processing_task 处理
                            int prefix_len = 5;
                            int json_len = command_pos;
                            if (json_len > (int)sizeof(qcmd.command) - prefix_len - 1) {
                                json_len = sizeof(qcmd.command) - prefix_len - 1;
                            }
                            memcpy(qcmd.command, "JSON:", prefix_len);
                            memcpy(qcmd.command + prefix_len, command_buffer, json_len);
                            qcmd.command[prefix_len + json_len] = '\0';
                            qcmd.length = prefix_len + json_len;
                            if (xQueueSend(uart_command_queue, &qcmd, 0) != pdTRUE) {
                                ESP_LOGW(TAG, "UART cmd queue full, JSON dropped");
                            }
                        }

                        // 重置状态
                        in_json = false;
                        command_pos = 0;
                        json_brace_count = 0;
                    }
                } else if (in_json) {
                    // JSON内部的其他字符
                    if (command_pos < sizeof(command_buffer) - 1) {
                        command_buffer[command_pos++] = c;
                    } else {
                        // 缓冲区溢出保护 - 重置JSON解析状态
                        ESP_LOGW(TAG, "JSON command buffer overflow, resetting");
                        in_json = false;
                        command_pos = 0;
                        json_brace_count = 0;
                    }
                } else {
                    // 非JSON命令处理（传统命令）
                    if (c == '\r' || c == '\n') {
                        if (command_pos > 0) {
                            command_buffer[command_pos] = '\0';
                            
                            // 其他命令处理...
                            
                            command_pos = 0;
                        }
                    } else if (command_pos < sizeof(command_buffer) - 1) {
                        command_buffer[command_pos++] = c;
                    } else {
                        // 非 JSON 行过长，丢弃并复位，防止残留
                        ESP_LOGW(TAG, "Line buffer overflow, resetting");
                        command_pos = 0;
                    }
                }
            }
#else
            // 仅RS485模式 - 忽略接收到的数据或记录日志
            ESP_LOGD(TAG, "Received %d bytes (RS485 mode, ignoring JSON commands)", len);
#endif
        } else {
            // len == 0: 超时无数据
            idle_cycles++;
#if ENABLE_JSON_PASSIVE_MODE
            // 若解析状态挂起过久(例如收到半个 JSON 就断开)，复位避免"卡死"
            if ((in_json || command_pos > 0) &&
                (xTaskGetTickCount() - parse_last_activity) > PARSE_TIMEOUT_TICKS) {
                ESP_LOGW(TAG, "UART1 parse state stale (%dms), resetting",
                         (int)((xTaskGetTickCount() - parse_last_activity) * portTICK_PERIOD_MS));
                in_json = false;
                command_pos = 0;
                json_brace_count = 0;
                parse_last_activity = xTaskGetTickCount();
            }
#endif
            // 长时间无数据 -> 周期性刷新驱动 RX 以兜底异常状态(约每 30s)
            if (idle_cycles >= 300) {
                idle_cycles = 0;
                size_t buffered = 0;
                if (uart_get_buffered_data_len(UART_NUM_1, &buffered) == ESP_OK && buffered > 0) {
                    ESP_LOGW(TAG, "UART1 idle but %u bytes stuck in buffer, flushing", (unsigned)buffered);
                    uart_flush_input(UART_NUM_1);
                }
            }
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

/**
 * @brief LVGL显示刷新任务
 */
static void lvgl_tick_task(void *pvParameters)
{
    while (1) {
    lv_tick_inc(5);
    vTaskDelay(pdMS_TO_TICKS(5));
    }
}

/**
 * @brief LVGL任务处理
 */
static void lvgl_task(void *pvParameters)
{   uint16_t cnt = 0;
    while (1) {
        // 处理LVGL定时器
        lv_timer_handler();
        
        // 检查是否有待处理的UI更新
        if (ui_update_pending) {
            update_battery_ui();
            ui_update_pending = false;
        }
        
        // 每10次处理一次，避免过于频繁
        cnt++;
        if (cnt >= 10) {
            cnt = 0;
            LED_TOGGLE();
        }
        
        // 等待任务通知或超时，实现更快的响应
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(10));  // 等待通知或10ms超时
    }
}

/**
 * @brief       程序入口
 * @param       无
 * @retval      无
 */
void app_main(void)
{
    ESP_LOGI(TAG, "ESP32-S3 Battery Monitor Starting");
    ESP_LOGI(TAG, "Display Resolution: %dx%d pixels (Landscape)", LCD_W, LCD_H);
    
    // 初始化NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // 初始化WS2812灯带
    ESP_ERROR_CHECK(ws2812_init());

    // 初始化I2S麦克风
    ESP_ERROR_CHECK(i2s_mic_init());

    // 创建Web服务器任务
    httpd_handle_t web_server_handle = web_server_init();
    if (web_server_handle == NULL) {
        ESP_LOGE(TAG, "Failed to initialize web server");
    } else {
        ESP_LOGI(TAG, "Web server initialized successfully");
    }

    // 初始化高性能UART处理
    uart_command_queue = xQueueCreate(UART_QUEUE_SIZE, sizeof(uart_command_t));
    if (uart_command_queue == NULL) {
        ESP_LOGE(TAG, "Failed to create UART command queue");
    } else {
        ESP_LOGI(TAG, "UART command queue created (size: %d)", UART_QUEUE_SIZE);
    }
    
    // 创建NVS延时保存定时器 (已停用，不再创建)
    // const esp_timer_create_args_t nvs_timer_args = {
    //     .callback = &nvs_save_timer_callback,
    //     .name = "nvs_save"
    // };
    // ESP_ERROR_CHECK(esp_timer_create(&nvs_timer_args, &nvs_save_timer));

    // 创建"无信号"红色闪烁定时器 (持续运行，内部按信号状态决定是否驱动)
    const esp_timer_create_args_t blink_timer_args = {
        .callback = &no_signal_blink_timer_callback,
        .name = "nosig_blink"
    };
    ESP_ERROR_CHECK(esp_timer_create(&blink_timer_args, &no_signal_blink_timer));
    ESP_ERROR_CHECK(esp_timer_start_periodic(no_signal_blink_timer,
                                             BATTERY_NO_SIGNAL_BLINK_MS * 1000));
    
    // 从 NVS 恢复上次的电池读取模式 (默认 JSON)
    g_battery_read_mode = load_battery_read_mode_from_nvs();
    ESP_LOGI(TAG, "Battery read mode restored: %s",
             battery_read_mode_name(g_battery_read_mode));

    // 初始化UART1 (统一串口)
    uart1_init();

    // 按恢复的模式设置 UART 波特率 (uart1_init 使用编译时默认波特率)
    {
        uint32_t baud = 115200;
        if (g_battery_read_mode == BATT_READ_MODE_RS485_1 ||
            g_battery_read_mode == BATT_READ_MODE_RS485_2) baud = 9600;
        uart_set_baudrate(UART_NUM_1, baud);
        uart_flush_input(UART_NUM_1);
    }

    // 初始化LED
    led_init();

    // 初始化LVGL
    lv_init();
    
    // 初始化LVGL显示驱动（内部会初始化LCD）
    lv_port_disp_init();
    ESP_LOGI(TAG, "LVGL display driver initialized");
    
    // 设置LVGL时基
    const esp_timer_create_args_t lvgl_tick_timer_args = {
        .callback = &increase_lvgl_tick,
        .name = "lvgl_tick"
    };
    esp_timer_handle_t lvgl_tick_timer = NULL;
    ESP_ERROR_CHECK(esp_timer_create(&lvgl_tick_timer_args, &lvgl_tick_timer));
    // 使用 5ms LVGL tick，提高与任务处理的配合，同时避免 tick 过于频繁
    ESP_ERROR_CHECK(esp_timer_start_periodic(lvgl_tick_timer, 5 * 1000));
    
    // 创建电池监控UI界面
    create_battery_ui();
    
    // 将首次刷新交给 LVGL 任务，避免在主任务中进行绘制导致栈溢出
    ui_update_pending = true;
    ESP_LOGI(TAG, "Queued initial UI update to LVGL task");
    
    // 创建LVGL相关任务 - 增加栈大小
    xTaskCreate(lvgl_tick_task, "lvgl_tick", 3072, NULL, 4, NULL);  // 增加栈大小
    xTaskCreate(lvgl_task, "lvgl", 6144, NULL, 3, &lvgl_task_handle);  // 增加栈大小并保存句柄
    
    // 创建电池监控任务 - 可禁用以避免WDT问题
#if ENABLE_BATTERY_MONITOR_TASK
    TaskHandle_t battery_task_handle = NULL;
    xTaskCreate(battery_monitor_task, "battery_monitor", 6144, NULL, 2, &battery_task_handle);  // 增加栈大小
    // 将电池监控任务添加到watchdog监控中
    if (battery_task_handle != NULL) {
        esp_task_wdt_add(battery_task_handle);
        ESP_LOGI(TAG, "Battery monitor task added to watchdog");
    }
#else
    ESP_LOGW(TAG, "Battery monitor task disabled by config (ENABLE_BATTERY_MONITOR_TASK=0)");
#endif
    
    // 创建UART命令处理任务（快速响应版）
    xTaskCreate(uart_command_task, "uart_command", 6144, NULL, 4, NULL);  // 提高优先级以实现快速响应
    
    // 创建后台处理任务
    if (uart_command_queue != NULL) {
        xTaskCreate(background_processing_task, "background", 4096, NULL, 1, NULL);  // 较低优先级的后台处理
        ESP_LOGI(TAG, "Background processing task created");
    }
    
    // 创建WS2812控制任务
    xTaskCreate(ws2812_task, "ws2812", 4096, NULL, 2, NULL);

    // 创建 BOOT 按键任务 (切换电池读取模式)
    xTaskCreate(boot_button_task, "boot_btn", 2560, NULL, 3, NULL);

    // 创建 RS485-1 主动查询任务 (仅在 RS485-1 模式下工作)
    xTaskCreate(rs485_1_query_task, "rs485_1", 3584, NULL, 3, NULL);

    // 创建 RS485-2 主动查询任务 (仅在 RS485-2 模式下工作)
    xTaskCreate(rs485_2_query_task, "rs485_2", 3072, NULL, 3, NULL);
    
    ESP_LOGI(TAG, "All tasks created successfully");
#if ENABLE_JSON_PASSIVE_MODE
    ESP_LOGI(TAG, "Battery Monitor System Ready! (RS485 + JSON Hybrid Mode, runtime selectable)");
#else
    ESP_LOGI(TAG, "Battery Monitor System Ready! (RS485 Mode, runtime selectable)");
#endif
    ESP_LOGI(TAG, "UART1 Communication: TX Pin=%d, RX Pin=%d, Baud=%d", 
             UART1_TXD_PIN, UART1_RXD_PIN, UART1_BAUD_RATE);
    ESP_LOGI(TAG, "RS485 Mode: Standard UART communication");
    ESP_LOGI(TAG, "WS2812 Multi-Channel System: %d channels on GPIO 18-21, configurable LEDs per channel", 
             WS2812_CHANNEL_COUNT);
    ESP_LOGI(TAG, "Battery Auto Query: %s (Period: %dms, Timeout: %dms)", 
             g_battery_auto_query ? "Enabled" : "Disabled", BATTERY_QUERY_PERIOD, BATTERY_TIMEOUT_MS);
    ESP_LOGI(TAG, "Charging Animation: %s (Period: %dms, Full Battery Threshold: %d%%)", 
             charging_animation_enabled ? "Enabled" : "Disabled", CHARGING_ANIM_PERIOD, BATTERY_FULL_THRESHOLD);
    ESP_LOGI(TAG, "Command formats:");
#if ENABLE_JSON_PASSIVE_MODE
    ESP_LOGI(TAG, "  Battery Query: BATTERY or BATTERY:JSON or {\"query\": \"battery\"}");
    ESP_LOGI(TAG, "  Battery Control: {\"battery\": 75} or BATTERY:75");
    ESP_LOGI(TAG, "  Battery Detail: {\"query\": \"battery_detail\"}");
    ESP_LOGI(TAG, "  Manual Query: {\"trigger_battery_query\": true}");
    ESP_LOGI(TAG, "  Auto Query Control: {\"battery_auto_query\": true/false}");
    ESP_LOGI(TAG, "  Charging Animation: {\"charging_animation\": true/false}");
    ESP_LOGI(TAG, "  Simulate Charging: {\"simulate_charging\": true/false}");
    ESP_LOGI(TAG, "  WS2812 JSON: {\"channel\": 0, \"mode\": 1, \"color\": {\"r\": 255, \"g\": 0, \"b\": 0}}");
    ESP_LOGI(TAG, "  WS2812 Direct: {\"channel\": 255, \"brightness\": 128} (no prefix needed)");
#else
    ESP_LOGI(TAG, "  JSON Control: Disabled (RS485 only mode)");
#endif
}