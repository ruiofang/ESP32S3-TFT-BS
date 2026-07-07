#ifndef WEB_SERVER_H
#define WEB_SERVER_H

#include "esp_err.h"
#include "esp_http_server.h"
#include <stdbool.h>

// WiFi 配置
#define WIFI_SSID "ESP32_LightControl"
#define WIFI_PASS "12345678"
#define WIFI_CHANNEL 1
#define WIFI_MAX_STA_CONN 4
#define RGB_TCP_CONTROL_PORT 8267

/**
 * @brief 初始化WiFi (优先 STA; 无凭据/连接失败则进入 AP 配网模式)
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

/**
 * @brief 获取 RGB/Web 网络当前 IP 地址
 * @return IP 字符串；未联网时通常为 192.168.4.1 或 0.0.0.0
 */
const char *web_server_get_ip(void);

/**
 * @brief RGB/Web 网络是否已经连接到路由器 STA
 * @return true=STA 已获取 IP, false=AP 配网态或未连接
 */
bool web_server_is_sta_connected(void);

/**
 * @brief RGB TCP JSON 控制端口当前是否有客户端连接
 * @return true=有 TCP 客户端正在连接
 */
bool web_server_rgb_tcp_client_connected(void);

#endif // WEB_SERVER_H
