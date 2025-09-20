#!/usr/bin/env python3
"""
TCP电池监控程序 - 跨平台版本
连接到TCP服务器接收电池状态，然后通过串口发送给ESP32S3-TFT-BS板子显示电量
支持Windows和Linux系统
"""

import socket
import json
import time
import logging
import threading
import queue
import os
from datetime import datetime
import platform
try:
    import serial
    import serial.tools.list_ports
except ImportError:
    print("需要安装 pyserial: pip install pyserial")
    exit(1)

class BatteryMonitor:
    def __init__(self):
        # TCP连接配置
        self.tcp_host = "192.168.10.122"
        self.tcp_port = 8080
        self.tcp_socket = None
        self.tcp_connected = False
        
        # 串口配置 - 直接指定串口
        self.serial_port = "/dev/ttyUSB0"  # Linux默认USB串口，可根据需要修改
        self.serial_baudrate = 115200
        self.serial_conn = None
        
        # 日志配置 - 从环境变量读取或默认为简洁日志
        self.verbose_logging = os.getenv('VERBOSE_LOGGING', '0') == '1'
        
        # 数据队列
        self.battery_queue = queue.Queue()
        
        # 控制变量
        self.running = True
        
        # 数据缓存，避免重复发送
        self.last_battery_data = None
        self.last_send_time = 0
        self.min_send_interval = 1  # 最小发送间隔（秒）
        
        # 数据平滑
        self.battery_history = []  # 电量历史记录
        self.voltage_history = []  # 电压历史记录
        self.history_size = 3  # 保留最近3次数据用于平滑
        
        # 日志抑制机制
        self.serial_connection_status = None  # 记录串口连接状态（True/False/None）
        
        # 设置日志
        self.setup_logging()
        
    def setup_logging(self):
        """设置日志记录"""
        # 根据配置设置日志级别
        log_level = logging.DEBUG if self.verbose_logging else logging.INFO
        
        logging.basicConfig(
            level=log_level,
            format='%(asctime)s - %(levelname)s - %(message)s',
            handlers=[
                logging.FileHandler('battery_monitor.log', encoding='utf-8'),
                logging.StreamHandler()
            ]
        )
        self.logger = logging.getLogger(__name__)
    
    def connect_tcp(self):
        """连接TCP服务器"""
        try:
            if self.tcp_socket:
                self.tcp_socket.close()
                
            self.tcp_socket = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            self.tcp_socket.settimeout(10)  # 10秒超时
            self.tcp_socket.connect((self.tcp_host, self.tcp_port))
            self.tcp_connected = True
            self.logger.info(f"TCP连接成功: {self.tcp_host}:{self.tcp_port}")
            return True
        except Exception as e:
            self.logger.error(f"TCP连接失败: {e}")
            self.tcp_connected = False
            if self.tcp_socket:
                self.tcp_socket.close()
            return False
    
    def connect_serial(self):
        """连接串口"""
        try:
            if self.serial_conn and self.serial_conn.is_open:
                self.serial_conn.close()
                
            self.serial_conn = serial.Serial(
                port=self.serial_port,
                baudrate=self.serial_baudrate,
                bytesize=serial.EIGHTBITS,
                parity=serial.PARITY_NONE,
                stopbits=serial.STOPBITS_ONE,
                timeout=1
            )
            self.logger.info(f"串口连接成功: {self.serial_port}")
            # 更新串口连接状态
            self.serial_connection_status = True
            return True
        except Exception as e:
            self.logger.error(f"串口连接失败: {e}")
            return False
    
    def tcp_listener(self):
        """TCP监听线程"""
        buffer = ""
        reconnect_delay = 5
        
        while self.running:
            if not self.tcp_connected:
                self.logger.debug("尝试重新连接TCP...")
                if self.connect_tcp():
                    reconnect_delay = 5  # 重置重连延迟
                    continue
                else:
                    if reconnect_delay >= 30:  # 只在延迟较长时才记录警告日志
                        self.logger.warning(f"TCP连接失败，{reconnect_delay:.0f}秒后重试...")
                    time.sleep(reconnect_delay)
                    reconnect_delay = min(reconnect_delay * 1.5, 60)  # 指数退避，最大60秒
                    continue
            
            try:
                self.tcp_socket.settimeout(1)  # 1秒超时，允许定期检查running状态
                data = self.tcp_socket.recv(4096).decode('utf-8')
                
                if not data:
                    self.logger.warning("TCP连接断开")
                    self.tcp_connected = False
                    continue
                
                buffer += data
                
                # 查找完整的JSON对象
                while '{' in buffer and '}' in buffer:
                    start_idx = buffer.find('{')
                    if start_idx == -1:
                        break
                    
                    # 寻找匹配的右括号
                    brace_count = 0
                    end_idx = -1
                    for i, char in enumerate(buffer[start_idx:], start_idx):
                        if char == '{':
                            brace_count += 1
                        elif char == '}':
                            brace_count -= 1
                            if brace_count == 0:
                                end_idx = i
                                break
                    
                    if end_idx != -1:
                        json_str = buffer[start_idx:end_idx + 1]
                        buffer = buffer[end_idx + 1:]
                        
                        try:
                            json_data = json.loads(json_str)
                            self.battery_queue.put(json_data)
                            # 精简日志：只在调试模式下记录数据接收
                            self.logger.debug(f"接收到JSON数据")
                        except json.JSONDecodeError as e:
                            self.logger.error(f"JSON解析错误: {e}")
                    else:
                        break
                        
            except socket.timeout:
                continue
            except Exception as e:
                self.logger.error(f"TCP接收错误: {e}")
                self.tcp_connected = False
                time.sleep(1)
    
    def parse_battery_data(self, json_data):
        """解析电池数据并进行平滑处理"""
        try:
            battery_info = json_data.get('battery', {})
            
            # 提取原始数据
            raw_rsoc = battery_info.get('rsoc', 0)
            raw_voltage = battery_info.get('voltage', 0.0)
            current = battery_info.get('current', 0.0)
            is_charging = current > 0
            
            # 数据平滑处理
            self.battery_history.append(raw_rsoc)
            self.voltage_history.append(raw_voltage)
            
            # 保持历史记录大小
            if len(self.battery_history) > self.history_size:
                self.battery_history.pop(0)
            if len(self.voltage_history) > self.history_size:
                self.voltage_history.pop(0)
            
            # 计算平滑后的值
            smoothed_battery = sum(self.battery_history) / len(self.battery_history)
            smoothed_voltage = sum(self.voltage_history) / len(self.voltage_history)
            
            self.logger.debug(f"原始数据: 电量{raw_rsoc}%, 电压{raw_voltage}V")
            self.logger.debug(f"平滑后: 电量{smoothed_battery:.1f}%, 电压{smoothed_voltage:.1f}V")
            
            return {
                'battery_percentage': round(smoothed_battery),
                'voltage': round(smoothed_voltage, 1),
                'charging': is_charging,
                'current': current
            }
            
        except Exception as e:
            self.logger.error(f"解析电池数据错误: {e}")
            return None
    
    def send_to_esp32(self, battery_data):
        """发送数据到ESP32板子"""
        if not self.serial_conn or not self.serial_conn.is_open:
            # 只在串口状态从连接变为未连接时记录警告
            if self.serial_connection_status != False:
                self.logger.warning("串口未连接")
                self.serial_connection_status = False
            return False
        
        current_time = time.time()
        
        # 检查是否需要发送数据
        if self.last_battery_data is not None:
            # 检查数据是否有变化（允许小幅度的电压波动）
            voltage_change = abs(battery_data['voltage'] - self.last_battery_data['voltage'])
            battery_change = abs(battery_data['battery_percentage'] - self.last_battery_data['battery_percentage'])
            charging_change = battery_data['charging'] != self.last_battery_data['charging']
            
            # 如果数据变化很小且发送间隔太短，则跳过发送
            if (voltage_change < 0.1 and 
                battery_change < 1 and 
                not charging_change and 
                current_time - self.last_send_time < self.min_send_interval):
                return True  # 返回成功但不发送
        
        try:
            # 构造ESP32协议格式的JSON命令
            esp32_command = {
                "voltage": round(battery_data['voltage'], 1),  # 保留一位小数
                "battery": int(battery_data['battery_percentage']),  # 确保是整数
                "charging": battery_data['charging']
            }
            
            # 转换为JSON字符串并发送
            command_str = json.dumps(esp32_command)
            self.serial_conn.write((command_str + '\n').encode('utf-8'))
            self.serial_conn.flush()  # 确保数据发送
            
            # 更新串口连接状态（如果之前是断开状态，现在成功发送了）
            if self.serial_connection_status != True:
                self.serial_connection_status = True
            
            # 更新缓存
            self.last_battery_data = battery_data.copy()
            self.last_send_time = current_time
            
            # 精简日志：只显示关键信息，减少冗余输出
            self.logger.info(f"电池状态 - 电量: {battery_data['battery_percentage']}%, "
                           f"电压: {battery_data['voltage']}V, "
                           f"{'充电中' if battery_data['charging'] else '放电中'}")
            
            # 详细的串口数据只在调试模式下显示
            self.logger.debug(f"发送到ESP32: {command_str}")
            
            return True
            
        except Exception as e:
            self.logger.error(f"串口发送错误: {e}")
            # 尝试重新连接串口
            self.connect_serial()
            return False
    
    def battery_processor(self):
        """电池数据处理线程"""
        while self.running:
            try:
                # 从队列中获取数据，超时1秒
                json_data = self.battery_queue.get(timeout=1)
                
                # 解析电池数据
                battery_data = self.parse_battery_data(json_data)
                if battery_data is None:
                    continue
                
                # 发送到ESP32
                self.send_to_esp32(battery_data)
                
                # 标记任务完成
                self.battery_queue.task_done()
                
            except queue.Empty:
                continue
            except Exception as e:
                self.logger.error(f"电池数据处理错误: {e}")
    
    def run(self):
        """主运行函数"""
        print("=" * 50)
        print("ESP32S3 电池监控程序 - 跨平台版本")
        print(f"运行平台: {platform.system()}")
        print("=" * 50)
        
        self.logger.info("启动电池监控程序...")
        
        # 连接串口
        if not self.connect_serial():
            self.logger.warning("串口连接失败，程序将继续运行但无法发送数据到ESP32")
        
        # 启动TCP监听线程
        tcp_thread = threading.Thread(target=self.tcp_listener, daemon=True)
        tcp_thread.start()
        
        # 启动电池数据处理线程
        processor_thread = threading.Thread(target=self.battery_processor, daemon=True)
        processor_thread.start()
        
        try:
            self.logger.info(f"程序已启动 - TCP服务器: {self.tcp_host}:{self.tcp_port}, 串口: {self.serial_port}")
            print("等待数据... (按 Ctrl+C 退出)")
            
            while True:
                time.sleep(1)
                
        except KeyboardInterrupt:
            self.logger.info("接收到退出信号...")
            self.running = False
            
            # 关闭连接
            if self.tcp_socket:
                self.tcp_socket.close()
            if self.serial_conn and self.serial_conn.is_open:
                self.serial_conn.close()
            
            self.logger.info("程序已退出")

if __name__ == "__main__":
    monitor = BatteryMonitor()
    monitor.run()