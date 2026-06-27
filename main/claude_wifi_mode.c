/*
 * Claude WiFi 状态模式:
 *   - 与 BLE 路径 (claude_ble_mode.c) 共用 LCD 面板/WS2812 颜色/状态机
 *   - 传输换成 WiFi: UDP 只做发现, TCP 长连接承载状态
 *   - 配网逻辑: NVS 已存 STA 凭据 -> STA; 否则启动 AP 'ESP32_Claude_XXXX'
 *     供 PC 浏览器访问 http://192.168.4.1/wificfg 提交家庭 WiFi 凭据
 *   - 配对: 设备号 = MAC 后 4 位 hex (与 BLE 命名规则一致)
 *           PC 桥广播 {"q":"discover","id":"XXXX"} -> 设备回 {"r":"discover", ...}
 *           随后 PC 连接本设备 TCP 端口并持续发送状态 JSON
 *
 * 协议帧:
 *   PC -> ESP   {"q":"discover"}                -> ESP 回 discovery 应答 (向源端口)
 *   PC -> ESP   {"q":"discover","id":"AB12"}    -> id 匹配才回; 不匹配丢弃
 *   PC -> ESP   {"q":"ping"}                    -> ESP 回 {"r":"pong","id":"..."}
 *   PC -> ESP   TCP: {"q":"bind","id":"AB12","source":"host"}
 *   PC -> ESP   TCP: {"q":"ping", ...}         -> 心跳保活, 用于离线检测
 *   PC -> ESP   TCP: {状态 JSON, 字段同 BLE 协议} -> 喂入 claude_ble_mode_feed_json
 *   ESP -> PC   {"r":"discover","id":"AB12","name":"ESP32_Claude_AB12","ip":"1.2.3.4","tcp_port":8267}
 */

#include "claude_wifi_mode.h"
#include "claude_ble_mode.h"
#include "ota_updater.h"

#include "esp_log.h"
#include "esp_mac.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "esp_http_server.h"
#include "nvs.h"
#include "nvs_flash.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"

#include "lwip/sockets.h"
#include "lwip/inet.h"
#include "lwip/netdb.h"
#include "lwip/tcp.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#define TAG "CLAUDE_WIFI"

#define CLAUDE_WIFI_NVS_NS    "claude_wifi"
#define CLAUDE_WIFI_NVS_SSID  "ssid"
#define CLAUDE_WIFI_NVS_PASS  "pass"

#define AP_SSID_PREFIX       "ESP32_Claude_"
#define AP_PASSWORD          "12345678"        // >=8 chars, WPA2
#define AP_CHANNEL           6
#define AP_MAX_CONN          2

#define STA_CONNECT_TIMEOUT_MS  15000

#define WIFI_BIT_CONNECTED   BIT0
#define WIFI_BIT_FAIL        BIT1
#define WIFI_BIT_GOT_IP      BIT2

#define UDP_RX_BUF_LEN       1024
#define UDP_TX_BUF_LEN       256
#define TCP_RX_BUF_LEN       384
#define CLAUDE_TCP_ACCEPT_BACKLOG 4
#define TCP_MAX_CLIENTS      4
#define TCP_IDLE_TIMEOUT_MS  12000

// -----------------------------------------------------------------------------
// 模块全局状态
// -----------------------------------------------------------------------------
static bool s_inited = false;
static volatile bool s_active = false;
static volatile bool s_stop_req = false;

static char s_device_id[CLAUDE_WIFI_DEVID_LEN] = "----";
static char s_dev_name[24] = "";       // "ESP32_Claude_XXXX"

static EventGroupHandle_t s_wifi_evt = NULL;
static esp_netif_t *s_netif_sta = NULL;
static esp_netif_t *s_netif_ap  = NULL;
static httpd_handle_t s_httpd   = NULL;

typedef struct {
    int fd;
    TickType_t last_rx_tick;
    char ip[16];
    char name[24];
    char rx_buf[TCP_RX_BUF_LEN];
    size_t rx_len;
} tcp_client_t;

static TaskHandle_t s_net_task = NULL;
static int s_udp_sock = -1;
static int s_tcp_listen_sock = -1;
static tcp_client_t s_clients[TCP_MAX_CLIENTS];

static bool s_ap_mode = false;          // 当前是否处于 AP 配网模式
static char s_my_ip_str[16] = "0.0.0.0";

// 最后一次成功通信的对端 IP (用于显示和反馈, 不用于安全)
static char s_peer_ip_str[16] = "";

static void update_link_label(void);

static void reset_client_slot(tcp_client_t *client)
{
    if (!client) return;
    client->fd = -1;
    client->last_rx_tick = 0;
    client->ip[0] = '\0';
    client->name[0] = '\0';
    client->rx_buf[0] = '\0';
    client->rx_len = 0;
}

static int active_client_count(void)
{
    int count = 0;
    for (size_t i = 0; i < TCP_MAX_CLIENTS; i++) {
        if (s_clients[i].fd >= 0) count++;
    }
    return count;
}

static tcp_client_t *first_active_client(void)
{
    for (size_t i = 0; i < TCP_MAX_CLIENTS; i++) {
        if (s_clients[i].fd >= 0) return &s_clients[i];
    }
    return NULL;
}

static bool extract_json_string_field(const char *json, const char *key,
                                      char *out, size_t out_size)
{
    if (!json || !key || !out || out_size == 0) return false;
    char pattern[24];
    snprintf(pattern, sizeof(pattern), "\"%s\"", key);
    const char *p = strstr(json, pattern);
    if (!p) return false;

    p = strchr(p, ':');
    if (!p) return false;
    p++;
    while (*p == ' ' || *p == '\t') p++;

    bool quoted = (*p == '\"');
    if (quoted) p++;

    size_t i = 0;
    while (*p && i + 1 < out_size) {
        char c = *p++;
        if (quoted) {
            if (c == '\"') break;
        } else if (c == ',' || c == '}' || c == ' ' || c == '\r' || c == '\n' || c == '\t') {
            break;
        }
        out[i++] = c;
    }
    out[i] = '\0';
    return i > 0;
}

static bool message_id_matches_device(const char *json)
{
    char id_seen[16];
    if (!extract_json_string_field(json, "id", id_seen, sizeof(id_seen))) return true;
    if (id_seen[0] == '\0') return true;
    return strcasecmp(id_seen, s_device_id) == 0;
}

static void close_client_slot(tcp_client_t *client, const char *reason)
{
    if (!client || client->fd < 0) return;

    int fd = client->fd;
    char ip[16];
    char name[24];
    snprintf(ip, sizeof(ip), "%s", client->ip);
    snprintf(name, sizeof(name), "%s", client->name);

    shutdown(fd, SHUT_RDWR);
    close(fd);
    reset_client_slot(client);

    ESP_LOGI(TAG, "TCP client closed: %s (%s)", name[0] ? name : ip, reason ? reason : "closed");

    if (active_client_count() == 0) {
        claude_ble_mode_set_ready_msg(reason && *reason ? reason : "Waiting for WiFi host...");
    }
    update_link_label();
}

static void close_all_clients(const char *reason)
{
    for (size_t i = 0; i < TCP_MAX_CLIENTS; i++) {
        if (s_clients[i].fd >= 0) close_client_slot(&s_clients[i], reason);
    }
}

static void configure_tcp_socket(int fd)
{
    int yes = 1;
    setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &yes, sizeof(yes));
#ifdef TCP_KEEPIDLE
    {
        int idle = 6;
        setsockopt(fd, IPPROTO_TCP, TCP_KEEPIDLE, &idle, sizeof(idle));
    }
#endif
#ifdef TCP_KEEPINTVL
    {
        int intvl = 3;
        setsockopt(fd, IPPROTO_TCP, TCP_KEEPINTVL, &intvl, sizeof(intvl));
    }
#endif
#ifdef TCP_KEEPCNT
    {
        int cnt = 2;
        setsockopt(fd, IPPROTO_TCP, TCP_KEEPCNT, &cnt, sizeof(cnt));
    }
#endif
}

// -----------------------------------------------------------------------------
// 工具
// -----------------------------------------------------------------------------
static void compute_device_id(void)
{
    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(s_device_id, sizeof(s_device_id), "%02X%02X", mac[4], mac[5]);
    snprintf(s_dev_name, sizeof(s_dev_name), "%s%s", AP_SSID_PREFIX, s_device_id);
}

const char *claude_wifi_device_id(void)
{
    return s_device_id;
}

bool claude_wifi_mode_is_active(void)
{
    return s_active;
}

// -----------------------------------------------------------------------------
// NVS 凭据
// -----------------------------------------------------------------------------
esp_err_t claude_wifi_creds_save(const char *ssid, const char *pass)
{
    if (!ssid || !*ssid) return ESP_ERR_INVALID_ARG;
    if (!pass) pass = "";
    nvs_handle_t h;
    esp_err_t err = nvs_open(CLAUDE_WIFI_NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    err = nvs_set_str(h, CLAUDE_WIFI_NVS_SSID, ssid);
    if (err == ESP_OK) err = nvs_set_str(h, CLAUDE_WIFI_NVS_PASS, pass);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}

esp_err_t claude_wifi_creds_load(char *ssid, size_t ssid_size, char *pass, size_t pass_size)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(CLAUDE_WIFI_NVS_NS, NVS_READONLY, &h);
    if (err != ESP_OK) return err;
    size_t l = ssid_size;
    err = nvs_get_str(h, CLAUDE_WIFI_NVS_SSID, ssid, &l);
    if (err == ESP_OK) {
        l = pass_size;
        esp_err_t err2 = nvs_get_str(h, CLAUDE_WIFI_NVS_PASS, pass, &l);
        if (err2 == ESP_ERR_NVS_NOT_FOUND) {
            // 无密码也允许
            pass[0] = '\0';
            err = ESP_OK;
        } else if (err2 != ESP_OK) {
            err = err2;
        }
    }
    nvs_close(h);
    return err;
}

bool claude_wifi_creds_present(void)
{
    char ssid[33];
    nvs_handle_t h;
    if (nvs_open(CLAUDE_WIFI_NVS_NS, NVS_READONLY, &h) != ESP_OK) return false;
    size_t l = sizeof(ssid);
    esp_err_t err = nvs_get_str(h, CLAUDE_WIFI_NVS_SSID, ssid, &l);
    nvs_close(h);
    return err == ESP_OK && ssid[0] != '\0';
}

esp_err_t claude_wifi_creds_clear(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(CLAUDE_WIFI_NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    nvs_erase_key(h, CLAUDE_WIFI_NVS_SSID);
    nvs_erase_key(h, CLAUDE_WIFI_NVS_PASS);
    err = nvs_commit(h);
    nvs_close(h);
    return err;
}

// -----------------------------------------------------------------------------
// WiFi 事件
// -----------------------------------------------------------------------------
static int s_sta_retry = 0;
static volatile bool s_want_sta_connect = false;
#define STA_MAX_RETRY 6

static void wifi_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    if (base == WIFI_EVENT) {
        switch (id) {
        case WIFI_EVENT_STA_START:
            // APSTA 配网模式下 STA 仅用于扫描, 不要自动 connect (会拿空 SSID 反复失败)
            if (s_want_sta_connect) esp_wifi_connect();
            break;
        case WIFI_EVENT_STA_DISCONNECTED:
            if (!s_want_sta_connect) break;
            if (s_sta_retry < STA_MAX_RETRY && !s_stop_req) {
                s_sta_retry++;
                ESP_LOGW(TAG, "STA disconnect, retry %d/%d", s_sta_retry, STA_MAX_RETRY);
                esp_wifi_connect();
            } else {
                xEventGroupSetBits(s_wifi_evt, WIFI_BIT_FAIL);
            }
            break;
        case WIFI_EVENT_AP_STACONNECTED: {
            wifi_event_ap_staconnected_t *e = (wifi_event_ap_staconnected_t *)data;
            ESP_LOGI(TAG, "AP: station joined " MACSTR, MAC2STR(e->mac));
            break;
        }
        case WIFI_EVENT_AP_STADISCONNECTED: {
            wifi_event_ap_stadisconnected_t *e = (wifi_event_ap_stadisconnected_t *)data;
            ESP_LOGI(TAG, "AP: station left " MACSTR, MAC2STR(e->mac));
            break;
        }
        default:
            break;
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = (ip_event_got_ip_t *)data;
        snprintf(s_my_ip_str, sizeof(s_my_ip_str), IPSTR, IP2STR(&e->ip_info.ip));
        ESP_LOGI(TAG, "STA got IP: %s", s_my_ip_str);
        s_sta_retry = 0;
        xEventGroupSetBits(s_wifi_evt, WIFI_BIT_GOT_IP | WIFI_BIT_CONNECTED);
    }
}

// -----------------------------------------------------------------------------
// 启动 STA 或 AP
// -----------------------------------------------------------------------------
static esp_err_t wifi_netif_event_init_once(void)
{
    static bool done = false;
    if (done) return ESP_OK;

    ESP_ERROR_CHECK(esp_netif_init());
    esp_err_t err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) return err;

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                       &wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                                       &wifi_event_handler, NULL, NULL));
    done = true;
    return ESP_OK;
}

static esp_err_t start_sta(const char *ssid, const char *pass)
{
    ESP_LOGI(TAG, "Starting STA, ssid='%s'", ssid);
    if (!s_netif_sta) s_netif_sta = esp_netif_create_default_wifi_sta();

    wifi_config_t wc = {0};
    strncpy((char *)wc.sta.ssid, ssid, sizeof(wc.sta.ssid) - 1);
    strncpy((char *)wc.sta.password, pass, sizeof(wc.sta.password) - 1);
    wc.sta.threshold.authmode = WIFI_AUTH_OPEN;
    wc.sta.pmf_cfg.capable = true;

    s_sta_retry = 0;
    s_want_sta_connect = true;
    xEventGroupClearBits(s_wifi_evt, WIFI_BIT_CONNECTED | WIFI_BIT_FAIL | WIFI_BIT_GOT_IP);

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wc));
    ESP_ERROR_CHECK(esp_wifi_start());

    EventBits_t bits = xEventGroupWaitBits(s_wifi_evt,
                                           WIFI_BIT_GOT_IP | WIFI_BIT_FAIL,
                                           pdFALSE, pdFALSE,
                                           pdMS_TO_TICKS(STA_CONNECT_TIMEOUT_MS));
    if (bits & WIFI_BIT_GOT_IP) {
        s_ap_mode = false;
        return ESP_OK;
    }
    ESP_LOGW(TAG, "STA connect timeout or failed");
    s_want_sta_connect = false;
    esp_wifi_stop();
    return ESP_FAIL;
}

static esp_err_t start_ap_provisioning(void)
{
    ESP_LOGI(TAG, "Starting AP fallback for provisioning: %s", s_dev_name);
    if (!s_netif_ap) s_netif_ap = esp_netif_create_default_wifi_ap();
    // 同时创建 STA netif, 这样 APSTA 模式下扫描周围 WiFi 才能工作
    if (!s_netif_sta) s_netif_sta = esp_netif_create_default_wifi_sta();

    wifi_config_t wc = {0};
    strncpy((char *)wc.ap.ssid, s_dev_name, sizeof(wc.ap.ssid) - 1);
    wc.ap.ssid_len = strlen(s_dev_name);
    strncpy((char *)wc.ap.password, AP_PASSWORD, sizeof(wc.ap.password) - 1);
    wc.ap.channel = AP_CHANNEL;
    wc.ap.max_connection = AP_MAX_CONN;
    wc.ap.authmode = WIFI_AUTH_WPA_WPA2_PSK;

    // 用 APSTA: AP 给 PC 接入做配网, STA 用来扫描周围 WiFi (esp_wifi_scan 需要 STA)
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wc));
    wifi_config_t sta_empty = {0};
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta_empty));
    ESP_ERROR_CHECK(esp_wifi_start());

    strncpy(s_my_ip_str, "192.168.4.1", sizeof(s_my_ip_str));
    s_ap_mode = true;
    return ESP_OK;
}

// -----------------------------------------------------------------------------
// HTTP 配网页面 — 分页, 每页只渲染自己需要的内容, 减少单次响应体积
// 共用 HEAD (含样式) + NAV (导航条), 然后各页独立 body
// -----------------------------------------------------------------------------
static const char k_head[] =
    "<!DOCTYPE html><html><head><meta charset='utf-8'>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>ESP32 Claude</title>"
    "<style>body{font-family:sans-serif;max-width:480px;margin:0 auto;padding:10px;font-size:14px}"
    "h1{font-size:17px;margin:8px 0}h2{font-size:15px;margin:12px 0 6px}"
    "nav{display:flex;gap:4px;margin:6px 0 10px}"
    "nav a{flex:1;text-align:center;padding:6px;background:#eee;color:#222;text-decoration:none;border-radius:4px;font-size:13px}"
    "nav a.on{background:#3b82f6;color:#fff}"
    "label{display:block;margin-top:8px}"
    "input{width:100%;padding:7px;font-size:15px;box-sizing:border-box}"
    "button{margin-top:8px;padding:7px 12px;font-size:14px}"
    "small{color:#666;font-size:12px}"
    "table{width:100%;font-size:13px}td:first-child{color:#666;padding-right:8px;width:80px}"
    ".aps{margin-top:6px;border:1px solid #ddd;border-radius:4px;max-height:160px;overflow:auto}"
    ".aps .ap{padding:5px 8px;border-bottom:1px solid #eee;cursor:pointer;display:flex;justify-content:space-between}"
    ".aps .ap:last-child{border-bottom:0}.aps .ap:hover{background:#f3f7ff}"
    ".lk{color:#888;font-size:12px}.msg{color:#888;font-size:12px;margin-top:4px}"
    ".danger{color:#c00}"
    "</style></head><body>";

// 注意: nav 里 %s 用于把当前页 class 标成 'on'
static const char k_nav[] =
    "<nav>"
    "<a href='/' class='%s'>主页</a>"
    "<a href='/wifi' class='%s'>WiFi</a>"
    "<a href='/ota' class='%s'>OTA</a>"
    "<a href='/tools' class='%s'>高级</a>"
    "</nav>";

static const char k_foot[] = "</body></html>";

// 主页: 显示设备号 / 当前 IP / TCP 端口 / 简短说明
static const char k_home_body[] =
    "<h1>ESP32 Claude</h1>"
    "<table>"
    "<tr><td>设备号</td><td><b>%s</b></td></tr>"
    "<tr><td>当前 IP</td><td>%s</td></tr>"
    "<tr><td>TCP 端口</td><td>%d</td></tr>"
    "<tr><td>模式</td><td>%s</td></tr>"
    "</table>"
    "<p><small>UDP 发现端口固定为 8266; PC 桥按设备号自动发现后连接 TCP.</small></p>";

// WiFi 页: 扫描 + SSID/PSK 表单
static const char k_wifi_body[] =
    "<h1>WiFi 配网</h1>"
    "<form method='POST' action='/wificfg'>"
    "<label>SSID<input id='s' name='ssid' maxlength='32' required></label>"
    "<button type='button' onclick='scan()'>扫描</button>"
    "<div class='msg' id='m'></div><div class='aps' id='d'></div>"
    "<label>密码<input name='pass' type='password' maxlength='64'></label>"
    "<button type='submit'>保存并连接</button></form>"
    "<script>"
    "function b(r){return r>=-55?'\xe2\x96\x88\xe2\x96\x88\xe2\x96\x88\xe2\x96\x88':r>=-65?'\xe2\x96\x88\xe2\x96\x88\xe2\x96\x88\xe2\x96\x91':r>=-75?'\xe2\x96\x88\xe2\x96\x88\xe2\x96\x91\xe2\x96\x91':'\xe2\x96\x88\xe2\x96\x91\xe2\x96\x91\xe2\x96\x91';}"
    "async function scan(){var m=document.getElementById('m'),d=document.getElementById('d');"
    "m.textContent='扫描中...';d.innerHTML='';"
    "try{var j=await(await fetch('/wifiscan')).json();"
    "if(!j.length){m.textContent='未发现 AP';return;}"
    "m.textContent='发现 '+j.length+' 个, 点击选择';"
    "j.forEach(function(a){var e=document.createElement('div');e.className='ap';"
    "e.innerHTML='<span>'+(a.a>0?'\xf0\x9f\x94\x92':'')+a.s+'</span><span class=lk>'+b(a.r)+' '+a.r+'</span>';"
    "e.onclick=function(){document.getElementById('s').value=a.s;};d.appendChild(e);});}"
    "catch(e){m.textContent='扫描失败: '+e;}}"
    "</script>";

// 高级页: 清除凭据等危险动作 (固件相关已拆到 /ota)
static const char k_tools_body[] =
    "<h1>高级</h1>"
    "<h2>WiFi 凭据</h2>"
    "<p>清除后设备会重启回到 AP 配网模式 (热点: %s).</p>"
    "<form method='POST' action='/wificlear'>"
    "<button class='danger' type='submit'>清除已保存 WiFi 凭据</button></form>"
    "<p><small>固件升级/地址配置/本地上传请前往 <a href='/ota'>OTA</a> 页面.</small></p>";

// OTA 专页: 状态 / 切换 / 检查 / 地址编辑 / 本地上传
static const char k_ota_body[] =
    "<h1>固件升级 OTA</h1>"
    "<h2>状态</h2>"
    "<div id='ota' class='msg'>加载中...</div>"
    "<progress id='otaProg' max='100' value='0' style='display:none;width:100%;height:20px;margin-top:5px'></progress>"
    "<div id='otaProgMsg' style='text-align:center;font-size:.9em;color:#666;display:none'></div>"
    "<button onclick='otaToggle()'>切换自动更新</button>"
    "<button onclick='otaCheck()'>立即检查更新</button>"
    "<h2>升级地址</h2>"
    "<label>主地址<input id='urlP' type='url' placeholder='https://...'/></label>"
    "<label>备用地址<input id='urlB' type='url' placeholder='https://...'/></label>"
    "<button onclick='urlSave()'>保存地址</button>"
    "<button class='danger' onclick='urlReset()'>恢复默认</button>"
    "<div id='urlMsg' class='msg'>加载中...</div>"
    "<h2>本地上传升级</h2>"
    "<p><small>选择固件文件 (bin 格式).</small></p>"
    "<input type='file' id='fwfile' accept='.bin'/>"
    "<button id='upbtn' onclick='otaUpload()'>上传并升级</button>"
    "<progress id='upprog' max='100' value='0'></progress>"
    "<div id='upmsg' class='msg'>就绪</div>"
    "<script>"
    "var otaTimer=null;"
    "function otaPoll(){"
    "fetch('/api/ota/status?_='+Date.now()).then(function(r){return r.json()}).then(function(d){"
    "var p=document.getElementById('otaProg'),m=document.getElementById('otaProgMsg');"
    "if(d.downloading){"
    "p.style.display='block';m.style.display='block';"
    "p.value=d.progress||0;"
    "m.textContent=(d.progress_msg||'')+' ('+(d.progress||0)+'%)';"
    "}"
    "if(d.progress>=100&&d.downloading){"
    "clearInterval(otaTimer);otaTimer=null;"
    "document.getElementById('ota').innerHTML='\xe2\x9c\x85 \xe5\x8d\x87\xe7\xba\xa7\xe5\xae\x8c\xe6\x88\x90\xef\xbc\x8c\xe8\xae\xbe\xe5\xa4\x87\xe5\xb0\x86\xe9\x87\x8d\xe5\x90\xaf...';"
    "setTimeout(function(){p.style.display='none';m.style.display='none';},3000);"
    "p.value=100;"
    "}"
    "if(!d.downloading&&d.progress<100&&otaTimer){clearInterval(otaTimer);otaTimer=null;otaStatus();}"
    "}).catch(function(){})}"
    "async function otaStatus(){try{var d=await(await fetch('/api/ota/status?_='+Date.now())).json();"
    "var html='\xe7\x89\x88\xe6\x9c\xac: '+d.version+"
    "'<br>\xe8\x87\xaa\xe5\x8a\xa8\xe6\x9b\xb4\xe6\x96\xb0: '+(d.auto_update?'\xe2\x9c\x85 \xe5\xbc\x80':'\xe2\x9d\x8c \xe5\x85\xb3')+"
    "'<br>\xe6\x9c\x80\xe8\xbf\x91: '+d.last_status;"
    "if(d.downloading){"
    "var p=document.getElementById('otaProg'),m=document.getElementById('otaProgMsg');"
    "p.style.display='block';m.style.display='block';"
    "p.value=d.progress||0;"
    "m.textContent=(d.progress_msg||'')+' ('+(d.progress||0)+'%)';"
    "if(!otaTimer)otaTimer=setInterval(otaPoll,1000);"
    "}"
    "if(d.pending_update&&d.pending_update.waiting_confirmation){"
    "html+='<br><span style=\"color:red\">\xe6\x96\xb0\xe7\x89\x88\xe6\x9c\xac: '+d.pending_update.version+'</span>';"
    "html+='<br><button onclick=\"confirmOtaUpdate()\">\xe7\xa1\xae\xe8\xae\xa4\xe5\x8d\x87\xe7\xba\xa7</button>';}"
    "document.getElementById('ota').innerHTML=html;}"
    "catch(e){document.getElementById('ota').textContent='\xe6\x9f\xa5\xe8\xaf\xa2\xe5\xa4\xb1\xe8\xb4\xa5';}}"
    "async function otaToggle(){var d=await(await fetch('/api/ota/status?_='+Date.now())).json();"
    "await fetch('/api/ota/enable',{method:'POST',headers:{'Content-Type':'application/json'},"
    "body:JSON.stringify({enabled:!d.auto_update})});otaStatus();}"
    "async function otaCheck(){var d=await(await fetch('/api/ota/check_now',{method:'POST'})).json();"
    "alert(d.message||'\xe5\xb7\xb2\xe8\xaf\xb7\xe6\xb1\x82');setTimeout(otaStatus,3000);}"
    "async function confirmOtaUpdate(){"
    "if(!confirm('\xe7\xa1\xae\xe8\xae\xa4\xe5\x8d\x87\xe7\xba\xa7\x3f'))return;"
    "var p=document.getElementById('otaProg'),m=document.getElementById('otaProgMsg');"
    "p.style.display='block';m.style.display='block';p.value=0;"
    "m.textContent='\xe6\xad\xa3\xe5\x9c\xa8\xe5\x90\xaf\xe5\x8a\xa8\xe4\xb8\x8b\xe8\xbd\xbd...';"
    "document.getElementById('ota').innerHTML='\xe2\x8f\xb3 \xe4\xb8\x8b\xe8\xbd\xbd\xe4\xb8\xad...';"
    "fetch('/api/ota/confirm',{method:'POST',headers:{'Content-Type':'application/json'},body:'{}'})"
    ".then(function(r){return r.json()}).then(function(d){"
    "if(d.ok){if(!otaTimer)otaTimer=setInterval(otaPoll,1000);}"
    "else{alert('\xe5\xa4\xb1\xe8\xb4\xa5: '+d.message);otaStatus()}"
    "}).catch(function(e){alert('\xe9\x94\x99\xe8\xaf\xaf: '+e.message);otaStatus()})"
    "}"
    "function otaUpload(){"
    "var f=document.getElementById('fwfile').files[0];"
    "if(!f){alert('\xe8\xaf\xb7\xe5\x85\x88\xe9\x80\x89\xe6\x8b\xa9 .bin \xe6\x96\x87\xe4\xbb\xb6');return;}"
    "var m=document.getElementById('upmsg'),p=document.getElementById('upprog'),b=document.getElementById('upbtn');"
    "b.disabled=true;m.textContent='\xe4\xb8\x8a\xe4\xbc\xa0\xe4\xb8\xad: '+f.name+' ('+f.size+' B)';p.value=0;"
    "var x=new XMLHttpRequest();x.open('POST','/api/ota/upload');"
    "x.setRequestHeader('Content-Type','application/octet-stream');"
    "x.upload.onprogress=function(e){if(e.lengthComputable)p.value=e.loaded*100/e.total;};"
    "x.onload=function(){m.textContent=x.status+': '+x.responseText;b.disabled=false;"
    "if(x.status>=200&&x.status<300)setTimeout(function(){m.textContent+=' \xe2\x9c\x85 \xe8\xae\xbe\xe5\xa4\x87\xe9\x87\x8d\xe5\x90\xaf\xe4\xb8\xad...';},800);};"
    "x.onerror=function(){m.textContent='\xe4\xbc\xa0\xe8\xbe\x93\xe9\x94\x99\xe8\xaf\xaf';b.disabled=false;};"
    "x.send(f);}"
    "async function urlLoad(){try{var d=await(await fetch('/api/ota/urls')).json();"
    "document.getElementById('urlP').value=d.primary||'';"
    "document.getElementById('urlB').value=d.backup||'';"
    "document.getElementById('urlMsg').innerHTML='';"
    "}catch(e){document.getElementById('urlMsg').textContent='\xe5\x8a\xa0\xe8\xbd\xbd\xe5\xa4\xb1\xe8\xb4\xa5';}}"
    "async function urlSave(){var p=document.getElementById('urlP').value.trim(),b=document.getElementById('urlB').value.trim();"
    "if(!p||!b){alert('\xe4\xb8\xa4\xe4\xb8\xaa\xe5\x9c\xb0\xe5\x9d\x80\xe9\x83\xbd\xe8\xa6\x81\xe5\xa1\xab');return;}"
    "var r=await fetch('/api/ota/urls',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({primary:p,backup:b})});"
    "var d=await r.json();document.getElementById('urlMsg').textContent=d.ok?'\xe2\x9c\x85 \xe5\xb7\xb2\xe4\xbf\x9d\xe5\xad\x98':('\xe2\x9d\x8c '+(d.err||'\xe5\xa4\xb1\xe8\xb4\xa5'));}"
    "async function urlReset(){if(!confirm('\xe6\x81\xa2\xe5\xa4\x8d\xe7\xbc\x96\xe8\xaf\x91\xe9\xbb\x98\xe8\xae\xa4\xe5\x9c\xb0\xe5\x9d\x80?'))return;"
    "await fetch('/api/ota/urls/reset',{method:'POST'});urlLoad();}"
    "urlLoad();otaStatus();"
    "</script>";

// 分块发送页面: head + nav + body + foot. 各块都用栈上小缓冲 (<512B), 安全.
typedef enum {
    PAGE_HOME = 0,
    PAGE_WIFI,
    PAGE_OTA,
    PAGE_TOOLS,
} page_id_t;

static esp_err_t send_page_chunks(httpd_req_t *req, page_id_t page, const char *body_fmt,
                                  const char *a1, const char *a2, int n3, const char *a4)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");

    // 头
    if (httpd_resp_send_chunk(req, k_head, sizeof(k_head) - 1) != ESP_OK) return ESP_FAIL;

    // 导航 (当前页 'on' 高亮)
    char nav[sizeof(k_nav) + 24];
    snprintf(nav, sizeof(nav), k_nav,
             page == PAGE_HOME  ? "on" : "",
             page == PAGE_WIFI  ? "on" : "",
             page == PAGE_OTA   ? "on" : "",
             page == PAGE_TOOLS ? "on" : "");
    if (httpd_resp_send_chunk(req, nav, HTTPD_RESP_USE_STRLEN) != ESP_OK) return ESP_FAIL;

    // 正文 (按页变长, 仍放堆; OTA 页含完整脚本约 ~3.0 KB)
    enum { BODY_BUF = 6144 };
    char *body = malloc(BODY_BUF);
    if (!body) {
        httpd_resp_send_chunk(req, NULL, 0);
        return ESP_FAIL;
    }
    if (page == PAGE_HOME) {
        snprintf(body, BODY_BUF, body_fmt, a1, a2, n3, a4 ? a4 : "", a1);
    } else if (page == PAGE_TOOLS) {
        snprintf(body, BODY_BUF, body_fmt, a1);  // a1 = AP SSID
    } else {
        // wifi/ota 页正文是纯静态 (没有服务端注入)
        snprintf(body, BODY_BUF, "%s", body_fmt);
    }
    esp_err_t err = httpd_resp_send_chunk(req, body, HTTPD_RESP_USE_STRLEN);
    free(body);
    if (err != ESP_OK) return err;

    // 尾
    if (httpd_resp_send_chunk(req, k_foot, sizeof(k_foot) - 1) != ESP_OK) return ESP_FAIL;
    return httpd_resp_send_chunk(req, NULL, 0);  // end
}

static esp_err_t home_get_handler(httpd_req_t *req)
{
    const char *mode = s_ap_mode ? "AP 配网" : "STA 已连接";
    return send_page_chunks(req, PAGE_HOME, k_home_body,
                            s_device_id, s_my_ip_str, CLAUDE_WIFI_TCP_PORT, mode);
}

static esp_err_t wifi_get_handler(httpd_req_t *req)
{
    return send_page_chunks(req, PAGE_WIFI, k_wifi_body, NULL, NULL, 0, NULL);
}

static esp_err_t tools_get_handler(httpd_req_t *req)
{
    return send_page_chunks(req, PAGE_TOOLS, k_tools_body, s_dev_name, NULL, 0, NULL);
}

static esp_err_t ota_get_handler(httpd_req_t *req)
{
    return send_page_chunks(req, PAGE_OTA, k_ota_body, NULL, NULL, 0, NULL);
}

// JSON-escape: " 和 \, 控制字符过滤掉
static size_t json_escape(const char *in, size_t in_len, char *out, size_t out_size)
{
    size_t j = 0;
    for (size_t i = 0; i < in_len && j + 2 < out_size; i++) {
        unsigned char c = (unsigned char)in[i];
        if (c == '"' || c == '\\') {
            if (j + 2 >= out_size) break;
            out[j++] = '\\';
            out[j++] = (char)c;
        } else if (c < 0x20) {
            // 跳过控制字符
            continue;
        } else {
            out[j++] = (char)c;
        }
    }
    out[j] = '\0';
    return j;
}

static esp_err_t scan_get_handler(httpd_req_t *req)
{
    // 触发一次阻塞扫描 (passive 不一定能扫到隐藏 AP; 用 active)
    wifi_scan_config_t cfg = {0};
    cfg.scan_type = WIFI_SCAN_TYPE_ACTIVE;
    cfg.scan_time.active.min = 50;
    cfg.scan_time.active.max = 150;

    esp_err_t err = esp_wifi_scan_start(&cfg, true);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "scan_start failed: %s", esp_err_to_name(err));
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_send(req, "[]", 2);
    }

    uint16_t n = 0;
    esp_wifi_scan_get_ap_num(&n);
    if (n > 24) n = 24;  // 限制结果数, 避免大缓冲

    wifi_ap_record_t *recs = NULL;
    if (n > 0) {
        recs = calloc(n, sizeof(*recs));
        if (!recs) {
            esp_wifi_clear_ap_list();
            httpd_resp_set_type(req, "application/json");
            return httpd_resp_send(req, "[]", 2);
        }
        esp_wifi_scan_get_ap_records(&n, recs);
    } else {
        esp_wifi_clear_ap_list();
    }

    // 拼 JSON: [{"s":"ssid","r":-50,"a":3}, ...]
    // 单条最大约 32(ssid escape) + 25(其它) = ~60. 留 2KB 安全余量. 走堆避免栈溢出.
    enum { JSON_BUF = 2048 };
    char *json = malloc(JSON_BUF);
    if (!json) {
        free(recs);
        esp_wifi_clear_ap_list();
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_send(req, "[]", 2);
    }
    size_t pos = 0;
    json[pos++] = '[';

    // 去重 SSID (同 SSID 多 BSSID 只保留信号最好的)
    for (uint16_t i = 0; i < n; i++) {
        if (recs[i].ssid[0] == '\0') continue;  // 隐藏 SSID 跳过
        bool dup = false;
        for (uint16_t k = 0; k < i; k++) {
            if (strncmp((char *)recs[i].ssid, (char *)recs[k].ssid,
                        sizeof(recs[i].ssid)) == 0 && recs[k].ssid[0] != '\0') {
                dup = true;
                break;
            }
        }
        if (dup) continue;

        char esc[80];
        size_t ssid_len = strnlen((char *)recs[i].ssid, sizeof(recs[i].ssid));
        json_escape((const char *)recs[i].ssid, ssid_len, esc, sizeof(esc));

        int wrote = snprintf(json + pos, JSON_BUF - pos,
                             "%s{\"s\":\"%s\",\"r\":%d,\"a\":%d}",
                             pos > 1 ? "," : "",
                             esc, recs[i].rssi, (int)recs[i].authmode);
        if (wrote < 0 || (size_t)wrote >= JSON_BUF - pos) break;
        pos += wrote;
    }
    if (pos + 1 < JSON_BUF) json[pos++] = ']';
    json[pos] = '\0';

    free(recs);
    esp_wifi_clear_ap_list();

    httpd_resp_set_type(req, "application/json");
    esp_err_t send_err = httpd_resp_send(req, json, pos);
    free(json);
    return send_err;
}

// 简陋 URL 解码 (够用即可: + -> space, %XX -> byte)
static void url_decode(const char *src, char *dst, size_t dst_size)
{
    size_t i = 0, j = 0;
    while (src[i] && j + 1 < dst_size) {
        if (src[i] == '+') { dst[j++] = ' '; i++; }
        else if (src[i] == '%' && src[i+1] && src[i+2]) {
            char hex[3] = { src[i+1], src[i+2], 0 };
            dst[j++] = (char)strtol(hex, NULL, 16);
            i += 3;
        } else {
            dst[j++] = src[i++];
        }
    }
    dst[j] = '\0';
}

static esp_err_t cfg_post_handler(httpd_req_t *req)
{
    char body[256];
    int total = req->content_len < (int)sizeof(body) - 1 ? req->content_len : (int)sizeof(body) - 1;
    int r = httpd_req_recv(req, body, total);
    if (r <= 0) return ESP_FAIL;
    body[r] = '\0';

    char ssid_raw[64] = {0}, pass_raw[128] = {0};
    httpd_query_key_value(body, "ssid", ssid_raw, sizeof(ssid_raw));
    httpd_query_key_value(body, "pass", pass_raw, sizeof(pass_raw));

    char ssid[64], pass[128];
    url_decode(ssid_raw, ssid, sizeof(ssid));
    url_decode(pass_raw, pass, sizeof(pass));

    if (!ssid[0]) {
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_send(req, "ssid required", HTTPD_RESP_USE_STRLEN);
    }

    esp_err_t err = claude_wifi_creds_save(ssid, pass);
    if (err != ESP_OK) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        return httpd_resp_send(req, "save failed", HTTPD_RESP_USE_STRLEN);
    }

    const char *msg = "<html><body><p>已保存. 设备将重启以连接新的 WiFi.</p></body></html>";
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_send(req, msg, HTTPD_RESP_USE_STRLEN);

    ESP_LOGI(TAG, "Creds saved, rebooting in 1s...");
    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();
    return ESP_OK; // unreachable
}

static esp_err_t cfg_clear_handler(httpd_req_t *req)
{
    claude_wifi_creds_clear();
    const char *msg = "<html><body><p>已清除. 设备将重启回到 AP 配网模式.</p></body></html>";
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_send(req, msg, HTTPD_RESP_USE_STRLEN);
    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();
    return ESP_OK;
}

static esp_err_t start_http(void)
{
    if (s_httpd) return ESP_OK;
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.lru_purge_enable = true;
    cfg.max_uri_handlers = 20;  // 7 builtin + 9 OTA endpoints (status/enable/check/confirm/upload_page/upload/urls{get,set,reset}); headroom for growth.
    cfg.stack_size = 6144;  // 默认 4 KB, snprintf 路径 + chunk send 时容易溢出, 抬到 6 KB
    esp_err_t err = httpd_start(&s_httpd, &cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start failed: %s", esp_err_to_name(err));
        return err;
    }
    static const httpd_uri_t u_root = {
        .uri = "/", .method = HTTP_GET, .handler = home_get_handler
    };
    static const httpd_uri_t u_wifi = {
        .uri = "/wifi", .method = HTTP_GET, .handler = wifi_get_handler
    };
    static const httpd_uri_t u_wifi_alias = {
        .uri = "/wificfg", .method = HTTP_GET, .handler = wifi_get_handler
    };
    static const httpd_uri_t u_tools = {
        .uri = "/tools", .method = HTTP_GET, .handler = tools_get_handler
    };
    static const httpd_uri_t u_ota_page = {
        .uri = "/ota",   .method = HTTP_GET, .handler = ota_get_handler
    };
    static const httpd_uri_t u_post = {
        .uri = "/wificfg", .method = HTTP_POST, .handler = cfg_post_handler
    };
    static const httpd_uri_t u_clear = {
        .uri = "/wificlear", .method = HTTP_POST, .handler = cfg_clear_handler
    };
    static const httpd_uri_t u_scan = {
        .uri = "/wifiscan", .method = HTTP_GET, .handler = scan_get_handler
    };
    httpd_register_uri_handler(s_httpd, &u_root);
    httpd_register_uri_handler(s_httpd, &u_wifi);
    httpd_register_uri_handler(s_httpd, &u_wifi_alias);
    httpd_register_uri_handler(s_httpd, &u_tools);
    httpd_register_uri_handler(s_httpd, &u_ota_page);
    httpd_register_uri_handler(s_httpd, &u_post);
    httpd_register_uri_handler(s_httpd, &u_clear);
    httpd_register_uri_handler(s_httpd, &u_scan);
    ota_updater_register_http_handlers(s_httpd);
    ESP_LOGI(TAG, "HTTP provisioning server up");
    return ESP_OK;
}

// -----------------------------------------------------------------------------
// UDP 监听 / 协议
// -----------------------------------------------------------------------------
static void try_handle_query(const char *json, struct sockaddr_in *peer)
{
    char qv[16];
    if (!extract_json_string_field(json, "q", qv, sizeof(qv))) return;
    bool is_discover = strcasecmp(qv, "discover") == 0;
    bool is_ping     = strcasecmp(qv, "ping") == 0;
    if (!is_discover && !is_ping) return;

    if (!message_id_matches_device(json)) {
        char id_seen[16] = {0};
        extract_json_string_field(json, "id", id_seen, sizeof(id_seen));
        ESP_LOGD(TAG, "discover/ping id mismatch (asked %s, have %s)", id_seen, s_device_id);
        return;
    }

    char reply[UDP_TX_BUF_LEN];
    int n;
    if (is_discover) {
        n = snprintf(reply, sizeof(reply),
                     "{\"r\":\"discover\",\"id\":\"%s\",\"name\":\"%s\",\"ip\":\"%s\",\"port\":%d,\"udp_port\":%d,\"tcp_port\":%d,\"clients\":%d}\n",
                     s_device_id, s_dev_name, s_my_ip_str,
                     CLAUDE_WIFI_TCP_PORT, CLAUDE_WIFI_UDP_PORT, CLAUDE_WIFI_TCP_PORT,
                     active_client_count());
    } else {
        n = snprintf(reply, sizeof(reply),
                     "{\"r\":\"pong\",\"id\":\"%s\"}\n", s_device_id);
    }
    if (n > 0 && s_udp_sock >= 0) {
        sendto(s_udp_sock, reply, n, 0, (struct sockaddr *)peer, sizeof(*peer));
    }
}

static void handle_tcp_query(tcp_client_t *client, const char *line)
{
    char qv[16];
    if (!extract_json_string_field(line, "q", qv, sizeof(qv))) return;

    if (!message_id_matches_device(line)) {
        close_client_slot(client, "TCP id mismatch");
        return;
    }

    if (strcasecmp(qv, "bind") == 0 || strcasecmp(qv, "ping") == 0) {
        char source[24] = {0};
        if (!extract_json_string_field(line, "source", source, sizeof(source))) {
            extract_json_string_field(line, "name", source, sizeof(source));
        }
        if (source[0]) {
            snprintf(client->name, sizeof(client->name), "%s", source);
        } else if (!client->name[0] && client->ip[0]) {
            snprintf(client->name, sizeof(client->name), "%s", client->ip);
        }
        client->last_rx_tick = xTaskGetTickCount();
        if (strcasecmp(qv, "bind") == 0) {
            claude_ble_mode_set_ready_msg("TCP linked");
        }
        update_link_label();
    }
}

static void handle_tcp_status_line(tcp_client_t *client, const char *line)
{
    client->last_rx_tick = xTaskGetTickCount();
    if (!client->name[0] && client->ip[0]) {
        snprintf(client->name, sizeof(client->name), "%s", client->ip);
    }
    claude_ble_mode_feed_json(line, strlen(line));
    claude_ble_mode_feed_json("\n", 1);
    update_link_label();
}

static void handle_tcp_client_data(tcp_client_t *client, const char *buf, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        char c = buf[i];
        if (c == '\r') continue;
        if (c == '\n' || c == '\0') {
            if (client->rx_len > 0) {
                client->rx_buf[client->rx_len] = '\0';
                if (strstr(client->rx_buf, "\"q\"")) {
                    handle_tcp_query(client, client->rx_buf);
                    if (client->fd < 0) return;
                } else {
                    handle_tcp_status_line(client, client->rx_buf);
                }
                client->rx_len = 0;
            }
            continue;
        }

        if (client->rx_len + 1 < sizeof(client->rx_buf)) {
            client->rx_buf[client->rx_len++] = c;
        } else {
            ESP_LOGW(TAG, "TCP RX overflow from %s", client->ip);
            client->rx_len = 0;
        }
    }

    if (client->rx_len > 0 && client->rx_buf[client->rx_len - 1] == '}') {
        client->rx_buf[client->rx_len] = '\0';
        if (strstr(client->rx_buf, "\"q\"")) {
            handle_tcp_query(client, client->rx_buf);
        } else {
            handle_tcp_status_line(client, client->rx_buf);
        }
        client->rx_len = 0;
    }
}

static void accept_tcp_client(void)
{
    struct sockaddr_in peer = {0};
    socklen_t plen = sizeof(peer);
    int fd = accept(s_tcp_listen_sock, (struct sockaddr *)&peer, &plen);
    if (fd < 0) {
        ESP_LOGW(TAG, "accept errno=%d", errno);
        return;
    }

    tcp_client_t *slot = NULL;
    for (size_t i = 0; i < TCP_MAX_CLIENTS; i++) {
        if (s_clients[i].fd < 0) {
            slot = &s_clients[i];
            break;
        }
    }
    if (!slot) {
        ESP_LOGW(TAG, "too many TCP clients, reject new connection");
        shutdown(fd, SHUT_RDWR);
        close(fd);
        return;
    }

    reset_client_slot(slot);
    slot->fd = fd;
    slot->last_rx_tick = xTaskGetTickCount();
    inet_ntoa_r(peer.sin_addr, slot->ip, sizeof(slot->ip));
    strncpy(slot->name, slot->ip, sizeof(slot->name) - 1);
    slot->name[sizeof(slot->name) - 1] = '\0';
    snprintf(s_peer_ip_str, sizeof(s_peer_ip_str), "%s", slot->ip);
    configure_tcp_socket(fd);
    ESP_LOGI(TAG, "TCP client connected: %s", slot->ip);
    update_link_label();
}

static void network_task(void *param)
{
    (void)param;
    char udp_buf[UDP_RX_BUF_LEN];

    while (!s_stop_req) {
        s_udp_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (s_udp_sock < 0) {
            ESP_LOGE(TAG, "socket() failed errno=%d", errno);
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }
        int yes = 1;
        setsockopt(s_udp_sock, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
        struct sockaddr_in addr = {
            .sin_family = AF_INET,
            .sin_port = htons(CLAUDE_WIFI_UDP_PORT),
            .sin_addr.s_addr = htonl(INADDR_ANY),
        };
        if (bind(s_udp_sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
            ESP_LOGE(TAG, "bind(%d) failed errno=%d", CLAUDE_WIFI_UDP_PORT, errno);
            close(s_udp_sock);
            s_udp_sock = -1;
            vTaskDelay(pdMS_TO_TICKS(2000));
            continue;
        }
        // 100ms recv 超时, 便于响应停止请求
        struct timeval tv = { .tv_sec = 0, .tv_usec = 100 * 1000 };
        setsockopt(s_udp_sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

        s_tcp_listen_sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (s_tcp_listen_sock < 0) {
            ESP_LOGE(TAG, "tcp socket() failed errno=%d", errno);
            close(s_udp_sock);
            s_udp_sock = -1;
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }
        setsockopt(s_tcp_listen_sock, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
        struct sockaddr_in tcp_addr = {
            .sin_family = AF_INET,
            .sin_port = htons(CLAUDE_WIFI_TCP_PORT),
            .sin_addr.s_addr = htonl(INADDR_ANY),
        };
        if (bind(s_tcp_listen_sock, (struct sockaddr *)&tcp_addr, sizeof(tcp_addr)) < 0) {
            ESP_LOGE(TAG, "tcp bind(%d) failed errno=%d", CLAUDE_WIFI_TCP_PORT, errno);
            close(s_tcp_listen_sock);
            close(s_udp_sock);
            s_tcp_listen_sock = -1;
            s_udp_sock = -1;
            vTaskDelay(pdMS_TO_TICKS(2000));
            continue;
        }
        if (listen(s_tcp_listen_sock, CLAUDE_TCP_ACCEPT_BACKLOG) < 0) {
            ESP_LOGE(TAG, "listen(%d) failed errno=%d", CLAUDE_WIFI_TCP_PORT, errno);
            close(s_tcp_listen_sock);
            close(s_udp_sock);
            s_tcp_listen_sock = -1;
            s_udp_sock = -1;
            vTaskDelay(pdMS_TO_TICKS(2000));
            continue;
        }

        ESP_LOGI(TAG, "UDP discovery on 0.0.0.0:%d, TCP status on 0.0.0.0:%d (devid=%s)",
                 CLAUDE_WIFI_UDP_PORT, CLAUDE_WIFI_TCP_PORT, s_device_id);

        while (!s_stop_req) {
            fd_set rfds;
            FD_ZERO(&rfds);
            int maxfd = -1;

            if (s_udp_sock >= 0) {
                FD_SET(s_udp_sock, &rfds);
                maxfd = s_udp_sock;
            }
            if (s_tcp_listen_sock >= 0) {
                FD_SET(s_tcp_listen_sock, &rfds);
                if (s_tcp_listen_sock > maxfd) maxfd = s_tcp_listen_sock;
            }
            for (size_t i = 0; i < TCP_MAX_CLIENTS; i++) {
                if (s_clients[i].fd >= 0) {
                    FD_SET(s_clients[i].fd, &rfds);
                    if (s_clients[i].fd > maxfd) maxfd = s_clients[i].fd;
                }
            }

            struct timeval sel_tv = { .tv_sec = 0, .tv_usec = 250 * 1000 };
            int ready = select(maxfd + 1, &rfds, NULL, NULL, &sel_tv);
            if (ready < 0) {
                if (errno == EINTR) continue;
                ESP_LOGW(TAG, "select errno=%d", errno);
                break;
            }

            if (ready > 0 && s_udp_sock >= 0 && FD_ISSET(s_udp_sock, &rfds)) {
                struct sockaddr_in peer = {0};
                socklen_t plen = sizeof(peer);
                int n = recvfrom(s_udp_sock, udp_buf, sizeof(udp_buf) - 1, 0,
                                 (struct sockaddr *)&peer, &plen);
                if (n > 0) {
                    udp_buf[n] = '\0';
                    try_handle_query(udp_buf, &peer);
                    inet_ntoa_r(peer.sin_addr, s_peer_ip_str, sizeof(s_peer_ip_str));
                } else if (!(errno == EAGAIN || errno == EWOULDBLOCK)) {
                    ESP_LOGW(TAG, "recvfrom errno=%d", errno);
                    break;
                }
            }

            if (ready > 0 && s_tcp_listen_sock >= 0 && FD_ISSET(s_tcp_listen_sock, &rfds)) {
                accept_tcp_client();
            }

            for (size_t i = 0; i < TCP_MAX_CLIENTS; i++) {
                tcp_client_t *client = &s_clients[i];
                if (client->fd < 0) continue;
                if (ready > 0 && FD_ISSET(client->fd, &rfds)) {
                    char buf[160];
                    int n = recv(client->fd, buf, sizeof(buf), 0);
                    if (n <= 0) {
                        close_client_slot(client, "TCP disconnected");
                        continue;
                    }
                    handle_tcp_client_data(client, buf, (size_t)n);
                }
            }

            TickType_t now = xTaskGetTickCount();
            for (size_t i = 0; i < TCP_MAX_CLIENTS; i++) {
                tcp_client_t *client = &s_clients[i];
                if (client->fd >= 0 &&
                    (now - client->last_rx_tick) > pdMS_TO_TICKS(TCP_IDLE_TIMEOUT_MS)) {
                    close_client_slot(client, "TCP idle timeout");
                }
            }
        }

        close_all_clients("TCP offline");
        if (s_tcp_listen_sock >= 0) close(s_tcp_listen_sock);
        if (s_udp_sock >= 0) close(s_udp_sock);
        s_tcp_listen_sock = -1;
        s_udp_sock = -1;
    }

    ESP_LOGI(TAG, "network task exit");
    s_net_task = NULL;
    vTaskDelete(NULL);
}

// -----------------------------------------------------------------------------
// LCD 链路标签更新
// -----------------------------------------------------------------------------
static void update_link_label(void)
{
    char buf[48];
    if (s_ap_mode) {
        // AP 模式下 SSID 已含 ID, 显示完整 SSID
        snprintf(buf, sizeof(buf), "AP: %s", s_dev_name);
        claude_ble_mode_set_link_text(buf, 0xFFAA40);
    } else {
        int clients = active_client_count();
        if (clients > 0) {
            tcp_client_t *client = first_active_client();
            const char *who = (client && client->name[0]) ? client->name : s_peer_ip_str;
            snprintf(buf, sizeof(buf), "ID:%s B:%d %s", s_device_id, clients,
                     who && *who ? who : s_my_ip_str);
            claude_ble_mode_set_link_text(buf, 0x00FF80);
        } else {
            // STA 已连接但尚未绑定主机
            snprintf(buf, sizeof(buf), "ID:%s  %s", s_device_id, s_my_ip_str);
            claude_ble_mode_set_link_text(buf, 0x66CCFF);
        }
    }
}

// -----------------------------------------------------------------------------
// 对外 API
// -----------------------------------------------------------------------------
esp_err_t claude_wifi_mode_init(void)
{
    if (s_inited) return ESP_OK;
    compute_device_id();
    s_wifi_evt = xEventGroupCreate();
    if (!s_wifi_evt) return ESP_ERR_NO_MEM;
    for (size_t i = 0; i < TCP_MAX_CLIENTS; i++) reset_client_slot(&s_clients[i]);
    s_inited = true;
    ESP_LOGI(TAG, "Claude WiFi mode ready (device id=%s, name=%s)",
             s_device_id, s_dev_name);
    return ESP_OK;
}

// WiFi/HTTP/UDP 启动是阻塞流程 (STA 最长 15s), 放到独立任务里跑,
// 避免阻塞调用方 (LVGL 任务里如果阻塞 15s, 用户按 BOOT 看不到响应)
static void wifi_setup_task(void *param)
{
    (void)param;
    ESP_LOGI(TAG, "wifi setup task: started");

    if (wifi_netif_event_init_once() != ESP_OK) {
        ESP_LOGE(TAG, "wifi init failed");
        claude_ble_mode_set_link_text("WiFi: INIT FAIL", 0xFF4040);
        vTaskDelete(NULL);
        return;
    }

    char ssid[64] = {0}, pass[64] = {0};
    bool have_creds = (claude_wifi_creds_load(ssid, sizeof(ssid), pass, sizeof(pass)) == ESP_OK
                        && ssid[0] != '\0');

    if (have_creds) {
        claude_ble_mode_set_link_text("WiFi: connecting", 0xFFAA40);
        if (start_sta(ssid, pass) != ESP_OK) {
            if (!s_stop_req) start_ap_provisioning();
        }
    } else if (!s_stop_req) {
        start_ap_provisioning();
    }

    if (!s_stop_req) {
        start_http();
        update_link_label();
        if (!s_net_task) {
            xTaskCreate(network_task, "claude_net", 6144, NULL, 4, &s_net_task);
        }
    }

    ESP_LOGI(TAG, "wifi setup task: done (ap_mode=%d ip=%s)", s_ap_mode, s_my_ip_str);
    vTaskDelete(NULL);
}

void claude_wifi_mode_enter(void)
{
    if (!s_inited) {
        ESP_LOGE(TAG, "enter before init");
        return;
    }
    if (s_active) return;

    s_active = true;
    s_stop_req = false;
    // 这些都是非阻塞的本地操作, 调用方 (LVGL 任务) 立刻返回
    claude_ble_mode_panel_show(true);
    claude_ble_mode_drive_ws2812(true);
    claude_ble_mode_set_ready_msg("Waiting for WiFi host...");
    claude_ble_mode_set_link_text("WiFi: starting", 0xFFAA40);

    // 真正的 WiFi/HTTP/UDP 启动 (含可能 15s 的 STA 等待) 放到后台任务
    xTaskCreate(wifi_setup_task, "wifi_setup", 4096, NULL, 4, NULL);
}

void claude_wifi_mode_exit(void)
{
    if (!s_active) return;
    s_active = false;
    s_stop_req = true;

    if (s_udp_sock >= 0) {
        shutdown(s_udp_sock, 0);
    }
    if (s_tcp_listen_sock >= 0) {
        shutdown(s_tcp_listen_sock, SHUT_RDWR);
    }
    for (size_t i = 0; i < TCP_MAX_CLIENTS; i++) {
        if (s_clients[i].fd >= 0) shutdown(s_clients[i].fd, SHUT_RDWR);
    }
    // 等任务自行退出 (最多 500ms)
    for (int i = 0; i < 50 && s_net_task; i++) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    if (s_httpd) {
        httpd_stop(s_httpd);
        s_httpd = NULL;
    }
    esp_wifi_stop();

    claude_ble_mode_drive_ws2812(false);
    claude_ble_mode_set_link_text(NULL, 0);
    claude_ble_mode_panel_show(false);
}
