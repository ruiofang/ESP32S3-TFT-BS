# ESP32S3 电池监控系统 - 统一串口版本

## 概述

本版本将RS485电池查询和JSON被动控制功能整合到UART1上，通过宏定义选择工作模式，避免了使用多个串口的复杂性。

## 工作模式

### 🔧 模式1: 仅RS485电池查询模式
```c
#define ENABLE_RS485_BATTERY_QUERY  1
#define ENABLE_JSON_PASSIVE_MODE    0
```
- **波特率**: 9600
- **功能**: 自动查询BMS电池信息并显示
- **适用**: 专门的电池监控应用

### 📱 模式2: 仅JSON被动控制模式
```c
#define ENABLE_RS485_BATTERY_QUERY  0
#define ENABLE_JSON_PASSIVE_MODE    1
```
- **波特率**: 115200
- **功能**: 通过串口JSON命令控制显示
- **适用**: 上位机控制显示应用

### 🔄 模式3: 混合模式 (推荐)
```c
#define ENABLE_RS485_BATTERY_QUERY  1
#define ENABLE_JSON_PASSIVE_MODE    1
```
- **波特率**: 9600 (兼容RS485)
- **功能**: 同时支持RS485电池查询和JSON控制
- **适用**: 需要电池监控和外部控制的应用

### ⚡ 模式4: 基本模式
```c
#define ENABLE_RS485_BATTERY_QUERY  0
#define ENABLE_JSON_PASSIVE_MODE    0
```
- **波特率**: 115200
- **功能**: 基本显示，无外部控制
- **适用**: 最简单的应用场景

## 快速配置

### 方法1: 配置切换工具 (推荐)
```bash
# Windows
cd tools
config_switch.bat

# Linux/Mac
cd tools
python3 config_switcher.py
```

### 方法2: 手动修改代码
在 `main/main.c` 文件开头修改宏定义：
```c
// 功能模式选择 (通过宏定义控制)
#define ENABLE_RS485_BATTERY_QUERY  1  // 1=启用RS485电池查询, 0=禁用
#define ENABLE_JSON_PASSIVE_MODE    1  // 1=启用JSON被动控制, 0=禁用
```

## 硬件连接

### 混合模式/RS485模式连接
```
ESP32S3          RS485模块        BMS
GPIO16(TX)  -->  DI       
GPIO17(RX)  -->  RO              
                 A        -->     A
                 B        -->     B
```

### JSON模式连接
```
ESP32S3          串口工具
GPIO16(TX)  -->  RX
GPIO17(RX)  -->  TX
```

## API接口

### RS485电池查询 (模式1, 3)
- 自动查询BMS基本信息 (3秒周期)
- 支持485通用协议 V19
- 自动充电检测 (电流>0.1A)

### JSON命令 (模式2, 3)

#### 基本查询
```json
{"query": "battery"}
```

#### 详细信息查询 (仅混合模式)
```json
{"query": "battery_detail"}
```

#### 手动触发查询 (仅混合模式)
```json
{"trigger_battery_query": true}
```

#### 控制自动查询 (仅混合模式)
```json
{"battery_auto_query": true}   // 启用
{"battery_auto_query": false}  // 禁用
```

#### 设置电池参数
```json
{"battery": 75}                // 设置电量75%
{"simulate_charging": true}    // 模拟充电
{"voltage": 25.2}             // 设置电压
```

## 屏幕显示

### 电量显示
- **正常**: `75% 25.2V`
- **充电**: `75% [charging]`

### 状态信息
- **RS485连接**: `RS485: 2.5A 15串 23.5℃ 37500mAh`
- **JSON控制**: `手动模式 | 电量:75% | JSON控制`
- **混合模式**: `RS485: 2.5A 充电中 15串 23.5℃`
- **连接断开**: `自动模式 | RS485通信断开 | JSON控制可用`

## 配置文件

参考 `main/battery_config.h` 文件进行详细配置：

```c
// 硬件引脚配置
#define UART1_TXD_PIN       16
#define UART1_RXD_PIN       17

// RS485参数
#define BATTERY_QUERY_PERIOD 3000   // 查询周期 (ms)
#define BATTERY_TIMEOUT_MS   500    // 响应超时 (ms)

// 显示参数
#define BATTERY_FULL_THRESHOLD  98  // 充满电阈值 (%)
#define BATTERY_LOW_THRESHOLD   20  // 低电量阈值 (%)
```

## 编译上传

1. **选择模式**: 使用配置工具或手动修改宏定义
2. **编译**: 在ESP-IDF环境中编译项目
3. **上传**: 烧录到ESP32S3开发板
4. **测试**: 根据选择的模式进行相应测试

## 测试工具

### RS485通信测试
```bash
cd test
python battery_rs485_test.py COM3
```

### JSON控制测试
```bash
cd test  
python simple_controller.py COM3
```

### 配置验证
```bash
cd tools
python config_switcher.py show
```

## 故障排除

### 1. 通信异常
- 检查宏定义配置是否正确
- 确认波特率设置 (RS485:9600, JSON:115200)
- 验证硬件连接

### 2. 模式切换无效
- 确保重新编译并上传
- 检查配置文件备份是否正确
- 使用配置工具验证当前配置

### 3. 显示异常
- 检查屏幕上的状态信息
- 确认当前工作模式
- 查看串口日志输出

## 版本特性

✅ **统一串口**: 使用UART1处理所有通信  
✅ **模式切换**: 通过宏定义灵活选择功能  
✅ **自动配置**: 根据模式自动选择波特率  
✅ **向下兼容**: 保持原有接口不变  
✅ **配置工具**: 提供便捷的模式切换工具  
✅ **详细文档**: 完整的使用说明和示例  

## 技术支持

- **配置问题**: 使用 `tools/config_switcher.py` 进行诊断
- **通信问题**: 参考 `doc/RS485_BATTERY_README.md`
- **代码参考**: 直接查看 `main/main.c` 中的RS485实现

---

*最后更新: 2025年10月13日*