
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
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

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

// UART1 配置
#define UART_NUM_1          UART_NUM_1
#define UART1_TXD_PIN       16
#define UART1_RXD_PIN       17
#define UART1_BAUD_RATE     115200
#define UART_BUF_SIZE       1024

// 电池电量相关定义
#define BATTERY_MIN_VOLTAGE 3.0f    // 最低电压 (V)
#define BATTERY_MAX_VOLTAGE 4.2f    // 最高电压 (V)
#define BATTERY_UPDATE_PERIOD 1000  // 更新周期 (ms)

// 充电动画相关定义
#define CHARGING_ANIM_PERIOD 600    // 充电动画周期 (ms) - 降低频率
#define CHARGING_ANIM_STEPS 10      // 动画步骤数
#define BATTERY_FULL_THRESHOLD 95   // 电池充满阈值 (%)

// 全局变量
static float g_battery_voltage = 3.7f;  // 当前电池电压
static int g_battery_percentage = 50;   // 当前电池百分比
static bool g_charging_status = false;  // 充电状态

// 外部控制状态
static bool battery_display_override = false;  // 外部控制电量显示标志
static int external_battery_value = 50;        // 外部设置的电量值
static bool charging_status_override = false;  // 外部控制充电状态标志
static bool external_charging_status = false;  // 外部设置的充电状态

static bool ui_update_pending = false;         // UI更新待处理标志

// 充电动画相关变量
static bool charging_animation_enabled = true;  // 充电动画开关
static uint32_t charging_animation_step = 0;    // 动画步骤计数器
static bool charging_animation_direction = true; // 动画方向（true为增加，false为减少）
static esp_timer_handle_t charging_animation_timer = NULL; // 充电动画定时器

/**
 * @brief 获取有效的充电状态（考虑外部控制）
 */
static bool get_effective_charging_status(void)
{
    return charging_status_override ? external_charging_status : g_charging_status;
}

/**
 * @brief 充电动画定时器回调函数
 */
static void charging_animation_timer_callback(void *arg)
{
    // 只在充电显示模式下启用动画
    bool is_charging_display = charging_status_override && external_charging_status;
    
    if (!charging_animation_enabled || !is_charging_display) {
        return;
    }
    
    // 更新动画步骤
    if (charging_animation_direction) {
        charging_animation_step++;
        if (charging_animation_step >= CHARGING_ANIM_STEPS * 2 - 1) {
            charging_animation_direction = false;
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

// UART发送函数，带调试信息
static void uart_send_response(const char* response) {
    if (response == NULL) {
        ESP_LOGE(TAG, "Null response, skipping send");
        return;
    }
    
    size_t len = strlen(response);
    ESP_LOGI(TAG, "=== UART SEND DEBUG ===");
    ESP_LOGI(TAG, "Sending UART response: %s", response);
    ESP_LOGI(TAG, "Response length: %d", len);
    
    if (len == 0) {
        ESP_LOGE(TAG, "Empty response, skipping send");
        return;
    }
    
    int sent = uart_write_bytes(UART_NUM_1, response, len);
    ESP_LOGI(TAG, "UART write result: %d bytes written", sent);
    
    if (sent < 0) {
        ESP_LOGE(TAG, "UART write error: %d", sent);
        return;
    }
    
    // 发送换行符
    sent = uart_write_bytes(UART_NUM_1, "\r\n", 2);
    ESP_LOGI(TAG, "UART newline write result: %d bytes", sent);
    
    // 等待发送完成
    esp_err_t wait_result = uart_wait_tx_done(UART_NUM_1, pdMS_TO_TICKS(1000));
    if (wait_result == ESP_OK) {
        ESP_LOGI(TAG, "UART transmission completed successfully");
    } else {
        ESP_LOGE(TAG, "UART wait_tx_done failed: %d", wait_result);
    }
    ESP_LOGI(TAG, "=== UART SEND END ===");
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

// LVGL 对象
static lv_obj_t *battery_bar;          // 电池条
static lv_obj_t *battery_label;        // 电池百分比标签
static lv_obj_t *title_label;          // 标题标签
static lv_obj_t *info_label;           // 信息标签

// 函数声明
static void update_battery_ui(void);
static void force_update_battery_ui(void);
void set_external_battery_level(int level);
void set_external_charging_status(bool charging);
void restore_auto_battery_mode(void);
void restore_auto_charging_mode(void);

// LCD控制函数声明
static uint16_t parse_color_string(const char* color_str);
static uint16_t parse_color_json(cJSON* color_obj);
static esp_err_t handle_lcd_command(cJSON* json);

/**
 * @brief UART1 初始化
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

    ESP_ERROR_CHECK(uart_driver_install(UART_NUM_1, UART_BUF_SIZE * 2, 0, 0, NULL, 0));
    ESP_ERROR_CHECK(uart_param_config(UART_NUM_1, &uart_config));
    ESP_ERROR_CHECK(uart_set_pin(UART_NUM_1, UART1_TXD_PIN, UART1_RXD_PIN, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
    
    ESP_LOGI(TAG, "UART1 initialized - TX:%d, RX:%d, Baud:%d", UART1_TXD_PIN, UART1_RXD_PIN, UART1_BAUD_RATE);
}

/**
 * @brief 通过串口发送电池信息
 */
static void send_battery_info_via_uart(void)
{
    char buffer[128];
    
    // 格式：BATTERY:电压,百分比,充电状态
    snprintf(buffer, sizeof(buffer), "BATTERY:%.2fV,%d%%,%s", 
             g_battery_voltage, g_battery_percentage, g_charging_status ? "CHARGING" : "DISCHARGING");
    
    uart_send_response(buffer);
    
    ESP_LOGI(TAG, "Sent via UART1: %s", buffer);
}

/**
 * @brief 解析颜色字符串为RGB565格式
 */
static uint16_t parse_color_string(const char* color_str)
{
    if (!color_str) return LCD_COLOR_WHITE;
    
    if (strcmp(color_str, "red") == 0) return LCD_COLOR_RED;
    if (strcmp(color_str, "green") == 0) return LCD_COLOR_GREEN;
    if (strcmp(color_str, "blue") == 0) return LCD_COLOR_BLUE;
    if (strcmp(color_str, "yellow") == 0) return LCD_COLOR_YELLOW;
    if (strcmp(color_str, "purple") == 0) return LCD_COLOR_PURPLE;
    if (strcmp(color_str, "cyan") == 0) return LCD_COLOR_CYAN;
    if (strcmp(color_str, "white") == 0) return LCD_COLOR_WHITE;
    if (strcmp(color_str, "black") == 0) return LCD_COLOR_BLACK;
    
    return LCD_COLOR_WHITE; // 默认白色
}

/**
 * @brief 解析JSON颜色对象为RGB565格式
 */
static uint16_t parse_color_json(cJSON* color_obj)
{
    if (!color_obj) return LCD_COLOR_WHITE;
    
    // 如果是字符串，使用字符串解析
    if (cJSON_IsString(color_obj)) {
        return parse_color_string(color_obj->valuestring);
    }
    
    // 如果是数字，直接使用
    if (cJSON_IsNumber(color_obj)) {
        return (uint16_t)color_obj->valueint;
    }
    
    // 如果是RGB对象
    if (cJSON_IsObject(color_obj)) {
        cJSON* r = cJSON_GetObjectItem(color_obj, "r");
        cJSON* g = cJSON_GetObjectItem(color_obj, "g");
        cJSON* b = cJSON_GetObjectItem(color_obj, "b");
        
        if (cJSON_IsNumber(r) && cJSON_IsNumber(g) && cJSON_IsNumber(b)) {
            uint8_t red = (uint8_t)r->valueint;
            uint8_t green = (uint8_t)g->valueint;
            uint8_t blue = (uint8_t)b->valueint;
            
            // 转换为RGB565格式
            return ((red >> 3) << 11) | ((green >> 2) << 5) | (blue >> 3);
        }
    }
    
    return LCD_COLOR_WHITE;
}

/**
 * @brief 处理LCD控制命令
 */
static esp_err_t handle_lcd_command(cJSON* json)
{
    cJSON* lcd_item = cJSON_GetObjectItem(json, "lcd");
    if (!lcd_item || !cJSON_IsString(lcd_item)) {
        return ESP_ERR_INVALID_ARG;
    }
    
    const char* command = lcd_item->valuestring;
    ESP_LOGI(TAG, "Processing LCD command: %s", command);
    
    if (strcmp(command, "clear") == 0) {
        // 清屏命令
        cJSON* color_item = cJSON_GetObjectItem(json, "color");
        uint16_t color = color_item ? parse_color_json(color_item) : LCD_COLOR_BLACK;
        LCD_FastFill(color);
        ESP_LOGI(TAG, "LCD cleared with color: 0x%04X", color);
        
    } else if (strcmp(command, "fill") == 0) {
        // 填充命令
        cJSON* color_item = cJSON_GetObjectItem(json, "color");
        uint16_t color = color_item ? parse_color_json(color_item) : LCD_COLOR_WHITE;
        LCD_FastFill(color);
        ESP_LOGI(TAG, "LCD filled with color: 0x%04X", color);
        
    } else if (strcmp(command, "text") == 0) {
        // 文字显示命令
        cJSON* content_item = cJSON_GetObjectItem(json, "content");
        if (!content_item || !cJSON_IsString(content_item)) {
            return ESP_ERR_INVALID_ARG;
        }
        
        // 获取参数
        cJSON* x_item = cJSON_GetObjectItem(json, "x");
        cJSON* y_item = cJSON_GetObjectItem(json, "y");
        cJSON* color_item = cJSON_GetObjectItem(json, "color");
        cJSON* size_item = cJSON_GetObjectItem(json, "size");
        
        uint16_t x = x_item ? (uint16_t)x_item->valueint : 10;
        uint16_t y = y_item ? (uint16_t)y_item->valueint : 10;
        uint16_t color = color_item ? parse_color_json(color_item) : LCD_COLOR_WHITE;
        uint16_t bg_color = LCD_COLOR_BLACK;
        uint8_t size = size_item ? (uint8_t)size_item->valueint : 16;
        
        // 确保字体大小有效
        if (size != 12 && size != 16 && size != 24) size = 16;
        
        LCD_ShowStr(x, y, content_item->valuestring, color, bg_color, size, 0);
        ESP_LOGI(TAG, "LCD text displayed: '%s' at (%d,%d) size:%d color:0x%04X", 
                 content_item->valuestring, x, y, size, color);
        
    } else if (strcmp(command, "rect") == 0) {
        // 矩形绘制命令
        cJSON* x_item = cJSON_GetObjectItem(json, "x");
        cJSON* y_item = cJSON_GetObjectItem(json, "y");
        cJSON* width_item = cJSON_GetObjectItem(json, "width");
        cJSON* height_item = cJSON_GetObjectItem(json, "height");
        cJSON* fill_item = cJSON_GetObjectItem(json, "fill");
        cJSON* color_item = cJSON_GetObjectItem(json, "color");
        
        if (!x_item || !y_item || !width_item || !height_item) {
            return ESP_ERR_INVALID_ARG;
        }
        
        uint16_t x = (uint16_t)x_item->valueint;
        uint16_t y = (uint16_t)y_item->valueint;
        uint16_t width = (uint16_t)width_item->valueint;
        uint16_t height = (uint16_t)height_item->valueint;
        uint16_t color = color_item ? parse_color_json(color_item) : LCD_COLOR_WHITE;
        bool fill = fill_item ? cJSON_IsTrue(fill_item) : false;
        
        if (fill) {
            LCD_DrawFillRectangle(x, y, x + width - 1, y + height - 1, color);
        } else {
            LCD_DrawRectangle(x, y, x + width - 1, y + height - 1, color);
        }
        ESP_LOGI(TAG, "LCD rectangle drawn: (%d,%d) %dx%d %s color:0x%04X", 
                 x, y, width, height, fill ? "filled" : "outline", color);
        
    } else if (strcmp(command, "circle") == 0) {
        // 圆形绘制命令
        cJSON* x_item = cJSON_GetObjectItem(json, "x");
        cJSON* y_item = cJSON_GetObjectItem(json, "y");
        cJSON* radius_item = cJSON_GetObjectItem(json, "radius");
        cJSON* fill_item = cJSON_GetObjectItem(json, "fill");
        cJSON* color_item = cJSON_GetObjectItem(json, "color");
        
        if (!x_item || !y_item || !radius_item) {
            return ESP_ERR_INVALID_ARG;
        }
        
        uint16_t x = (uint16_t)x_item->valueint;
        uint16_t y = (uint16_t)y_item->valueint;
        uint16_t radius = (uint16_t)radius_item->valueint;
        uint16_t color = color_item ? parse_color_json(color_item) : LCD_COLOR_WHITE;
        bool fill = fill_item ? cJSON_IsTrue(fill_item) : false;
        
        LCD_DrawCircle(x, y, radius, color, fill ? 1 : 0);
        ESP_LOGI(TAG, "LCD circle drawn: center(%d,%d) radius:%d %s color:0x%04X", 
                 x, y, radius, fill ? "filled" : "outline", color);
        
    } else if (strcmp(command, "line") == 0) {
        // 线条绘制命令
        cJSON* x1_item = cJSON_GetObjectItem(json, "x1");
        cJSON* y1_item = cJSON_GetObjectItem(json, "y1");
        cJSON* x2_item = cJSON_GetObjectItem(json, "x2");
        cJSON* y2_item = cJSON_GetObjectItem(json, "y2");
        cJSON* color_item = cJSON_GetObjectItem(json, "color");
        
        if (!x1_item || !y1_item || !x2_item || !y2_item) {
            return ESP_ERR_INVALID_ARG;
        }
        
        uint16_t x1 = (uint16_t)x1_item->valueint;
        uint16_t y1 = (uint16_t)y1_item->valueint;
        uint16_t x2 = (uint16_t)x2_item->valueint;
        uint16_t y2 = (uint16_t)y2_item->valueint;
        uint16_t color = color_item ? parse_color_json(color_item) : LCD_COLOR_WHITE;
        
        LCD_DrawLine(x1, y1, x2, y2, color);
        ESP_LOGI(TAG, "LCD line drawn: (%d,%d) to (%d,%d) color:0x%04X", 
                 x1, y1, x2, y2, color);
        
    } else if (strcmp(command, "backlight") == 0) {
        // 背光控制命令
        cJSON* state_item = cJSON_GetObjectItem(json, "state");
        if (!state_item) {
            return ESP_ERR_INVALID_ARG;
        }
        
        bool state = cJSON_IsTrue(state_item);
        if (state) {
            LCD_BLK_Set();
        } else {
            LCD_BLK_Clr();
        }
        ESP_LOGI(TAG, "LCD backlight %s", state ? "ON" : "OFF");
        
    } else {
        ESP_LOGW(TAG, "Unknown LCD command: %s", command);
        return ESP_ERR_INVALID_ARG;
    }
    
    return ESP_OK;
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

    ret = nvs_set_u8(nvs_handle, NVS_KEY_PERCENTAGE, (uint8_t)g_battery_percentage);
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
        g_battery_voltage = 3.7f; // 默认值
    }

    // 加载电池百分比
    ret = nvs_get_u8(nvs_handle, NVS_KEY_PERCENTAGE, &temp_u8);
    if (ret == ESP_OK) {
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

    nvs_close(nvs_handle);
    
    ESP_LOGI(TAG, "Battery state loaded from NVS: Override=%s, ExtValue=%d%%, ChargingOverride=%s, ExtCharging=%s, Voltage=%.2fV, Percentage=%d%%, Charging=%s",
             battery_display_override ? "true" : "false",
             external_battery_value,
             charging_status_override ? "true" : "false",
             external_charging_status ? "true" : "false",
             g_battery_voltage,
             g_battery_percentage,
             g_charging_status ? "true" : "false");
    
    return ESP_OK;
}

/**
 * @brief 生成电池状态JSON字符串
 */
static char* create_battery_status_json(void)
{
    cJSON *json = cJSON_CreateObject();
    if (json == NULL) {
        return NULL;
    }

    // 直接添加电池基本信息到根级别（取消battery对象）
    cJSON_AddNumberToObject(json, "voltage", (double)g_battery_voltage);
    cJSON_AddNumberToObject(json, "percentage", g_battery_percentage);
    
    // 获取有效的充电状态
    bool effective_charging = get_effective_charging_status();
    cJSON_AddStringToObject(json, "charging_status", effective_charging ? "CHARGING" : "DISCHARGING");
    
    // 添加电量和充电状态控制信息
    int display_percentage = battery_display_override ? external_battery_value : g_battery_percentage;
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
    // 注意：上面已将 display 添加到 json，避免重复添加

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
    static float voltage = 3.7f;
    static float direction = 0.01f;
    static bool prev_charging_status = false;
    
    voltage += direction;
    
    if (voltage >= BATTERY_MAX_VOLTAGE) {
        voltage = BATTERY_MAX_VOLTAGE;
        direction = -0.01f;
        g_charging_status = false;
    } else if (voltage <= BATTERY_MIN_VOLTAGE) {
        voltage = BATTERY_MIN_VOLTAGE;
        direction = 0.01f;
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
 * @brief 计算电池百分比
 */
static int calculate_battery_percentage(float voltage)
{
    if (voltage <= BATTERY_MIN_VOLTAGE) {
        return 0;
    } else if (voltage >= BATTERY_MAX_VOLTAGE) {
        return 100;
    }
    
    float percentage = ((voltage - BATTERY_MIN_VOLTAGE) / (BATTERY_MAX_VOLTAGE - BATTERY_MIN_VOLTAGE)) * 100.0f;
    return (int)percentage;
}

/**
 * @brief 获取充电动画颜色
 */
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

/**
 * @brief 创建简洁的电池电量条界面（横屏布局 428x142）
 */
static void create_battery_ui(void)
{
    // 创建主屏幕
    lv_obj_t *scr = lv_scr_act();
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x000000), 0);  // 黑色背景
    
    // 标题标签 - 居中显示
    title_label = lv_label_create(scr);
    lv_label_set_text(title_label, "BATTERY LEVEL");
    lv_obj_set_style_text_color(title_label, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(title_label, &lv_font_montserrat_18, 0);
    lv_obj_align(title_label, LV_ALIGN_TOP_MID, 0, 15);
    
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
    int initial_percentage = battery_display_override ? external_battery_value : g_battery_percentage;
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
    
    // 信息标签 - 显示控制模式
    info_label = lv_label_create(scr);
    if (battery_display_override) {
        lv_label_set_text_fmt(info_label, "JSON Mode | %d%%", initial_percentage);
        lv_obj_set_style_text_color(info_label, lv_color_hex(0x00FF00), 0);  // 绿色表示外部控制
    } else {
        lv_label_set_text(info_label, "Auto Mode");
        lv_obj_set_style_text_color(info_label, lv_color_hex(0x888888), 0);  // 灰色表示自动模式
    }
    lv_obj_set_style_text_font(info_label, &lv_font_montserrat_12, 0);
    lv_obj_align(info_label, LV_ALIGN_BOTTOM_MID, 0, -15);
    
    ESP_LOGI(TAG, "Simplified battery UI created successfully (428x142) with %d%%", initial_percentage);
}

/**
 * @brief 更新电池UI显示
 */
static void update_battery_ui(void)
{
    // 获取要显示的电量值和充电状态
    int display_percentage = battery_display_override ? external_battery_value : g_battery_percentage;
    bool effective_charging = get_effective_charging_status();
    
    // 确保数值在有效范围内
    if (display_percentage < 1) display_percentage = 1;
    if (display_percentage > 100) display_percentage = 100;
    
    ESP_LOGI(TAG, "Updating UI - Display: %d%%, BatteryOverride: %s, ChargingOverride: %s, EffectiveCharging: %s", 
             display_percentage, 
             battery_display_override ? "true" : "false",
             charging_status_override ? "true" : "false",
             effective_charging ? "true" : "false");
    
    if (battery_bar) {
        // 更新电量条值
        lv_bar_set_value(battery_bar, display_percentage, LV_ANIM_OFF);
        
        // 根据控制指令区分充电显示和普通电量显示
        lv_color_t bar_color;
        bool is_charging_display = charging_status_override && external_charging_status;
        
        if (is_charging_display) {
            // 充电显示模式 - 使用动画颜色和特效
            if (charging_animation_enabled) {
                if (display_percentage >= BATTERY_FULL_THRESHOLD) {
                    // 充满电特殊效果
                    bar_color = get_full_battery_color(charging_animation_step);
                } else {
                    bar_color = get_charging_animation_color(display_percentage, charging_animation_step);
                }
            } else {
                // 充电状态但动画禁用时使用静态充电颜色
                bar_color = lv_color_hex(0x00AAFF);  // 蓝色表示充电
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
        // 根据控制指令区分显示方式
        bool is_charging_display = charging_status_override && external_charging_status;
        
        if (is_charging_display) {
            // 充电显示模式 - 显示充电状态文字
            if (display_percentage >= BATTERY_FULL_THRESHOLD) {
                lv_label_set_text_fmt(battery_label, "%d%% FULL [charging]", display_percentage);
            } else {
                lv_label_set_text_fmt(battery_label, "%d%% [charging]", display_percentage);
            }
        } else {
            // 普通电量显示模式 - 保持简约，只显示数字
            if (display_percentage >= BATTERY_FULL_THRESHOLD) {
                lv_label_set_text_fmt(battery_label, "%d%% FULL", display_percentage);
            } else {
                lv_label_set_text_fmt(battery_label, "%d%%", display_percentage);
            }
        }
        
        // 根据显示模式和电量调整字体颜色
        if (is_charging_display) {
            // 充电显示模式的字体颜色 - 在渐变背景上使用黑色
            lv_obj_set_style_text_color(battery_label, lv_color_hex(0x808080), 0);  // 充电时黑色
        } else {
            // 普通显示模式的字体颜色 - 根据电量调整颜色以确保可见性
            lv_obj_set_style_text_color(battery_label, lv_color_hex(0x808080), 0);  // 电量用灰色
        }
        
        // 强制重新绘制标签
        lv_obj_invalidate(battery_label);
    }
    
    if (info_label) {
        // 根据控制指令区分显示模式信息
        bool is_charging_display = charging_status_override && external_charging_status;
        
        if (battery_display_override || charging_status_override) {
            // 外部控制模式
            char mode_text[128];
            if (is_charging_display) {
                // 充电显示模式
                if (battery_display_override) {
                    snprintf(mode_text, sizeof(mode_text), "Charging Mode | Level:%d%% | Anim:%s", 
                            display_percentage, charging_animation_enabled ? "ON" : "OFF");
                } else {
                    snprintf(mode_text, sizeof(mode_text), "Charging Mode | Auto Level | Anim:%s", 
                            charging_animation_enabled ? "ON" : "OFF");
                }
                lv_obj_set_style_text_color(info_label, lv_color_hex(0xFFAA00), 0);  // 橙色表示充电模式
            } else {
                // 普通显示模式
                if (battery_display_override) {
                    snprintf(mode_text, sizeof(mode_text), "Normal Mode | Level:%d%% | White Bar", display_percentage);
                } else {
                    snprintf(mode_text, sizeof(mode_text), "Normal Mode | Auto Level | White Bar");
                }
                lv_obj_set_style_text_color(info_label, lv_color_hex(0x888888), 0);  // 灰色表示普通模式
            }
            lv_label_set_text(info_label, mode_text);
        } else {
            // 完全自动模式
            if (effective_charging) {
                if (display_percentage >= BATTERY_FULL_THRESHOLD) {
                    lv_label_set_text(info_label, "Auto Mode | Battery Full | JSON Control Available");
                    lv_obj_set_style_text_color(info_label, lv_color_hex(0x00AAFF), 0);  // 蓝色表示充满
                } else {
                    lv_label_set_text(info_label, "Auto Mode | Charging | JSON Control Available");
                    lv_obj_set_style_text_color(info_label, lv_color_hex(0xFFAA00), 0);  // 橙色表示充电
                }
            } else {
                lv_label_set_text(info_label, "Auto Mode | JSON Control Available");
                lv_obj_set_style_text_color(info_label, lv_color_hex(0x888888), 0);  // 灰色表示自动模式
            }
        }
        
        // 强制重新绘制标签
        lv_obj_invalidate(info_label);
    }
    
    ESP_LOGI(TAG, "UI update completed");
}

/**
 * @brief 强制立即更新电池UI显示（用于外部控制）
 */
static void force_update_battery_ui(void)
{
    ESP_LOGI(TAG, "Force UI update (deferred to LVGL task)");
    ESP_LOGI(TAG, "Current values - external_battery_value: %d%%, battery_display_override: %s, external_charging_status: %s, charging_status_override: %s",
             external_battery_value, battery_display_override ? "true" : "false", 
             external_charging_status ? "true" : "false", charging_status_override ? "true" : "false");
    
    // 获取要显示的电量值和有效充电状态
    int display_percentage = battery_display_override ? external_battery_value : g_battery_percentage;
    bool effective_charging = get_effective_charging_status();
    
    // 确保数值在有效范围内
    if (display_percentage < 1) display_percentage = 1;
    if (display_percentage > 100) display_percentage = 100;
    
    ESP_LOGI(TAG, "Final display_percentage: %d%%, effective_charging: %s", display_percentage, effective_charging ? "true" : "false");
    
    // 检查对象是否存在
    if (!battery_bar || !battery_label || !info_label) {
        ESP_LOGE(TAG, "UI objects not ready: bar=%p, label=%p, info=%p", 
                 battery_bar, battery_label, info_label);
        return;
    }
    
    ESP_LOGI(TAG, "Updating battery bar to %d%%", display_percentage);
    lv_bar_set_value(battery_bar, display_percentage, LV_ANIM_OFF);
    
    // 根据控制指令区分充电显示和普通电量显示
    lv_color_t bar_color;
    bool is_charging_display = charging_status_override && external_charging_status;
    
    if (is_charging_display) {
        // 充电显示模式 - 使用动画颜色和特效
        if (charging_animation_enabled) {
            if (display_percentage >= BATTERY_FULL_THRESHOLD) {
                // 充满电特殊效果
                bar_color = get_full_battery_color(charging_animation_step);
            } else {
                bar_color = get_charging_animation_color(display_percentage, charging_animation_step);
            }
        } else {
            // 充电状态但动画禁用时使用静态充电颜色
            bar_color = lv_color_hex(0x00AAFF);  // 蓝色表示充电
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
    
    ESP_LOGI(TAG, "Updating battery label to %d%%", display_percentage);
    // 根据控制指令区分显示方式
    if (is_charging_display) {
        // 充电显示模式 - 显示充电状态文字
        if (display_percentage >= BATTERY_FULL_THRESHOLD) {
            lv_label_set_text_fmt(battery_label, "%d%% FULL [charging]", display_percentage);
        } else {
            lv_label_set_text_fmt(battery_label, "%d%% [charging]", display_percentage);
        }
    } else {
        // 普通电量显示模式 - 保持简约，只显示数字
        if (display_percentage >= BATTERY_FULL_THRESHOLD) {
            lv_label_set_text_fmt(battery_label, "%d%% FULL", display_percentage);
        } else {
            lv_label_set_text_fmt(battery_label, "%d%%", display_percentage);
        }
    }
    
    // 根据显示模式和电量调整字体颜色 - 确保文字始终可见
    if (is_charging_display) {
        // 充电显示模式的字体颜色 - 在渐变背景上使用黑色
        lv_obj_set_style_text_color(battery_label, lv_color_hex(0x000000), 0);  // 充电时黑色
        ESP_LOGI(TAG, "Set label color to BLACK (charging display mode)");
    } else {
        // 普通显示模式的字体颜色 - 根据电量调整颜色以确保可见性
        if (display_percentage < 20) {
            // 低电量时进度条是深色（红色），使用白色字体确保可见
            lv_obj_set_style_text_color(battery_label, lv_color_hex(0xFFFFFF), 0);  // 低电量用白色
            ESP_LOGI(TAG, "Set label color to WHITE (low battery mode)");
        } else {
            // 正常电量时进度条是白色，使用黑色字体确保可见
            lv_obj_set_style_text_color(battery_label, lv_color_hex(0x000000), 0);  // 正常黑色
            ESP_LOGI(TAG, "Set label color to BLACK (normal mode)");
        }
    }
    
    ESP_LOGI(TAG, "Updating info label");
    // 根据控制指令区分显示模式信息
    if (battery_display_override || charging_status_override) {
        // 外部控制模式
        char mode_text[128];
        if (is_charging_display) {
            // 充电显示模式
            if (battery_display_override) {
                snprintf(mode_text, sizeof(mode_text), "Charging Mode | Level:%d%% | Anim:%s", 
                        display_percentage, charging_animation_enabled ? "ON" : "OFF");
            } else {
                snprintf(mode_text, sizeof(mode_text), "Charging Mode | Auto Level | Anim:%s", 
                        charging_animation_enabled ? "ON" : "OFF");
            }
            lv_label_set_text(info_label, mode_text);
            lv_obj_set_style_text_color(info_label, lv_color_hex(0xFFAA00), 0);  // 橙色表示充电模式
            ESP_LOGI(TAG, "Info: %s", mode_text);
        } else {
            // 普通显示模式
            if (battery_display_override) {
                snprintf(mode_text, sizeof(mode_text), "Normal Mode | Level:%d%% | White Bar", display_percentage);
            } else {
                snprintf(mode_text, sizeof(mode_text), "Normal Mode | Auto Level | White Bar");
            }
            lv_label_set_text(info_label, mode_text);
            lv_obj_set_style_text_color(info_label, lv_color_hex(0x888888), 0);  // 灰色表示普通模式
            ESP_LOGI(TAG, "Info: %s", mode_text);
        }
    } else {
        // 完全自动模式
        if (effective_charging) {
            if (display_percentage >= BATTERY_FULL_THRESHOLD) {
                lv_label_set_text(info_label, "Auto Mode | Battery Full | JSON Control Available");
                lv_obj_set_style_text_color(info_label, lv_color_hex(0x00AAFF), 0);  // 蓝色表示充满
                ESP_LOGI(TAG, "Info: Auto Mode | Battery Full");
            } else {
                lv_label_set_text(info_label, "Auto Mode | Charging | JSON Control Available");
                lv_obj_set_style_text_color(info_label, lv_color_hex(0xFFAA00), 0);  // 橙色表示充电
                ESP_LOGI(TAG, "Info: Auto Mode | Charging");
            }
        } else {
            lv_label_set_text(info_label, "Auto Mode | JSON Control Available");
            lv_obj_set_style_text_color(info_label, lv_color_hex(0x888888), 0);  // 灰色表示自动模式
            ESP_LOGI(TAG, "Info: Auto Mode");
        }
    }
    
    // 强制重绘所有对象
    lv_obj_invalidate(battery_bar);
    lv_obj_invalidate(battery_label);
    lv_obj_invalidate(info_label);
    
    // 多次处理确保更新
    // 将更新交由 LVGL 任务，避免跨任务调用 LVGL API
    ui_update_pending = true;
}
/**
 * @brief 测试UI更新功能
 */
static void test_ui_update(void)
{
    ESP_LOGI(TAG, "=== Testing UI Update ===");
    
    // 测试设置不同的电量值
    for (int test_value = 10; test_value <= 100; test_value += 10) {
        ESP_LOGI(TAG, "Testing with %d%%", test_value);
        set_external_battery_level(test_value);
        vTaskDelay(pdMS_TO_TICKS(500)); // 等待500ms观察变化
    }
    
    ESP_LOGI(TAG, "=== UI Update Test Completed ===");
}
void set_external_battery_level(int level)
{
    if (level >= 1 && level <= 100) {
        ESP_LOGI(TAG, "Setting external battery level from %d%% to %d%%", external_battery_value, level);
        ESP_LOGI(TAG, "Previous override state: %s", battery_display_override ? "true" : "false");
        
        external_battery_value = level;
        battery_display_override = true;
        
        ESP_LOGI(TAG, "New values - external_battery_value: %d%%, battery_display_override: %s", 
                 external_battery_value, battery_display_override ? "true" : "false");
        
        // 设置待处理标志，让LVGL任务处理UI更新
        ui_update_pending = true;
        ESP_LOGI(TAG, "UI update pending flag set");
        
        // *** 删除这里的force_update_battery_ui()调用，避免跨任务LVGL调用 ***
        // force_update_battery_ui();
        
        // 立即保存到NVS
        save_battery_state_to_nvs();
        
        ESP_LOGI(TAG, "External battery level updated and saved");
    } else {
        ESP_LOGW(TAG, "Invalid battery level: %d (must be 1-100)", level);
    }
}

/**
 * @brief 设置外部充电状态
 */
void set_external_charging_status(bool charging)
{
    ESP_LOGI(TAG, "Setting external charging status from %s to %s", 
             external_charging_status ? "charging" : "not charging",
             charging ? "charging" : "not charging");
    ESP_LOGI(TAG, "Previous charging override state: %s", charging_status_override ? "true" : "false");
    
    external_charging_status = charging;
    charging_status_override = true;
    
    ESP_LOGI(TAG, "New values - external_charging_status: %s, charging_status_override: %s", 
             external_charging_status ? "true" : "false", 
             charging_status_override ? "true" : "false");
    
    // 根据充电显示模式控制动画（只有在充电显示模式下才启用动画）
    bool is_charging_display = charging_status_override && external_charging_status;
    if (is_charging_display) {
        ESP_LOGI(TAG, "Starting charging animation for charging display mode");
        start_charging_animation();
    } else {
        ESP_LOGI(TAG, "Stopping charging animation (not in charging display mode)");
        stop_charging_animation();
    }
    
    // 设置待处理标志，让LVGL任务处理UI更新
    ui_update_pending = true;
    ESP_LOGI(TAG, "UI update pending flag set");
    
    // 立即保存到NVS
    save_battery_state_to_nvs();
    
    ESP_LOGI(TAG, "External charging status updated and saved");
}

/**
 * @brief 恢复自动充电状态模式
 */
void restore_auto_charging_mode(void)
{
    charging_status_override = false;
    
    ESP_LOGI(TAG, "Restoring auto charging mode - Forcing UI update...");
    
    // 恢复自动模式后，不再是充电显示模式，停止动画
    ESP_LOGI(TAG, "Stopping charging animation (restored to auto charging mode)");
    stop_charging_animation();
    
    // 将刷新交由 LVGL 任务，避免跨任务调用
    ui_update_pending = true;
    
    // 立即保存到NVS
    save_battery_state_to_nvs();
    
    ESP_LOGI(TAG, "Auto charging mode restored and saved");
}

/**
 * @brief 恢复自动电池显示模式
 */
void restore_auto_battery_mode(void)
{
    battery_display_override = false;
    
    ESP_LOGI(TAG, "Restoring auto battery mode - Forcing UI update...");
    
    // 将刷新交由 LVGL 任务，避免跨任务调用
    ui_update_pending = true;
    
    // 立即保存到NVS
    save_battery_state_to_nvs();
    
    ESP_LOGI(TAG, "Auto battery mode restored and saved");
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
    ESP_LOGI(TAG, "Battery monitor task started (Passive Mode)");
    
    TickType_t last_wake_time = xTaskGetTickCount();
    uint32_t ui_update_counter = 0;
    uint32_t save_counter = 0;
    
    while (1) {
        // 读取电池电压 (在实际应用中，这里应该是从ADC读取)
        g_battery_voltage = simulate_battery_voltage();
        
        // 计算电池百分比
        g_battery_percentage = calculate_battery_percentage(g_battery_voltage);
        
        // 只在自动模式下才更新UI显示，降低频率
        ui_update_counter++;
        if (ui_update_counter >= 20 && !battery_display_override) {  // 降低更新频率
            ui_update_pending = true;  // 设置待处理标志，让LVGL任务处理
            ui_update_counter = 0;
        }
        
        // 定期保存状态到NVS (每200次循环，约200秒)
        save_counter++;
        if (save_counter >= 200) {
            save_battery_state_to_nvs();
            save_counter = 0;
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
    ESP_LOGI(TAG, "UART command task started");
    
    uint8_t data[UART_BUF_SIZE];
    char command_buffer[512]; // 增加缓冲区大小以支持较长的JSON
    int command_pos = 0;
    int json_brace_count = 0;
    bool in_json = false;
    
    while (1) {
        int len = uart_read_bytes(UART_NUM_1, data, UART_BUF_SIZE - 1, pdMS_TO_TICKS(100));
        
        if (len > 0) {
            data[len] = '\0';
            
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
                    
                    // JSON对象完成
                    if (json_brace_count == 0) {
                        command_buffer[command_pos] = '\0';
                        ESP_LOGI(TAG, "=== JSON COMMAND PROCESSING START ===");
                        ESP_LOGI(TAG, "Received JSON command: %s", command_buffer);
                        
                        // 解析JSON命令
                        cJSON *json = cJSON_Parse(command_buffer);
                        if (json) {
                            ESP_LOGI(TAG, "JSON parsing successful");
                            // 查询优先，单独处理并返回
                            cJSON *query_item = cJSON_GetObjectItem(json, "query");
                            if (query_item && cJSON_IsString(query_item)) {
                                const char *query_type = query_item->valuestring;
                                if (strcmp(query_type, "battery") == 0) {
                                    char *json_response = create_battery_status_json();
                                    if (json_response) {
                                        uart_send_response(json_response);
                                        free(json_response);
                                    } else {
                                        uart_send_response("{\"status\":\"error\",\"message\":\"Failed to create JSON response\"}");
                                    }
                                } else {
                                    uart_send_response("{\"status\":\"error\",\"message\":\"Unknown query type\"}");
                                }
                            } else {
                                bool did_any = false;
                                // 自动模式优先，若设置自动则忽略对应的手动字段
                                cJSON *auto_mode = cJSON_GetObjectItem(json, "auto_mode");
                                if (auto_mode && cJSON_IsBool(auto_mode) && cJSON_IsTrue(auto_mode)) {
                                    restore_auto_battery_mode();
                                    did_any = true;
                                }
                                cJSON *auto_charging = cJSON_GetObjectItem(json, "auto_charging");
                                if (auto_charging && cJSON_IsBool(auto_charging) && cJSON_IsTrue(auto_charging)) {
                                    restore_auto_charging_mode();
                                    did_any = true;
                                }
                                // 电量与充电可合并
                                if (!(auto_mode && cJSON_IsBool(auto_mode) && cJSON_IsTrue(auto_mode))) {
                                    cJSON *battery_item = cJSON_GetObjectItem(json, "battery");
                                    cJSON *percentage_item = cJSON_GetObjectItem(json, "percentage");
                                    if ((battery_item && cJSON_IsNumber(battery_item)) || (percentage_item && cJSON_IsNumber(percentage_item))) {
                                        int battery_level = battery_item ? battery_item->valueint : percentage_item->valueint;
                                        set_external_battery_level(battery_level);
                                        did_any = true;
                                    }
                                }
                                if (!(auto_charging && cJSON_IsBool(auto_charging) && cJSON_IsTrue(auto_charging))) {
                                    cJSON *charging_status = cJSON_GetObjectItem(json, "charging");
                                    if (charging_status && cJSON_IsBool(charging_status)) {
                                        set_external_charging_status(cJSON_IsTrue(charging_status));
                                        did_any = true;
                                    }
                                }
                                // 充电动画开关
                                cJSON *charging_anim = cJSON_GetObjectItem(json, "charging_animation");
                                if (charging_anim && cJSON_IsBool(charging_anim)) {
                                    bool enable_anim = cJSON_IsTrue(charging_anim);
                                    charging_animation_enabled = enable_anim;
                                    bool eff = get_effective_charging_status();
                                    if (!enable_anim) stop_charging_animation(); else if (eff) start_charging_animation();
                                    did_any = true;
                                }
                                // 模拟充电状态（测试）
                                cJSON *simulate_charging = cJSON_GetObjectItem(json, "simulate_charging");
                                if (simulate_charging && cJSON_IsBool(simulate_charging)) {
                                    bool charging = cJSON_IsTrue(simulate_charging);
                                    bool prev_status = g_charging_status;
                                    g_charging_status = charging;
                                    bool eff = get_effective_charging_status();
                                    if (eff && charging_animation_enabled) start_charging_animation(); else stop_charging_animation();
                                    if (prev_status != g_charging_status) ui_update_pending = true;
                                    did_any = true;
                                }
                                // LCD 控制
                                cJSON *lcd_ctrl = cJSON_GetObjectItem(json, "lcd");
                                if (lcd_ctrl) {
                                    esp_err_t ret2 = handle_lcd_command(json);
                                    if (ret2 == ESP_OK) {
                                        uart_send_response("{\"status\":\"success\",\"message\":\"LCD command executed\"}");
                                    } else {
                                        uart_send_response("{\"status\":\"error\",\"message\":\"LCD command failed\"}");
                                    }
                                } else if (did_any) {
                                    // 统一成功响应
                                    uart_send_response("{\"status\":\"success\"}");
                                } else {
                                    // 其余交给 WS2812 处理
                                    esp_err_t ret3 = ws2812_handle_json_command(command_buffer);
                                    if (ret3 == ESP_OK) uart_send_response("OK"); else uart_send_response("ERROR");
                                }
                            }
                            cJSON_Delete(json);
                            ESP_LOGI(TAG, "JSON object deleted");
                        } else {
                            ESP_LOGE(TAG, "Failed to parse JSON: %s", command_buffer);
                            uart_send_response("{\"status\":\"error\",\"message\":\"Invalid JSON format\"}");
                        }
                        
                        ESP_LOGI(TAG, "=== JSON COMMAND PROCESSING END ===");
                        
                        // 重置状态
                        in_json = false;
                        command_pos = 0;
                    }
                } else if (in_json) {
                    // JSON内部的其他字符
                    if (command_pos < sizeof(command_buffer) - 1) {
                        command_buffer[command_pos++] = c;
                    }
                } else {
                    // 非JSON命令处理（传统命令）
                    if (c == '\r' || c == '\n') {
                        if (command_pos > 0) {
                            command_buffer[command_pos] = '\0';
                            
                            // 处理传统命令
                            if (strncmp(command_buffer, "JSON:", 5) == 0) {
                                esp_err_t ret = ws2812_handle_json_command(command_buffer + 5);
                                if (ret == ESP_OK) {
                                    uart_send_response("OK");
                                } else {
                                    uart_send_response("ERROR");
                                }
                            }
                            else if (strcmp(command_buffer, "BATTERY") == 0) {
                                send_battery_info_via_uart();
                            }
                            else if (strcmp(command_buffer, "BATTERY:JSON") == 0) {
                                // 返回JSON格式的电池状态
                                char *json_response = create_battery_status_json();
                                if (json_response) {
                                    uart_send_response(json_response);
                                    free(json_response);
                                } else {
                                    uart_send_response("JSON_CREATE_ERROR");
                                }
                            }
                            else if (strncmp(command_buffer, "BATTERY:", 8) == 0) {
                                // 传统格式设置电池电量: BATTERY:75
                                int level = atoi(command_buffer + 8);
                                set_external_battery_level(level);
                                if (level >= 1 && level <= 100) {
                                    uart_send_response("BATTERY_SET_OK");
                                } else {
                                    uart_send_response("BATTERY_RANGE_ERROR");
                                }
                            }
                            else if (strcmp(command_buffer, "BATTERY:AUTO") == 0) {
                                restore_auto_battery_mode();
                                uart_send_response("AUTO_MODE_OK");
                            }
                            else if (strncmp(command_buffer, "CHARGING:", 9) == 0) {
                                // 充电状态控制命令: CHARGING:ON 或 CHARGING:OFF
                                const char* charging_cmd = command_buffer + 9;
                                if (strcmp(charging_cmd, "ON") == 0) {
                                    set_external_charging_status(true);
                                    uart_send_response("CHARGING_ON_OK");
                                } else if (strcmp(charging_cmd, "OFF") == 0) {
                                    set_external_charging_status(false);
                                    uart_send_response("CHARGING_OFF_OK");
                                } else if (strcmp(charging_cmd, "AUTO") == 0) {
                                    restore_auto_charging_mode();
                                    uart_send_response("CHARGING_AUTO_OK");
                                } else {
                                    uart_send_response("CHARGING_COMMAND_ERROR");
                                }
                            }
                            else if (strcmp(command_buffer, "BATTERY:SAVE") == 0) {
                                esp_err_t ret = save_battery_state_to_nvs();
                                if (ret == ESP_OK) {
                                    uart_send_response("BATTERY_SAVED");
                                } else {
                                    uart_send_response("BATTERY_SAVE_FAILED");
                                }
                            }
                            else if (strcmp(command_buffer, "UI:TEST") == 0) {
                                ESP_LOGI(TAG, "Manual UI test command received");
                                test_ui_update();
                                uart_send_response("UI_TEST_COMPLETED");
                            }
                            else if (strcmp(command_buffer, "UI:FORCE") == 0) {
                                ESP_LOGI(TAG, "Manual force UI update command received");
                                force_update_battery_ui();
                                uart_send_response("UI_FORCE_COMPLETED");
                            }
                            else if (strcmp(command_buffer, "WS2812:TEST") == 0) {
                                ESP_LOGI(TAG, "Starting WS2812 channel test...");
                                esp_err_t ret = ws2812_test_all_channels();
                                if (ret == ESP_OK) {
                                    uart_send_response("TEST_COMPLETED");
                                } else {
                                    uart_send_response("TEST_FAILED");
                                }
                            }
                            else if (strcmp(command_buffer, "WS2812:SAVE") == 0) {
                                ESP_LOGI(TAG, "Saving WS2812 configuration...");
                                esp_err_t ret = ws2812_save_config();
                                if (ret == ESP_OK) {
                                    uart_send_response("CONFIG_SAVED");
                                } else {
                                    uart_send_response("SAVE_FAILED");
                                }
                            }
                            else if (strcmp(command_buffer, "WS2812:LOAD") == 0) {
                                ESP_LOGI(TAG, "Loading WS2812 configuration...");
                                esp_err_t ret = ws2812_load_config();
                                if (ret == ESP_OK) {
                                    uart_send_response("CONFIG_LOADED");
                                } else {
                                    uart_send_response("LOAD_FAILED");
                                }
                            }
                            else if (strcmp(command_buffer, "WS2812:RESET") == 0) {
                                ESP_LOGI(TAG, "Resetting WS2812 configuration to defaults...");
                                esp_err_t ret = ws2812_reset_config();
                                if (ret == ESP_OK) {
                                    // 立即保存默认配置
                                    ret = ws2812_save_config();
                                    if (ret == ESP_OK) {
                                        uart_send_response("CONFIG_RESET");
                                    } else {
                                        uart_send_response("RESET_SAVE_FAILED");
                                    }
                                } else {
                                    uart_send_response("RESET_FAILED");
                                }
                            }
                            else if (strncmp(command_buffer, "LCD:", 4) == 0) {
                                // 处理传统格式的LCD命令
                                ESP_LOGI(TAG, "Processing LCD command: %s", command_buffer);
                                
                                if (strcmp(command_buffer, "LCD:CLEAR") == 0) {
                                    LCD_FastFill(LCD_COLOR_BLACK);
                                    uart_send_response("LCD_CLEARED");
                                } else if (strncmp(command_buffer, "LCD:CLEAR:", 10) == 0) {
                                    // LCD:CLEAR:RED格式
                                    const char* color_str = command_buffer + 10;
                                    uint16_t color = parse_color_string(color_str);
                                    LCD_FastFill(color);
                                    uart_send_response("LCD_CLEARED_COLOR");
                                } else if (strncmp(command_buffer, "LCD:TEXT:", 9) == 0) {
                                    // LCD:TEXT:Hello 或 LCD:TEXT:50:20:Hi格式
                                    const char* text_params = command_buffer + 9;
                                    
                                    // 尝试解析坐标和文本
                                    int x, y;
                                    char text[64];
                                    if (sscanf(text_params, "%d:%d:%63s", &x, &y, text) == 3) {
                                        // 有坐标的格式
                                        LCD_ShowStr((uint16_t)x, (uint16_t)y, text, LCD_COLOR_WHITE, LCD_COLOR_BLACK, 16, 0);
                                        uart_send_response("LCD_TEXT_POSITIONED");
                                    } else {
                                        // 直接文本格式
                                        LCD_ShowStr(10, 10, text_params, LCD_COLOR_WHITE, LCD_COLOR_BLACK, 16, 0);
                                        uart_send_response("LCD_TEXT_DEFAULT");
                                    }
                                } else if (strcmp(command_buffer, "LCD:BACKLIGHT:ON") == 0) {
                                    LCD_BLK_Set();
                                    uart_send_response("LCD_BACKLIGHT_ON");
                                } else if (strcmp(command_buffer, "LCD:BACKLIGHT:OFF") == 0) {
                                    LCD_BLK_Clr();
                                    uart_send_response("LCD_BACKLIGHT_OFF");
                                } else {
                                    uart_send_response("LCD_UNKNOWN_COMMAND");
                                }
                            }
                            else if (strcmp(command_buffer, "HELP") == 0) {
                                // 显示帮助信息
                                ESP_LOGI(TAG, "=== Available Commands ===");
                                ESP_LOGI(TAG, "Battery Control:");
                                ESP_LOGI(TAG, "  {\"query\": \"battery\"} - Get battery status in JSON format");
                                ESP_LOGI(TAG, "  {\"battery\": 75} - Set battery display to 75%% (1-100)");
                                ESP_LOGI(TAG, "  {\"auto_mode\": true} - Restore automatic battery display");
                                ESP_LOGI(TAG, "Charging Animation Control:");
                                ESP_LOGI(TAG, "  {\"charging_animation\": true} - Enable charging animation");
                                ESP_LOGI(TAG, "  {\"charging_animation\": false} - Disable charging animation");
                                ESP_LOGI(TAG, "  {\"simulate_charging\": true} - Simulate charging status (for testing)");
                                ESP_LOGI(TAG, "  {\"simulate_charging\": false} - Simulate not charging (for testing)");
                                ESP_LOGI(TAG, "Legacy Battery Commands:");
                                ESP_LOGI(TAG, "  BATTERY - Show battery status (legacy format)");
                                ESP_LOGI(TAG, "  BATTERY:JSON - Get battery status in JSON format");
                                ESP_LOGI(TAG, "  BATTERY:75 - Set battery display to 75%% (legacy format)");
                                ESP_LOGI(TAG, "  BATTERY:AUTO - Restore automatic display (legacy format)");
                                ESP_LOGI(TAG, "  BATTERY:SAVE - Manually save battery state to NVS");
                                ESP_LOGI(TAG, "  UI:TEST - Test UI update with multiple values");
                                ESP_LOGI(TAG, "  UI:FORCE - Force UI update immediately");
                                ESP_LOGI(TAG, "JSON Commands for WS2812:");
                                ESP_LOGI(TAG, "  {\"channel\": 0, \"mode\": 1, \"color\": {\"r\": 255, \"g\": 0, \"b\": 0}}");
                                ESP_LOGI(TAG, "  {\"channel\": 255, \"brightness\": 128}");
                                ESP_LOGI(TAG, "  {\"channel\": 1, \"enabled\": false}");
                                ESP_LOGI(TAG, "  {\"action\": \"status\"}");
                                ESP_LOGI(TAG, "JSON Commands for LCD:");
                                ESP_LOGI(TAG, "  {\"lcd\": \"clear\"} - Clear screen to black");
                                ESP_LOGI(TAG, "  {\"lcd\": \"clear\", \"color\": \"red\"} - Clear screen with color");
                                ESP_LOGI(TAG, "  {\"lcd\": \"text\", \"content\": \"Hello\"} - Show text at default position");
                                ESP_LOGI(TAG, "  {\"lcd\": \"text\", \"x\": 50, \"y\": 20, \"content\": \"Hi\", \"color\": \"green\"} - Show text at position");
                                ESP_LOGI(TAG, "  {\"lcd\": \"rect\", \"x\": 10, \"y\": 10, \"width\": 100, \"height\": 50} - Draw rectangle");
                                ESP_LOGI(TAG, "  {\"lcd\": \"circle\", \"x\": 100, \"y\": 70, \"radius\": 30, \"fill\": true} - Draw circle");
                                ESP_LOGI(TAG, "  {\"lcd\": \"line\", \"x1\": 0, \"y1\": 0, \"x2\": 100, \"y2\": 100} - Draw line");
                                ESP_LOGI(TAG, "  {\"lcd\": \"backlight\", \"state\": true} - Control backlight");
                                ESP_LOGI(TAG, "Legacy LCD Commands:");
                                ESP_LOGI(TAG, "  LCD:CLEAR - Clear screen");
                                ESP_LOGI(TAG, "  LCD:CLEAR:RED - Clear screen with red color");
                                ESP_LOGI(TAG, "  LCD:TEXT:Hello - Show text at default position");
                                ESP_LOGI(TAG, "  LCD:TEXT:50:20:Hi - Show text at position (50,20)");
                                ESP_LOGI(TAG, "  LCD:BACKLIGHT:ON/OFF - Control backlight");
                                ESP_LOGI(TAG, "Other Commands:");
                                ESP_LOGI(TAG, "  BATTERY - Show battery status");
                                ESP_LOGI(TAG, "  WS2812:TEST - Test all WS2812 channels");
                                ESP_LOGI(TAG, "  WS2812:SAVE - Save current WS2812 configuration");
                                ESP_LOGI(TAG, "  WS2812:LOAD - Load saved WS2812 configuration");
                                ESP_LOGI(TAG, "  WS2812:RESET - Reset WS2812 to default configuration");
                                ESP_LOGI(TAG, "  HELP - Show this help");
                                ESP_LOGI(TAG, "Channels: 0-3 (GPIO 18-21), Broadcast ID: 255");
                                uart_send_response("HELP_DISPLAYED");
                            }
                            else {
                                ESP_LOGW(TAG, "Unknown command: %s", command_buffer);
                                uart_send_response("UNKNOWN_COMMAND");
                            }
                            
                            command_pos = 0;
                        }
                    } else if (command_pos < sizeof(command_buffer) - 1) {
                        command_buffer[command_pos++] = c;
                    }
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
{
    while (1) {
        // 处理LVGL定时器
        lv_timer_handler();
        
        // 检查是否有待处理的UI更新
        if (ui_update_pending) {
            ESP_LOGI(TAG, "Processing pending UI update in LVGL task context");
            update_battery_ui();
            ui_update_pending = false;
            ESP_LOGI(TAG, "UI update completed in LVGL task context");
        }
        LED_TOGGLE();
        vTaskDelay(pdMS_TO_TICKS(20));  // 增加延迟至20ms，进一步减少CPU占用
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

    // 初始化WS2812
    ESP_ERROR_CHECK(ws2812_init());
    
    // 从NVS加载电池显示状态
    load_battery_state_from_nvs();
    
    // 初始化充电动画（如果处于充电状态）
    ESP_LOGI(TAG, "Initializing charging animation system");
    ESP_LOGI(TAG, "Charging animation enabled: %s", charging_animation_enabled ? "true" : "false");
    ESP_LOGI(TAG, "Current charging status: %s", g_charging_status ? "charging" : "not charging");
    
    if (g_charging_status && charging_animation_enabled) {
        ESP_LOGI(TAG, "Starting charging animation on initialization");
        start_charging_animation();
    }
    
    // 初始化Web服务器
    httpd_handle_t web_server_handle = web_server_init();
    if (web_server_handle == NULL) {
        ESP_LOGE(TAG, "Failed to initialize web server");
    } else {
        ESP_LOGI(TAG, "Web server initialized successfully");
    }

    // 初始化UART1
    uart1_init();

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
    xTaskCreate(lvgl_task, "lvgl", 6144, NULL, 3, NULL);           // 增加栈大小
    
    // 创建电池监控任务 - 增加栈大小
    TaskHandle_t battery_task_handle = NULL;
    xTaskCreate(battery_monitor_task, "battery_monitor", 6144, NULL, 2, &battery_task_handle);  // 增加栈大小
    
    // 将电池监控任务添加到watchdog监控中
    if (battery_task_handle != NULL) {
        esp_task_wdt_add(battery_task_handle);
        ESP_LOGI(TAG, "Battery monitor task added to watchdog");
    }
    
    // 创建UART命令处理任务
    xTaskCreate(uart_command_task, "uart_command", 4096, NULL, 2, NULL);
    
    // 创建WS2812控制任务
    xTaskCreate(ws2812_task, "ws2812", 4096, NULL, 2, NULL);
    
    ESP_LOGI(TAG, "All tasks created successfully");
    ESP_LOGI(TAG, "Battery Monitor System Ready! (Passive Mode with Charging Animation)");
    ESP_LOGI(TAG, "UART1 Communication: TX Pin=%d, RX Pin=%d, Baud=%d", 
             UART1_TXD_PIN, UART1_RXD_PIN, UART1_BAUD_RATE);
    ESP_LOGI(TAG, "WS2812 Multi-Channel System: %d channels on GPIO 18-21, %d LEDs per channel", 
             WS2812_CHANNEL_COUNT, WS2812_LED_COUNT);
    ESP_LOGI(TAG, "Battery Info: Query-only mode (no automatic push)");
    ESP_LOGI(TAG, "Charging Animation: %s (Period: %dms, Full Battery Threshold: %d%%)", 
             charging_animation_enabled ? "Enabled" : "Disabled", CHARGING_ANIM_PERIOD, BATTERY_FULL_THRESHOLD);
    ESP_LOGI(TAG, "Command formats:");
    ESP_LOGI(TAG, "  Battery Query: BATTERY or BATTERY:JSON or {\"query\": \"battery\"}");
    ESP_LOGI(TAG, "  Battery Control: {\"battery\": 75} or BATTERY:75");
    ESP_LOGI(TAG, "  Charging Animation: {\"charging_animation\": true/false}");
    ESP_LOGI(TAG, "  Simulate Charging: {\"simulate_charging\": true/false}");
    ESP_LOGI(TAG, "  WS2812 JSON: {\"channel\": 0, \"mode\": 1, \"color\": {\"r\": 255, \"g\": 0, \"b\": 0}}");
    ESP_LOGI(TAG, "  WS2812 Direct: {\"channel\": 255, \"brightness\": 128} (no prefix needed)");
}