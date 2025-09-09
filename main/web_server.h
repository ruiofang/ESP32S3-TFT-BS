#ifndef WEB_SERVER_H
#define WEB_SERVER_H

#include "esp_err.h"
#include "esp_http_server.h"

// WiFi 配置
#define WIFI_SSID "ESP32_LightControl"
#define WIFI_PASS "12345678"
#define WIFI_CHANNEL 1
#define WIFI_MAX_STA_CONN 4

/**
 * @brief 初始化WiFi AP模式
 * @return ESP_OK 成功，其他值失败
 */
esp_err_t wifi_init_ap(void);

/**
 * @brief 启动HTTP网页服务器
 * @return HTTP服务器句柄，NULL表示启动失败
 */
httpd_handle_t start_webserver(void);

/**
 * @brief 停止HTTP网页服务器
 * @param server HTTP服务器句柄
 * @return ESP_OK 成功，其他值失败
 */
esp_err_t stop_webserver(httpd_handle_t server);

/**
 * @brief 初始化完整的Web服务器系统（WiFi + HTTP服务器）
 * @return HTTP服务器句柄，NULL表示初始化失败
 */
httpd_handle_t web_server_init(void);

#endif // WEB_SERVER_H
