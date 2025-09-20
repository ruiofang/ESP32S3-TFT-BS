# TCP电池监控程序

这是一个TCP电池监控程序，用于从TCP服务器接收电池状态信息，并通过串口发送给ESP32S3-TFT-BS板子进行显示。

## 文件说明

- `tcp_battery_monitor.py` - 主程序文件
- `start_battery_monitor.sh` - 启动脚本，用于手动控制程序
- `battery-monitor.service` - systemd服务配置文件
- `install_service.sh` - 服务安装脚本
- `README.md` - 本说明文件

## 快速开始

### 1. 手动启动程序

```bash
# 启动程序
./start_battery_monitor.sh start

# 查看状态
./start_battery_monitor.sh status

# 查看日志
./start_battery_monitor.sh logs

# 停止程序
./start_battery_monitor.sh stop

# 重启程序
./start_battery_monitor.sh restart
```

### 2. 安装为系统服务（开机自启动）

```bash
# 安装服务（需要root权限）
sudo ./install_service.sh install

# 查看服务状态
sudo ./install_service.sh status

# 卸载服务
sudo ./install_service.sh uninstall
```

## 详细使用说明

### 启动脚本 (start_battery_monitor.sh)

这个脚本提供了完整的程序控制功能：

#### 命令格式
```bash
./start_battery_monitor.sh {start|stop|restart|status|logs [行数]}
```

#### 日志模式
- **简洁模式** (默认): 只显示重要的状态信息，减少日志输出
- **详细模式** (-v 或 --verbose): 显示完整的调试信息

#### 功能说明
- **start**: 启动电池监控程序
  - 检查程序是否已运行
  - 检查依赖包是否安装
  - 后台启动程序并记录PID
  - 支持 `-v` 或 `--verbose` 参数启用详细日志
  
- **stop**: 停止电池监控程序
  - 优雅停止程序（发送SIGTERM信号）
  - 如果程序不响应，强制终止
  - 清理PID文件
  
- **restart**: 重启程序
  - 先停止程序，再启动
  
- **status**: 查看程序状态
  - 显示进程信息
  - 显示最近的日志记录
  
- **logs**: 查看程序日志
  - 默认显示最后20行
  - 可指定显示行数：`./start_battery_monitor.sh logs 50`

#### 使用示例
```bash
# 启动程序 (简洁日志模式)
./start_battery_monitor.sh start

# 启动程序 (详细日志模式)
./start_battery_monitor.sh start -v

# 查看状态和最近日志
./start_battery_monitor.sh status

# 查看最近50行日志
./start_battery_monitor.sh logs 50

# 重启程序
./start_battery_monitor.sh restart
```

### 系统服务 (systemd)

将程序安装为系统服务后，可以实现开机自启动和更好的管理。

#### 安装服务
```bash
sudo ./install_service.sh install
```

安装过程会：
1. 检查并安装Python依赖包
2. 设置正确的文件权限
3. 复制服务文件到系统目录
4. 启用开机自启动
5. 立即启动服务

#### 服务管理命令
```bash
# 查看服务状态
sudo systemctl status battery-monitor

# 启动服务
sudo systemctl start battery-monitor

# 停止服务
sudo systemctl stop battery-monitor

# 重启服务
sudo systemctl restart battery-monitor

# 查看实时日志
sudo journalctl -u battery-monitor -f

# 查看最近日志
sudo journalctl -u battery-monitor --since "1 hour ago"

# 禁用开机自启动
sudo systemctl disable battery-monitor

# 启用开机自启动
sudo systemctl enable battery-monitor
```

#### 卸载服务
```bash
sudo ./install_service.sh uninstall
```

### 配置说明

#### 日志配置
程序支持两种日志模式：

1. **简洁模式** (默认)
   - 只显示电池状态变化和重要事件
   - 适合正常运行时使用
   - 日志输出示例：`电池状态 - 电量: 85%, 电压: 12.6V, 充电中`

2. **详细模式**
   - 显示所有调试信息，包括数据接收、JSON解析等
   - 适合调试和故障排除
   - 启动时使用 `-v` 或 `--verbose` 参数

#### TCP连接配置
在 `tcp_battery_monitor.py` 中修改以下参数：
```python
self.tcp_host = "192.168.10.122"  # TCP服务器IP地址
self.tcp_port = 8080              # TCP服务器端口
```

#### 串口配置
```python
self.serial_port = "/dev/ttyUSB0"  # 串口设备路径
self.serial_baudrate = 115200      # 波特率
```

常见串口设备：
- `/dev/ttyUSB0` - USB转串口设备
- `/dev/ttyACM0` - Arduino兼容设备
- `/dev/ttyS0` - 传统串口

#### 查找串口设备
```bash
# 查看所有串口设备
ls /dev/tty*

# 查看USB串口设备
ls /dev/ttyUSB*

# 查看设备信息
dmesg | grep tty
```

## 日志文件

程序运行时会生成以下日志文件：

- `battery_monitor.log` - 主程序日志
- `startup.log` - 启动脚本日志

日志位置：程序所在目录

### 查看日志
```bash
# 查看主程序日志
tail -f battery_monitor.log

# 查看启动脚本日志
tail -f startup.log

# 查看系统服务日志（如果安装为服务）
sudo journalctl -u battery-monitor -f
```

## 故障排除

### 常见问题

1. **串口权限问题**
```bash
# 将用户添加到dialout组
sudo usermod -a -G dialout $USER
# 重新登录后生效
```

2. **依赖包未安装**
```bash
pip3 install pyserial
```

3. **TCP连接失败**
- 检查网络连接
- 确认TCP服务器IP和端口
- 检查防火墙设置

4. **串口设备不存在**
- 检查设备是否连接
- 确认串口设备路径
- 查看系统日志：`dmesg | tail`

### 调试方法

1. **手动运行程序**
```bash
cd /home/phi/tcp_battery_monitor
python3 tcp_battery_monitor.py
```

2. **查看详细日志**
```bash
# 启动脚本日志
./start_battery_monitor.sh logs 100

# 系统服务日志
sudo journalctl -u battery-monitor --since today
```

3. **测试串口连接**
```bash
# 安装串口测试工具
sudo apt install minicom

# 测试串口（退出用Ctrl+A然后X）
minicom -D /dev/ttyUSB0 -b 115200
```

## 系统要求

- Linux系统（推荐Ubuntu/Debian）
- Python 3.6+
- pyserial库
- 系统管理员权限（仅安装服务时需要）

## 许可证

本程序仅供学习和个人使用。

## 更新日志

- v1.0 - 初始版本
  - TCP连接和数据接收
  - 串口通信
  - 数据平滑处理
  - 启动脚本和systemd服务支持