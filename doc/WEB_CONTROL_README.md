# ESP32S3 WS2812 灯光控制系统

这是一个基于ESP32S3的WS2812多通道灯光控制系统，支持通过Web界面进行控制。

## 新增功能

### HTTP网页控制

1. **WiFi AP模式**
   - SSID: `ESP32_LightControl`
   - 密码: `12345678`
   - IP地址: `192.168.4.1`

2. **Web界面功能**
   - 广播控制：同时控制所有4个通道
   - 单通道控制：独立控制每个通道
   - 实时预览：颜色和亮度实时预览
   - 快速预设：一键设置常用模式

3. **支持的灯光效果**
   - 关闭
   - 静态颜色
   - 彩虹效果
   - 呼吸灯
   - 跑马灯
   - 闪烁效果
   - 波浪效果
   - 自动循环

## 使用方法

1. 烧录固件到ESP32S3
2. 连接手机或电脑WiFi到 `ESP32_LightControl`，密码 `12345678`
3. 打开浏览器访问 `http://192.168.4.1`
4. 使用Web界面控制灯光效果

## API接口

### 控制接口
- POST `/api/control`
- Content-Type: `application/json`

示例数据：
```json
{
  "channel": 0,        // 0-3为单通道，255为广播
  "mode": 1,           // 灯光模式
  "color": {           // RGB颜色
    "r": 255,
    "g": 0, 
    "b": 0
  },
  "brightness": 128,   // 亮度 0-255
  "speed": 100        // 效果速度（毫秒）
}
```

### 状态查询接口
- GET `/api/status?channel=0`

## 硬件连接

- 通道1: GPIO18
- 通道2: GPIO19  
- 通道3: GPIO20
- 通道4: GPIO21
- 每通道支持160个LED

## 编译说明

确保ESP-IDF环境已正确设置，然后执行：

```bash
idf.py build
idf.py flash monitor
```

系统将自动下载所需的组件依赖（cJSON、LVGL、LED_STRIP等）。
