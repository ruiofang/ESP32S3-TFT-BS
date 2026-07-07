#include "web_server.h"
#include "ws2812_control.h"
#include "battery_control.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_task_wdt.h"
#include "nvs_flash.h"
#include "esp_netif.h"
#include "esp_mac.h"
#include "Lib/cJSON/cJSON.h"
#include "ota_updater.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "nvs.h"
#include "lwip/sockets.h"
#include "lwip/inet.h"
#include "lwip/netdb.h"
#include <string.h>
#include <stdlib.h>
#include <errno.h>

static const char *TAG = "WEB_SERVER";
static httpd_handle_t server = NULL;

#define RGB_WIFI_NVS_NS    "rgb_wifi"
#define RGB_WIFI_NVS_SSID  "ssid"
#define RGB_WIFI_NVS_PASS  "pass"
#define RGB_AP_SSID_PREFIX "ESP32_Light_"
#define RGB_STA_CONNECT_TIMEOUT_MS 15000
#define RGB_STA_MAX_RETRY 6
#define RGB_TCP_RX_BUF_LEN 768

#define RGB_WIFI_BIT_GOT_IP BIT0
#define RGB_WIFI_BIT_FAIL   BIT1

static EventGroupHandle_t s_wifi_evt = NULL;
static esp_netif_t *s_netif_sta = NULL;
static esp_netif_t *s_netif_ap = NULL;
static int s_sta_retry = 0;
static bool s_want_sta_connect = false;
static bool s_ap_mode = false;
static char s_light_ssid[32] = WIFI_SSID;
static char s_ip_str[16] = "192.168.4.1";
static TaskHandle_t s_rgb_tcp_task = NULL;
static volatile int s_rgb_tcp_active_clients = 0;

static void compute_light_ssid(void)
{
    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
    snprintf(s_light_ssid, sizeof(s_light_ssid), "%s%02X%02X",
             RGB_AP_SSID_PREFIX, mac[4], mac[5]);
}

static esp_err_t rgb_wifi_creds_save(const char *ssid, const char *pass)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(RGB_WIFI_NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    err = nvs_set_str(h, RGB_WIFI_NVS_SSID, ssid ? ssid : "");
    if (err == ESP_OK) err = nvs_set_str(h, RGB_WIFI_NVS_PASS, pass ? pass : "");
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}

static esp_err_t rgb_wifi_creds_load(char *ssid, size_t ssid_size,
                                     char *pass, size_t pass_size)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(RGB_WIFI_NVS_NS, NVS_READONLY, &h);
    if (err != ESP_OK) return err;
    size_t len = ssid_size;
    err = nvs_get_str(h, RGB_WIFI_NVS_SSID, ssid, &len);
    if (err == ESP_OK) {
        len = pass_size;
        esp_err_t pass_err = nvs_get_str(h, RGB_WIFI_NVS_PASS, pass, &len);
        if (pass_err == ESP_ERR_NVS_NOT_FOUND) {
            pass[0] = '\0';
        } else if (pass_err != ESP_OK) {
            err = pass_err;
        }
    }
    nvs_close(h);
    return err;
}

static esp_err_t rgb_wifi_creds_clear(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(RGB_WIFI_NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    nvs_erase_key(h, RGB_WIFI_NVS_SSID);
    nvs_erase_key(h, RGB_WIFI_NVS_PASS);
    err = nvs_commit(h);
    nvs_close(h);
    return err;
}

static void url_decode(const char *src, char *dst, size_t dst_size)
{
    size_t i = 0, j = 0;
    while (src[i] && j + 1 < dst_size) {
        if (src[i] == '+') {
            dst[j++] = ' ';
            i++;
        } else if (src[i] == '%' && src[i + 1] && src[i + 2]) {
            char hex[3] = {src[i + 1], src[i + 2], 0};
            dst[j++] = (char)strtol(hex, NULL, 16);
            i += 3;
        } else {
            dst[j++] = src[i++];
        }
    }
    dst[j] = '\0';
}

static size_t json_escape(const char *in, size_t in_len, char *out, size_t out_size)
{
    size_t j = 0;
    for (size_t i = 0; i < in_len && j + 2 < out_size; i++) {
        unsigned char c = (unsigned char)in[i];
        if (c == '"' || c == '\\') {
            out[j++] = '\\';
            out[j++] = (char)c;
        } else if (c >= 0x20) {
            out[j++] = (char)c;
        }
    }
    out[j] = '\0';
    return j;
}

static inline void web_server_task_wdt_reset_if_registered(void)
{
    TaskHandle_t current_task = xTaskGetCurrentTaskHandle();
    esp_err_t status = esp_task_wdt_status(current_task);
    if (status == ESP_OK) {
        esp_task_wdt_reset();
    }
}

const char *web_server_get_ip(void)
{
    return s_ip_str;
}

bool web_server_is_sta_connected(void)
{
    return !s_ap_mode && strcmp(s_ip_str, "0.0.0.0") != 0 && strcmp(s_ip_str, "192.168.4.1") != 0;
}

bool web_server_rgb_tcp_client_connected(void)
{
    return s_rgb_tcp_active_clients > 0;
}

// 完整的HTML网页内容
static const char* complete_html_page = 
"<!DOCTYPE html>"
"<html>"
"<head>"
"<meta charset='UTF-8'>"
"<meta name='viewport' content='width=device-width, initial-scale=1.0'>"
"<title>ESP32S3-TFT-BS 智能控制面板</title>"
"<style>"
"* { box-sizing: border-box; }"
"body { font-family: Arial, sans-serif; margin: 20px; background: #f0f0f0; }"
".container { width: 100%; max-width: 800px; margin: 0 auto; background: white; padding: 20px; border-radius: 10px; box-shadow: 0 2px 10px rgba(0,0,0,0.1); }"
".channel-group { width: 100%; border: 2px solid #ddd; border-radius: 10px; margin: 15px 0; padding: 15px; }"
".channel-title { font-size: 18px; font-weight: bold; color: #333; margin-bottom: 10px; }"
".control-row { display: flex; align-items: center; margin: 10px 0; gap: 15px; flex-wrap: wrap; }"
".control-row.center { justify-content: center; }"
".control-label { min-width: 80px; font-weight: bold; }"
"select, input[type=range], input[type=number], button { padding: 8px; border-radius: 5px; border: 1px solid #ccc; }"
"button { background: #007bff; color: white; border: none; cursor: pointer; padding: 10px 20px; }"
"button:hover { background: #0056b3; }"
"button:disabled { background: #ccc; cursor: not-allowed; }"
".color-preview { width: 40px; height: 40px; border-radius: 50%; border: 2px solid #ddd; display: inline-block; margin-left: 10px; }"
".range-value { min-width: 50px; text-align: center; font-weight: bold; }"
".broadcast-section { background: #e8f5e8; }"
".channel-section { background: #f8f9fa; }"
".battery-section { background: #fff3cd; }"
".voltage-section { background: #cce5ff; }"
".advanced-section { background: #f0f0f0; }"
".status-info { background: #d4edda; border-left: 4px solid #28a745; padding: 10px; margin: 10px 0; }"
"input[type=number] { padding: 8px; border-radius: 5px; border: 1px solid #ccc; width: 80px; }"
".number-input { width: 60px; margin-left: 5px; }"
".small-button { padding: 5px 10px; font-size: 12px; margin-left: 5px; }"
".config-grid { display: grid; grid-template-columns: 1fr 1fr; gap: 10px; }"
"@media (max-width: 600px) { .config-grid { grid-template-columns: 1fr; } }"
"</style>"
"</head>"
"<body>"
"<div class='container'>"
"<h1>🌈 ESP32S3-TFT-BS 智能控制面板</h1>"
"<div class='status-info' id='status'>状态: 连接成功 | <a href='/wifi'>WiFi 配网</a> | <a href='/ota'>OTA 升级</a> | TCP: 8267</div>"
"<div class='channel-group broadcast-section'>"
"<div class='channel-title'>📡 广播控制 (所有通道)</div>"
"<div class='control-row'>"
"<span class='control-label'>模式:</span>"
"<select id='broadcast-mode'>"
"<option value='0'>关闭</option>"
"<option value='1'>静态颜色</option>"
"<option value='2'>彩虹效果</option>"
"<option value='3'>呼吸灯</option>"
"<option value='4'>跑马灯</option>"
"<option value='5'>闪烁效果</option>"
"<option value='6'>波浪效果</option>"
"<option value='7'>自动循环</option>"
"<option value='8'>电量显示</option>"
"<option value='9'>音乐律动1</option>"
"<option value='10'>音乐律动2</option>"
"</select>"
"</div>"
"<div class='control-row'>"
"<span>R</span><input type='range' id='broadcast-r' min='0' max='255' value='255' style='width:100px'><input type='number' id='broadcast-r-num' class='number-input' min='0' max='255' value='255'>"
"<span>G</span><input type='range' id='broadcast-g' min='0' max='255' value='255' style='width:100px'><input type='number' id='broadcast-g-num' class='number-input' min='0' max='255' value='255'>"
"<span>B</span><input type='range' id='broadcast-b' min='0' max='255' value='255' style='width:100px'><input type='number' id='broadcast-b-num' class='number-input' min='0' max='255' value='255'>"
"<div class='color-preview' id='broadcast-color'></div>"
"</div>"
"<div class='control-row'>"
"<span class='control-label'>亮度:</span>"
"<input type='range' id='broadcast-brightness' min='0' max='255' value='128' style='width:100px'>"
"<input type='number' id='broadcast-brightness-num' class='number-input' min='0' max='255' value='128'>"
"</div>"
"<div class='control-row'>"
"<span class='control-label'>速度:</span>"
"<input type='range' id='broadcast-speed' min='10' max='2000' value='100' style='width:100px'>"
"<input type='number' id='broadcast-speed-num' class='number-input' min='10' max='2000' value='100'>ms"
"</div>"
"<button onclick='applyBroadcast()'>应用到所有通道</button>"
"</div>"
"<div class='channel-group music-section' style='background: #e6e6fa;'>"
"<div class='channel-title'>🎵 音乐律动全局设置</div>"
"<div class='control-row'>"
"<span class='control-label'>灵敏度:</span>"
"<input type='range' id='global-music-sensitivity' min='0' max='255' value='128' style='width:100px'>"
"<input type='number' id='global-music-sensitivity-num' class='number-input' min='0' max='255' value='128'>"
"<button onclick='applyGlobalSensitivity()'>应用灵敏度</button>"
"</div>"
"</div>"
"<div class='channel-group channel-section'>"
"<div class='channel-title'>🔧 单通道控制</div>"
"<div class='control-row'>"
"<span class='control-label'>通道:</span>"
"<select id='channel-select'>"
"<option value='0'>通道 1</option>"
"<option value='1'>通道 2</option>"
"<option value='2'>通道 3</option>"
"<option value='3'>通道 4</option>"
"</select>"
"<button onclick='loadChannelConfig()'>加载配置</button>"
"</div>"
"<div class='control-row'>"
"<span class='control-label'>启用:</span>"
"<input type='checkbox' id='channel-enabled' checked>"
"</div>"
"<div class='control-row'>"
"<span class='control-label'>模式:</span>"
"<select id='channel-mode'>"
"<option value='0'>关闭</option>"
"<option value='1'>静态颜色</option>"
"<option value='2'>彩虹效果</option>"
"<option value='3'>呼吸灯</option>"
"<option value='4'>跑马灯</option>"
"<option value='5'>闪烁效果</option>"
"<option value='6'>波浪效果</option>"
"<option value='7'>自动循环</option>"
"<option value='8'>电量显示</option>"
"<option value='9'>音乐律动1</option>"
"<option value='10'>音乐律动2</option>"
"</select>"
"</div>"
"<div class='control-row'>"
"<span>R</span><input type='range' id='channel-r' min='0' max='255' value='255' style='width:100px'><input type='number' id='channel-r-num' class='number-input' min='0' max='255' value='255'>"
"<span>G</span><input type='range' id='channel-g' min='0' max='255' value='255' style='width:100px'><input type='number' id='channel-g-num' class='number-input' min='0' max='255' value='255'>"
"<span>B</span><input type='range' id='channel-b' min='0' max='255' value='255' style='width:100px'><input type='number' id='channel-b-num' class='number-input' min='0' max='255' value='255'>"
"<div class='color-preview' id='channel-color'></div>"
"</div>"
"<div class='control-row'>"
"<span class='control-label'>亮度:</span>"
"<input type='range' id='channel-brightness' min='0' max='255' value='128' style='width:100px'>"
"<input type='number' id='channel-brightness-num' class='number-input' min='0' max='255' value='128'>"
"</div>"
"<div class='control-row'>"
"<span class='control-label'>速度:</span>"
"<input type='range' id='channel-speed' min='10' max='2000' value='100' style='width:100px'>"
"<input type='number' id='channel-speed-num' class='number-input' min='10' max='2000' value='100'>ms"
"</div>"
"<div class='control-row' id='music-mode2-controls'>"
"<span class='control-label'>律动背景:</span>"
"<input type='range' id='channel-music-bg' min='0' max='255' value='10' style='width:100px'>"
"<input type='number' id='channel-music-bg-num' class='number-input' min='0' max='255' value='10'>"
"<span class='control-label'>多彩:</span>"
"<input type='checkbox' id='channel-music-colorful' checked>"
"</div>"
"<div class='control-row'>"
"<span class='control-label'>灵敏度:</span>"
"<input type='range' id='channel-music-sensitivity' min='0' max='255' value='128' style='width:100px'>"
"<input type='number' id='channel-music-sensitivity-num' class='number-input' min='0' max='255' value='128'>"
"</div>"
"<div class='control-row'>"
"<span class='control-label'>LED数量:</span>"
"<input type='range' id='channel-led-count' min='1' max='300' value='60' style='width:100px'>"
"<input type='number' id='channel-led-count-num' class='number-input' min='1' max='300' value='60'>"
"<span class='control-label'>循环:</span>"
"<input type='range' id='channel-cycle-duration' min='1000' max='60000' value='8000' style='width:100px'>"
"<input type='number' id='channel-cycle-duration-num' class='number-input' min='1000' max='60000' value='8000'>ms"
"</div>"
"<button onclick='applyChannel()'>应用到选中通道</button>"
"</div>"
"<div class='channel-group battery-section'>"
"<div class='channel-title'>🔋 电池电量显示配置</div>"
"<div class='control-row'>"
"<span class='control-label'>电量显示通道:</span>"
"<select id='battery-channel'>"
"<option value='255'>禁用电量显示</option>"
"<option value='0'>通道 1</option>"
"<option value='1'>通道 2</option>"
"<option value='2'>通道 3</option>"
"<option value='3'>通道 4</option>"
"</select>"
"<button onclick='setBatteryChannel()'>设置电量显示通道</button>"
"</div>"
"<div class='control-row'>"
"<span class='control-label'>背景亮度:</span>"
"<input type='range' id='battery-bg-brightness' min='0' max='255' value='20'>"
"<input type='number' id='battery-bg-brightness-num' class='number-input' min='0' max='255' value='20'>"
"<button onclick='setBatteryDisplay()'>应用显示配置</button>"
"</div>"
"</div>"
"<div class='channel-group voltage-section'>"
"<div class='channel-title'>⚡ 电压与电池状态控制</div>"
"<div class='control-row'>"
"<span class='control-label'>电压设置:</span>"
"<input type='number' id='voltage-input' min='18.0' max='29.4' step='0.1' value='24.0' style='width: 100px;'>"
"<span>V</span>"
"<button onclick='setVoltage()'>设置电压</button>"
"<button onclick='restoreAutoVoltage()'>恢复自动检测</button>"
"</div>"
"<div class='control-row'>"
"<button onclick='getBatteryStatus()'>查询电池状态</button>"
"<button onclick='restoreAutoMode()'>恢复自动模式</button>"
"</div>"
"<div class='control-row'>"
"<span class='control-label'>电量设置:</span>"
"<input type='range' id='battery-level' min='0' max='100' value='50'>"
"<input type='number' id='battery-level-num' class='number-input' min='0' max='100' value='50'>%"
"<button onclick='setBatteryLevel()'>设置电量</button>"
"</div>"
"<div class='control-row'>"
"<span class='control-label'>充电状态:</span>"
"<select id='charging-status'>"
"<option value='false'>未充电</option>"
"<option value='true'>充电中</option>"
"<option value='auto'>自动检测</option>"
"</select>"
"<button onclick='setChargingStatus()'>设置充电状态</button>"
"</div>"
"<div class='control-row'>"
"<div id='battery-info' style='background: #e8f5e8; padding: 10px; border-radius: 5px; margin: 10px 0;'>"
"电池状态信息将显示在这里"
"</div>"
"</div>"
"</div>"
"<div class='channel-group advanced-section'>"
"<div class='channel-title'>⚙️ 高级配置</div>"
"<div class='control-row'>"
"<button onclick='getSystemInfo()'>系统信息</button>"
"<button onclick='saveConfig()'>保存配置</button>"
"<button onclick='loadConfig()'>加载配置</button>"
"</div>"
"</div>"

"</div>"
"<script>"
"function updateSliderValue(sliderId) {"
"  const slider = document.getElementById(sliderId);"
"  const number = document.getElementById(sliderId + '-num');"
"  if (number) number.value = slider.value;"
"  if (sliderId.includes('-r') || sliderId.includes('-g') || sliderId.includes('-b')) {"
"    updateColorPreview(sliderId.split('-')[0]);"
"  }"
"}"
"function updateNumberValue(sliderId) {"
"  const slider = document.getElementById(sliderId);"
"  const number = document.getElementById(sliderId + '-num');"
"  if (slider) slider.value = number.value;"
"  if (sliderId.includes('-r') || sliderId.includes('-g') || sliderId.includes('-b')) {"
"    updateColorPreview(sliderId.split('-')[0]);"
"  }"
"}"
"function updateColorPreview(prefix) {"
"  const r = document.getElementById(prefix + '-r').value;"
"  const g = document.getElementById(prefix + '-g').value;"
"  const b = document.getElementById(prefix + '-b').value;"
"  const color = 'rgb(' + r + ',' + g + ',' + b + ')';"
"  document.getElementById(prefix + '-color').style.backgroundColor = color;"
"}"
"function sendRequest(endpoint, data) {"
"  console.log('Sending request to:', endpoint, 'with data:', data);"
"  document.getElementById('status').innerHTML = '状态: 🔄 发送中...';"
"  fetch(endpoint, {"
"    method: 'POST',"
"    headers: {'Content-Type': 'application/json'},"
"    body: JSON.stringify(data)"
"  })"
"  .then(function(response) {"
"    console.log('Response received:', response.status);"
"    if (response.ok) {"
"      document.getElementById('status').innerHTML = '状态: ✅ 命令执行成功';"
"    } else {"
"      document.getElementById('status').innerHTML = '状态: ❌ 命令执行失败';"
"    }"
"  })"
"  .catch(function(error) {"
"    console.error('Request failed:', error);"
"    document.getElementById('status').innerHTML = '状态: ❌ 连接失败';"
"  });"
"}"
"function preset(mode) {"
"  console.log('Preset clicked:', mode);"
"  var data = { channel: 255 };"
"  switch(mode) {"
"    case 'off':"
"      data.mode = 0;"
"      break;"
"    case 'white':"
"      data.mode = 1;"
"      data.color = {r: 255, g: 255, b: 255};"
"      data.brightness = 200;"
"      break;"
"    case 'rainbow':"
"      data.mode = 4;"
"      data.speed = 50;"
"      data.brightness = 150;"
"      break;"
"    case 'party':"
"      data.mode = 7;"
"      data.speed = 30;"
"      data.brightness = 255;"
"      break;"
"    case 'battery':"
"      data = { battery_channel: 0 };"
"      sendRequest('/api/control', data);"
"      return;"
"    case 'breathing':"
"      data.mode = 2;"
"      data.color = {r: 0, g: 255, b: 128};"
"      data.speed = 200;"
"      data.brightness = 180;"
"      break;"
"  }"
"  console.log('Preset data:', data);"
"  sendRequest('/api/control', data);"
"}"
"function testLights() {"
"  console.log('Test lights clicked');"
"  document.getElementById('status').innerHTML = '状态: 🔄 测试中...';"
"  fetch('/api/test', { "
"    method: 'POST',"
"    headers: {'Content-Type': 'application/json'}"
"  })"
"  .then(function(response) {"
"    if (response.ok) {"
"      document.getElementById('status').innerHTML = '状态: ✅ 测试完成';"
"    } else {"
"      document.getElementById('status').innerHTML = '状态: ❌ 测试失败';"
"    }"
"  })"
"  .catch(function(error) {"
"    console.error('Test failed:', error);"
"    document.getElementById('status').innerHTML = '状态: ❌ 测试失败';"
"  });"
"}"
"function simpleTest() {"
"  console.log('Simple test clicked');"
"  fetch('/api/simple', { method: 'POST' })"
"  .then(function(response) { "
"    if(response.ok) console.log('Simple test OK'); else console.log('Simple test failed');"
"  })"
"  .catch(function(error) { console.error('Simple test error:', error); });"
"}"
"function debugTest() {"
"  console.log('Debug test clicked');"
"  document.getElementById('status').innerHTML = '状态: 🔄 调试测试中...';"
"  fetch('/', { method: 'GET' })"
"  .then(function(response) {"
"    console.log('Root GET response status:', response.status);"
"    return fetch('/api/test', { method: 'POST', headers: { 'Content-Type': 'application/json' } });"
"  })"
"  .then(function(response) {"
"    console.log('API test response status:', response.status);"
"    if(response.ok) {"
"      document.getElementById('status').innerHTML = '状态: ✅ 调试成功';"
"    } else {"
"      document.getElementById('status').innerHTML = '状态: ❌ 调试失败';"
"    }"
"  })"
"  .catch(function(error) {"
"    console.error('Debug test error:', error);"
"    document.getElementById('status').innerHTML = '状态: ❌ 调试失败';"
"  });"
"}"
"function applyGlobalSensitivity() {"
"  var sensitivity = parseInt(document.getElementById('global-music-sensitivity').value);"
"  var data = { music_sensitivity: sensitivity };"
"  sendRequest('/api/control', data);"
"}"
"function loadGlobalSettings() {"
"  fetch('/api/status', { method: 'GET' })"
"  .then(function(response) { return response.json(); })"
"  .then(function(data) {"
"    if (data.music_sensitivity !== undefined) {"
"      updateInputPair('global-music-sensitivity', data.music_sensitivity);"
"    }"
"  });"
"}"
"function applyBroadcast() {"
"  var data = {"
"    channel: 255,"
"    mode: parseInt(document.getElementById('broadcast-mode').value),"
"    color: {"
"      r: parseInt(document.getElementById('broadcast-r').value),"
"      g: parseInt(document.getElementById('broadcast-g').value),"
"      b: parseInt(document.getElementById('broadcast-b').value)"
"    },"
"    brightness: parseInt(document.getElementById('broadcast-brightness').value),"
"    speed: parseInt(document.getElementById('broadcast-speed').value)"
"  };"
"  sendRequest('/api/control', data);"
"}"
"function applyChannel() {"
"  var channelId = parseInt(document.getElementById('channel-select').value);"
"  var data = {"
"    channel: channelId,"
"    enabled: document.getElementById('channel-enabled').checked,"
"    mode: parseInt(document.getElementById('channel-mode').value),"
"    color: {"
"      r: parseInt(document.getElementById('channel-r').value),"
"      g: parseInt(document.getElementById('channel-g').value),"
"      b: parseInt(document.getElementById('channel-b').value)"
"    },"
"    brightness: parseInt(document.getElementById('channel-brightness').value),"
"    speed: parseInt(document.getElementById('channel-speed').value),"
"    music_bg_brightness: parseInt(document.getElementById('channel-music-bg').value),"
"    music_colorful_mode: document.getElementById('channel-music-colorful').checked,"
"    led_count: parseInt(document.getElementById('channel-led-count').value),"
"    cycle_duration: parseInt(document.getElementById('channel-cycle-duration').value)"
"  };"
"  sendRequest('/api/control', data);"
"}"
"function loadChannelConfig() {"
"  console.log('Loading channel config...');"
"  var channelId = parseInt(document.getElementById('channel-select').value);"
"  document.getElementById('status').innerHTML = '状态: 🔄 获取通道配置中...';"
"  fetch('/api/status', { method: 'GET' })"
"  .then(function(response) { return response.json(); })"
"  .then(function(data) {"
"    if (data.channels) {"
"      var channel = data.channels.find(function(c) { return c.id === channelId; });"
"      if (channel) {"
"        document.getElementById('channel-enabled').checked = channel.enabled;"
"        document.getElementById('channel-mode').value = channel.mode;"
"        updateInputPair('channel-r', channel.color.r);"
"        updateInputPair('channel-g', channel.color.g);"
"        updateInputPair('channel-b', channel.color.b);"
"        updateInputPair('channel-brightness', channel.brightness);"
"        updateInputPair('channel-speed', channel.speed);"
"        updateInputPair('channel-music-bg', channel.music_bg_brightness !== undefined ? channel.music_bg_brightness : 10);"
"        document.getElementById('channel-music-colorful').checked = channel.music_colorful_mode !== undefined ? channel.music_colorful_mode : true;"
"        updateInputPair('channel-led-count', channel.led_count);"
"        updateInputPair('channel-cycle-duration', channel.cycle_duration);"
"        updateColorPreview('channel');"
"        document.getElementById('status').innerHTML = '状态: ✅ 通道配置加载成功';"
"      }"
"    }"
"  })"
"  .catch(function(error) {"
"    console.error('Error loading config:', error);"
"    document.getElementById('status').innerHTML = '状态: ❌ 加载配置失败';"
"  });"
"}"
"function updateInputPair(id, value) {"
"  var slider = document.getElementById(id);"
"  var number = document.getElementById(id + '-num');"
"  if (slider && number) {"
"    slider.value = value;"
"    number.value = value;"
"  }"
"}"
"function getBatteryStatus() {"
"  console.log('Getting battery status...');"
"  document.getElementById('status').innerHTML = '状态: 🔄 查询电池状态中...';"
"  fetch('/api/battery/status', { method: 'GET' })"
"  .then(function(response) {"
"    if (response.ok) {"
"      return response.json();"
"    } else {"
"      throw new Error('Battery status request failed');"
"    }"
"  })"
"  .then(function(data) {"
"    console.log('Battery status:', data);"
"    updateBatteryInfo(data);"
"    document.getElementById('status').innerHTML = '状态: ✅ 电池状态查询成功';"
"  })"
"  .catch(function(error) {"
"    console.error('Battery status error:', error);"
"    document.getElementById('status').innerHTML = '状态: ❌ 电池状态查询失败';"
"  });"
"}"
"function updateBatteryInfo(data) {"
"  var info = '电池信息:<br/>';"
"  info += '电压: ' + data.voltage + 'V<br/>';"
"  info += '电量: ' + data.percentage + '%<br/>';"
"  info += '充电状态: ' + data.charging_status + '<br/>';"
"  if (data.display) {"
"    info += '显示模式: ' + data.display.mode + '<br/>';"
"    info += '当前显示: ' + data.display.current_display + '%<br/>';"
"  }"
"  if (data.animation) {"
"    info += '动画效果: ' + data.animation.effect + '<br/>';"
"  }"
"  document.getElementById('battery-info').innerHTML = info;"
"}"
"function setBatteryLevel() {"
"  var level = parseInt(document.getElementById('battery-level').value);"
"  var data = { battery: level };"
"  console.log('Setting battery level:', level);"
"  sendRequest('/api/unified', data);"
"}"
"function setChargingStatus() {"
"  var status = document.getElementById('charging-status').value;"
"  var data = {};"
"  if (status === 'auto') {"
"    data.auto_charging = true;"
"  } else {"
"    data.charging = (status === 'true');"
"  }"
"  console.log('Setting charging status:', data);"
"  sendRequest('/api/unified', data);"
"}"
"function restoreAutoMode() {"
"  var data = { auto_mode: true, auto_charging: true };"
"  console.log('Restoring auto mode');"
"  sendRequest('/api/battery/control', data);"
"}"
"function setBatteryChannel() {"
"  var channel = parseInt(document.getElementById('battery-channel').value);"
"  var data = { battery_channel: channel };"
"  console.log('Setting battery channel:', channel);"
"  sendRequest('/api/unified', data);"
"}"
"function setBatteryDisplay() {"
"  var channel = parseInt(document.getElementById('battery-channel').value);"
"  var bg_brightness = parseInt(document.getElementById('battery-bg-brightness').value);"
"  var data = {"
"    battery_display: {"
"      channel: channel,"
"      show_charging_effect: true,"
"      background_brightness: bg_brightness"
"    }"
"  };"
"  console.log('Setting battery display config:', data);"
"  sendRequest('/api/unified', data);"
"}"
"function setVoltage() {"
"  var voltage = parseFloat(document.getElementById('voltage-input').value);"
"  var data = { voltage: voltage };"
"  console.log('Setting voltage:', voltage);"
"  sendRequest('/api/unified', data);"
"}"
"function restoreAutoVoltage() {"
"  var data = { auto_voltage: true };"
"  console.log('Restoring auto voltage mode');"
"  sendRequest('/api/unified', data);"
"}"
"function systemTest() {"
"  console.log('System test clicked');"
"  var data = { action: 'test_all_channels' };"
"  sendRequest('/api/control', data);"
"}"
"function getSystemInfo() {"
"  console.log('Getting system info...');"
"  document.getElementById('status').innerHTML = '状态: 🔄 获取系统信息中...';"
"  fetch('/api/status', { method: 'GET' })"
"  .then(function(response) {"
"    if (response.ok) {"
"      return response.json();"
"    } else {"
"      throw new Error('System info request failed');"
"    }"
"  })"
"  .then(function(data) {"
"    console.log('System info:', data);"
"    var info = '系统信息:<br/>';"
"    if (data.battery_percentage !== undefined) info += '电量: ' + data.battery_percentage + '%<br/>';"
"    if (data.voltage !== undefined) info += '电压: ' + data.voltage + 'V<br/>';"
"    if (data.is_charging !== undefined) info += '充电: ' + (data.is_charging ? '是' : '否') + '<br/>';"
"    if (data.channels) {"
"      info += '通道配置:<br/>';"
"      data.channels.forEach(function(ch) {"
"        info += '  通道' + ch.id + ': ' + (ch.enabled ? '启用' : '禁用') + ', 模式' + ch.mode + ', ' + ch.led_count + '个LED<br/>';"
"      });"
"    }"
"    document.getElementById('battery-info').innerHTML = info;"
"    document.getElementById('status').innerHTML = '状态: ✅ 系统信息获取成功';"
"  })"
"  .catch(function(error) {"
"    console.error('System info error:', error);"
"    document.getElementById('status').innerHTML = '状态: ❌ 系统信息获取失败';"
"  });"
"}"
"function saveConfig() {"
"  console.log('Saving config');"
"  var data = { action: 'save_config' };"
"  sendRequest('/api/control', data);"
"}"
"function loadConfig() {"
"  console.log('Loading config');"
"  var data = { action: 'load_config' };"
"  sendRequest('/api/control', data);"
"}"

"window.onload = function() {"
"  console.log('Page loaded, initializing...');"
"  loadGlobalSettings();"
"  var sliders = ['broadcast-r', 'broadcast-g', 'broadcast-b', 'broadcast-brightness', 'broadcast-speed',"
"                 'channel-r', 'channel-g', 'channel-b', 'channel-brightness', 'channel-speed',"
"                 'channel-music-bg', 'global-music-sensitivity', 'channel-led-count', 'channel-cycle-duration', 'battery-level', 'battery-bg-brightness'];"
"  for(var i = 0; i < sliders.length; i++) {"
"    var id = sliders[i];"
"    var slider = document.getElementById(id);"
"    var number = document.getElementById(id + '-num');"
"    if (slider && number) {"
"      slider.oninput = (function(sid) { return function() { updateSliderValue(sid); }; })(id);"
"      number.oninput = (function(sid) { return function() { updateNumberValue(sid); }; })(id);"
"      updateSliderValue(id);"
"    }"
"  }"
"  updateColorPreview('broadcast');"
"  updateColorPreview('channel');"
"  console.log('Page initialization complete');"
"  console.log('Final RGB values - Broadcast: R=', document.getElementById('broadcast-r').value, "
"              'G=', document.getElementById('broadcast-g').value, 'B=', document.getElementById('broadcast-b').value);"
"};"

"</script>"
"</body>"
"</html>";

// 网页处理函数
static esp_err_t root_get_handler(httpd_req_t *req)
{
    ESP_LOGI(TAG, "root_get_handler: serving control page to client");
    
    // 设置HTTP响应头
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache, no-store, must-revalidate");
    httpd_resp_set_hdr(req, "Pragma", "no-cache");
    httpd_resp_set_hdr(req, "Expires", "0");
    
    // 一次性发送完整页面
    esp_err_t ret = httpd_resp_sendstr(req, complete_html_page);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to send complete HTML page");
        return ret;
    }
    
    ESP_LOGI(TAG, "root_get_handler: successfully sent complete response");
    return ESP_OK;
}

static esp_err_t wifi_config_get_handler(httpd_req_t *req)
{
    char page[2048];
    snprintf(page, sizeof(page),
             "<!DOCTYPE html><html><head><meta charset='UTF-8'>"
             "<meta name='viewport' content='width=device-width,initial-scale=1.0'>"
             "<title>RGB WiFi 配网</title>"
             "<style>body{font-family:Arial,sans-serif;margin:20px;background:#f5f5f5}"
             ".box{max-width:520px;margin:0 auto;background:#fff;padding:20px;border-radius:8px}"
             "input,button{width:100%%;box-sizing:border-box;padding:10px;margin-top:8px;font-size:15px}"
             "button{background:#007bff;color:#fff;border:0;border-radius:5px;cursor:pointer}"
             ".ap{padding:8px;border-bottom:1px solid #eee;cursor:pointer;display:flex;justify-content:space-between}"
             ".ap:hover{background:#eef5ff}.muted{color:#666;font-size:13px}.danger{background:#dc3545}</style>"
             "</head><body><div class='box'>"
             "<h2>RGB WiFi 配网</h2>"
             "<p class='muted'>模式: %s | 当前 IP: %s | TCP: %d | 热点: %s</p>"
             "<p><a href='/'>打开灯光控制面板</a></p>"
             "<form method='POST' action='/wificfg'>"
             "<label>SSID</label><input id='ssid' name='ssid' maxlength='32' required>"
             "<button type='button' onclick='scan()'>扫描附近 WiFi</button>"
             "<div id='msg' class='muted'></div><div id='aps'></div>"
             "<label>密码</label><input name='pass' type='password' maxlength='64'>"
             "<button type='submit'>保存并连接</button></form>"
             "<form method='POST' action='/wificlear'>"
             "<button class='danger' type='submit'>清除已保存 WiFi</button></form>"
             "<script>"
             "async function scan(){let m=document.getElementById('msg'),d=document.getElementById('aps');"
             "m.textContent='扫描中...';d.innerHTML='';try{let j=await(await fetch('/wifiscan')).json();"
             "m.textContent=j.length?'发现 '+j.length+' 个，点击选择':'未发现 AP';"
             "j.forEach(a=>{let e=document.createElement('div');e.className='ap';"
             "e.innerHTML='<span>'+(a.a?'锁 ':'')+a.s+'</span><span>'+a.r+' dBm</span>';"
             "e.onclick=()=>document.getElementById('ssid').value=a.s;d.appendChild(e);});}"
             "catch(e){m.textContent='扫描失败: '+e;}}"
             "</script></div></body></html>",
             s_ap_mode ? "AP 配网" : "STA 已连接",
             s_ip_str, RGB_TCP_CONTROL_PORT, s_light_ssid);
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return httpd_resp_sendstr(req, page);
}

static esp_err_t wifi_scan_get_handler(httpd_req_t *req)
{
    wifi_scan_config_t cfg = {0};
    cfg.scan_type = WIFI_SCAN_TYPE_ACTIVE;
    cfg.scan_time.active.min = 50;
    cfg.scan_time.active.max = 150;

    esp_err_t err = esp_wifi_scan_start(&cfg, true);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "WiFi scan failed: %s", esp_err_to_name(err));
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_sendstr(req, "[]");
    }

    uint16_t count = 0;
    esp_wifi_scan_get_ap_num(&count);
    if (count > 24) count = 24;

    wifi_ap_record_t *recs = NULL;
    if (count > 0) {
        recs = calloc(count, sizeof(*recs));
        if (!recs) {
            esp_wifi_clear_ap_list();
            httpd_resp_set_type(req, "application/json");
            return httpd_resp_sendstr(req, "[]");
        }
        esp_wifi_scan_get_ap_records(&count, recs);
    }

    char *json = malloc(2048);
    if (!json) {
        free(recs);
        esp_wifi_clear_ap_list();
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_sendstr(req, "[]");
    }

    size_t pos = 0;
    json[pos++] = '[';
    for (uint16_t i = 0; i < count; i++) {
        if (recs[i].ssid[0] == '\0') continue;
        bool duplicate = false;
        for (uint16_t k = 0; k < i; k++) {
            if (strncmp((char *)recs[i].ssid, (char *)recs[k].ssid,
                        sizeof(recs[i].ssid)) == 0 && recs[k].ssid[0] != '\0') {
                duplicate = true;
                break;
            }
        }
        if (duplicate) continue;

        char esc[80];
        size_t ssid_len = strnlen((char *)recs[i].ssid, sizeof(recs[i].ssid));
        json_escape((const char *)recs[i].ssid, ssid_len, esc, sizeof(esc));
        int n = snprintf(json + pos, 2048 - pos,
                         "%s{\"s\":\"%s\",\"r\":%d,\"a\":%d}",
                         pos > 1 ? "," : "", esc, recs[i].rssi, (int)recs[i].authmode);
        if (n < 0 || (size_t)n >= 2048 - pos) break;
        pos += n;
    }
    if (pos + 1 < 2048) json[pos++] = ']';
    json[pos] = '\0';

    free(recs);
    esp_wifi_clear_ap_list();

    httpd_resp_set_type(req, "application/json");
    esp_err_t send_err = httpd_resp_send(req, json, pos);
    free(json);
    return send_err;
}

static esp_err_t wifi_config_post_handler(httpd_req_t *req)
{
    char body[256];
    int total = req->content_len < (int)sizeof(body) - 1 ? req->content_len : (int)sizeof(body) - 1;
    int r = httpd_req_recv(req, body, total);
    if (r <= 0) return ESP_FAIL;
    body[r] = '\0';

    char ssid_raw[64] = {0};
    char pass_raw[128] = {0};
    httpd_query_key_value(body, "ssid", ssid_raw, sizeof(ssid_raw));
    httpd_query_key_value(body, "pass", pass_raw, sizeof(pass_raw));

    char ssid[64] = {0};
    char pass[128] = {0};
    url_decode(ssid_raw, ssid, sizeof(ssid));
    url_decode(pass_raw, pass, sizeof(pass));

    if (!ssid[0]) {
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_sendstr(req, "ssid required");
    }
    if (rgb_wifi_creds_save(ssid, pass) != ESP_OK) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        return httpd_resp_sendstr(req, "save failed");
    }

    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_sendstr(req, "<html><body><p>已保存 WiFi，设备将在 1 秒后重启并连接路由器。</p></body></html>");
    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();
    return ESP_OK;
}

static esp_err_t wifi_clear_post_handler(httpd_req_t *req)
{
    rgb_wifi_creds_clear();
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_sendstr(req, "<html><body><p>已清除 WiFi，设备将在 1 秒后重启回配网热点。</p></body></html>");
    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();
    return ESP_OK;
}

static esp_err_t ota_page_get_handler(httpd_req_t *req)
{
    static const char page[] =
        "<!DOCTYPE html><html><head><meta charset='UTF-8'>"
        "<meta name='viewport' content='width=device-width,initial-scale=1.0'>"
        "<title>ESP32 OTA</title>"
        "<style>body{font-family:Arial,sans-serif;margin:20px;background:#f5f5f5}"
        ".box{max-width:620px;margin:0 auto;background:#fff;padding:20px;border-radius:8px}"
        "button,input{box-sizing:border-box;padding:10px;margin:6px 0;font-size:15px}"
        "button{background:#007bff;color:#fff;border:0;border-radius:5px;cursor:pointer}"
        "button.danger{background:#dc3545}input{width:100%}"
        ".row{display:flex;gap:8px;flex-wrap:wrap}.muted{color:#666;font-size:13px}"
        "progress{width:100%;height:20px}</style></head><body><div class='box'>"
        "<h2>OTA 固件升级</h2><p><a href='/'>返回灯光控制</a> | <a href='/wifi'>WiFi 配网</a></p>"
        "<div id='status' class='muted'>加载中...</div>"
        "<progress id='prog' max='100' value='0'></progress><div id='progmsg' class='muted'></div>"
        "<div class='row'><button onclick='toggleAuto()'>切换自动更新</button>"
        "<button onclick='checkNow()'>立即检查更新</button><button onclick='confirmUpdate()'>确认升级</button></div>"
        "<h3>升级地址</h3><label>主地址</label><input id='pri' type='url'>"
        "<label>备用地址</label><input id='bak' type='url'>"
        "<div class='row'><button onclick='saveUrls()'>保存地址</button>"
        "<button class='danger' onclick='resetUrls()'>恢复默认</button></div>"
        "<h3>本地上传</h3><input type='file' id='fw' accept='.bin'>"
        "<button onclick='uploadFw()'>上传并升级</button><div id='msg' class='muted'></div>"
        "<script>"
        "async function j(u,o){return await(await fetch(u,o)).json()}"
        "async function load(){try{let s=await j('/api/ota/status?_='+Date.now());"
        "let h='版本: '+s.version+'<br>自动更新: '+(s.auto_update?'开':'关')+'<br>状态: '+s.last_status;"
        "if(s.pending_update)h+='<br>发现新版本: '+s.pending_update.version;"
        "document.getElementById('status').innerHTML=h;"
        "document.getElementById('prog').value=s.progress||0;"
        "document.getElementById('progmsg').textContent=(s.progress_msg||'')+' '+(s.progress||0)+'%';"
        "let u=await j('/api/ota/urls?_='+Date.now());document.getElementById('pri').value=u.primary||'';"
        "document.getElementById('bak').value=u.backup||'';}catch(e){document.getElementById('status').textContent='查询失败: '+e}}"
        "async function toggleAuto(){let s=await j('/api/ota/status?_='+Date.now());"
        "await fetch('/api/ota/enable',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({enabled:!s.auto_update})});load()}"
        "async function checkNow(){let r=await j('/api/ota/check_now',{method:'POST'});document.getElementById('msg').textContent=r.message||'已请求';setTimeout(load,1500)}"
        "async function confirmUpdate(){let r=await j('/api/ota/confirm',{method:'POST',headers:{'Content-Type':'application/json'},body:'{}'});document.getElementById('msg').textContent=r.message||JSON.stringify(r);setTimeout(load,1000)}"
        "async function saveUrls(){let primary=document.getElementById('pri').value.trim(),backup=document.getElementById('bak').value.trim();"
        "let r=await j('/api/ota/urls',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({primary,backup})});document.getElementById('msg').textContent=r.ok?'已保存':(r.err||'保存失败')}"
        "async function resetUrls(){await fetch('/api/ota/urls/reset',{method:'POST'});load()}"
        "function uploadFw(){let f=document.getElementById('fw').files[0];if(!f){alert('请选择 .bin 文件');return}"
        "let x=new XMLHttpRequest(),m=document.getElementById('msg'),p=document.getElementById('prog');x.open('POST','/api/ota/upload');"
        "x.setRequestHeader('Content-Type','application/octet-stream');x.upload.onprogress=e=>{if(e.lengthComputable)p.value=e.loaded*100/e.total};"
        "x.onload=()=>{m.textContent=x.status+': '+x.responseText};x.onerror=()=>{m.textContent='上传失败'};x.send(f)}"
        "load();setInterval(load,3000)</script></div></body></html>";
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return httpd_resp_send(req, page, sizeof(page) - 1);
}

// API控制处理函数
static esp_err_t api_control_handler(httpd_req_t *req)
{
    ESP_LOGI(TAG, "=== API CONTROL HANDLER CALLED ===");
    web_server_task_wdt_reset_if_registered();
    
    char content[1024];
    size_t to_read = req->content_len;
    if (to_read >= sizeof(content)) {
        ESP_LOGE(TAG, "Request body too large: %d (max %d)", (int)to_read, (int)sizeof(content) - 1);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Body too large");
        return ESP_FAIL;
    }

    size_t received = 0;
    while (received < to_read) {
        int ret = httpd_req_recv(req, content + received, to_read - received);
        if (ret <= 0) {
            if (ret == HTTPD_SOCK_ERR_TIMEOUT) {
                ESP_LOGW(TAG, "httpd_req_recv timeout, retrying... received=%d/%d", (int)received, (int)to_read);
                continue;
            }
            ESP_LOGE(TAG, "Failed to receive HTTP request data, ret=%d", ret);
            httpd_resp_send_500(req);
            return ESP_FAIL;
        }
        received += (size_t)ret;
        web_server_task_wdt_reset_if_registered();
    }
    content[received] = '\0';

    ESP_LOGI(TAG, "Received control command (%d bytes of %d): %s", (int)received, (int)to_read, content);
    web_server_task_wdt_reset_if_registered();
    
    // 解析JSON并处理WS2812控制命令
    esp_err_t result = ws2812_handle_json_command(content);
    ESP_LOGI(TAG, "Command processing result: %s", result == ESP_OK ? "SUCCESS" : "FAILED");

    // 发送JSON响应
    httpd_resp_set_type(req, "application/json");
    if (result == ESP_OK) {
        httpd_resp_sendstr(req, "{\"status\":\"success\",\"message\":\"命令执行成功\"}");
    } else {
        httpd_resp_sendstr(req, "{\"status\":\"error\",\"message\":\"命令执行失败\"}");
    }

    return ESP_OK;
}

// API测试处理函数
static esp_err_t api_test_handler(httpd_req_t *req)
{
    ESP_LOGI(TAG, "API test endpoint called");
    web_server_task_wdt_reset_if_registered();

    // 发送测试命令到WS2812控制任务
    esp_err_t result = ws2812_handle_json_command("{\"channel\":255,\"mode\":1,\"color\":{\"r\":255,\"g\":255,\"b\":255},\"brightness\":100}");
    
    httpd_resp_set_type(req, "application/json");
    if (result == ESP_OK) {
        httpd_resp_sendstr(req, "{\"status\":\"success\",\"message\":\"测试完成\"}");
        ESP_LOGI(TAG, "API test completed successfully");
    } else {
        httpd_resp_sendstr(req, "{\"status\":\"error\",\"message\":\"测试失败\"}");
        ESP_LOGE(TAG, "API test failed");
    }

    return ESP_OK;
}

// API简单测试处理函数
static esp_err_t api_simple_test_handler(httpd_req_t *req)
{
    ESP_LOGI(TAG, "Simple test API called");
    web_server_task_wdt_reset_if_registered();

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"status\":\"ok\"}");
    ESP_LOGI(TAG, "Simple test responded");

    return ESP_OK;
}

// API电池状态查询处理函数
static esp_err_t api_battery_status_handler(httpd_req_t *req)
{
    ESP_LOGI(TAG, "Battery status API called");
    web_server_task_wdt_reset_if_registered();

    // 获取电池状态JSON
    char *json_response = create_battery_status_json();
    
    httpd_resp_set_type(req, "application/json");
    if (json_response) {
        httpd_resp_sendstr(req, json_response);
        free(json_response);
        ESP_LOGI(TAG, "Battery status sent successfully");
    } else {
        httpd_resp_sendstr(req, "{\"status\":\"error\",\"message\":\"获取电池状态失败\"}");
        ESP_LOGE(TAG, "Failed to create battery status JSON");
    }

    return ESP_OK;
}

// API电池控制处理函数
static esp_err_t api_battery_control_handler(httpd_req_t *req)
{
    ESP_LOGI(TAG, "=== BATTERY CONTROL API CALLED ===");
    web_server_task_wdt_reset_if_registered();
    
    char content[1024];
    size_t to_read = req->content_len;
    if (to_read >= sizeof(content)) {
        ESP_LOGE(TAG, "Battery control request body too large: %d (max %d)", (int)to_read, (int)sizeof(content) - 1);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Body too large");
        return ESP_FAIL;
    }

    size_t received = 0;
    while (received < to_read) {
        int ret = httpd_req_recv(req, content + received, to_read - received);
        if (ret <= 0) {
            if (ret == HTTPD_SOCK_ERR_TIMEOUT) {
                ESP_LOGW(TAG, "httpd_req_recv timeout, retrying... received=%d/%d", (int)received, (int)to_read);
                continue;
            }
            ESP_LOGE(TAG, "Failed to receive battery control request data, ret=%d", ret);
            httpd_resp_send_500(req);
            return ESP_FAIL;
        }
        received += (size_t)ret;
        web_server_task_wdt_reset_if_registered();
    }
    content[received] = '\0';

    ESP_LOGI(TAG, "Received battery control command (%d bytes): %s", (int)received, content);
    web_server_task_wdt_reset_if_registered();
    
    // 解析JSON并处理电池控制命令
    cJSON *json = cJSON_Parse(content);
    if (json) {
        bool did_any = false;
        
        // 自动模式恢复
        cJSON *auto_mode = cJSON_GetObjectItem(json, "auto_mode");
        if (auto_mode && cJSON_IsBool(auto_mode) && cJSON_IsTrue(auto_mode)) {
            restore_auto_battery_mode();
            did_any = true;
            ESP_LOGI(TAG, "Restored auto battery mode");
        }
        
        cJSON *auto_charging = cJSON_GetObjectItem(json, "auto_charging");
        if (auto_charging && cJSON_IsBool(auto_charging) && cJSON_IsTrue(auto_charging)) {
            restore_auto_charging_mode();
            did_any = true;
            ESP_LOGI(TAG, "Restored auto charging mode");
        }
        
        // 电池电量设置
        cJSON *battery_item = cJSON_GetObjectItem(json, "battery");
        if (battery_item && cJSON_IsNumber(battery_item)) {
            int battery_level = battery_item->valueint;
            if (battery_level >= 0 && battery_level <= 100) {
                set_external_battery_level(battery_level);
                did_any = true;
                ESP_LOGI(TAG, "Set battery level to %d%%", battery_level);
            } else {
                ESP_LOGW(TAG, "Invalid battery level: %d", battery_level);
            }
        }
        
        // 充电状态设置
        cJSON *charging_status = cJSON_GetObjectItem(json, "charging");
        if (charging_status && cJSON_IsBool(charging_status)) {
            bool charging = cJSON_IsTrue(charging_status);
            set_external_charging_status(charging);
            did_any = true;
            ESP_LOGI(TAG, "Set charging status to %s", charging ? "true" : "false");
        }
        
        cJSON_Delete(json);
        
        // 更新WS2812电量显示
        if (did_any) {
            ws2812_update_battery_display(get_battery_percentage(), is_charging());
            ESP_LOGI(TAG, "Updated WS2812 battery display");
        }
        
        httpd_resp_set_type(req, "application/json");
        if (did_any) {
            httpd_resp_sendstr(req, "{\"status\":\"success\",\"message\":\"电池控制命令执行成功\"}");
            ESP_LOGI(TAG, "Battery control command processed successfully");
        } else {
            httpd_resp_sendstr(req, "{\"status\":\"warning\",\"message\":\"没有识别到有效的电池控制命令\"}");
            ESP_LOGW(TAG, "No valid battery control commands found");
        }
    } else {
        ESP_LOGE(TAG, "Failed to parse battery control JSON: %s", content);
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"status\":\"error\",\"message\":\"无效的JSON格式\"}");
    }

    return ESP_OK;
}

// API统一控制接口 - 支持WS2812和电池的所有功能
static esp_err_t api_unified_handler(httpd_req_t *req)
{
    ESP_LOGI(TAG, "=== UNIFIED API CALLED ===");
    web_server_task_wdt_reset_if_registered();
    
    char content[2048];  // 增大缓冲区以支持复杂命令
    size_t to_read = req->content_len;
    if (to_read >= sizeof(content)) {
        ESP_LOGE(TAG, "Unified API request body too large: %d (max %d)", (int)to_read, (int)sizeof(content) - 1);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Request body too large");
        return ESP_FAIL;
    }

    size_t received = 0;
    while (received < to_read) {
        int ret = httpd_req_recv(req, content + received, to_read - received);
        if (ret <= 0) {
            if (ret == HTTPD_SOCK_ERR_TIMEOUT) {
                continue;
            }
            ESP_LOGE(TAG, "Failed to receive unified API request body");
            return ESP_FAIL;
        }
        received += ret;
    }
    content[received] = '\0';
    
    ESP_LOGI(TAG, "Unified API received command: %s", content);
    
    // 尝试处理WS2812命令
    esp_err_t ws2812_result = ws2812_handle_json_command(content);
    
    // 尝试处理电池控制命令
    cJSON *json = cJSON_Parse(content);
    bool battery_processed = false;
    
    if (json) {
        // 处理电压设置
        cJSON *voltage = cJSON_GetObjectItem(json, "voltage");
        if (voltage && cJSON_IsNumber(voltage)) {
            float voltage_val = (float)voltage->valuedouble;
            set_external_voltage(voltage_val);
            battery_processed = true;
            ESP_LOGI(TAG, "Unified API: Set voltage to %.2fV", voltage_val);
        }
        
        // 处理电量设置
        cJSON *battery = cJSON_GetObjectItem(json, "battery");
        if (battery && cJSON_IsNumber(battery)) {
            int battery_percentage = battery->valueint;
            set_external_battery_percentage(battery_percentage);
            battery_processed = true;
            ESP_LOGI(TAG, "Unified API: Set battery to %d%%", battery_percentage);
        }
        
        // 处理充电状态
        cJSON *charging = cJSON_GetObjectItem(json, "charging");
        if (charging && cJSON_IsBool(charging)) {
            bool charging_status = cJSON_IsTrue(charging);
            set_external_charging_status(charging_status);
            battery_processed = true;
            ESP_LOGI(TAG, "Unified API: Set charging to %s", charging_status ? "true" : "false");
        }
        
        // 处理电量显示配置
        cJSON *battery_channel = cJSON_GetObjectItem(json, "battery_channel");
        cJSON *show_charging_effect = cJSON_GetObjectItem(json, "show_charging_effect");
        cJSON *background_brightness = cJSON_GetObjectItem(json, "background_brightness");
        
        // 处理battery_display对象
        cJSON *battery_display_obj = cJSON_GetObjectItem(json, "battery_display");
        if (battery_display_obj && cJSON_IsObject(battery_display_obj)) {
            cJSON *bd_channel = cJSON_GetObjectItem(battery_display_obj, "channel");
            cJSON *bd_show_effect = cJSON_GetObjectItem(battery_display_obj, "show_charging_effect");
            cJSON *bd_brightness = cJSON_GetObjectItem(battery_display_obj, "background_brightness");
            
            uint8_t channel = 255;  // 默认禁用
            bool show_effect = true;  // 默认启用充电特效
            uint8_t brightness = 10;  // 默认背景亮度
            
            if (bd_channel && cJSON_IsNumber(bd_channel)) {
                channel = (uint8_t)bd_channel->valueint;
            }
            if (bd_show_effect && cJSON_IsBool(bd_show_effect)) {
                show_effect = cJSON_IsTrue(bd_show_effect);
            }
            if (bd_brightness && cJSON_IsNumber(bd_brightness)) {
                brightness = (uint8_t)bd_brightness->valueint;
            }
            
            esp_err_t result = ws2812_set_battery_display(channel, show_effect, brightness);
            if (result == ESP_OK) {
                battery_processed = true;
                ESP_LOGI(TAG, "Unified API: Set battery display from object - channel=%d, effect=%s, brightness=%d", 
                        channel, show_effect ? "true" : "false", brightness);
                // 立即更新电量显示以反映新配置
                ws2812_update_battery_display(get_battery_percentage(), is_charging());
            } else {
                ESP_LOGE(TAG, "Unified API: Failed to set battery display configuration from object");
            }
        }
        // 处理简化的电量显示配置
        else if (battery_channel || show_charging_effect || background_brightness) {
            uint8_t channel = 255;  // 默认禁用
            bool show_effect = true;  // 默认启用充电特效
            uint8_t brightness = 10;  // 默认背景亮度
            
            if (battery_channel && cJSON_IsNumber(battery_channel)) {
                channel = (uint8_t)battery_channel->valueint;
            }
            if (show_charging_effect && cJSON_IsBool(show_charging_effect)) {
                show_effect = cJSON_IsTrue(show_charging_effect);
            }
            if (background_brightness && cJSON_IsNumber(background_brightness)) {
                brightness = (uint8_t)background_brightness->valueint;
            }
            
            esp_err_t result = ws2812_set_battery_display(channel, show_effect, brightness);
            if (result == ESP_OK) {
                battery_processed = true;
                ESP_LOGI(TAG, "Unified API: Set battery display - channel=%d, effect=%s, brightness=%d", 
                        channel, show_effect ? "true" : "false", brightness);
                // 立即更新电量显示以反映新配置
                ws2812_update_battery_display(get_battery_percentage(), is_charging());
            } else {
                ESP_LOGE(TAG, "Unified API: Failed to set battery display configuration");
            }
        }
        
        // 处理单通道电量模式设置
        cJSON *channel_battery_mode = cJSON_GetObjectItem(json, "channel_battery_mode");
        if (channel_battery_mode && cJSON_IsObject(channel_battery_mode)) {
            cJSON *channel = cJSON_GetObjectItem(channel_battery_mode, "channel");
            cJSON *enable = cJSON_GetObjectItem(channel_battery_mode, "enable");
            cJSON *bg_brightness = cJSON_GetObjectItem(channel_battery_mode, "background_brightness");
            
            if (channel && cJSON_IsNumber(channel) && enable && cJSON_IsBool(enable)) {
                uint8_t ch = (uint8_t)channel->valueint;
                bool en = cJSON_IsTrue(enable);
                uint8_t brightness = 10;  // 默认值
                
                if (bg_brightness && cJSON_IsNumber(bg_brightness)) {
                    brightness = (uint8_t)bg_brightness->valueint;
                }
                
                esp_err_t result = ws2812_set_channel_battery_mode(ch, en, brightness);
                if (result == ESP_OK) {
                    battery_processed = true;
                    ESP_LOGI(TAG, "Unified API: Set channel %d battery mode to %s, brightness=%d", 
                            ch, en ? "enabled" : "disabled", brightness);
                    // 立即更新电量显示以反映新配置
                    ws2812_update_battery_display(get_battery_percentage(), is_charging());
                } else {
                    ESP_LOGE(TAG, "Unified API: Failed to set channel battery mode");
                }
            }
        }
        
        // 同步更新WS2812电量显示
        if (battery_processed) {
            ws2812_update_battery_display(get_battery_percentage(), is_charging());
        }
        
        cJSON_Delete(json);
    }
    
    // 准备响应
    httpd_resp_set_type(req, "application/json");
    
    if (ws2812_result == ESP_OK || battery_processed) {
        httpd_resp_sendstr(req, "{\"status\":\"success\",\"message\":\"命令执行成功\"}");
        ESP_LOGI(TAG, "Unified API: Command processed successfully");
    } else if (ws2812_result != ESP_OK) {
        httpd_resp_sendstr(req, "{\"status\":\"error\",\"message\":\"命令执行失败\"}");
        ESP_LOGE(TAG, "Unified API: Command processing failed");
    } else {
        httpd_resp_sendstr(req, "{\"status\":\"warning\",\"message\":\"未识别到有效命令\"}");
        ESP_LOGW(TAG, "Unified API: No valid commands found");
    }
    
    return ESP_OK;
}

// API系统状态查询接口
static esp_err_t api_status_handler(httpd_req_t *req)
{
    ESP_LOGI(TAG, "System status API called");
    web_server_task_wdt_reset_if_registered();
    
    // 获取系统状态信息
    cJSON *status = cJSON_CreateObject();
    
    // 电池信息 - 直接添加到根对象以匹配前端JS期望
    cJSON_AddNumberToObject(status, "voltage", get_battery_voltage());
    cJSON_AddNumberToObject(status, "battery_percentage", get_battery_percentage());
    cJSON_AddBoolToObject(status, "is_charging", is_charging());
    cJSON_AddNumberToObject(status, "music_sensitivity", ws2812_get_music_sensitivity());
    
    // WS2812通道状态
    cJSON *channels = cJSON_CreateArray();
    for (int i = 0; i < 4; i++) {
        cJSON *channel = cJSON_CreateObject();
        ws2812_channel_t config = ws2812_get_channel_config(i);
        uint32_t cycle_duration = ws2812_get_cycle_duration(i);

        cJSON_AddNumberToObject(channel, "id", i);
        cJSON_AddBoolToObject(channel, "enabled", config.enabled);
        cJSON_AddNumberToObject(channel, "mode", config.config.mode);
        cJSON_AddNumberToObject(channel, "led_count", config.led_count);
        cJSON_AddNumberToObject(channel, "cycle_duration", cycle_duration);
        
        cJSON *color = cJSON_CreateObject();
        cJSON_AddNumberToObject(color, "r", config.config.color.r);
        cJSON_AddNumberToObject(color, "g", config.config.color.g);
        cJSON_AddNumberToObject(color, "b", config.config.color.b);
        cJSON_AddItemToObject(channel, "color", color);
        
        cJSON_AddNumberToObject(channel, "brightness", config.config.brightness);
        cJSON_AddNumberToObject(channel, "speed", config.config.speed);
        cJSON_AddNumberToObject(channel, "music_bg_brightness", config.config.music_bg_brightness);
        cJSON_AddBoolToObject(channel, "music_colorful_mode", config.config.music_colorful_mode);

        cJSON_AddItemToArray(channels, channel);
    }
    cJSON_AddItemToObject(status, "channels", channels);
    
    // 电量显示配置
    ws2812_battery_config_t battery_config = ws2812_get_battery_config();
    cJSON *battery_display = cJSON_CreateObject();
    cJSON_AddNumberToObject(battery_display, "battery_channel", battery_config.battery_channel);
    cJSON_AddBoolToObject(battery_display, "show_charging_effect", battery_config.show_charging_effect);
    cJSON_AddNumberToObject(battery_display, "background_brightness", battery_config.background_brightness);
    cJSON_AddItemToObject(status, "battery_display", battery_display);
    
    // 系统信息
    cJSON_AddStringToObject(status, "device", "ESP32S3-TFT-BS");
    cJSON_AddStringToObject(status, "version", "2.1");
    cJSON_AddNumberToObject(status, "uptime", xTaskGetTickCount() / configTICK_RATE_HZ);
    
    char *json_string = cJSON_Print(status);
    cJSON_Delete(status);
    
    httpd_resp_set_type(req, "application/json");
    if (json_string) {
        httpd_resp_sendstr(req, json_string);
        free(json_string);
        ESP_LOGI(TAG, "System status sent successfully");
    } else {
        httpd_resp_sendstr(req, "{\"status\":\"error\",\"message\":\"获取系统状态失败\"}");
        ESP_LOGE(TAG, "Failed to create system status JSON");
    }

    return ESP_OK;
}

// OTA HTTP handlers live in ota_updater.c; registered via
// ota_updater_register_http_handlers() in start_web_server().

static bool json_object_complete(const char *buf, size_t len)
{
    int depth = 0;
    bool in_string = false;
    bool escape = false;
    bool seen_open = false;

    for (size_t i = 0; i < len; i++) {
        char c = buf[i];
        if (escape) {
            escape = false;
            continue;
        }
        if (in_string) {
            if (c == '\\') escape = true;
            else if (c == '"') in_string = false;
            continue;
        }
        if (c == '"') in_string = true;
        else if (c == '{') {
            depth++;
            seen_open = true;
        } else if (c == '}') {
            depth--;
            if (depth == 0 && seen_open) return true;
            if (depth < 0) return false;
        }
    }
    return false;
}

static void rgb_tcp_send_result(int fd, esp_err_t err)
{
    const char *reply = (err == ESP_OK)
        ? "{\"status\":\"success\",\"message\":\"命令执行成功\"}\n"
        : "{\"status\":\"error\",\"message\":\"命令执行失败\"}\n";
    send(fd, reply, strlen(reply), 0);
}

static esp_err_t process_rgb_json_command(const char *content)
{
    esp_err_t ws2812_result = ws2812_handle_json_command(content);
    bool battery_processed = false;

    cJSON *json = cJSON_Parse(content);
    if (json) {
        cJSON *voltage = cJSON_GetObjectItem(json, "voltage");
        if (voltage && cJSON_IsNumber(voltage)) {
            set_external_voltage((float)voltage->valuedouble);
            battery_processed = true;
            ESP_LOGI(TAG, "TCP JSON: set voltage %.2fV", voltage->valuedouble);
        }

        cJSON *battery = cJSON_GetObjectItem(json, "battery");
        if (battery && cJSON_IsNumber(battery)) {
            set_external_battery_percentage(battery->valueint);
            battery_processed = true;
            ESP_LOGI(TAG, "TCP JSON: set battery %d%%", battery->valueint);
        }

        cJSON *charging = cJSON_GetObjectItem(json, "charging");
        if (charging && cJSON_IsBool(charging)) {
            bool charging_status = cJSON_IsTrue(charging);
            set_external_charging_status(charging_status);
            battery_processed = true;
            ESP_LOGI(TAG, "TCP JSON: set charging %s", charging_status ? "true" : "false");
        }

        cJSON *auto_mode = cJSON_GetObjectItem(json, "auto_mode");
        if (auto_mode && cJSON_IsBool(auto_mode) && cJSON_IsTrue(auto_mode)) {
            restore_auto_battery_mode();
            battery_processed = true;
            ESP_LOGI(TAG, "TCP JSON: restore auto battery mode");
        }

        cJSON *auto_charging = cJSON_GetObjectItem(json, "auto_charging");
        if (auto_charging && cJSON_IsBool(auto_charging) && cJSON_IsTrue(auto_charging)) {
            restore_auto_charging_mode();
            battery_processed = true;
            ESP_LOGI(TAG, "TCP JSON: restore auto charging mode");
        }

        cJSON *battery_channel = cJSON_GetObjectItem(json, "battery_channel");
        cJSON *show_charging_effect = cJSON_GetObjectItem(json, "show_charging_effect");
        cJSON *background_brightness = cJSON_GetObjectItem(json, "background_brightness");
        cJSON *battery_display_obj = cJSON_GetObjectItem(json, "battery_display");

        if (battery_display_obj && cJSON_IsObject(battery_display_obj)) {
            cJSON *bd_channel = cJSON_GetObjectItem(battery_display_obj, "channel");
            cJSON *bd_show_effect = cJSON_GetObjectItem(battery_display_obj, "show_charging_effect");
            cJSON *bd_brightness = cJSON_GetObjectItem(battery_display_obj, "background_brightness");
            uint8_t channel = 255;
            bool show_effect = true;
            uint8_t brightness = 10;

            if (bd_channel && cJSON_IsNumber(bd_channel)) channel = (uint8_t)bd_channel->valueint;
            if (bd_show_effect && cJSON_IsBool(bd_show_effect)) show_effect = cJSON_IsTrue(bd_show_effect);
            if (bd_brightness && cJSON_IsNumber(bd_brightness)) brightness = (uint8_t)bd_brightness->valueint;

            if (ws2812_set_battery_display(channel, show_effect, brightness) == ESP_OK) {
                battery_processed = true;
                ESP_LOGI(TAG, "TCP JSON: set battery display channel=%d brightness=%d", channel, brightness);
            }
        } else if (battery_channel || show_charging_effect || background_brightness) {
            uint8_t channel = 255;
            bool show_effect = true;
            uint8_t brightness = 10;

            if (battery_channel && cJSON_IsNumber(battery_channel)) channel = (uint8_t)battery_channel->valueint;
            if (show_charging_effect && cJSON_IsBool(show_charging_effect)) show_effect = cJSON_IsTrue(show_charging_effect);
            if (background_brightness && cJSON_IsNumber(background_brightness)) brightness = (uint8_t)background_brightness->valueint;

            if (ws2812_set_battery_display(channel, show_effect, brightness) == ESP_OK) {
                battery_processed = true;
                ESP_LOGI(TAG, "TCP JSON: set battery display channel=%d brightness=%d", channel, brightness);
            }
        }

        cJSON *channel_battery_mode = cJSON_GetObjectItem(json, "channel_battery_mode");
        if (channel_battery_mode && cJSON_IsObject(channel_battery_mode)) {
            cJSON *channel = cJSON_GetObjectItem(channel_battery_mode, "channel");
            cJSON *enable = cJSON_GetObjectItem(channel_battery_mode, "enable");
            cJSON *bg_brightness = cJSON_GetObjectItem(channel_battery_mode, "background_brightness");
            if (channel && cJSON_IsNumber(channel) && enable && cJSON_IsBool(enable)) {
                uint8_t brightness = 10;
                if (bg_brightness && cJSON_IsNumber(bg_brightness)) {
                    brightness = (uint8_t)bg_brightness->valueint;
                }
                if (ws2812_set_channel_battery_mode((uint8_t)channel->valueint,
                                                    cJSON_IsTrue(enable),
                                                    brightness) == ESP_OK) {
                    battery_processed = true;
                    ESP_LOGI(TAG, "TCP JSON: set channel %d battery mode", channel->valueint);
                }
            }
        }

        if (battery_processed) {
            ws2812_update_battery_display(get_battery_percentage(), is_charging());
        }

        cJSON_Delete(json);
    }

    return (ws2812_result == ESP_OK || battery_processed) ? ESP_OK : ESP_FAIL;
}

static void rgb_tcp_handle_client(int client_fd, const char *peer_ip)
{
    char rx[RGB_TCP_RX_BUF_LEN];
    size_t rx_len = 0;

    ESP_LOGI(TAG, "RGB TCP client connected: %s", peer_ip);
    s_rgb_tcp_active_clients++;
    const char *hello = "{\"status\":\"ready\",\"protocol\":\"ws2812-json\"}\n";
    send(client_fd, hello, strlen(hello), 0);

    while (true) {
        int n = recv(client_fd, rx + rx_len, sizeof(rx) - rx_len - 1, 0);
        if (n <= 0) break;
        rx_len += (size_t)n;
        rx[rx_len] = '\0';

        char *line_start = rx;
        while (true) {
            char *newline = strpbrk(line_start, "\r\n");
            if (!newline) break;
            *newline = '\0';
            if (line_start[0] != '\0') {
                ESP_LOGI(TAG, "RGB TCP JSON: %s", line_start);
                rgb_tcp_send_result(client_fd, process_rgb_json_command(line_start));
            }
            line_start = newline + 1;
            while (*line_start == '\r' || *line_start == '\n') line_start++;
        }

        size_t remain = rx + rx_len - line_start;
        if (line_start != rx && remain > 0) memmove(rx, line_start, remain);
        rx_len = remain;
        rx[rx_len] = '\0';

        if (rx_len > 0 && json_object_complete(rx, rx_len)) {
            ESP_LOGI(TAG, "RGB TCP JSON: %s", rx);
            rgb_tcp_send_result(client_fd, process_rgb_json_command(rx));
            rx_len = 0;
            rx[0] = '\0';
        } else if (rx_len >= sizeof(rx) - 1) {
            ESP_LOGW(TAG, "RGB TCP receive buffer overflow, dropping partial command");
            const char *err = "{\"status\":\"error\",\"message\":\"JSON too large\"}\n";
            send(client_fd, err, strlen(err), 0);
            rx_len = 0;
            rx[0] = '\0';
        }
    }

    shutdown(client_fd, SHUT_RDWR);
    close(client_fd);
    if (s_rgb_tcp_active_clients > 0) s_rgb_tcp_active_clients--;
    ESP_LOGI(TAG, "RGB TCP client disconnected: %s", peer_ip);
}

static void rgb_tcp_server_task(void *arg)
{
    (void)arg;
    int listen_fd = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
    if (listen_fd < 0) {
        ESP_LOGE(TAG, "RGB TCP socket failed: errno=%d", errno);
        s_rgb_tcp_task = NULL;
        vTaskDelete(NULL);
        return;
    }

    int yes = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(RGB_TCP_CONTROL_PORT);

    if (bind(listen_fd, (struct sockaddr *)&addr, sizeof(addr)) != 0 ||
        listen(listen_fd, 3) != 0) {
        ESP_LOGE(TAG, "RGB TCP bind/listen failed: errno=%d", errno);
        close(listen_fd);
        s_rgb_tcp_task = NULL;
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "RGB TCP JSON control listening on port %d", RGB_TCP_CONTROL_PORT);
    while (true) {
        struct sockaddr_in peer = {0};
        socklen_t peer_len = sizeof(peer);
        int client_fd = accept(listen_fd, (struct sockaddr *)&peer, &peer_len);
        if (client_fd < 0) {
            ESP_LOGW(TAG, "RGB TCP accept failed: errno=%d", errno);
            continue;
        }
        char peer_ip[16];
        inet_ntoa_r(peer.sin_addr, peer_ip, sizeof(peer_ip));
        rgb_tcp_handle_client(client_fd, peer_ip);
    }
}

static void start_rgb_tcp_server(void)
{
    if (s_rgb_tcp_task) return;
    xTaskCreate(rgb_tcp_server_task, "rgb_tcp", 4096, NULL, 4, &s_rgb_tcp_task);
}

// 启动Web服务器
httpd_handle_t start_webserver(void)
{
    if (server != NULL) {
        ESP_LOGW(TAG, "Web server already running");
        return server;
    }

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_uri_handlers = 24;  // builtin APIs + WiFi config + OTA endpoints
    config.max_resp_headers = 8;
    config.task_priority = 5;
    config.stack_size = 8192;
    config.server_port = 80;
    config.ctrl_port = 32768;
    config.max_open_sockets = 4;
    config.send_wait_timeout = 10;
    config.recv_wait_timeout = 10;

    ESP_LOGI(TAG, "Starting HTTP server on port %d", config.server_port);

    if (httpd_start(&server, &config) == ESP_OK) {
        // 根路径处理器
        httpd_uri_t root_uri = {
            .uri = "/",
            .method = HTTP_GET,
            .handler = root_get_handler,
            .user_ctx = NULL
        };
        httpd_register_uri_handler(server, &root_uri);

        httpd_uri_t wifi_uri = {
            .uri = "/wifi",
            .method = HTTP_GET,
            .handler = wifi_config_get_handler,
            .user_ctx = NULL
        };
        httpd_register_uri_handler(server, &wifi_uri);

        httpd_uri_t wifi_cfg_get_uri = {
            .uri = "/wificfg",
            .method = HTTP_GET,
            .handler = wifi_config_get_handler,
            .user_ctx = NULL
        };
        httpd_register_uri_handler(server, &wifi_cfg_get_uri);

        httpd_uri_t wifi_cfg_post_uri = {
            .uri = "/wificfg",
            .method = HTTP_POST,
            .handler = wifi_config_post_handler,
            .user_ctx = NULL
        };
        httpd_register_uri_handler(server, &wifi_cfg_post_uri);

        httpd_uri_t wifi_clear_uri = {
            .uri = "/wificlear",
            .method = HTTP_POST,
            .handler = wifi_clear_post_handler,
            .user_ctx = NULL
        };
        httpd_register_uri_handler(server, &wifi_clear_uri);

        httpd_uri_t wifi_scan_uri = {
            .uri = "/wifiscan",
            .method = HTTP_GET,
            .handler = wifi_scan_get_handler,
            .user_ctx = NULL
        };
        httpd_register_uri_handler(server, &wifi_scan_uri);

        httpd_uri_t ota_page_uri = {
            .uri = "/ota",
            .method = HTTP_GET,
            .handler = ota_page_get_handler,
            .user_ctx = NULL
        };
        httpd_register_uri_handler(server, &ota_page_uri);

        // API控制处理器
        httpd_uri_t control_uri = {
            .uri = "/api/control",
            .method = HTTP_POST,
            .handler = api_control_handler,
            .user_ctx = NULL
        };
        httpd_register_uri_handler(server, &control_uri);

        // API测试处理器
        httpd_uri_t test_uri = {
            .uri = "/api/test",
            .method = HTTP_POST,
            .handler = api_test_handler,
            .user_ctx = NULL
        };
        httpd_register_uri_handler(server, &test_uri);

        // API简单测试处理器
        httpd_uri_t simple_uri = {
            .uri = "/api/simple",
            .method = HTTP_POST,
            .handler = api_simple_test_handler,
            .user_ctx = NULL
        };
        httpd_register_uri_handler(server, &simple_uri);

        // API电池状态查询处理器
        httpd_uri_t battery_status_uri = {
            .uri = "/api/battery/status",
            .method = HTTP_GET,
            .handler = api_battery_status_handler,
            .user_ctx = NULL
        };
        httpd_register_uri_handler(server, &battery_status_uri);

        // API电池控制处理器
        httpd_uri_t battery_control_uri = {
            .uri = "/api/battery/control",
            .method = HTTP_POST,
            .handler = api_battery_control_handler,
            .user_ctx = NULL
        };
        httpd_register_uri_handler(server, &battery_control_uri);

        // API统一控制接口 - 支持所有WS2812和电池功能
        httpd_uri_t unified_uri = {
            .uri = "/api/unified",
            .method = HTTP_POST,
            .handler = api_unified_handler,
            .user_ctx = NULL
        };
        httpd_register_uri_handler(server, &unified_uri);

        // API系统状态查询接口
        httpd_uri_t status_uri = {
            .uri = "/api/status",
            .method = HTTP_GET,
            .handler = api_status_handler,
            .user_ctx = NULL
        };
        httpd_register_uri_handler(server, &status_uri);

        ota_updater_register_http_handlers(server);
        start_rgb_tcp_server();

        ESP_LOGI(TAG, "Web server started successfully with HTTP APIs and TCP:%d", RGB_TCP_CONTROL_PORT);
        return server;
    } else {
        ESP_LOGE(TAG, "Failed to start web server");
        return NULL;
    }
}

// 停止Web服务器
esp_err_t stop_webserver(httpd_handle_t server_handle)
{
    if (server_handle != NULL) {
        ESP_LOGI(TAG, "Stopping web server");
        httpd_stop(server_handle);
        server = NULL;
    }
    return ESP_OK;
}

// 初始化完整的Web服务器系统
httpd_handle_t web_server_init(void)
{
    ESP_LOGI(TAG, "Initializing Web Server System");
    
    // 初始化WiFi: 优先连接已保存路由器, 失败则进入 AP 配网
    esp_err_t ret = wifi_init_ap();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize WiFi");
        return NULL;
    }
    
    // 启动HTTP服务器
    httpd_handle_t server_handle = start_webserver();
    if (server_handle == NULL) {
        ESP_LOGE(TAG, "Failed to start HTTP server");
        return NULL;
    }
    
    ESP_LOGI(TAG, "Web Server System initialized successfully");
    return server_handle;
}

// WiFi事件处理器
static void wifi_event_handler(void* arg, esp_event_base_t event_base,
                              int32_t event_id, void* event_data)
{
    (void)arg;
    if (event_base == WIFI_EVENT) {
        if (event_id == WIFI_EVENT_STA_START) {
            if (s_want_sta_connect) esp_wifi_connect();
        } else if (event_id == WIFI_EVENT_STA_DISCONNECTED) {
            if (!s_want_sta_connect) return;
            if (s_sta_retry < RGB_STA_MAX_RETRY) {
                s_sta_retry++;
                ESP_LOGW(TAG, "STA disconnected, retry %d/%d", s_sta_retry, RGB_STA_MAX_RETRY);
                esp_wifi_connect();
            } else if (s_wifi_evt) {
                xEventGroupSetBits(s_wifi_evt, RGB_WIFI_BIT_FAIL);
            }
        } else if (event_id == WIFI_EVENT_AP_STACONNECTED) {
            wifi_event_ap_staconnected_t* event = (wifi_event_ap_staconnected_t*) event_data;
            ESP_LOGI(TAG, "Station "MACSTR" joined, AID=%d", MAC2STR(event->mac), event->aid);
        } else if (event_id == WIFI_EVENT_AP_STADISCONNECTED) {
            wifi_event_ap_stadisconnected_t* event = (wifi_event_ap_stadisconnected_t*) event_data;
            ESP_LOGI(TAG, "Station "MACSTR" left, AID=%d", MAC2STR(event->mac), event->aid);
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        snprintf(s_ip_str, sizeof(s_ip_str), IPSTR, IP2STR(&event->ip_info.ip));
        s_sta_retry = 0;
        s_ap_mode = false;
        ESP_LOGI(TAG, "==================================================");
        ESP_LOGI(TAG, "WiFi connected, device IP: %s", s_ip_str);
        ESP_LOGI(TAG, "Web control: http://%s/", s_ip_str);
        ESP_LOGI(TAG, "WiFi config:  http://%s/wifi", s_ip_str);
        ESP_LOGI(TAG, "OTA update:   http://%s/ota", s_ip_str);
        ESP_LOGI(TAG, "TCP JSON:     %s:%d", s_ip_str, RGB_TCP_CONTROL_PORT);
        ESP_LOGI(TAG, "==================================================");
        if (s_wifi_evt) xEventGroupSetBits(s_wifi_evt, RGB_WIFI_BIT_GOT_IP);
        ota_updater_note_sta_got_ip();
    }
}

static esp_err_t start_wifi_sta(const char *ssid, const char *pass)
{
    if (!s_netif_sta) s_netif_sta = esp_netif_create_default_wifi_sta();

    wifi_config_t wifi_config = {0};
    strncpy((char *)wifi_config.sta.ssid, ssid, sizeof(wifi_config.sta.ssid) - 1);
    strncpy((char *)wifi_config.sta.password, pass, sizeof(wifi_config.sta.password) - 1);
    wifi_config.sta.threshold.authmode = WIFI_AUTH_OPEN;
    wifi_config.sta.pmf_cfg.capable = true;

    s_sta_retry = 0;
    s_want_sta_connect = true;
    xEventGroupClearBits(s_wifi_evt, RGB_WIFI_BIT_GOT_IP | RGB_WIFI_BIT_FAIL);

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    EventBits_t bits = xEventGroupWaitBits(s_wifi_evt,
                                           RGB_WIFI_BIT_GOT_IP | RGB_WIFI_BIT_FAIL,
                                           pdFALSE,
                                           pdFALSE,
                                           pdMS_TO_TICKS(RGB_STA_CONNECT_TIMEOUT_MS));
    if (bits & RGB_WIFI_BIT_GOT_IP) {
        ESP_LOGI(TAG, "WiFi STA initialized. IP: %s", s_ip_str);
        return ESP_OK;
    }

    ESP_LOGW(TAG, "WiFi STA connect failed or timed out");
    s_want_sta_connect = false;
    esp_wifi_stop();
    return ESP_FAIL;
}

static esp_err_t start_wifi_ap_provisioning(void)
{
    if (!s_netif_ap) s_netif_ap = esp_netif_create_default_wifi_ap();
    if (!s_netif_sta) s_netif_sta = esp_netif_create_default_wifi_sta();

    wifi_config_t ap_config = {
        .ap = {
            .channel = WIFI_CHANNEL,
            .password = WIFI_PASS,
            .max_connection = WIFI_MAX_STA_CONN,
            .authmode = WIFI_AUTH_WPA_WPA2_PSK
        },
    };
    size_t ssid_len = strnlen(s_light_ssid, sizeof(s_light_ssid));
    memcpy(ap_config.ap.ssid, s_light_ssid, ssid_len);
    ap_config.ap.ssid_len = ssid_len;

    wifi_config_t sta_empty = {0};
    s_want_sta_connect = false;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_config));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta_empty));
    ESP_ERROR_CHECK(esp_wifi_start());

    snprintf(s_ip_str, sizeof(s_ip_str), "192.168.4.1");
    s_ap_mode = true;
    ESP_LOGI(TAG, "==================================================");
    ESP_LOGI(TAG, "WiFi provisioning AP started");
    ESP_LOGI(TAG, "AP SSID:      %s", s_light_ssid);
    ESP_LOGI(TAG, "AP Password:  %s", WIFI_PASS);
    ESP_LOGI(TAG, "WiFi config:  http://%s/wifi", s_ip_str);
    ESP_LOGI(TAG, "Web control:  http://%s/", s_ip_str);
    ESP_LOGI(TAG, "==================================================");
    return ESP_OK;
}

// 初始化WiFi: 优先 STA, 失败则 APSTA 配网
esp_err_t wifi_init_ap(void)
{
    compute_light_ssid();

    ESP_ERROR_CHECK(esp_netif_init());
    esp_err_t err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) return err;

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
                                                        ESP_EVENT_ANY_ID,
                                                        &wifi_event_handler,
                                                        NULL,
                                                        NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT,
                                                        IP_EVENT_STA_GOT_IP,
                                                        &wifi_event_handler,
                                                        NULL,
                                                        NULL));

    if (!s_wifi_evt) {
        s_wifi_evt = xEventGroupCreate();
        if (!s_wifi_evt) return ESP_ERR_NO_MEM;
    }

    char ssid[64] = {0};
    char pass[64] = {0};
    if (rgb_wifi_creds_load(ssid, sizeof(ssid), pass, sizeof(pass)) == ESP_OK && ssid[0] != '\0') {
        ESP_LOGI(TAG, "Saved RGB WiFi credentials found, trying STA: %s", ssid);
        if (start_wifi_sta(ssid, pass) == ESP_OK) return ESP_OK;
    }

    return start_wifi_ap_provisioning();
}
