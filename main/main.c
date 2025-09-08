
#include "nvs_flash.h"
#include "lvgl_demo.h"
#include "esp_vfs.h"
#include "esp_spiffs.h"
#include "led.h"
#include "lcd.h"
#include "lcd_init.h"
#include "ws2812_control.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "driver/uart.h"
#include "driver/gpio.h"
#include "esp_timer.h"
#include "lvgl.h"
#include <stdio.h>
#include <string.h>

#define TAG "BATTERY_MONITOR"

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

// 全局变量
static float g_battery_voltage = 3.7f;  // 当前电池电压
static int g_battery_percentage = 50;   // 当前电池百分比
static bool g_charging_status = false;  // 充电状态

// LVGL 对象
static lv_obj_t *battery_bar;          // 电池条
static lv_obj_t *battery_label;        // 电池百分比标签
static lv_obj_t *voltage_label;        // 电压标签
static lv_obj_t *status_label;         // 状态标签
static lv_obj_t *charging_icon;        // 充电图标

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
    snprintf(buffer, sizeof(buffer), "BATTERY:%.2fV,%d%%,%s\r\n", 
             g_battery_voltage, g_battery_percentage, g_charging_status ? "CHARGING" : "DISCHARGING");
    
    uart_write_bytes(UART_NUM_1, buffer, strlen(buffer));
    
    ESP_LOGI(TAG, "Sent via UART1: %s", buffer);
}

/**
 * @brief 模拟电池电压读取
 */
static float simulate_battery_voltage(void)
{
    static float voltage = 3.7f;
    static float direction = 0.01f;
    
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
 * @brief 获取电池条颜色
 */
static lv_color_t get_battery_color(int percentage)
{
    if (percentage >= 60) {
        return lv_color_hex(0x00FF00);  // 绿色
    } else if (percentage >= 30) {
        return lv_color_hex(0xFFA500);  // 橙色
    } else {
        return lv_color_hex(0xFF0000);  // 红色
    }
}

/**
 * @brief 创建电池UI界面（横屏布局 428x142）
 */
static void create_battery_ui(void)
{
    // 创建主屏幕
    lv_obj_t *scr = lv_scr_act();
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x000000), 0);  // 黑色背景
    
    // 左侧标题和电池图标区域
    lv_obj_t *title = lv_label_create(scr);
    lv_label_set_text(title, "Battery Monitor");
    lv_obj_set_style_text_color(title, lv_color_hex(0xFFFFFF), 0);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 10, 5);
    
    // 大型电池图标容器 - 几乎全屏显示
    lv_obj_t *battery_container = lv_obj_create(scr);
    lv_obj_set_size(battery_container, 380, 80);  // 大幅增加尺寸：60x30 -> 380x80
    lv_obj_set_style_bg_color(battery_container, lv_color_hex(0x333333), 0);
    lv_obj_set_style_border_color(battery_container, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_border_width(battery_container, 3, 0);  // 增加边框宽度
    lv_obj_set_style_radius(battery_container, 8, 0);  // 增加圆角
    lv_obj_align(battery_container, LV_ALIGN_CENTER, 0, 0);  // 居中偏左一点，为正极留空间
    
    // 大型电池条 - 相应调整尺寸
    battery_bar = lv_bar_create(battery_container);
    lv_obj_set_size(battery_bar, 350, 50);  // 大幅增加：48x18 -> 320x50
    lv_obj_center(battery_bar);
    lv_bar_set_range(battery_bar, 0, 100);
    lv_bar_set_value(battery_bar, g_battery_percentage, LV_ANIM_ON);
    
    // 大型电池正极 - 调整尺寸
    lv_obj_t *battery_tip = lv_obj_create(scr);
    lv_obj_set_size(battery_tip, 8, 40);  // 大幅增加：2x12 -> 8x40
    lv_obj_set_style_bg_color(battery_tip, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_radius(battery_tip, 4, 0);  // 增加圆角
    lv_obj_align_to(battery_tip, battery_container, LV_ALIGN_OUT_RIGHT_MID, 5, 0);
    
    // // 充电图标 - 调整大小和位置
    // charging_icon = lv_label_create(scr);
    // lv_label_set_text(charging_icon, "⚡");
    // lv_obj_set_style_text_color(charging_icon, lv_color_hex(0xFFFF00), 0);
    // lv_obj_set_style_text_font(charging_icon, &lv_font_montserrat_24, 0);  // 使用更大的字体
    // lv_obj_align_to(charging_icon, battery_tip, LV_ALIGN_OUT_RIGHT_MID, 10, 0);
    // lv_obj_add_flag(charging_icon, LV_OBJ_FLAG_HIDDEN);  // 初始隐藏
    
    // 中间区域 - 主要数据显示
    // 电池百分比标签 - 大字体，显示在电池图标内部
    battery_label = lv_label_create(scr);
    lv_label_set_text_fmt(battery_label, "%d%%", g_battery_percentage);
    lv_obj_set_style_text_color(battery_label, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(battery_label, &lv_font_montserrat_24, 0);  // 使用更大字体
    lv_obj_align(battery_label, LV_ALIGN_CENTER, 0, 0);  // 显示在电池中心
    
    // // 电压标签 - 显示在电池图标上方
    // voltage_label = lv_label_create(scr);
    // lv_label_set_text_fmt(voltage_label, "%.2fV", g_battery_voltage);
    // lv_obj_set_style_text_color(voltage_label, lv_color_hex(0x00FF00), 0);
    // lv_obj_set_style_text_font(voltage_label, &lv_font_montserrat_18, 0);
    // lv_obj_align(voltage_label, LV_ALIGN_CENTER, -20, -50);  // 显示在电池上方
    
    // 右上角区域 - 状态信息
    // 状态标签 - 移到右上角
    // status_label = lv_label_create(scr);
    // lv_label_set_text(status_label, g_charging_status ? "Charging" : "Discharging");
    // lv_obj_set_style_text_color(status_label, lv_color_hex(0xCCCCCC), 0);
    // lv_obj_set_style_text_font(status_label, &lv_font_montserrat_14, 0);
    // lv_obj_align(status_label, LV_ALIGN_TOP_RIGHT, -10, 25);  // 向下移动避免与标题重叠
    
    // 通信信息标签 - 显示在右下角，字体稍小
    lv_obj_t *comm_label = lv_label_create(scr);
    lv_label_set_text_fmt(comm_label, "UART1:TX%d RX%d", UART1_TXD_PIN, UART1_RXD_PIN);
    lv_obj_set_style_text_color(comm_label, lv_color_hex(0x666666), 0);
    lv_obj_set_style_text_font(comm_label, &lv_font_montserrat_10, 0);  // 使用更小字体
    lv_obj_align(comm_label, LV_ALIGN_BOTTOM_RIGHT, -5, -5);
    
    // 信息标签 - 显示在左下角，字体稍小
    lv_obj_t *info_label = lv_label_create(scr);
    lv_label_set_text(info_label, "UART1 Protocol");
    lv_obj_set_style_text_color(info_label, lv_color_hex(0x888888), 0);
    lv_obj_set_style_text_font(info_label, &lv_font_montserrat_10, 0);  // 使用更小字体
    lv_obj_align(info_label, LV_ALIGN_BOTTOM_LEFT, 10, -5);
    
    ESP_LOGI(TAG, "Battery UI created successfully (Landscape 428x142)");
}

/**
 * @brief 更新电池UI显示
 */
static void update_battery_ui(void)
{
    if (battery_bar) {
        // 更新电池条值和颜色
        lv_bar_set_value(battery_bar, g_battery_percentage, LV_ANIM_ON);
        lv_obj_set_style_bg_color(battery_bar, get_battery_color(g_battery_percentage), LV_PART_INDICATOR);
    }
    
    if (battery_label) {
        lv_label_set_text_fmt(battery_label, "%d%%", g_battery_percentage);
    }
    
    if (voltage_label) {
        lv_label_set_text_fmt(voltage_label, "%.2fV", g_battery_voltage);
    }
    
    if (status_label) {
        lv_label_set_text(status_label, g_charging_status ? "Charging" : "Discharging");
        lv_obj_set_style_text_color(status_label, 
                                   g_charging_status ? lv_color_hex(0x00FF00) : lv_color_hex(0xCCCCCC), 0);
    }
    
    if (charging_icon) {
        if (g_charging_status) {
            lv_obj_clear_flag(charging_icon, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(charging_icon, LV_OBJ_FLAG_HIDDEN);
        }
    }
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
    ESP_LOGI(TAG, "Battery monitor task started");
    
    TickType_t last_wake_time = xTaskGetTickCount();
    
    while (1) {
        // 读取电池电压 (在实际应用中，这里应该是从ADC读取)
        g_battery_voltage = simulate_battery_voltage();
        
        // 计算电池百分比
        g_battery_percentage = calculate_battery_percentage(g_battery_voltage);
        
        // 更新UI显示
        update_battery_ui();
        
        // 通过串口发送电池信息
        send_battery_info_via_uart();
        
        ESP_LOGI(TAG, "Battery: %.2fV (%d%%) - %s", 
                 g_battery_voltage, g_battery_percentage, 
                 g_charging_status ? "CHARGING" : "DISCHARGING");
        
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
                        ESP_LOGI(TAG, "Received JSON command: %s", command_buffer);
                        
                        esp_err_t ret = ws2812_handle_json_command(command_buffer);
                        if (ret == ESP_OK) {
                            uart_write_bytes(UART_NUM_1, "OK\r\n", 4);
                        } else {
                            uart_write_bytes(UART_NUM_1, "ERROR\r\n", 7);
                        }
                        
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
                                    uart_write_bytes(UART_NUM_1, "OK\r\n", 4);
                                } else {
                                    uart_write_bytes(UART_NUM_1, "ERROR\r\n", 7);
                                }
                            }
                            else if (strcmp(command_buffer, "BATTERY") == 0) {
                                send_battery_info_via_uart();
                            }
                            else if (strcmp(command_buffer, "WS2812:TEST") == 0) {
                                ESP_LOGI(TAG, "Starting WS2812 channel test...");
                                esp_err_t ret = ws2812_test_all_channels();
                                if (ret == ESP_OK) {
                                    uart_write_bytes(UART_NUM_1, "TEST_COMPLETED\r\n", 16);
                                } else {
                                    uart_write_bytes(UART_NUM_1, "TEST_FAILED\r\n", 13);
                                }
                            }
                            else if (strcmp(command_buffer, "WS2812:SAVE") == 0) {
                                ESP_LOGI(TAG, "Saving WS2812 configuration...");
                                esp_err_t ret = ws2812_save_config();
                                if (ret == ESP_OK) {
                                    uart_write_bytes(UART_NUM_1, "CONFIG_SAVED\r\n", 14);
                                } else {
                                    uart_write_bytes(UART_NUM_1, "SAVE_FAILED\r\n", 13);
                                }
                            }
                            else if (strcmp(command_buffer, "WS2812:LOAD") == 0) {
                                ESP_LOGI(TAG, "Loading WS2812 configuration...");
                                esp_err_t ret = ws2812_load_config();
                                if (ret == ESP_OK) {
                                    uart_write_bytes(UART_NUM_1, "CONFIG_LOADED\r\n", 15);
                                } else {
                                    uart_write_bytes(UART_NUM_1, "LOAD_FAILED\r\n", 13);
                                }
                            }
                            else if (strcmp(command_buffer, "WS2812:RESET") == 0) {
                                ESP_LOGI(TAG, "Resetting WS2812 configuration to defaults...");
                                esp_err_t ret = ws2812_reset_config();
                                if (ret == ESP_OK) {
                                    // 立即保存默认配置
                                    ret = ws2812_save_config();
                                    if (ret == ESP_OK) {
                                        uart_write_bytes(UART_NUM_1, "CONFIG_RESET\r\n", 14);
                                    } else {
                                        uart_write_bytes(UART_NUM_1, "RESET_SAVE_FAILED\r\n", 19);
                                    }
                                } else {
                                    uart_write_bytes(UART_NUM_1, "RESET_FAILED\r\n", 14);
                                }
                            }
                            else if (strcmp(command_buffer, "HELP") == 0) {
                                // 显示帮助信息
                                ESP_LOGI(TAG, "=== Available Commands ===");
                                ESP_LOGI(TAG, "JSON Commands for WS2812:");
                                ESP_LOGI(TAG, "  {\"channel\": 0, \"mode\": 1, \"color\": {\"r\": 255, \"g\": 0, \"b\": 0}}");
                                ESP_LOGI(TAG, "  {\"channel\": 255, \"brightness\": 128}");
                                ESP_LOGI(TAG, "  {\"channel\": 1, \"enabled\": false}");
                                ESP_LOGI(TAG, "  {\"action\": \"status\"}");
                                ESP_LOGI(TAG, "Other Commands:");
                                ESP_LOGI(TAG, "  BATTERY - Show battery status");
                                ESP_LOGI(TAG, "  WS2812:TEST - Test all WS2812 channels");
                                ESP_LOGI(TAG, "  WS2812:SAVE - Save current WS2812 configuration");
                                ESP_LOGI(TAG, "  WS2812:LOAD - Load saved WS2812 configuration");
                                ESP_LOGI(TAG, "  WS2812:RESET - Reset WS2812 to default configuration");
                                ESP_LOGI(TAG, "  HELP - Show this help");
                                ESP_LOGI(TAG, "Channels: 0-3 (GPIO 18-21), Broadcast ID: 255");
                                uart_write_bytes(UART_NUM_1, "HELP_DISPLAYED\r\n", 16);
                            }
                            else {
                                ESP_LOGW(TAG, "Unknown command: %s", command_buffer);
                                uart_write_bytes(UART_NUM_1, "UNKNOWN_COMMAND\r\n", 17);
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
        lv_tick_inc(10);
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

/**
 * @brief LVGL任务处理
 */
static void lvgl_task(void *pvParameters)
{
    while (1) {
        lv_timer_handler();
        vTaskDelay(pdMS_TO_TICKS(5));
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
    ESP_ERROR_CHECK(esp_timer_start_periodic(lvgl_tick_timer, 1 * 1000));
    
    // 创建电池监控UI界面
    create_battery_ui();
    
    // 创建LVGL相关任务
    xTaskCreate(lvgl_tick_task, "lvgl_tick", 2048, NULL, 4, NULL);
    xTaskCreate(lvgl_task, "lvgl", 4096, NULL, 3, NULL);
    
    // 创建电池监控任务
    xTaskCreate(battery_monitor_task, "battery_monitor", 4096, NULL, 2, NULL);
    
    // 创建UART命令处理任务
    xTaskCreate(uart_command_task, "uart_command", 4096, NULL, 2, NULL);
    
    // 创建WS2812控制任务
    xTaskCreate(ws2812_task, "ws2812", 4096, NULL, 2, NULL);
    
    ESP_LOGI(TAG, "All tasks created successfully");
    ESP_LOGI(TAG, "Battery Monitor System Ready!");
    ESP_LOGI(TAG, "UART1 Communication: TX Pin=%d, RX Pin=%d, Baud=%d", 
             UART1_TXD_PIN, UART1_RXD_PIN, UART1_BAUD_RATE);
    ESP_LOGI(TAG, "WS2812 Multi-Channel System: %d channels on GPIO 18-21, %d LEDs per channel", 
             WS2812_CHANNEL_COUNT, WS2812_LED_COUNT);
    ESP_LOGI(TAG, "Command formats:");
    ESP_LOGI(TAG, "  Legacy: WS2812:MODE:1 (broadcasts to all channels)");
    ESP_LOGI(TAG, "  JSON: {\"channel\": 0, \"mode\": 1, \"color\": {\"r\": 255, \"g\": 0, \"b\": 0}}");
    ESP_LOGI(TAG, "  Direct JSON: {\"channel\": 255, \"brightness\": 128} (no prefix needed)");
}
  