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
"<title>ESP32S3-TFT-BS 智能控制面板</title>"
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
"<div class='status-info' id='status'>状态: 连接成功 | 设备IP: 192.168.4.1 | WiFi: ESP32_LightControl</div>"
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

// API电池状态查询处理函数
static esp_err_t api_battery_status_handler(httpd_req_t *req)
{
    ESP_LOGI(TAG, "Battery status API called");
    esp_task_wdt_reset();

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
    esp_task_wdt_reset();
    
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
        esp_task_wdt_reset();
    }
    content[received] = '\0';

    ESP_LOGI(TAG, "Received battery control command (%d bytes): %s", (int)received, content);
    esp_task_wdt_reset();
    
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
    esp_task_wdt_reset();
    
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
    esp_task_wdt_reset();
    
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

        ESP_LOGI(TAG, "Web server started successfully with %d API endpoints", 8);
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

    /* 基于芯片 MAC 生成唯一 SSID：ESP32_Light_XXXX (取 MAC 后 2 字节, 同一芯片固定) */
    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
    char ssid_buf[32] = {0};
    int ssid_len = snprintf(ssid_buf, sizeof(ssid_buf), "ESP32_Light_%02X%02X", mac[4], mac[5]);

    wifi_config_t wifi_config = {
        .ap = {
            .channel = WIFI_CHANNEL,
            .password = WIFI_PASS,
            .max_connection = WIFI_MAX_STA_CONN,
            .authmode = WIFI_AUTH_WPA_WPA2_PSK
        },
    };
    memcpy(wifi_config.ap.ssid, ssid_buf, ssid_len);
    wifi_config.ap.ssid_len = ssid_len;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "WiFi AP initialized. SSID: %s, Password: %s", ssid_buf, wifi_config.ap.password);
    
    return ESP_OK;
}
