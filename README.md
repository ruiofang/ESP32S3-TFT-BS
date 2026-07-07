# ESP32S3-TFT-BS

基于ESP32-S3的TFT显示屏、WS2812 RGB灯带控制系统和电池管理系统

## 更新日志
## test5.1 2026-07-07
- 修复 Web API 回调中的任务看门狗误复位：HTTP 回调任务在未注册到 TWDT 时不再调用 `esp_task_wdt_reset()`，消除运行日志中的 `task not found` 报错；
- 修复无信号覆盖逻辑导致的“设置模式后 RGB 不亮”：仅在无信号覆盖**实际命中电量通道**时才跳过常规渲染，避免非电量模式被错误卡黑；

## test5 2026-06-27
- 新增 **OTA 固件升级**功能：设备可通过 WiFi 远程下载并安装新固件，支持自动检查更新和手动确认升级；
- OTA 升级网页界面：在 `/ota` 页面查看当前版本、自动更新开关、手动检查更新、确认升级；升级过程中显示实时进度条（百分比 + 已下载/总大小）；
- OTA 后端 API：`/api/ota/status` 查询状态与进度，`/api/ota/check_now` 触发检查，`/api/ota/confirm` 确认升级，`/api/ota/upload` 本地上传固件；
- OTA 下载改用多步 API（`esp_https_ota_begin/perform/finish`），支持实时进度反馈（每 5% 打印日志，15 秒无进展告警）；
- 固件版本号从 `CMakeLists.txt` 的 `PROJECT_VER` 自动读取，`tools/make_release.sh` 一键构建发布包；
- 默认 OTA 升级地址：`http://120.27.145.121:8090/releases/latest.json`（清单）/ `http://120.27.145.121:8090/releases/panda.bin`（固件）；
- 支持断点续传：下载中断后恢复，避免重复下载；
- 升级完成自动重启，rollback 定时器确认镜像有效后提交；
- 网页 OTA 页面 BODY_BUF 扩展至 6144 字节，适配完整 JS 脚本；
- 修复 rollback 定时器在下载中途重置状态导致网页轮询中断的问题。
- 新增 **CLAUDE_WIFI 模式** (第 5 种 BOOT 循环模式)：通过 **WiFi** 接收 PC 推送的 Claude 状态，与 BLE 路径共用同一套 LCD 面板 / WS2812 状态色 / 状态机；链路采用 **UDP 发现 + TCP 长连接状态传输**，便于设备可靠判断主机离线；
- 配网流程：首次进入时自动起 AP `ESP32_Claude_XXXX` (密码 `12345678`)，浏览器打开 `http://192.168.4.1/` 选 SSID → 输密码 → 保存 → 设备重启进 STA；STA 连接成功后 LCD 显示 `WiFi: <IP>` 和 `ID:XXXX`；
- WiFi 配网网页分页设计（主页 / WiFi / 高级），并提供 `/wifiscan` AP 扫描接口，列表点击即可填入 SSID；
- **BOOT 长按 (≥5s)**：在 CLAUDE_WIFI 模式下清除已保存 WiFi 凭据并重启回 AP 配网，方便换网；短按仍循环模式（改为释放时触发以便与长按区分）；
- 设备号 = MAC 后 4 位 hex (`ESP32_Claude_XXXX` 的 `XXXX`)，PC 桥通过 UDP 广播 `{"q":"discover","id":"XXXX"}` 完成配对，并自动获取设备 IP / TCP 端口；之后桥与 ESP32 建立持久 **TCP** 连接，断开或心跳超时会被判定为离线；
- 新增 PC 桥 `tools/claude_status_wifi_bridge.py`，无命令行参数，所有配置走同目录 `claude_wifi_bridge.json`（首次运行自动写默认模板）；支持单设备或多设备同时绑定显示；Claude Code Hooks 配置无需改变。

## test3 2026-06-18
- 新增 **CLAUDE 状态模式**：通过 BLE (NimBLE Nordic UART Service) 接收 PC 推送的 Claude Code 运行状态；LCD 用 LVGL 面板显示 状态/工具/模型/token 计数/消息；WS2812 用颜色映射当前状态（idle=绿 / thinking=蓝 / tool=青 / writing=品红 / waiting=黄 / error=红 / done=白 / 未连接=灰）；
- BOOT 按键循环扩展为 4 种模式：`JSON → RS485-1 → RS485-2 → CLAUDE → JSON ...`；模式持久化到 NVS，重启自动恢复；
- 新增 PC 端桥接脚本 `tools/claude_status_ble_bridge.py` 与钩子助手 `tools/claude_hook_post.py`，通过 Claude Code Hooks 自动推送状态；
- BLE 设备名为 `ESP32_Claude_XXXX`（XXXX = MAC 后 2 字节），无 PIN，直接连接。
/usr/bin/python3 tools/claude_status_ble_bridge.py --connect-on-start -v

## test2.5 2026-04-17
- WiFi AP SSID 改为基于芯片 MAC 自动生成：`ESP32_Light_XXXX`（后 2 字节，同一芯片固定，不同芯片唯一）；密码仍为 `12345678`；
- BOOT 按键（GPIO0）循环切换电池读取模式：JSON(115200) → RS485-1(9600, 0xDD 协议) → RS485-2(9600, 0xAA 协议)，LCD 左上角实时显示当前模式；
- 电池读取模式持久化：新增独立 NVS 命名空间 `batt_mode`，仅写入变化值，断电后重启自动恢复上次模式并同步 UART 波特率；
- 新增 RS485-2 协议支持：0xAA 帧头、小端、sum 校验，支持握手 0x00 与电池信息 0x21 响应解析；
- 信号丢失可视化：超过 15 秒未收到电池数据时，LCD 与 WS2812 红色闪烁提示 "NO SIGNAL"，不再显示陈旧值；
- 停止将电池电量写入 Flash，降低 NVS 写入压力；
- UART1 稳定性修复：RX 环形缓冲扩大到 4×，增加错误恢复、JSON 解析超时复位与空闲刷新；非 JSON 模式让出 UART，避免协议冲突；
## test2.4 2025-12-29
- 律动模式1背景颜色设置为白色固定，底部噪音500->100；
## test2.3 2025-12-25_2
- 增加律动模式2,优化调节方式；
## test2.2 2025-12-25
- Web UI 布局重构：RGB滑块与输入框同行显示，亮度和速度独立分行；
- 交互优化：增加滑块与数字输入框的双向同步功能；
- 功能完善：单通道控制支持实时获取并显示当前设备状态；
- 界面精简：移除高级配置中不常用的系统测试按钮；

## test2.1 2025-12-24
- V2.1板子，同步相同模式的亮度和效果；
## test2
- V2.1板子，增加音乐律动模式；
## test1
- 直接读取电池电量；
### V1.2.5
- 优化电量设置和参数保存；
### V1.2.1
- 优化LCD模式3的横屏显示；
- 优化RUN指示灯的闪烁延时；
### V1.2 
- **新增电池管理系统**: 完整的电池电量显示和充电状态管理
- **改进充电动画**: 基于设定电量的进度条动态增长效果
- **Web端电池控制**: 浏览器端完整的电池管理界面
- **HTTP API扩展**: 新增电池状态查询和控制接口
- **智能模式切换**: 支持自动/手动模式灵活切换
- **UI交互优化**: 改进用户界面响应和动画效果

### V1.1
- 添加Web控制面板居中布局
- 增加WiFi热点密码保护
- 优化用户界面体验
- 完善API文档

### V1.0
- 基础TFT显示功能
- WS2812多通道控制
- WiFi热点模式
- Web远程控制

### 硬件支持
- **主控芯片**: ESP32-S3
- **TFT显示屏**: 2.79寸
- **显示驱动**: NV3007
- **分辨率**: 142x428
- **RGB灯带**: WS2812 (4通道控制)
- **麦克风**: I2S PDM 数字麦克风

### 软件功能
- **LVGL图形界面**: 基于LVGL的用户界面
- **WiFi热点模式**: 自动创建WiFi接入点
- **Web控制面板**: 通过浏览器远程控制
- **多通道RGB控制**: 支持4通道WS2812独立控制
- **多种灯光效果**: 静态颜色、彩虹、呼吸、跑马灯、闪烁、波浪等
- **电池管理系统**: 电量显示、充电状态监控、充电动画
- **HTTP API控制**: 完整的RESTful API接口

### 🌟 技术亮点
- **动态充电动画**: 进度条基于设定电量动态增长的视觉效果
- **双模式管理**: 自动检测与手动控制的灵活切换
- **跨平台控制**: 支持串口命令、Web界面、HTTP API多种控制方式
- **实时状态同步**: UI界面与后端状态实时同步更新
- **模块化设计**: 清晰的代码结构，易于扩展和维护

## 快速开始

### 1. 编译和烧录
```bash
# 配置ESP-IDF环境
idf.py set-target esp32s3

# 编译项目
idf.py build

# 烧录固件
idf.py flash

# 查看串口输出
idf.py monitor
```

### 2. WiFi连接
- **热点名称**: `ESP32_Light_XXXX`（`XXXX` 为本芯片 MAC 后 2 字节大写十六进制，烧录后可在串口日志 `WiFi AP initialized. SSID: ...` 中查看；同一芯片固定不变）
- **连接密码**: `12345678`
- **网关地址**: `192.168.4.1`

### 3. Web控制
1. 连接到ESP32的WiFi热点
2. 在浏览器中访问 `http://192.168.4.1`
3. 使用Web界面控制RGB灯带和电池显示
4. 支持实时查询电池状态和充电动画控制

## 🧪 功能测试

### Python测试工具
本项目提供了完整的Python测试工具，方便用户测试所有功能：

#### 🔧 快速开始
```bash
# Windows用户 - 双击运行
run_test.bat

# 或手动运行
python test_esp32_functions.py
```

#### 📋 测试功能
- **WS2812灯带控制**: 静态颜色、动画效果、广播控制
- **电池管理**: 电量显示、充电状态、充电动画、自动模式
- **LCD显示**: 文字显示、图形绘制、界面演示
- **Web控制测试**: 浏览器端功能完整性测试
- **兼容命令**: 传统格式命令支持

#### 📖 详细使用说明
参见 [测试工具使用指南](TEST_USAGE.md)

## 控制功能

### 🎨 RGB灯带控制

#### 广播控制
- 同时控制所有4个通道
- 支持所有灯光模式（含音乐律动）
- 可调节颜色、亮度、速度
- **智能同步**: 广播设置时自动同步所有通道状态；单通道设置时，自动同步其他处于相同模式的通道

#### 单通道控制
- 独立控制每个通道
- 个性化设置每个通道的参数
- 支持启用/禁用单个通道

#### 快速预设
- **全部关闭**: 关闭所有灯光
- **白色**: 设置为白色静态光
- **彩虹**: 彩虹循环效果
- **派对模式**: 自动循环所有效果

### 🔋 电池管理控制

#### 电量显示
- 实时电池电量百分比显示
- 电池电压监控
- 可手动设置显示电量

#### 充电状态管理
- 充电状态检测和显示
- 充电动画效果（进度条动态增长）
- 可手动设置充电状态用于测试

#### 智能模式
- **自动模式**: 自动检测电池状态
- **手动模式**: 手动控制电量和充电状态显示
- **混合模式**: 部分参数手动控制，部分自动检测

#### 充电动画特性
- 基于设定电量的进度条增长动画
- 充电时进度条在设定值基础上动态扩展
- 平滑的呼吸式动画效果
- 可开启/关闭动画功能

### 灯光模式
1. **关闭** (0): 关闭灯光
2. **静态颜色** (1): 显示固定颜色
3. **彩虹效果** (2): 彩虹色彩循环
4. **呼吸灯** (3): 亮度渐变效果
5. **跑马灯** (4): 流水灯效果
6. **闪烁效果** (5): 闪烁模式
7. **波浪效果** (6): 波浪传播效果
8. **自动循环** (7): 自动切换各种效果
9. **电量显示** (8): 显示电池电量
10. **音乐律动** (9): 随音乐节奏律动

## API接口

### 🎨 RGB灯带控制接口
- **POST** `/api/control` - RGB灯带控制
- **POST** `/api/test` - 灯光测试
- **POST** `/api/simple` - 简单测试

### 🔋 电池管理接口
- **GET** `/api/battery/status` - 查询电池状态
- **POST** `/api/battery/control` - 电池控制设置

### 📡 OTA 固件升级接口
- **GET** `/api/ota/status` - 查询 OTA 状态（版本、进度、下载中标记）
- **POST** `/api/ota/check_now` - 立即检查更新（返回是否有新版本）
- **POST** `/api/ota/confirm` - 用户确认升级，开始下载
- **POST** `/api/ota/upload` - 本地上传 .bin 固件直接升级
- **POST** `/api/ota/enable` - 开关自动更新
- **GET** `/api/ota/urls` - 查询升级地址
- **POST** `/api/ota/urls` - 保存升级地址
- **POST** `/api/ota/urls/reset` - 恢复默认升级地址

### RGB控制参数
```json
{
  "channel": 255,        // 通道号 (0-3单通道, 255广播)
  "mode": 1,            // 灯光模式 (0-7)
  "color": {            // RGB颜色值
    "r": 255,
    "g": 255, 
    "b": 255
  },
  "brightness": 128,    // 亮度 (0-255)
  "speed": 100,         // 速度 (10-2000ms)
  "enabled": true       // 通道启用状态
}
```

### 电池控制参数
```json
{
  "battery": 75,          // 设置电池电量百分比 (0-100)
  "charging": true,       // 设置充电状态 (true/false)
  "auto_mode": true,      // 恢复自动电池检测模式
  "auto_charging": true   // 恢复自动充电状态检测
}
```

### 电池状态查询响应
```json
{
  "voltage": 3.85,        // 电池电压
  "percentage": 75,       // 电池电量百分比
  "charging_status": "CHARGING",  // 充电状态
  "display": {
    "mode": "external",   // 显示模式 (auto/external)
    "current_display": 75 // 当前显示电量
  },
  "animation": {
    "enabled": true,      // 动画是否启用
    "active": true,       // 动画是否激活
    "effect": "charging"  // 当前动画效果
  }
}
```

### OTA 状态查询响应
```json
{
  "version": "1.0.0",           // 当前固件版本
  "auto_update": false,         // 是否启用自动更新
  "last_status": "idle",        // 最近状态
  "check_in_progress": false,   // 是否正在检查更新
  "progress": 37,               // 下载进度 (0-100)
  "progress_msg": "下载中 0.4/1.8 MB",  // 进度描述
  "downloading": true,          // 是否正在下载
  "pending_update": {           // 待确认更新（仅当有新版本时出现）
    "version": "1.0.1",
    "url": "http://...",
    "waiting_confirmation": true
  }
}
```

## 文件结构
```
├── main/
│   ├── main.c              # 主程序入口
│   ├── web_server.c        # Web服务器实现
│   ├── web_server.h        # Web服务器头文件
│   ├── ws2812_control.c    # WS2812控制实现
│   ├── ws2812_control.h    # WS2812控制头文件
│   ├── battery_control.h   # 电池管理头文件
│   ├── claude_ble_mode.c   # CLAUDE 模式: BLE NUS + LVGL 状态面板
│   ├── claude_ble_mode.h   # CLAUDE 模式头文件
│   ├── claude_wifi_mode.c  # CLAUDE_WIFI 模式: WiFi (STA/AP 配网) + UDP 发现 + TCP 状态监听
│   ├── claude_wifi_mode.h  # CLAUDE_WIFI 模式头文件
│   ├── ota_updater.c       # OTA 固件升级: 版本检查、下载、进度、断点续传
│   ├── ota_updater.h       # OTA 固件升级头文件
│   ├── APP/                # 应用程序模块
│   └── Lib/                # 第三方库
├── components/             # ESP-IDF组件
├── tools/
│   ├── claude_status_ble_bridge.py    # PC -> BLE 桥接守护脚本
│   ├── claude_status_wifi_bridge.py   # PC -> WiFi 桥接守护脚本 (UDP 发现 + TCP 长连接)
│   ├── claude_hook_post.py            # Claude Code 钩子助手
│   ├── claude_hooks_settings.example.json  # 钩子配置示例
│   └── make_release.sh                # OTA 发布包构建脚本（自动读取版本号）
├── build/                  # 编译输出目录
└── managed_components/     # 管理的组件
```

## OTA 固件升级 (test5+)

### 网页端升级
1. 设备连接 WiFi 后，浏览器访问 `http://<设备IP>/ota`；
2. 页面显示当前版本、自动更新开关、最近状态；
3. 点击「立即检查更新」—— 设备从默认服务器拉取 `latest.json` 比对新版本；
4. 发现新版本后显示红色提示和「确认升级」按钮，点击确认开始下载；
5. 下载过程中页面实时显示进度条和百分比，下载完成后设备自动重启；
6. 重启后 rollback 定时器（30 秒）确认新固件运行正常，自动提交。

### 本地上传升级
- 在 `/ota` 页面选择本地 `build/panda.bin` 文件，点击「上传并升级」直接推送固件到设备，不经外网。

### 发布新版本
```bash
# 1. 修改 CMakeLists.txt 中的版本号
#    set(PROJECT_VER "1.0.1")

# 2. 一键构建发布包
./tools/make_release.sh

# 脚本自动：
#   - 从 CMakeLists.txt 读取 PROJECT_VER
#   - 编译固件
#   - 生成 latest.json 清单（含版本号、下载地址、文件大小、SHA256）
#   - 输出到 releases/ 目录
```
发布包上传到服务器后，设备即可通过 OTA 检测到新版本。

### 升级地址配置
- 网页 `/ota` 页面可修改主/备升级地址，保存到 NVS；
- 默认主地址：`http://120.27.145.121:8090/releases/latest.json`；
- 默认备地址：`https://github.com/ruiofang/ESP32S3-TFT-BS/releases/download/latest/latest.json`。

## CLAUDE 状态模式 (test3+)

### 启用方式
1. 编译固件（首次启用 BLE 需要重新生成 sdkconfig）：
   ```bash
   idf.py reconfigure   # 拉取 sdkconfig.defaults 里的 BLE 选项
   idf.py build flash monitor
   ```
2. 短按 BOOT 键循环模式，直到 LCD 左上角显示 `MODE: CLAUDE`。
3. 设备开始广播 BLE 设备 `ESP32_Claude_XXXX`，LCD 显示 `BLE: ADV / Waiting for BLE host...`，WS2812 进入灰色待机色。

> **WiFi 与 BLE 互斥**: ESP32-S3 内部 DRAM 只有 ~32 KiB，无法同时塞下 WiFi softAP + BT 控制器 + LVGL DMA 缓冲。因此 CLAUDE 模式下 WiFi 软热点 / Web 控制面板会关闭，只有 BLE 工作；其它模式（JSON / RS485-1 / RS485-2）下 WiFi 工作，BLE 不初始化。
>
> **切换需要软重启**: 在 CLAUDE ↔ 其它模式之间切换时设备会自动 `esp_restart()` 重新启动（约 1 秒），按目标模式做对应的射频初始化。JSON / RS485-1 / RS485-2 三者之间切换仍然热生效，无重启。

### PC 端
```bash
# 安装依赖
pip install bleak

# 启动桥接守护进程
python3 tools/claude_status_ble_bridge.py --listen-port 8765 --connect-on-start

# 手动单次测试
python3 tools/claude_status_ble_bridge.py --once \
    --json '{"state":"thinking","tool":"Read","msg":"hello world"}'
```
桥接进程会自动扫描 `ESP32_Claude_*` 设备并连接。

### 接入 Claude Code Hooks
复制 `tools/claude_hooks_settings.example.json` 中的 `hooks` 段合并到 `~/.claude/settings.json`，并把 `/ABS/PATH/` 替换为本仓库的绝对路径。这样 Claude Code 的每个生命周期事件（提交 prompt / 调用工具 / 工具完成 / 通知 / Stop）都会通过桥接转推到设备。

可选环境变量：`CLAUDE_BRIDGE_HOST`、`CLAUDE_BRIDGE_PORT`、`CLAUDE_MODEL`、`CLAUDE_TOKENS_IN`、`CLAUDE_TOKENS_OUT`。

### 通用协议 (PC 端可直接对接其它来源)
ESP32 暴露标准 NUS：
- 服务 UUID: `6E400001-B5A3-F393-E0A9-E50E24DCCA9E`
- RX（写入）: `6E400002-B5A3-F393-E0A9-E50E24DCCA9E`
- TX（通知）: `6E400003-B5A3-F393-E0A9-E50E24DCCA9E`

往 RX 写入一行 JSON（可换行结尾或括号配平即解析）：
```json
{"state":"thinking","tool":"Read","model":"Opus 4.7","ti":12345,"to":678,"msg":"Reading main.c"}
```
字段说明：

| 字段  | 类型   | 说明 |
| ----- | ------ | ---- |
| state | string | `idle / thinking / tool / writing / waiting / error / done` |
| tool  | string | 当前工具名 (≤23 字符) |
| model | string | 模型名 (≤23 字符) |
| ti    | int    | 累计输入 token |
| to    | int    | 累计输出 token |
| msg   | string | 简短消息 (≤63 字符) |

未提供的字段保留上次值，可做增量更新。

## CLAUDE_WIFI 状态模式 (test4+)

与 BLE 路径共用同一个状态面板和 WS2812 配色，但通过 **WiFi** 代替 BLE NUS 传输。链路模型为 **UDP 发现 + TCP 长连接状态推送**，适合没有蓝牙适配器、希望走家庭网络远程推送，或需要让 ESP32 明确感知主机离线的场景。

### 启用方式
1. 短按 BOOT 循环到 LCD 显示 `MODE: CLAUDE_WIFI` (期间会软重启切换射频)；
2. 首次进入没有 STA 凭据时设备自动起 AP `ESP32_Claude_XXXX` (密码 `claude123`)，LCD 显示 `AP: ESP32_Claude_XXXX`；
3. 手机/电脑连这个热点，浏览器打开 `http://192.168.4.1/` → 进入 **WiFi** 页 → 点 *扫描* → 选 SSID → 输密码 → *保存并连接*；
4. 设备保存凭据后自动重启进 STA 模式，连接成功 LCD 显示 `WiFi: <IP>` 与 `ID:<XXXX>` (设备号 = MAC 后 4 位 hex，与 BLE 命名规则一致)。

### 重新配网 / 换 AP
- 在 CLAUDE_WIFI 模式下按住 **BOOT 键 ≥ 2.5 秒**，设备清除已保存凭据并重启回到 AP 配网态；
- 或在 STA 已连接状态下访问 `http://<设备IP>/tools` 页，点击「清除已保存 WiFi 凭据」。

### PC 端
桥不接受命令行参数，所有设置走同目录 `tools/claude_wifi_bridge.json`。首次运行会自动写一份默认模板：

```bash
python3 tools/claude_status_wifi_bridge.py
# -> 提示已创建 tools/claude_wifi_bridge.json, 编辑后重跑
```

`claude_wifi_bridge.json` 字段：

| 字段 | 默认 | 说明 |
| ---- | ---- | ---- |
| `targets`          | `[]`              | 推荐写法；数组元素形如 `{"device_id":"AB12","label":"desk-left"}`，可同时绑定多个设备 |
| `device_ids`       | `[]`              | 多设备简写；例如 `["AB12","CD34"]` |
| `device_id`        | `""`              | 旧单设备兼容写法；LCD 上 `ID:XXXX` 的设备号；留空 = 配对任意应答设备 |
| `static_ip`        | `null`            | 旧单设备兼容写法；填了就跳过广播发现，直接连接这个 IP |
| `broadcast`        | `255.255.255.255` | 默认广播地址（会与本地每个接口的 /24 定向广播一起 fan-out） |
| `listen_host`      | `127.0.0.1`       | TCP 监听地址（Claude Code 钩子连这里） |
| `listen_port`      | `8765`            | TCP 监听端口 |
| `source_name`      | `copilot`         | 桥发给 ESP32 的绑定名称，LCD 会优先显示它 |
| `connect_on_start` | `false`           | 启动时立即发一条 idle，顺便完成发现 |
| `verbose`          | `false`           | 打开 DEBUG 日志 |

推荐多设备配置示例：

```json
{
  "targets": [
    {"device_id": "AB12", "label": "desk-left"},
    {"device_id": "CD34", "label": "desk-right"}
  ],
  "listen_host": "127.0.0.1",
  "listen_port": 8765,
  "source_name": "copilot",
  "connect_on_start": false,
  "verbose": false
}
```

桥会广播 `{"q":"discover","id":"XXXX"}` 到 `255.255.255.255` + 本地各子网 `192.168.x.255`。匹配设备号的设备回 discovery 应答，里面会自动带上设备 IP 与 `tcp_port`；桥随后建立持久 TCP 连接，并定时发心跳维持在线状态。Claude Code Hooks 仍指向 `localhost:8765`，与 BLE 桥配置可二选一。

### WiFi 协议

#### UDP 发现 (端口 8266)
| 方向 | 帧 | 说明 |
| ---- | -- | ---- |
| PC → ESP | `{"q":"discover"}` 或 `{"q":"discover","id":"XXXX"}` | 广播发现；id 不匹配时设备丢弃 |
| ESP → PC | `{"r":"discover","id":"XXXX","name":"ESP32_Claude_XXXX","ip":"...","port":8267,"udp_port":8266,"tcp_port":8267,"clients":0}` | 发现应答，自动携带 TCP 端口和当前已绑定客户端数量 |
| PC → ESP | `{"q":"ping","id":"XXXX"}` | 可选发现层探活 |
| ESP → PC | `{"r":"pong","id":"XXXX"}` | ping 应答 |

#### TCP 状态传输 (端口 8267)
| 方向 | 帧 | 说明 |
| ---- | -- | ---- |
| PC → ESP | `{"q":"bind","id":"XXXX","source":"copilot","label":"desk-left"}` | 建立连接后的绑定声明；ESP32 用它更新 LCD 绑定信息 |
| PC → ESP | `{"q":"ping","id":"XXXX","source":"copilot"}` | 桥定时发送的心跳；断开或超时后 ESP32 判定主机离线 |
| PC → ESP | `{"state":"thinking","tool":"Read",...}` | 状态推送，字段同 BLE 协议 |

TCP 长连接断开、RST、或 ESP32 侧心跳超时都会触发离线态；这是选择 TCP 而不是 HTTP 的主要原因。

### 配网页接口
- `GET /` — 主页 (设备号 / IP / TCP 端口 / 当前模式)
- `GET /wifi` (`/wificfg` 别名) — WiFi 配网表单 + AP 扫描列表
- `GET /wifiscan` — 返回 `[{"s":"SSID","r":-50,"a":3}, ...]` 的 AP 列表 (`a` = authmode, 0 = open)
- `GET /tools` — 高级页，含「清除已保存 WiFi 凭据」按钮
- `POST /wificfg` — 保存 SSID / 密码，写 NVS 后软重启
- `POST /wificlear` — 清除凭据后软重启

> **CLAUDE_WIFI 与 CLAUDE/BLE 互斥**：与 CLAUDE (BLE) 模式同理，CLAUDE_WIFI ↔ CLAUDE 之间切换会触发软重启重新选择射频；CLAUDE_WIFI ↔ JSON/RS485 之间切换也会软重启，因为 WiFi 接口初始化路径不同。

## 开发环境
- **ESP-IDF**: v5.5.2
- **编译器**: GCC
- **开发平台**: Windows/Linux/macOS

## 更新日志

### V1.2.1
- 优化LCD模式3的横屏显示；
- 优化RUN指示灯的闪烁延时；
### V1.2 
- **新增电池管理系统**: 完整的电池电量显示和充电状态管理
- **改进充电动画**: 基于设定电量的进度条动态增长效果
- **Web端电池控制**: 浏览器端完整的电池管理界面
- **HTTP API扩展**: 新增电池状态查询和控制接口
- **智能模式切换**: 支持自动/手动模式灵活切换
- **UI交互优化**: 改进用户界面响应和动画效果

### V1.1
- 添加Web控制面板居中布局
- 增加WiFi热点密码保护
- 优化用户界面体验
- 完善API文档

### V1.0
- 基础TFT显示功能
- WS2812多通道控制
- WiFi热点模式
- Web远程控制

## 致谢
感谢 [betwowt/ESP-IDF-TFT-NV3007-LVGL](https://github.com/betwowt/ESP-IDF-TFT-NV3007-LVGL.git) 项目提供的基础代码支持。

## 许可证
本项目采用MIT许可证，详见LICENSE文件。