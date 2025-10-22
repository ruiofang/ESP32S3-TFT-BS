# ESP32S3 RS485电池通信功能说明

## 概述

本项目新增了RS485电池通信主站功能，可以自动查询符合485通用协议的电池管理系统(BMS)，实时获取电池电量、电压、电流、温度等信息，并显示在LCD屏幕上。

## 主要功能

### 1. RS485硬件配置
- **串口**: UART2 (RS485_UART_NUM)
- **TX引脚**: GPIO 43 (可修改)
- **RX引脚**: GPIO 44 (可修改) 
- **RTS引脚**: GPIO 42 (RS485收发控制)
- **波特率**: 9600 (电池协议标准)

### 2. 自动电池查询
- **查询周期**: 3秒 (可配置)
- **响应超时**: 500ms
- **支持协议**: 485通用电池协议
- **查询命令**: 0x03 (基本信息)

### 3. 电池数据显示
- **电压显示**: 实时电池组电压
- **电量百分比**: 基于BMS SOC
- **充电状态**: 根据电流自动判断 (>0.1A为充电)
- **详细信息**: 电流、串数、温度、容量等

### 4. 通信协议支持

#### 查询帧格式
```
起始位 | 读取位 | 命令码 | 长度 | 校验高 | 校验低 | 结束位
 0xDD  |  0xA5  |  0x03  | 0x00 |   高   |   低   | 0x77
```

#### 响应帧解析
- **总电压**: 2字节，单位10mV
- **电流**: 2字节，单位10mA，带符号
- **剩余容量**: 2字节，单位10mAh
- **SOC**: 1字节，百分比
- **电池串数**: 1字节
- **温度**: 多个2字节，单位0.1K

## API接口

### 1. JSON命令 (通过UART1)

#### 查询电池基本信息
```json
{"query": "battery"}
```
响应：
```json
{
  "battery": 75,
  "voltage": 25.2,
  "charging": true,
  "mode": "auto"
}
```

#### 查询电池详细信息
```json
{"query": "battery_detail"}
```
响应：
```json
{
  "voltage": 25.2,
  "percentage": 75,
  "charging": true,
  "rs485_connected": true,
  "auto_query": true,
  "rs485_data": {
    "pack_voltage": 25.2,
    "pack_current": 2.5,
    "soc": 75,
    "remain_capacity_mah": 37500,
    "full_capacity_mah": 50000,
    "battery_strings": 15,
    "protect_status": 0,
    "fet_status": 3,
    "temperatures": [23.5, 24.1]
  }
}
```

#### 手动触发电池查询
```json
{"trigger_battery_query": true}
```

#### 控制自动查询
```json
{"battery_auto_query": true}   // 启用
{"battery_auto_query": false}  // 禁用
```

### 2. 传统命令

#### 快速电池查询
```
BATTERY
```
响应: `BATTERY:25.2V,75%,CHARGING`

#### JSON格式查询
```
BATTERY:JSON
```

## 屏幕显示

### 1. 电量条显示
- **颜色指示**:
  - 绿色: 电量≥98%
  - 蓝绿色: 充电中
  - 红色: 电量<20%
  - 白色: 正常电量

### 2. 信息显示
- **正常模式**: `75% 25.2V`
- **充电模式**: `75% [charging]`

### 3. 状态信息
- **RS485连接**: `RS485: 2.5A 15串 23.5℃ 37500mAh`
- **连接断开**: `自动模式 | RS485通信断开 | JSON控制可用`

## 充电检测逻辑

### 自动充电判断
```c
bool is_charging = (pack_current > 0.1f);  // 电流>0.1A为充电
```

### 充电动画
- 充电时自动启用进度条动画
- 动画范围可配置 (默认15%)
- 充电停止时自动停止动画

## 错误处理

### 1. 通信超时
- 500ms无响应视为超时
- 连续失败10秒后标记数据无效
- 自动降级到备用模式

### 2. 数据校验
- 帧头尾验证
- 校验和验证
- 数据长度检查

### 3. 异常恢复
- 自动重试机制
- 数据有效性检查
- 备用电压源切换

## 测试工具

### Python测试脚本
运行 `test/battery_rs485_test.py` 进行功能测试：
```bash
python test/battery_rs485_test.py COM3
```

### Windows批处理
双击 `test/rs485_test.bat` 自动运行测试

### 测试内容
- 基本电池查询
- RS485详细信息
- 手动查询触发
- 自动查询控制
- 充电状态模拟
- 电量控制测试

## 配置说明

### 1. 硬件引脚配置
修改 `main.c` 中的宏定义：
```c
#define RS485_TXD_PIN       43  // TX引脚
#define RS485_RXD_PIN       44  // RX引脚  
#define RS485_RTS_PIN       42  // 收发控制引脚
```

### 2. 通信参数配置
```c
#define RS485_BAUD_RATE     9600  // 波特率
#define BATTERY_QUERY_PERIOD 3000 // 查询周期(ms)
#define BATTERY_TIMEOUT_MS   500  // 超时时间(ms)
```

### 3. 功能开关
```c
static bool g_battery_auto_query = true;  // 自动查询使能
```

## 日志输出

### 启动信息
```
I (2345) BATTERY_MONITOR: RS485 initialized - TX:43, RX:44, RTS:42, Baud:9600
I (2346) BATTERY_MONITOR: Battery Monitor System Ready! (RS485 Auto Query Mode)
I (2347) BATTERY_MONITOR: Battery Auto Query: Enabled (Period: 3000ms, Timeout: 500ms)
```

### 查询日志
```
I (5678) BATTERY_MONITOR: Battery: 25.20V, 2.50A, 75%, 37500mAh/50000mAh, 15串
I (5679) BATTERY_MONITOR: Started charging animation (current: 2.50A)
```

### 错误日志
```
W (8901) BATTERY_MONITOR: No battery response received
W (8902) BATTERY_MONITOR: Battery data expired, marked as invalid
```

## 兼容性

### 支持的BMS协议
- 485通用协议 V19
- 嘉佰达软件板协议
- 支持9600/115200波特率

### ESP32兼容性
- ESP32S3 (推荐)
- ESP32 (需修改引脚定义)
- 支持UART2 RS485模式

## 故障排除

### 1. 无法连接BMS
- 检查RS485接线
- 确认波特率设置
- 检查RTS引脚配置

### 2. 数据解析错误
- 检查协议版本兼容性
- 确认BMS响应格式
- 查看详细日志输出

### 3. 显示异常
- 检查UI更新任务状态
- 确认数据有效性标志
- 重启系统清除状态

## 开发说明

### 添加新协议支持
1. 在 `parse_battery_info_response()` 中添加解析逻辑
2. 修改 `send_battery_query()` 构建查询帧
3. 更新 `battery_data_t` 结构体定义

### 扩展显示功能
1. 修改 `update_battery_ui()` 显示逻辑
2. 在 `get_battery_detailed_info()` 中添加JSON字段
3. 更新屏幕布局和颜色配置

---

*最后更新: 2025年10月13日*