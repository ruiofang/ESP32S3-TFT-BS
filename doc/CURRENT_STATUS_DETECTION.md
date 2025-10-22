# RS485电流状态判断功能说明

## 概述

根据485通用协议V19，电流数据为2字节有符号整数，单位10mA：
- **充电**: 电流为正值 (> 0.05A)
- **放电**: 电流为负值 (< -0.05A)  
- **静置**: 电流接近0 (-0.05A ≤ 电流 ≤ 0.05A)

## 功能特性

### 🔋 智能状态检测
- **精确阈值**: 使用±0.05A阈值避免噪声误判
- **实时更新**: 每次RS485查询成功后立即更新状态
- **状态日志**: 充电/放电状态变化时自动记录

### 📱 显示增强
- **电量标签**: 根据电流状态显示不同图标
  - 充电: `75% 🔋⚡ 26.6V`
  - 放电: `75% ⚡ 26.6V`
  - 静置: `75% 26.6V`

- **信息标签**: 显示详细电流状态
  - 充电中: `RS485: 2.500A 充电中 15串 23.5℃`
  - 放电中: `RS485: -1.200A 放电中 15串 23.5℃`
  - 静置: `RS485: 0.020A 静置 15串 23.5℃`

### 🎨 视觉反馈
- **颜色编码**:
  - 🔵 **充电**: 蓝色 (#00AAFF)
  - 🟠 **放电**: 橙色 (#FFAA00) 
  - ⚪ **静置**: 灰色 (#888888)

- **动态动画**: 充电时启动充电动画效果

## 测试示例

### 示例1: 充电状态
```
收到数据: DD 03 00 26 0A 67 00 64 09 65 ... 77
解析结果:
- 电压: 26.63V (0A67 = 2663, ÷100)
- 电流: 1.00A (0064 = 100, ÷100) 
- 电量: 99% (63)

显示效果:
- 电量: "99% 🔋⚡ 26.6V" (蓝色)
- 信息: "RS485: 1.000A 充电中 15串 23.5℃" (蓝色)
- 日志: "🔋 Charging started: 1.000A"
```

### 示例2: 放电状态  
```
收到数据: DD 03 00 26 0A 67 FF 9C 09 65 ... 77
解析结果:
- 电压: 26.63V
- 电流: -1.00A (FF9C = -100补码, ÷100)
- 电量: 99%

显示效果:
- 电量: "99% ⚡ 26.6V" (橙色)
- 信息: "RS485: -1.000A 放电中 15串 23.5℃" (橙色)
- 日志: "⚡ Discharging started: -1.000A"
```

### 示例3: 静置状态
```
收到数据: DD 03 00 26 0A 67 00 02 09 65 ... 77
解析结果:
- 电压: 26.63V
- 电流: 0.02A (0002 = 2, ÷100)
- 电量: 99%

显示效果:
- 电量: "99% 26.6V" (灰色)
- 信息: "RS485: 0.020A 静置 15串 23.5℃" (灰色)  
- 日志: "🔌 Battery idle: 0.020A"
```

## 核心代码逻辑

```c
// 根据485协议判断充放电状态
bool is_charging_now = false;
bool is_discharging_now = false;

if (g_battery_data.pack_current > 0.05f) {
    is_charging_now = true;  // 充电
} else if (g_battery_data.pack_current < -0.05f) {
    is_discharging_now = true;  // 放电
} else {
    // 静置状态 (-0.05A ≤ 电流 ≤ 0.05A)
}

// 更新全局状态
g_charging_status = is_charging_now;
external_charging_status = is_charging_now;

// 状态变化日志
if (is_charging_now != last_charging_state) {
    if (is_charging_now) {
        ESP_LOGI(TAG, "🔋 Charging started: %.3fA", current);
    } else if (is_discharging_now) {
        ESP_LOGI(TAG, "⚡ Discharging started: %.3fA", current);
    } else {
        ESP_LOGI(TAG, "🔌 Battery idle: %.3fA", current);
    }
}
```

## 配置要求

### 硬件连接
```
ESP32S3          RS485模块        BMS
GPIO16(TX)  -->  DI       
GPIO17(RX)  -->  RO              
                 A        -->     A
                 B        -->     B
```

### 软件配置
```c
// 启用RS485功能
#define ENABLE_RS485_BATTERY_QUERY  1

// 查询周期 (建议3秒)
#define BATTERY_QUERY_PERIOD 3000

// 电流判断阈值 (可调整)
#define CHARGING_CURRENT_THRESHOLD 0.05f
```

## 调试命令

### JSON触发查询
```json
{"trigger_battery_query": true}
```

### 传统命令
```
BATTERY:QUERY
```

### 检查当前状态
```json
{"query": "battery_detail"}
```

## 预期效果

当RS485通信正常时：
1. **自动检测**: 每3秒自动查询，根据电流自动判断充放电状态
2. **即时更新**: 状态变化时立即更新显示和颜色
3. **准确显示**: 99%电量和26.63V电压准确显示
4. **状态指示**: 充电/放电/静置状态清晰区分

---

*更新时间: 2025年10月14日*  
*版本: V1.0 - 基于485通用协议V19*