/*
 * Claude WiFi 状态模式:
 *   - 与 BLE 路径 (claude_mode.c) 共用 LCD 面板/WS2812 颜色/状态机
 *   - 传输换成 WiFi UDP, 固定端口 CLAUDE_WIFI_UDP_PORT (8266)
 *   - 配网逻辑: NVS 已存 STA 凭据 -> STA; 否则启动 AP 'ESP32_Claude_XXXX'
 *     供 PC 浏览器访问 http://192.168.4.1/wificfg 提交家庭 WiFi 凭据
 *   - 配对: 设备号 = MAC 后 4 位 hex (与 BLE 命名规则一致)
 *           PC 桥广播 {"q":"discover","id":"XXXX"} -> 设备回 {"r":"discover", ...}
 *           随后 PC 单播状态 JSON 到本设备 IP:8266
 *
 * 协议帧 (UDP, line 不强制, 单包一条 JSON):
 *   PC -> ESP   {"q":"discover"}                -> ESP 回 discovery 应答 (向源端口)
 *   PC -> ESP   {"q":"discover","id":"AB12"}    -> id 匹配才回; 不匹配丢弃
 *   PC -> ESP   {"q":"ping"}                    -> ESP 回 {"r":"pong","id":"..."}
 *   PC -> ESP   {状态 JSON, 字段同 BLE 协议}    -> 喂入 claude_mode_feed_json
 *   ESP -> PC   {"r":"discover","id":"AB12","name":"ESP32_Claude_AB12","ip":"1.2.3.4"}
 */

#include "claude_wifi_mode.h"
#include "claude_mode.h"

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

static TaskHandle_t s_udp_task = NULL;
static int s_udp_sock = -1;

static bool s_ap_mode = false;          // 当前是否处于 AP 配网模式
static char s_my_ip_str[16] = "0.0.0.0";

// 最后一次成功通信的对端 IP (用于显示和反馈, 不用于安全)
static char s_peer_ip_str[16] = "";

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
    "<a href='/tools' class='%s'>高级</a>"
    "</nav>";

static const char k_foot[] = "</body></html>";

// 主页: 显示设备号 / 当前 IP / UDP 端口 / 简短说明
static const char k_home_body[] =
    "<h1>ESP32 Claude</h1>"
    "<table>"
    "<tr><td>设备号</td><td><b>%s</b></td></tr>"
    "<tr><td>当前 IP</td><td>%s</td></tr>"
    "<tr><td>UDP 端口</td><td>%d</td></tr>"
    "<tr><td>模式</td><td>%s</td></tr>"
    "</table>"
    "<p><small>PC 桥命令: claude_status_wifi_bridge.py --id %s</small></p>";

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

// 高级页: 清除凭据等危险动作
static const char k_tools_body[] =
    "<h1>高级</h1>"
    "<h2>WiFi 凭据</h2>"
    "<p>清除后设备会重启回到 AP 配网模式 (热点: %s).</p>"
    "<form method='POST' action='/wificlear'>"
    "<button class='danger' type='submit'>清除已保存 WiFi 凭据</button></form>";

// 分块发送页面: head + nav + body + foot. 各块都用栈上小缓冲 (<512B), 安全.
typedef enum {
    PAGE_HOME = 0,
    PAGE_WIFI,
    PAGE_TOOLS,
} page_id_t;

static esp_err_t send_page_chunks(httpd_req_t *req, page_id_t page, const char *body_fmt,
                                  const char *a1, const char *a2, int n3, const char *a4)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");

    // 头
    if (httpd_resp_send_chunk(req, k_head, sizeof(k_head) - 1) != ESP_OK) return ESP_FAIL;

    // 导航 (当前页 'on' 高亮)
    char nav[sizeof(k_nav) + 16];
    snprintf(nav, sizeof(nav), k_nav,
             page == PAGE_HOME  ? "on" : "",
             page == PAGE_WIFI  ? "on" : "",
             page == PAGE_TOOLS ? "on" : "");
    if (httpd_resp_send_chunk(req, nav, HTTPD_RESP_USE_STRLEN) != ESP_OK) return ESP_FAIL;

    // 正文 (按页变长, 仍放堆; 最大 ~1.5 KB)
    enum { BODY_BUF = 1600 };
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
        // wifi 页正文是纯静态
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
    char udp_port_str[12];  // 不使用, 占位
    (void)udp_port_str;
    return send_page_chunks(req, PAGE_HOME, k_home_body,
                            s_device_id, s_my_ip_str, CLAUDE_WIFI_UDP_PORT, mode);
}

static esp_err_t wifi_get_handler(httpd_req_t *req)
{
    return send_page_chunks(req, PAGE_WIFI, k_wifi_body, NULL, NULL, 0, NULL);
}

static esp_err_t tools_get_handler(httpd_req_t *req)
{
    return send_page_chunks(req, PAGE_TOOLS, k_tools_body, s_dev_name, NULL, 0, NULL);
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
    cfg.max_uri_handlers = 10;
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
    httpd_register_uri_handler(s_httpd, &u_post);
    httpd_register_uri_handler(s_httpd, &u_clear);
    httpd_register_uri_handler(s_httpd, &u_scan);
    ESP_LOGI(TAG, "HTTP provisioning server up");
    return ESP_OK;
}

// -----------------------------------------------------------------------------
// UDP 监听 / 协议
// -----------------------------------------------------------------------------
static void try_handle_query(const char *json, struct sockaddr_in *peer)
{
    // 极简 JSON 字段提取 (避免在 UDP 任务里 strdup + cJSON 频繁分配)
    // 支持: "q":"discover", "q":"ping", 可选 "id":"XXXX"
    const char *q = strstr(json, "\"q\"");
    if (!q) return;
    const char *qv = strchr(q, ':');
    if (!qv) return;
    qv++;
    while (*qv == ' ' || *qv == '\"') qv++;
    bool is_discover = strncasecmp(qv, "discover", 8) == 0;
    bool is_ping     = strncasecmp(qv, "ping", 4) == 0;
    if (!is_discover && !is_ping) return;

    // id 过滤 (可选)
    const char *idp = strstr(json, "\"id\"");
    if (idp) {
        const char *iv = strchr(idp, ':');
        if (iv) {
            iv++;
            while (*iv == ' ' || *iv == '\"') iv++;
            char id_seen[8] = {0};
            for (size_t i = 0; i < sizeof(id_seen) - 1 && iv[i] && iv[i] != '\"'; i++) {
                id_seen[i] = iv[i];
            }
            if (id_seen[0] && strcasecmp(id_seen, s_device_id) != 0) {
                ESP_LOGD(TAG, "discover/ping id mismatch (asked %s, have %s)",
                         id_seen, s_device_id);
                return;
            }
        }
    }

    char reply[UDP_TX_BUF_LEN];
    int n;
    if (is_discover) {
        n = snprintf(reply, sizeof(reply),
                     "{\"r\":\"discover\",\"id\":\"%s\",\"name\":\"%s\",\"ip\":\"%s\",\"port\":%d}\n",
                     s_device_id, s_dev_name, s_my_ip_str, CLAUDE_WIFI_UDP_PORT);
    } else {
        n = snprintf(reply, sizeof(reply),
                     "{\"r\":\"pong\",\"id\":\"%s\"}\n", s_device_id);
    }
    if (n > 0 && s_udp_sock >= 0) {
        sendto(s_udp_sock, reply, n, 0, (struct sockaddr *)peer, sizeof(*peer));
    }
}

static void udp_task(void *param)
{
    (void)param;
    char buf[UDP_RX_BUF_LEN];

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

        ESP_LOGI(TAG, "UDP listener bound on 0.0.0.0:%d (devid=%s)",
                 CLAUDE_WIFI_UDP_PORT, s_device_id);

        while (!s_stop_req) {
            struct sockaddr_in peer = {0};
            socklen_t plen = sizeof(peer);
            int n = recvfrom(s_udp_sock, buf, sizeof(buf) - 1, 0,
                             (struct sockaddr *)&peer, &plen);
            if (n <= 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) continue;
                ESP_LOGW(TAG, "recvfrom errno=%d", errno);
                break;
            }
            buf[n] = '\0';

            // 同时支持 "查询/握手" 和 "状态推送"
            // 简单判定: 若帧里出现 "\"q\"" 走 query 处理, 否则当成状态 JSON 喂入
            if (strstr(buf, "\"q\"")) {
                try_handle_query(buf, &peer);
                // 记录最近一次发现的对端用于显示 (不强求)
                inet_ntoa_r(peer.sin_addr, s_peer_ip_str, sizeof(s_peer_ip_str));
            } else {
                // 状态帧: 直接复用 BLE 路径的 JSON 解析 + 应用
                claude_mode_feed_json(buf, n);
                inet_ntoa_r(peer.sin_addr, s_peer_ip_str, sizeof(s_peer_ip_str));
            }
        }

        close(s_udp_sock);
        s_udp_sock = -1;
    }

    ESP_LOGI(TAG, "UDP task exit");
    s_udp_task = NULL;
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
        claude_mode_set_link_text(buf, 0xFFAA40);
    } else {
        // STA 连接后突出显示设备号 (PC 桥 --id 用的就是这个)
        snprintf(buf, sizeof(buf), "ID:%s  %s", s_device_id, s_my_ip_str);
        claude_mode_set_link_text(buf, 0x00FF80);
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
        claude_mode_set_link_text("WiFi: INIT FAIL", 0xFF4040);
        vTaskDelete(NULL);
        return;
    }

    char ssid[64] = {0}, pass[64] = {0};
    bool have_creds = (claude_wifi_creds_load(ssid, sizeof(ssid), pass, sizeof(pass)) == ESP_OK
                        && ssid[0] != '\0');

    if (have_creds) {
        claude_mode_set_link_text("WiFi: connecting", 0xFFAA40);
        if (start_sta(ssid, pass) != ESP_OK) {
            if (!s_stop_req) start_ap_provisioning();
        }
    } else if (!s_stop_req) {
        start_ap_provisioning();
    }

    if (!s_stop_req) {
        start_http();
        update_link_label();
        if (!s_udp_task) {
            xTaskCreate(udp_task, "claude_udp", 4096, NULL, 4, &s_udp_task);
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
    claude_mode_panel_show(true);
    claude_mode_drive_ws2812(true);
    claude_mode_set_ready_msg("Waiting for WiFi host...");
    claude_mode_set_link_text("WiFi: starting", 0xFFAA40);

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
    // 等任务自行退出 (最多 500ms)
    for (int i = 0; i < 50 && s_udp_task; i++) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    if (s_httpd) {
        httpd_stop(s_httpd);
        s_httpd = NULL;
    }
    esp_wifi_stop();

    claude_mode_drive_ws2812(false);
    claude_mode_set_link_text(NULL, 0);
    claude_mode_panel_show(false);
}
