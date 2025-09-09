#include "web_server.h"
#include "ws2812_control.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_task_wdt.h"
#include "nvs_flash.h"
#include "esp_netif.h"
#include "esp_mac.h"
#include "Lib/cJSON/cJSON.h"
#include <string.h>

static const char *TAG = "WEB_SERVER";
static httpd_handle_t server = NULL;

// 完整的HTML网页内容
static const char* complete_html_page = 
"<!DOCTYPE html>"
"<html>"
"<head>"
"<meta charset='UTF-8'>"
"<meta name='viewport' content='width=device-width, initial-scale=1.0'>"
"<title>ESP32 灯光控制</title>"
"<style>"
"body { font-family: Arial, sans-serif; margin: 20px; background: #f0f0f0; }"
".container { max-width: 800px; margin: 0 auto; background: white; padding: 20px; border-radius: 10px; box-shadow: 0 2px 10px rgba(0,0,0,0.1); }"
".channel-group { border: 2px solid #ddd; border-radius: 10px; margin: 15px 0; padding: 15px; }"
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
".status-info { background: #d4edda; border-left: 4px solid #28a745; padding: 10px; margin: 10px 0; }"
"</style>"
"</head>"
"<body>"
"<div class='container'>"
"<h1>🌈 ESP32S3 WS2812 灯光控制面板</h1>"
"<div class='status-info' id='status'>状态: 连接成功</div>"
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
"</select>"
"</div>"
"<div class='control-row'>"
"<input type='range' id='broadcast-r' min='0' max='255' value='255'>"
"<span class='range-value' id='broadcast-r-val'>255</span>R"
"<input type='range' id='broadcast-g' min='0' max='255' value='255'>"
"<span class='range-value' id='broadcast-g-val'>255</span>G"
"<input type='range' id='broadcast-b' min='0' max='255' value='255'>"
"<span class='range-value' id='broadcast-b-val'>255</span>B"
"<span class='control-label'>颜色:</span>"
"<div class='color-preview' id='broadcast-color'></div>"
"</div>"
"<div class='control-row'>"
"<span class='control-label'>亮度:</span>"
"<input type='range' id='broadcast-brightness' min='0' max='255' value='128'>"
"<span class='range-value' id='broadcast-brightness-val'>128</span>"
"</div>"
"<div class='control-row'>"
"<span class='control-label'>速度:</span>"
"<input type='range' id='broadcast-speed' min='10' max='2000' value='100'>"
"<span class='range-value' id='broadcast-speed-val'>100</span>ms"
"</div>"
"<button onclick='applyBroadcast()'>应用到所有通道</button>"
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
"</select>"
"</div>"
"<div class='control-row'>"
"<input type='range' id='channel-r' min='0' max='255' value='255'>"
"<span class='range-value' id='channel-r-val'>255</span>R"
"<input type='range' id='channel-g' min='0' max='255' value='255'>"
"<span class='range-value' id='channel-g-val'>255</span>G"
"<input type='range' id='channel-b' min='0' max='255' value='255'>"
"<span class='range-value' id='channel-b-val'>255</span>B"
"<span class='control-label'>颜色:</span>"
"<div class='color-preview' id='channel-color'></div>"
"</div>"
"</div>"
"<div class='control-row'>"
"<span class='control-label'>亮度:</span>"
"<input type='range' id='channel-brightness' min='0' max='255' value='128'>"
"<span class='range-value' id='channel-brightness-val'>128</span>"
"</div>"
"<div class='control-row'>"
"<span class='control-label'>速度:</span>"
"<input type='range' id='channel-speed' min='10' max='2000' value='100'>"
"<span class='range-value' id='channel-speed-val'>100</span>ms"
"</div>"
"<button onclick='applyChannel()'>应用到选中通道</button>"
"</div>"
"<div class='channel-group'>"
"<div class='channel-title'>⚡ 快速设置</div>"
"<div class='control-row center'>"
"<button onclick='preset(\"off\")'>全部关闭</button>"
"<button onclick='preset(\"white\")'>白色</button>"
"<button onclick='preset(\"rainbow\")'>彩虹</button>"
"<button onclick='preset(\"party\")'>派对模式</button>"
"<button onclick='testLights()'>测试灯光</button>"
"<button onclick='simpleTest()'>简单测试</button>"
"<button onclick='debugTest()'>调试测试</button>"
"</div>"
"</div>"
"</div>"
"<script>"
"function updateSliderValue(sliderId, valueId, suffix='') {"
"  const slider = document.getElementById(sliderId);"
"  const valueSpan = document.getElementById(valueId);"
"  valueSpan.textContent = slider.value + suffix;"
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
"      data.mode = 2;"
"      data.speed = 50;"
"      data.brightness = 150;"
"      break;"
"    case 'party':"
"      data.mode = 7;"
"      data.speed = 30;"
"      data.brightness = 255;"
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
"    speed: parseInt(document.getElementById('channel-speed').value)"
"  };"
"  sendRequest('/api/control', data);"
"}"
"function loadChannelConfig() {"
"  console.log('Loading channel config...');"
"}"
"window.onload = function() {"
"  console.log('Page loaded, initializing...');"
"  var sliders = ['broadcast-r', 'broadcast-g', 'broadcast-b', 'broadcast-brightness', 'broadcast-speed',"
"                 'channel-r', 'channel-g', 'channel-b', 'channel-brightness', 'channel-speed'];"
"  for(var i = 0; i < sliders.length; i++) {"
"    var id = sliders[i];"
"    var slider = document.getElementById(id);"
"    if (slider) {"
"      var valueId = id + '-val';"
"      var suffix = id.includes('speed') ? 'ms' : '';"
"      console.log('Initializing slider:', id, 'with value:', slider.value);"
"      slider.oninput = (function(sid, vid, suf) {"
"        return function() { updateSliderValue(sid, vid, suf); };"
"      })(id, valueId, suffix);"
"      updateSliderValue(id, valueId, suffix);"
"    } else {"
"      console.error('Slider not found:', id);"
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

// API控制处理函数
static esp_err_t api_control_handler(httpd_req_t *req)
{
    ESP_LOGI(TAG, "=== API CONTROL HANDLER CALLED ===");
    esp_task_wdt_reset();
    
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
        esp_task_wdt_reset();
    }
    content[received] = '\0';

    ESP_LOGI(TAG, "Received control command (%d bytes of %d): %s", (int)received, (int)to_read, content);
    esp_task_wdt_reset();
    
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
    esp_task_wdt_reset();

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
    esp_task_wdt_reset();

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"status\":\"ok\"}");
    ESP_LOGI(TAG, "Simple test responded");

    return ESP_OK;
}

// 启动Web服务器
httpd_handle_t start_webserver(void)
{
    if (server != NULL) {
        ESP_LOGW(TAG, "Web server already running");
        return server;
    }

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_uri_handlers = 10;
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

        ESP_LOGI(TAG, "Web server started successfully");
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
    
    // 初始化WiFi AP模式
    esp_err_t ret = wifi_init_ap();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize WiFi AP");
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
    if (event_id == WIFI_EVENT_AP_STACONNECTED) {
        wifi_event_ap_staconnected_t* event = (wifi_event_ap_staconnected_t*) event_data;
        ESP_LOGI(TAG, "Station "MACSTR" joined, AID=%d", MAC2STR(event->mac), event->aid);
    } else if (event_id == WIFI_EVENT_AP_STADISCONNECTED) {
        wifi_event_ap_stadisconnected_t* event = (wifi_event_ap_stadisconnected_t*) event_data;
        ESP_LOGI(TAG, "Station "MACSTR" left, AID=%d", MAC2STR(event->mac), event->aid);
    }
}

// 初始化WiFi AP模式
esp_err_t wifi_init_ap(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_ap();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
                                                        ESP_EVENT_ANY_ID,
                                                        &wifi_event_handler,
                                                        NULL,
                                                        NULL));

    wifi_config_t wifi_config = {
        .ap = {
            .ssid = "ESP32_LightControl",
            .ssid_len = strlen("ESP32_LightControl"),
            .channel = 1,
            .password = "12345678",
            .max_connection = 4,
            .authmode = WIFI_AUTH_WPA_WPA2_PSK
        },
    };

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "WiFi AP initialized. SSID: %s, Password: %s", wifi_config.ap.ssid, wifi_config.ap.password);
    
    return ESP_OK;
}
