/*
 * ESP32S3 电池监控系统配置模板
 * 
 * 修改这些宏定义来选择不同的工作模式
 */

#ifndef BATTERY_CONFIG_H
#define BATTERY_CONFIG_H

// ===========================================
// 工作模式选择 (修改这些值来切换模式)
// ===========================================

// 模式1: 仅RS485电池查询模式
// #define ENABLE_RS485_BATTERY_QUERY  1
// #define ENABLE_JSON_PASSIVE_MODE    0

// 模式2: 仅JSON被动控制模式  
// #define ENABLE_RS485_BATTERY_QUERY  0
// #define ENABLE_JSON_PASSIVE_MODE    1

// 模式3: 混合模式 (推荐)
#define ENABLE_RS485_BATTERY_QUERY  1
#define ENABLE_JSON_PASSIVE_MODE    1

// 模式4: 基本模式 (无外部控制)
// #define ENABLE_RS485_BATTERY_QUERY  0
// #define ENABLE_JSON_PASSIVE_MODE    0

// ===========================================
// RS485硬件配置 (仅在启用RS485时有效)
// ===========================================

#if ENABLE_RS485_BATTERY_QUERY
    // 通信参数
    #define BATTERY_QUERY_PERIOD 3000   // 查询周期 (ms)
    #define BATTERY_TIMEOUT_MS   500    // 响应超时 (ms)
    
    // 充电检测阈值
    #define CHARGING_CURRENT_THRESHOLD 0.1f  // 充电电流阈值 (A)
#endif

// ===========================================
// UART配置 (自动选择波特率)
// ===========================================

// UART引脚配置
#define UART1_TXD_PIN       16
#define UART1_RXD_PIN       17

// 波特率自动选择
#if ENABLE_RS485_BATTERY_QUERY
    #define UART1_BAUD_RATE     9600   // RS485电池协议标准波特率
#else
    #define UART1_BAUD_RATE     115200 // JSON控制标准波特率
#endif

// ===========================================
// 功能特性开关
// ===========================================

// 充电动画
#define ENABLE_CHARGING_ANIMATION   1  // 1=启用充电动画, 0=禁用

// 调试输出
#define ENABLE_BATTERY_DEBUG_LOG    1  // 1=启用调试日志, 0=禁用

// 数据缓存
#define ENABLE_JSON_CACHE          1  // 1=启用JSON缓存, 0=禁用

// ===========================================
// 显示配置
// ===========================================

// 电池颜色定义
#define BATTERY_COLOR_FULL      0x00FF00  // 充满电绿色
#define BATTERY_COLOR_CHARGING  0x00AAFF  // 充电蓝绿色
#define BATTERY_COLOR_LOW       0xFF0000  // 低电量红色
#define BATTERY_COLOR_NORMAL    0xFFFFFF  // 正常白色

// 阈值设置
#define BATTERY_FULL_THRESHOLD  98   // 充满电阈值 (%)
#define BATTERY_LOW_THRESHOLD   20   // 低电量阈值 (%)

// ===========================================
// 使用说明
// ===========================================

/*
使用方法:

1. 选择工作模式:
   - 修改上面的 ENABLE_RS485_BATTERY_QUERY 和 ENABLE_JSON_PASSIVE_MODE
   
2. 配置硬件引脚:
   - 修改 UART1_TXD_PIN, UART1_RXD_PIN (默认: GPIO16/17)
   
3. 调整通信参数:
   - 波特率会根据模式自动选择
   - 可修改 BATTERY_QUERY_PERIOD 调整查询频率
   
4. 编译上传:
   - 修改完成后重新编译上传到ESP32S3

模式说明:

- 仅RS485模式: 专门用于电池监控，自动查询电池信息并显示
- 仅JSON模式: 通过串口JSON命令控制显示，无电池查询
- 混合模式: 同时支持RS485电池查询和JSON控制 (推荐)
- 基本模式: 最简单的显示，无外部控制功能

硬件连接:

RS485模式下的连接:
- ESP32 TX (GPIO16) -> RS485模块 DI
- ESP32 RX (GPIO17) -> RS485模块 RO  
- ESP32 RTS (GPIO42) -> RS485模块 DE/RE (可选)
- RS485模块 A/B -> BMS A/B接口

JSON模式下的连接:
- ESP32 TX (GPIO16) -> 串口工具 RX
- ESP32 RX (GPIO17) -> 串口工具 TX
- 通过串口发送JSON命令控制
*/

#endif // BATTERY_CONFIG_H