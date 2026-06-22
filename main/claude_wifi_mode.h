#ifndef CLAUDE_WIFI_MODE_H
#define CLAUDE_WIFI_MODE_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// UDP 服务端口 (PC 桥广播/单播都打到这个端口)
#define CLAUDE_WIFI_UDP_PORT 8266

// 设备号长度: MAC 后 4 位 (8 个 hex 字符 + 1 个 '\0' 留点余量)
#define CLAUDE_WIFI_DEVID_LEN 8

/**
 * @brief 初始化 WiFi-Claude 模块 (只准备状态, 不启动 radio/server)
 *        必须在 claude_mode_init 之后调用
 */
esp_err_t claude_wifi_mode_init(void);

/**
 * @brief 进入 CLAUDE_WIFI 模式: 启动 WiFi (STA 已配 -> STA; 否则 AP 配网), 启动 UDP 监听
 */
void claude_wifi_mode_enter(void);

/**
 * @brief 退出 CLAUDE_WIFI 模式
 */
void claude_wifi_mode_exit(void);

bool claude_wifi_mode_is_active(void);

/**
 * @brief 返回设备号 (MAC 后 4 位 hex, 大写, 4 字符), 永久存储在 flash 不可写
 */
const char *claude_wifi_device_id(void);

/**
 * @brief 写入 STA 凭据到 NVS
 */
esp_err_t claude_wifi_creds_save(const char *ssid, const char *pass);

/**
 * @brief 读取 STA 凭据; 缓冲不足返回 ESP_ERR_NVS_INVALID_LENGTH
 */
esp_err_t claude_wifi_creds_load(char *ssid, size_t ssid_size,
                                  char *pass, size_t pass_size);

/**
 * @brief 凭据是否已写入
 */
bool claude_wifi_creds_present(void);

/**
 * @brief 清除已保存的 STA 凭据 (下次进入 CLAUDE_WIFI 会回到 AP 配网模式)
 */
esp_err_t claude_wifi_creds_clear(void);

#ifdef __cplusplus
}
#endif

#endif // CLAUDE_WIFI_MODE_H
