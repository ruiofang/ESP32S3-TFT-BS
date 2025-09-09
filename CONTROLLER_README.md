# ESP32S3-TFT-BS Python控制器使用说明

本项目提供了两个Python控制脚本，用于控制ESP32S3-TFT-BS设备的WS2812灯带、电池管理和LCD显示。

## 📁 文件说明

### 1. `esp32_controller.py` - 完整交互式控制器
- **功能**: 提供完整的图形化菜单界面
- **适用**: 初学者和需要图形界面的用户
- **特点**: 
  - 友好的中文界面
  - 分步骤引导操作
  - 支持所有功能模块
  - 内置快速测试

### 2. `simple_controller.py` - 简化命令行控制器
- **功能**: 支持命令行参数和快速操作
- **适用**: 高级用户和自动化脚本
- **特点**:
  - 命令行参数支持
  - 配置文件管理
  - 快捷预设操作
  - 编程接口友好

## 🚀 快速开始

### 安装依赖
```bash
pip install pyserial
```

### 基础使用

#### 1. 交互式控制器 (推荐新手)
```bash
python esp32_controller.py
```

#### 2. 简化控制器
```bash
# 交互模式
python simple_controller.py

# 命令行模式
python simple_controller.py --ws-static 0 255 0 0 100  # 通道0红色静态，亮度100
```

## 🎮 完整交互式控制器使用指南

### 主要功能菜单
1. **连接设置** (1-3)
   - 串口连接 (COM端口)
   - 网络连接 (TCP/IP)
   - 断开连接

2. **WS2812灯带控制** (4-8)
   - 单通道控制
   - 广播控制(所有通道)
   - 通道启用/禁用
   - 状态查询
   - 自动循环设置

3. **电池管理** (9-12)
   - 电池状态查询
   - 设置电池电量
   - 设置充电状态
   - 恢复自动模式

4. **LCD显示控制** (13-16)
   - LCD清屏
   - 显示文字
   - 绘制图形
   - 背光控制

5. **其他功能** (17-18)
   - 灯效模式说明
   - 快速测试

### 使用示例

#### 连接设备
```
请选择操作 (0-18): 1
请输入串口号 (如 COM3): COM3
波特率 (留空=115200): 
✅ 串口连接成功: COM3
```

#### WS2812控制
```
请选择操作 (0-18): 4
🎨 单通道WS2812控制
请输入通道号 (0-3): 0
灯效模式:
0=关闭 1=静态 2=彩虹 3=呼吸 4=跑马灯 5=闪烁 6=波浪 7=自动循环
请选择模式 (0-7): 1
颜色设置选项:
1. 预设颜色
2. 自定义RGB
请选择颜色输入方式 (1-2): 1
预设颜色:
1=红色 2=绿色 3=蓝色 4=黄色 5=洋红 6=青色 7=白色
请选择颜色 (1-7): 1
请输入亮度 (0-255, 建议50-150): 100
```

## ⚡ 简化控制器使用指南

### 配置文件 (config.ini)
程序会自动创建配置文件，包含默认设置：
```ini
[CONNECTION]
type = serial
port = COM3
baudrate = 115200

[WS2812]
default_brightness = 100
default_speed = 200

[LCD]
default_font_size = 16
default_color = white
```

### 命令行参数

#### WS2812控制
```bash
# 静态颜色: 通道 R G B [亮度]
python simple_controller.py --ws-static 0 255 0 0 100

# 彩虹效果: 通道 [亮度] [速度]
python simple_controller.py --ws-rainbow 0 150 200

# 呼吸灯: 通道 R G B [亮度] [速度]  
python simple_controller.py --ws-breathing 0 255 255 255 120 300

# 跑马灯: 通道 R G B [亮度] [速度]
python simple_controller.py --ws-running 0 255 255 0 150 100

# 关闭灯带
python simple_controller.py --ws-off 0
```

#### 电池控制
```bash
# 查询电池状态
python simple_controller.py --bat-query

# 设置电池状态: 电量 充电状态
python simple_controller.py --bat-set 75 false

# 恢复自动模式
python simple_controller.py --bat-auto
```

#### LCD控制
```bash
# 清屏
python simple_controller.py --lcd-clear black

# 显示文字: 文字 X Y [颜色] [大小]
python simple_controller.py --lcd-text "Hello" 10 20 green 16

# 绘制矩形: X Y 宽 高 [颜色] [填充]
python simple_controller.py --lcd-rect 10 10 100 50 red true

# 背光控制
python simple_controller.py --lcd-backlight on
```

### 交互式快捷菜单
```bash
python simple_controller.py
```
提供9个快捷操作：
1. 红色静态 - 所有通道红色
2. 绿色静态 - 所有通道绿色  
3. 蓝色静态 - 所有通道蓝色
4. 彩虹效果 - 所有通道彩虹
5. 呼吸灯 - 所有通道白色呼吸
6. 跑马灯 - 所有通道黄色跑马灯
7. 关闭灯带 - 所有通道关闭
8. 电池查询 - 查询当前电池状态
9. LCD清屏 - 清空LCD屏幕

## 📋 常用操作示例

### 场景1: 氛围灯设置
```bash
# 方法1: 完整交互式
python esp32_controller.py
# 选择5(广播控制) -> 模式3(呼吸) -> 蓝色 -> 亮度80 -> 速度400

# 方法2: 命令行快速
python simple_controller.py --ws-breathing 255 0 0 255 80 400
```

### 场景2: 状态显示
```bash
# 设置电池为充电中85%
python simple_controller.py --bat-set 85 true

# LCD显示系统信息
python simple_controller.py --lcd-clear black
python simple_controller.py --lcd-text "系统状态" 10 10 cyan 16
python simple_controller.py --lcd-text "电池: 85%" 10 30 green 16
```

### 场景3: 测试序列
```bash
# 使用完整控制器的快速测试
python esp32_controller.py
# 选择18(快速测试)

# 或手动命令序列
python simple_controller.py --ws-static 255 255 0 0 100
python simple_controller.py --ws-rainbow 255 150 200  
python simple_controller.py --ws-off 255
```

## ⚙️ 编程接口

### 在Python程序中使用
```python
from simple_controller import SimpleController

# 创建控制器实例
controller = SimpleController('my_config.ini')

# 连接设备
controller.connect()

# 控制WS2812
controller.ws2812_static(0, 255, 0, 0, 100)  # 通道0红色
controller.ws2812_rainbow(255, 150, 200)     # 所有通道彩虹

# 控制电池显示
controller.battery_set(75, True)             # 75%充电中

# 控制LCD
controller.lcd_clear('black')                # 清屏
controller.lcd_text('Hello', 10, 20, 'white', 16)  # 显示文字

# 断开连接
controller.disconnect()
```

## 🔧 故障排除

### 常见问题
1. **串口连接失败**
   - 检查COM端口号是否正确
   - 确认设备已连接并识别
   - 尝试不同的波特率

2. **网络连接失败**
   - 检查IP地址和端口
   - 确认设备在同一网络
   - 检查防火墙设置

3. **命令无响应**
   - 确认设备已正确连接
   - 检查JSON格式是否正确
   - 尝试重启设备和程序

### 调试技巧
- 使用完整控制器的状态查询功能
- 检查设备串口输出
- 验证JSON命令格式

## 📝 参数范围

### WS2812参数
- **通道**: 0-3 (单通道) 或 255 (广播)
- **颜色**: R/G/B 0-255
- **亮度**: 0-255 (建议50-150)
- **速度**: 1-10000ms (建议50-500)

### LCD参数
- **坐标**: X 0-427, Y 0-141
- **字体大小**: 12, 16, 24
- **颜色**: 预定义颜色名或RGB对象

### 电池参数
- **电量**: 0-100%
- **充电状态**: true/false

## 🎯 最佳实践

1. **新用户**: 从完整交互式控制器开始
2. **自动化**: 使用简化控制器的命令行模式
3. **调试**: 利用状态查询和快速测试功能
4. **性能**: 使用较低亮度值节省功耗
5. **安全**: 定期检查连接状态

---

*这两个控制器提供了从简单到复杂的完整控制方案，满足不同用户的需求。*
