# ESP32S3-TFT-BS 通讯协议文档

## 📋 概述
ESP32S3-TFT-BS系统支持多种功能控制：
- **WS2812灯带控制**：4通道灯效控制（GPIO 18、19、20、21）
- **电池管理**：电池状态显示和充电管理
- **LCD显示控制**：TFT屏幕显示内容管理

所有控制命令均基于JSON格式，支持串口和网络通讯。

## 🔌 硬件配置
- **WS2812通道配置**:
  - 通道0: GPIO 18
  - 通道1: GPIO 19  
  - 通道2: GPIO 20
  - 通道3: GPIO 21
  - 每通道支持160个LED（可配置）
  
- **LCD屏幕参数**:
  - 分辨率：428×142像素（横屏模式）
  - 坐标系：原点(0,0)位于左上角
  - 颜色深度：RGB565

## 📡 通讯方式

### JSON命令格式（主要格式）
所有控制命令使用JSON格式，支持以下发送方式：

1. **直接发送完整JSON**（推荐）: 
   ```json
   {"channel": 0, "mode": 1}
   ```
   
2. **前缀格式**（需要回车）: 
   ```
   JSON:{"channel": 0, "mode": 1}
   ```

### 智能JSON检测机制
- 系统自动检测以 `{` 开头的JSON命令
- 当检测到完整的JSON对象（大括号匹配）时立即处理
- 支持嵌套JSON结构（如color对象）
- **无需手动添加回车符或换行符**

---

## 🎨 WS2812 灯带控制协议

### 基础控制命令

#### 1. 单通道控制
```json
{
  "channel": 0,
  "mode": 1,
  "color": {"r": 255, "g": 0, "b": 0},
  "brightness": 128,
  "speed": 100
}
```

#### 2. 广播控制（所有通道）
```json
{
  "channel": 255,
  "mode": 2,
  "brightness": 200
}
```

#### 3. 通道启用/禁用
```json
{
  "channel": 1,
  "enabled": false
}
```

#### 4. 状态查询
```json
{
  "action": "status"
}
```

#### 5. 自动循环持续时间设置
```json
{
  "channel": 0,
  "cycle_duration": 5000
}
```

### 灯效模式详解

| 模式值 | 模式名称 | 说明 | 速度控制 | 颜色控制 | 参数说明 |
|--------|----------|------|----------|----------|----------|
| 0 | OFF | 关闭 | - | - | 完全关闭LED |
| 1 | STATIC | 静态颜色 | - | ✅ | 固定颜色显示 |
| 2 | RAINBOW | 彩虹效果 | ✅ | - | 自动彩虹渐变 |
| 3 | BREATHING | 呼吸灯效果 | ✅ | ✅ | 正弦波亮度变化 |
| 4 | RUNNING | 跑马灯效果 | ✅ | ✅ | 带5像素拖尾的移动光点 |
| 5 | FLASH | 闪烁效果 | ✅ | ✅ | 全亮→全灭循环 |
| 6 | WAVE | 波浪效果 | ✅ | ✅ | 波浪形光效传播 |
| 7 | AUTO_CYCLE | 自动循环模式 | ✅ | 自动变化 | 自动切换各种效果 |

### WS2812参数范围
- **channel**: 0-3（单通道）或 255（广播）
- **mode**: 0-7
- **color.r/g/b**: 0-255
- **brightness**: 0-255
- **speed**: 1-10000 毫秒
- **cycle_duration**: 1000-60000 毫秒

### WS2812使用示例

#### 基础颜色控制
```json
{"channel": 0, "mode": 1, "color": {"r": 255, "g": 0, "b": 0}, "brightness": 100}  // 红色静态
{"channel": 255, "mode": 2, "brightness": 150}  // 所有通道彩虹模式
```

#### 动画效果控制
```json
{"channel": 1, "mode": 3, "color": {"r": 255, "g": 0, "b": 0}, "brightness": 150, "speed": 100}  // 红色呼吸灯（快速）
{"channel": 0, "mode": 4, "color": {"r": 0, "g": 255, "b": 0}, "brightness": 200, "speed": 80}   // 绿色跑马灯（快速）
{"channel": 255, "mode": 5, "color": {"r": 255, "g": 255, "b": 255}, "brightness": 200, "speed": 200}  // 白色闪烁
```

#### 自动循环模式
```json
{"channel": 255, "mode": 7, "cycle_duration": 3000}  // 每个效果持续3秒
```

---

## 🔋 电池管理协议

### JSON格式电池控制

#### 基础电池状态查询
```json
{"query": "battery"}  // 查询当前电池状态
```

#### 电池电量设置（立即刷新）
```json
{"battery": 75}  // 设置显示电量75%（白色简约模式）
```

#### 充电状态控制
```json
{"battery": 20, "charging": true}   // 20%充电中，橙色动画+⚡图标
{"battery": 60, "charging": true}   // 60%充电中，黄绿动画  
{"battery": 95, "charging": true}   // 95%充电中，绿色动画
{"battery": 100, "charging": true}  // 100%充满，彩虹特效
```

#### 非充电状态（简约显示）
```json
{"battery": 30}                     // 30%白色简约（默认非充电）
{"battery": 75, "charging": false}  // 75%白色简约（明确非充电）
{"battery": 100, "charging": false} // 100%绿色简约（满电非充电）
```

#### 恢复自动模式
```json
{"auto_mode": true}                                 // 恢复电池自动检测
{"auto_mode": true, "auto_charging": true}         // 恢复完全自动模式
```

### 电池显示效果说明

| 电量范围 | 充电状态 | 显示效果 | 动画类型 |
|----------|----------|----------|----------|
| 0-20% | 充电中 | 红色动画+⚡ | 呼吸灯效果 |
| 21-50% | 充电中 | 橙色动画+⚡ | 呼吸灯效果 |
| 51-80% | 充电中 | 黄绿动画+⚡ | 呼吸灯效果 |
| 81-94% | 充电中 | 绿色动画+⚡ | 呼吸灯效果 |
| ≥95% | 充电中 | 彩虹动画+FULL | 特殊彩虹效果 |
| 任意电量 | 非充电 | 白色简约条 | 静态进度条 |
| 100% | 非充电 | 绿色满电条 | 静态进度条 |

### 使用场景示例

| 场景描述 | JSON指令 | 显示效果 |
|----------|----------|----------|
| 📱 手机充电20% | `{"battery": 20, "charging": true}` | 橙色动画+⚡ |
| 🔋 手机待机75% | `{"battery": 75}` | 白色简约条 |
| 🟢 充电完成100% | `{"battery": 100, "charging": false}` | 绿色满电条 |
| ⚠️ 低电量5%充电 | `{"battery": 5, "charging": true}` | 红色动画+⚡ |
| 🌈 充满电特效 | `{"battery": 100, "charging": true}` | 彩虹动画+FULL |

---

## 📺 LCD显示控制协议

### JSON格式LCD控制

#### 基础显示操作
```json
{"lcd": "clear"}                                    // 清屏为黑色
{"lcd": "clear", "color": "red"}                   // 用指定颜色清屏
{"lcd": "fill", "color": "blue"}                   // 全屏填充指定颜色
```

#### 文字显示控制
```json
{"lcd": "text", "content": "Hello"}               // 在默认位置显示文字
{"lcd": "text", "x": 10, "y": 20, "content": "Hi"} // 在指定位置显示文字
{"lcd": "text", "x": 50, "y": 30, "content": "测试", "color": "green", "size": 16} // 显示中文，指定颜色和大小
```

#### 图形绘制控制
```json
// 矩形绘制
{"lcd": "rect", "x": 10, "y": 10, "width": 100, "height": 50}  // 绘制空心矩形
{"lcd": "rect", "x": 10, "y": 10, "width": 100, "height": 50, "fill": true, "color": "yellow"}  // 绘制填充矩形

// 圆形绘制
{"lcd": "circle", "x": 100, "y": 70, "radius": 30}  // 绘制空心圆
{"lcd": "circle", "x": 100, "y": 70, "radius": 30, "fill": true, "color": "purple"}  // 绘制填充圆

// 线条绘制
{"lcd": "line", "x1": 0, "y1": 0, "x2": 100, "y2": 100, "color": "white"}  // 绘制线条
```

#### 背光控制
```json
{"lcd": "backlight", "state": true}                // 开启背光
{"lcd": "backlight", "state": false}               // 关闭背光
```

### LCD参数说明
- **坐标系统**: 
  - 横屏模式：宽度428像素，高度142像素
  - 原点(0,0)位于左上角
  - 坐标范围：x: 0-427, y: 0-141
  
- **颜色支持**: 
  - 预定义颜色：`"red"`, `"green"`, `"blue"`, `"yellow"`, `"purple"`, `"cyan"`, `"white"`, `"black"`
  - RGB对象格式：`{"r": 255, "g": 0, "b": 0}`
  - 十六进制值：`0xF800`（RGB565格式）
  
- **字体参数**: 
  - 支持字体大小：12x12、16x16、24x24像素
  - 中文字符支持
  
- **几何图形参数**:
  - 矩形：width/height: 1-428/142
  - 圆形：radius: 1-214
  - 线条：坐标范围内任意两点

### LCD综合显示示例

#### 系统状态界面
```json
{"lcd": "clear", "color": "black"}
{"lcd": "text", "x": 10, "y": 10, "content": "系统状态", "color": "cyan", "size": 16}
{"lcd": "line", "x1": 10, "y1": 30, "x2": 400, "y2": 30, "color": "white"}
{"lcd": "text", "x": 10, "y": 40, "content": "电池: 85%", "color": "green", "size": 16}
{"lcd": "text", "x": 10, "y": 60, "content": "温度: 25°C", "color": "yellow", "size": 16}
{"lcd": "rect", "x": 10, "y": 85, "width": 200, "height": 20, "fill": false, "color": "white"}
{"lcd": "rect", "x": 12, "y": 87, "width": 170, "height": 16, "fill": true, "color": "green"}
```

#### 电池状态显示界面
```json
{"lcd": "clear"}
{"lcd": "text", "x": 50, "y": 20, "content": "电池状态", "color": "green", "size": 16}
{"lcd": "text", "x": 50, "y": 50, "content": "75%", "color": "white", "size": 24}
{"lcd": "rect", "x": 10, "y": 80, "width": 150, "height": 20, "fill": false, "color": "white"}
{"lcd": "rect", "x": 12, "y": 82, "width": 112, "height": 16, "fill": true, "color": "green"}
```

---

## 🔄 兼容性命令（传统格式）

### 基础命令（需要回车）
```
BATTERY                    // 查询电池状态
BATTERY:JSON              // 查询电池状态（JSON格式）
BATTERY:75               // 设置显示电量75%
BATTERY:AUTO             // 恢复自动模式
WS2812:TEST             // 测试所有通道
HELP                    // 显示帮助信息
```

### 传统LCD命令（需要回车）
```
LCD:CLEAR                  // 清屏
LCD:CLEAR:RED             // 用红色清屏
LCD:TEXT:Hello            // 显示文字
LCD:TEXT:50:20:Hi         // 在(50,20)位置显示文字
LCD:BACKLIGHT:ON          // 开启背光
LCD:BACKLIGHT:OFF         // 关闭背光
```

---

## 📋 系统响应格式

### 成功响应
| 功能模块 | 响应格式 | 示例 |
|----------|----------|------|
| WS2812控制 | 文本格式 | `OK` / `ERROR` |
| LCD控制 | JSON格式 | `{"status":"success","message":"LCD command executed"}` |
| 电池管理 | JSON格式 | `{"battery": 75, "charging": false, "voltage": 3.85}` |
| 状态查询 | JSON格式 | `{"channels": [{"id": 0, "mode": 1, "enabled": true}]}` |

### 错误响应
```json
{"status":"error","message":"Invalid channel number"}
{"status":"error","message":"LCD command failed"}
{"status":"error","message":"Invalid JSON format"}
```

### 测试完成响应
```
TEST_COMPLETED  // WS2812测试完成
TEST_FAILED     // WS2812测试失败
HELP_DISPLAYED  // 帮助信息已显示
```

---

## ⚠️ 注意事项

### 命令格式要求
1. **JSON命令为主要格式**：推荐使用JSON格式进行所有控制
2. **无需回车符**：JSON命令自动检测，无需手动添加换行符
3. **大括号匹配**：确保JSON格式正确，系统会验证大括号匹配

### 功能限制说明
1. **广播ID（255）**：可同时控制所有WS2812通道
2. **通道自动禁用**：初始化失败的通道会自动禁用
3. **被动电池监控**：不会自动推送电池信息，只在查询时返回
4. **立即刷新机制**：设置电量或LCD内容后立即刷新显示

### 性能优化建议
1. **亮度控制**：建议使用较低的亮度值以节省功耗
2. **速度参数**：根据效果需求调整speed参数，避免过快闪烁
3. **批量操作**：可连续发送多个JSON命令实现复杂控制
4. **状态查询**：定期查询状态以确保系统正常运行

---

## 📝 版本信息
- **文档版本**: V1.0
- **协议版本**: ESP32S3-TFT-BS V1.1
- **更新日期**: 2025年9月9日
- **兼容性**: 支持串口通讯和网络通讯

---

*本文档整合了WS2812灯带控制、电池管理和LCD显示控制的完整通讯协议，为ESP32S3-TFT-BS系统提供统一的控制接口规范。*
