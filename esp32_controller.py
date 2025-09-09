#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
ESP32S3-TFT-BS 控制器
一个用户友好的交互式控制程序，支持WS2812灯带、电池管理和LCD显示控制
"""

import json
import serial
import serial.tools.list_ports
import socket
import time
import subprocess
import ipaddress
import threading
from typing import Optional, Dict, Any, List, Tuple
import sys


class ESP32Controller:
    """ESP32S3-TFT-BS 控制器类"""
    
    def __init__(self):
        self.connection = None
        self.connection_type = None
    
    def scan_serial_ports(self) -> List[Tuple[str, str]]:
        """扫描可用串口"""
        print("🔍 正在扫描串口设备...")
        available_ports = []
        
        try:
            ports = serial.tools.list_ports.comports()
            for port in ports:
                # 尝试识别ESP32相关设备
                device_info = f"{port.device}"
                description = port.description or "未知设备"
                
                # ESP32相关关键词
                esp_keywords = ['esp32', 'esp', 'ch340', 'ch341', 'cp210', 'ftdi', 'usb-serial', 'silicon labs']
                is_likely_esp = any(keyword in description.lower() for keyword in esp_keywords)
                
                if is_likely_esp:
                    device_info += f" ⭐ ({description})"
                else:
                    device_info += f" ({description})"
                
                available_ports.append((port.device, device_info))
            
            if available_ports:
                print(f"✅ 找到 {len(available_ports)} 个串口设备")
            else:
                print("⚠️  未找到串口设备")
                
        except Exception as e:
            print(f"❌ 扫描串口失败: {e}")
        
        return available_ports
    
    def scan_network_devices(self, start_ip: str = "192.168.1.1", end_ip: str = "192.168.1.254", 
                           port: int = 80, timeout: float = 0.5) -> List[str]:
        """扫描网络设备"""
        print(f"🔍 正在扫描网络设备 {start_ip} - {end_ip}:{port}...")
        print("⏳ 扫描中，请稍候...")
        
        available_devices = []
        scan_results = []
        
        def scan_ip(ip: str):
            try:
                sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
                sock.settimeout(timeout)
                result = sock.connect_ex((ip, port))
                sock.close()
                if result == 0:
                    scan_results.append(ip)
            except:
                pass
        
        # 解析IP范围
        try:
            start_addr = ipaddress.IPv4Address(start_ip)
            end_addr = ipaddress.IPv4Address(end_ip)
            
            # 创建线程池进行并发扫描
            threads = []
            current_addr = start_addr
            
            while current_addr <= end_addr:
                ip_str = str(current_addr)
                thread = threading.Thread(target=scan_ip, args=(ip_str,))
                threads.append(thread)
                thread.start()
                current_addr += 1
                
                # 限制并发线程数量
                if len(threads) >= 50:
                    for t in threads:
                        t.join()
                    threads = []
            
            # 等待剩余线程完成
            for thread in threads:
                thread.join()
            
            available_devices = sorted(scan_results, key=lambda x: ipaddress.IPv4Address(x))
            
            if available_devices:
                print(f"✅ 找到 {len(available_devices)} 个网络设备")
            else:
                print("⚠️  未找到网络设备")
                
        except Exception as e:
            print(f"❌ 网络扫描失败: {e}")
        
        return available_devices
    
    def quick_network_scan(self) -> List[str]:
        """快速网络扫描 - 扫描常见网段"""
        print("🚀 执行快速网络扫描...")
        
        # 获取本机IP来确定扫描范围
        try:
            # 尝试获取本机IP
            s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
            s.connect(("8.8.8.8", 80))
            local_ip = s.getsockname()[0]
            s.close()
            
            # 根据本机IP确定扫描网段
            ip_parts = local_ip.split('.')
            network_base = '.'.join(ip_parts[:3])
            
            print(f"📍 本机IP: {local_ip}")
            print(f"🔍 扫描网段: {network_base}.1 - {network_base}.254")
            
            return self.scan_network_devices(
                f"{network_base}.1", 
                f"{network_base}.254", 
                port=80, 
                timeout=0.3
            )
        except:
            # 默认扫描常见网段
            print("🔍 扫描默认网段: 192.168.1.1 - 192.168.1.254")
            return self.scan_network_devices(
                "192.168.1.1", 
                "192.168.1.254", 
                port=80, 
                timeout=0.3
            )
        
    def connect_serial_with_scan(self) -> bool:
        """扫描并连接串口"""
        available_ports = self.scan_serial_ports()
        
        if not available_ports:
            print("❌ 未找到可用串口设备")
            manual = input("是否手动输入串口号? (y/n): ").strip().lower()
            if manual == 'y':
                port = input("请输入串口号 (如 COM3): ").strip()
                baudrate = input("波特率 (留空=115200): ").strip()
                baudrate = int(baudrate) if baudrate else 115200
                return self.connect_serial(port, baudrate)
            return False
        
        print("\n📋 可用串口设备:")
        for i, (port, info) in enumerate(available_ports, 1):
            print(f"  {i}. {info}")
        
        print(f"  {len(available_ports) + 1}. 手动输入串口号")
        print("  0. 返回上级菜单")
        
        while True:
            try:
                choice = input(f"\n请选择设备 (0-{len(available_ports) + 1}): ").strip()
                
                if choice == "0":
                    return False
                
                choice_num = int(choice)
                
                if 1 <= choice_num <= len(available_ports):
                    selected_port = available_ports[choice_num - 1][0]
                    baudrate = input("波特率 (留空=115200): ").strip()
                    baudrate = int(baudrate) if baudrate else 115200
                    return self.connect_serial(selected_port, baudrate)
                
                elif choice_num == len(available_ports) + 1:
                    port = input("请输入串口号: ").strip()
                    baudrate = input("波特率 (留空=115200): ").strip()
                    baudrate = int(baudrate) if baudrate else 115200
                    return self.connect_serial(port, baudrate)
                
                else:
                    print("⚠️  选择无效，请重新输入")
            
            except ValueError:
                print("⚠️  请输入有效数字")
    
    def connect_network_with_scan(self) -> bool:
        """扫描并连接网络设备"""
        print("\n🌐 网络连接选项:")
        print("  1. 快速扫描 (自动检测网段)")
        print("  2. 自定义扫描范围")
        print("  3. 手动输入IP地址")
        print("  0. 返回上级菜单")
        
        scan_choice = input("\n请选择扫描方式 (0-3): ").strip()
        
        if scan_choice == "0":
            return False
        
        available_devices = []
        
        if scan_choice == "1":
            available_devices = self.quick_network_scan()
        
        elif scan_choice == "2":
            try:
                start_ip = input("起始IP地址 (如 192.168.1.1): ").strip()
                end_ip = input("结束IP地址 (如 192.168.1.254): ").strip()
                port_input = input("扫描端口 (留空=80): ").strip()
                scan_port = int(port_input) if port_input else 80
                
                available_devices = self.scan_network_devices(start_ip, end_ip, scan_port)
            except ValueError:
                print("❌ IP地址或端口格式无效")
                return False
        
        elif scan_choice == "3":
            host = input("请输入IP地址: ").strip()
            port_input = input("端口号 (留空=80): ").strip()
            port = int(port_input) if port_input else 80
            return self.connect_tcp(host, port)
        
        else:
            print("⚠️  选择无效")
            return False
        
        # 显示扫描结果并让用户选择
        if not available_devices:
            print("❌ 未找到可用网络设备")
            manual = input("是否手动输入IP地址? (y/n): ").strip().lower()
            if manual == 'y':
                host = input("请输入IP地址: ").strip()
                port_input = input("端口号 (留空=80): ").strip()
                port = int(port_input) if port_input else 80
                return self.connect_tcp(host, port)
            return False
        
        print(f"\n📋 找到 {len(available_devices)} 个网络设备:")
        for i, device in enumerate(available_devices, 1):
            print(f"  {i}. {device}")
        
        print(f"  {len(available_devices) + 1}. 手动输入IP地址")
        print("  0. 返回上级菜单")
        
        while True:
            try:
                choice = input(f"\n请选择设备 (0-{len(available_devices) + 1}): ").strip()
                
                if choice == "0":
                    return False
                
                choice_num = int(choice)
                
                if 1 <= choice_num <= len(available_devices):
                    selected_ip = available_devices[choice_num - 1]
                    port_input = input("端口号 (留空=80): ").strip()
                    port = int(port_input) if port_input else 80
                    return self.connect_tcp(selected_ip, port)
                
                elif choice_num == len(available_devices) + 1:
                    host = input("请输入IP地址: ").strip()
                    port_input = input("端口号 (留空=80): ").strip()
                    port = int(port_input) if port_input else 80
                    return self.connect_tcp(host, port)
                
                else:
                    print("⚠️  选择无效，请重新输入")
            
            except ValueError:
                print("⚠️  请输入有效数字")
    
    def connect_serial(self, port: str, baudrate: int = 115200) -> bool:
        """连接串口"""
        try:
            self.connection = serial.Serial(port, baudrate, timeout=2)
            self.connection_type = "serial"
            print(f"✅ 串口连接成功: {port}")
            return True
        except Exception as e:
            print(f"❌ 串口连接失败: {e}")
            return False
    
    def connect_tcp(self, host: str, port: int = 80) -> bool:
        """连接TCP网络"""
        try:
            self.connection = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            self.connection.connect((host, port))
            self.connection.settimeout(5.0)
            self.connection_type = "tcp"
            print(f"✅ 网络连接成功: {host}:{port}")
            return True
        except Exception as e:
            print(f"❌ 网络连接失败: {e}")
            return False
    
    def send_command(self, command: Dict[str, Any]) -> str:
        """发送JSON命令"""
        if not self.connection:
            return "❌ 未连接设备"
        
        json_str = json.dumps(command, ensure_ascii=False)
        print(f"📤 发送命令: {json_str}")
        
        try:
            if self.connection_type == "serial":
                self.connection.write(json_str.encode('utf-8'))
                self.connection.flush()
                time.sleep(0.1)
                if self.connection.in_waiting > 0:
                    response = self.connection.read(self.connection.in_waiting).decode('utf-8', errors='ignore')
                    return f"📥 设备响应: {response.strip()}"
            
            elif self.connection_type == "tcp":
                self.connection.send(json_str.encode('utf-8'))
                response = self.connection.recv(1024).decode('utf-8', errors='ignore')
                return f"📥 设备响应: {response.strip()}"
                
        except Exception as e:
            return f"❌ 发送失败: {e}"
        
        return "✅ 命令已发送"
    
    def disconnect(self):
        """断开连接"""
        if self.connection:
            self.connection.close()
            self.connection = None
            self.connection_type = None
            print("🔌 连接已断开")


def print_menu():
    """显示主菜单"""
    print("\n" + "="*60)
    print("🎮 ESP32S3-TFT-BS 控制器")
    print("="*60)
    print("📡 连接选项:")
    print("  1. 串口连接")
    print("  2. 网络连接")
    print("  3. 断开连接")
    print("\n🎨 WS2812 灯带控制:")
    print("  4. 单通道控制")
    print("  5. 广播控制(所有通道)")
    print("  6. 通道启用/禁用")
    print("  7. 状态查询")
    print("  8. 自动循环设置")
    print("\n🔋 电池管理:")
    print("  9. 电池状态查询")
    print("  10. 设置电池电量")
    print("  11. 设置充电状态")
    print("  12. 恢复自动模式")
    print("\n📺 LCD显示控制:")
    print("  13. LCD清屏")
    print("  14. 显示文字")
    print("  15. 绘制图形")
    print("  16. 背光控制")
    print("\n⚙️  其他选项:")
    print("  17. 灯效模式说明")
    print("  18. 快速测试")
    print("  0. 退出程序")
    print("="*60)


def get_color_input() -> Dict[str, int]:
    """获取颜色输入"""
    print("\n颜色设置选项:")
    print("1. 预设颜色")
    print("2. 自定义RGB")
    
    choice = input("请选择颜色输入方式 (1-2): ").strip()
    
    if choice == "1":
        colors = {
            "1": {"r": 255, "g": 0, "b": 0},    # 红色
            "2": {"r": 0, "g": 255, "b": 0},    # 绿色
            "3": {"r": 0, "g": 0, "b": 255},    # 蓝色
            "4": {"r": 255, "g": 255, "b": 0},  # 黄色
            "5": {"r": 255, "g": 0, "b": 255},  # 洋红
            "6": {"r": 0, "g": 255, "b": 255},  # 青色
            "7": {"r": 255, "g": 255, "b": 255}, # 白色
        }
        print("\n预设颜色:")
        print("1=红色 2=绿色 3=蓝色 4=黄色 5=洋红 6=青色 7=白色")
        color_choice = input("请选择颜色 (1-7): ").strip()
        return colors.get(color_choice, {"r": 255, "g": 255, "b": 255})
    
    elif choice == "2":
        try:
            r = int(input("红色值 (0-255): ").strip())
            g = int(input("绿色值 (0-255): ").strip())
            b = int(input("蓝色值 (0-255): ").strip())
            return {"r": max(0, min(255, r)), "g": max(0, min(255, g)), "b": max(0, min(255, b))}
        except ValueError:
            print("⚠️  输入无效，使用默认白色")
            return {"r": 255, "g": 255, "b": 255}
    
    return {"r": 255, "g": 255, "b": 255}


def ws2812_single_control(controller: ESP32Controller):
    """单通道WS2812控制"""
    print("\n🎨 单通道WS2812控制")
    
    try:
        channel = int(input("请输入通道号 (0-3): ").strip())
        if channel not in range(4):
            print("⚠️  通道号无效，使用通道0")
            channel = 0
            
        print("\n灯效模式:")
        print("0=关闭 1=静态 2=彩虹 3=呼吸 4=跑马灯 5=闪烁 6=波浪 7=自动循环")
        mode = int(input("请选择模式 (0-7): ").strip())
        
        command = {"channel": channel, "mode": mode}
        
        if mode in [1, 3, 4, 5, 6]:  # 需要颜色的模式
            command["color"] = get_color_input()
        
        if mode != 0:  # 非关闭模式需要亮度
            brightness = int(input("请输入亮度 (0-255, 建议50-150): ").strip())
            command["brightness"] = max(0, min(255, brightness))
        
        if mode in [2, 3, 4, 5, 6, 7]:  # 需要速度的模式
            speed = int(input("请输入速度 (1-10000ms, 建议50-500): ").strip())
            command["speed"] = max(1, min(10000, speed))
        
        return controller.send_command(command)
        
    except ValueError:
        return "⚠️  输入格式错误"


def ws2812_broadcast_control(controller: ESP32Controller):
    """广播控制所有通道"""
    print("\n🎨 广播控制(所有通道)")
    
    try:
        print("灯效模式:")
        print("0=关闭 1=静态 2=彩虹 3=呼吸 4=跑马灯 5=闪烁 6=波浪 7=自动循环")
        mode = int(input("请选择模式 (0-7): ").strip())
        
        command = {"channel": 255, "mode": mode}  # 255为广播通道
        
        if mode in [1, 3, 4, 5, 6]:
            command["color"] = get_color_input()
        
        if mode != 0:
            brightness = int(input("请输入亮度 (0-255): ").strip())
            command["brightness"] = max(0, min(255, brightness))
        
        if mode in [2, 3, 4, 5, 6, 7]:
            speed = int(input("请输入速度 (1-10000ms): ").strip())
            command["speed"] = max(1, min(10000, speed))
        
        return controller.send_command(command)
        
    except ValueError:
        return "⚠️  输入格式错误"


def ws2812_channel_enable(controller: ESP32Controller):
    """通道启用/禁用"""
    print("\n🔧 通道启用/禁用")
    
    try:
        channel = int(input("请输入通道号 (0-3): ").strip())
        if channel not in range(4):
            print("⚠️  通道号无效")
            return "❌ 通道号无效"
        
        print("1. 启用通道")
        print("2. 禁用通道")
        choice = input("请选择操作 (1-2): ").strip()
        
        enabled = choice == "1"
        command = {"channel": channel, "enabled": enabled}
        
        return controller.send_command(command)
        
    except ValueError:
        return "⚠️  输入格式错误"


def battery_control(controller: ESP32Controller):
    """电池管理控制"""
    print("\n🔋 电池管理")
    print("1. 查询电池状态")
    print("2. 设置电池电量(非充电)")
    print("3. 设置充电状态")
    print("4. 恢复自动模式")
    
    choice = input("请选择操作 (1-4): ").strip()
    
    if choice == "1":
        command = {"query": "battery"}
    
    elif choice == "2":
        try:
            battery_level = int(input("请输入电池电量 (0-100): ").strip())
            command = {"battery": max(0, min(100, battery_level))}
        except ValueError:
            return "⚠️  电量输入无效"
    
    elif choice == "3":
        try:
            battery_level = int(input("请输入电池电量 (0-100): ").strip())
            command = {"battery": max(0, min(100, battery_level)), "charging": True}
        except ValueError:
            return "⚠️  电量输入无效"
    
    elif choice == "4":
        command = {"auto_mode": True}
    
    else:
        return "⚠️  选择无效"
    
    return controller.send_command(command)


def lcd_control(controller: ESP32Controller):
    """LCD显示控制"""
    print("\n📺 LCD显示控制")
    print("1. 清屏")
    print("2. 显示文字")
    print("3. 绘制矩形")
    print("4. 绘制圆形")
    print("5. 绘制线条")
    print("6. 背光控制")
    
    choice = input("请选择操作 (1-6): ").strip()
    
    if choice == "1":
        color_choice = input("清屏颜色 (留空=黑色, 或输入: red/green/blue/white): ").strip()
        command = {"lcd": "clear"}
        if color_choice:
            command["color"] = color_choice
    
    elif choice == "2":
        content = input("请输入要显示的文字: ").strip()
        command = {"lcd": "text", "content": content}
        
        pos_choice = input("是否指定位置? (y/n): ").strip().lower()
        if pos_choice == 'y':
            try:
                x = int(input("X坐标 (0-427): ").strip())
                y = int(input("Y坐标 (0-141): ").strip())
                command.update({"x": max(0, min(427, x)), "y": max(0, min(141, y))})
            except ValueError:
                pass
        
        color_choice = input("文字颜色 (留空=白色, 或输入颜色名): ").strip()
        if color_choice:
            command["color"] = color_choice
        
        size_choice = input("字体大小 (12/16/24, 留空=16): ").strip()
        if size_choice:
            try:
                command["size"] = int(size_choice)
            except ValueError:
                pass
    
    elif choice == "3":
        try:
            x = int(input("X坐标: ").strip())
            y = int(input("Y坐标: ").strip())
            width = int(input("宽度: ").strip())
            height = int(input("高度: ").strip())
            
            command = {"lcd": "rect", "x": x, "y": y, "width": width, "height": height}
            
            fill_choice = input("是否填充? (y/n): ").strip().lower()
            if fill_choice == 'y':
                command["fill"] = True
            
            color_choice = input("颜色 (留空=白色): ").strip()
            if color_choice:
                command["color"] = color_choice
                
        except ValueError:
            return "⚠️  坐标输入无效"
    
    elif choice == "4":
        try:
            x = int(input("圆心X坐标: ").strip())
            y = int(input("圆心Y坐标: ").strip())
            radius = int(input("半径: ").strip())
            
            command = {"lcd": "circle", "x": x, "y": y, "radius": radius}
            
            fill_choice = input("是否填充? (y/n): ").strip().lower()
            if fill_choice == 'y':
                command["fill"] = True
            
            color_choice = input("颜色 (留空=白色): ").strip()
            if color_choice:
                command["color"] = color_choice
                
        except ValueError:
            return "⚠️  坐标输入无效"
    
    elif choice == "5":
        try:
            x1 = int(input("起点X坐标: ").strip())
            y1 = int(input("起点Y坐标: ").strip())
            x2 = int(input("终点X坐标: ").strip())
            y2 = int(input("终点Y坐标: ").strip())
            
            command = {"lcd": "line", "x1": x1, "y1": y1, "x2": x2, "y2": y2}
            
            color_choice = input("颜色 (留空=白色): ").strip()
            if color_choice:
                command["color"] = color_choice
                
        except ValueError:
            return "⚠️  坐标输入无效"
    
    elif choice == "6":
        print("1. 开启背光")
        print("2. 关闭背光")
        backlight_choice = input("请选择 (1-2): ").strip()
        
        state = backlight_choice == "1"
        command = {"lcd": "backlight", "state": state}
    
    else:
        return "⚠️  选择无效"
    
    return controller.send_command(command)


def show_mode_info():
    """显示灯效模式说明"""
    print("\n🎨 WS2812灯效模式说明:")
    print("="*50)
    modes = [
        ("0", "OFF", "关闭", "完全关闭LED"),
        ("1", "STATIC", "静态颜色", "固定颜色显示"),
        ("2", "RAINBOW", "彩虹效果", "自动彩虹渐变"),
        ("3", "BREATHING", "呼吸灯", "正弦波亮度变化"),
        ("4", "RUNNING", "跑马灯", "带拖尾的移动光点"),
        ("5", "FLASH", "闪烁", "全亮→全灭循环"),
        ("6", "WAVE", "波浪", "波浪形光效传播"),
        ("7", "AUTO_CYCLE", "自动循环", "自动切换各种效果"),
    ]
    
    for mode_num, name, desc, detail in modes:
        print(f"{mode_num}. {name:12} - {desc:8} - {detail}")
    
    print("\n📋 参数范围:")
    print("• 通道: 0-3 (单通道) 或 255 (广播)")
    print("• 颜色: R/G/B 0-255")
    print("• 亮度: 0-255 (建议50-150)")
    print("• 速度: 1-10000ms (建议50-500)")
    print("="*50)


def quick_test(controller: ESP32Controller):
    """快速测试"""
    print("\n🚀 快速测试")
    print("正在执行快速测试序列...")
    
    test_commands = [
        ({"lcd": "clear"}, "清空LCD"),
        ({"lcd": "text", "x": 10, "y": 10, "content": "ESP32S3测试", "color": "green", "size": 16}, "显示标题"),
        ({"channel": 255, "mode": 1, "color": {"r": 255, "g": 0, "b": 0}, "brightness": 100}, "红色静态"),
        ({"channel": 255, "mode": 2, "brightness": 150, "speed": 200}, "彩虹效果"),
        ({"channel": 255, "mode": 3, "color": {"r": 0, "g": 255, "b": 0}, "brightness": 120, "speed": 300}, "绿色呼吸"),
        ({"channel": 255, "mode": 0}, "关闭灯带"),
        ({"battery": 75, "charging": False}, "设置电池75%"),
        ({"query": "battery"}, "查询电池状态"),
    ]
    
    for i, (command, desc) in enumerate(test_commands, 1):
        print(f"\n[{i}/{len(test_commands)}] {desc}")
        result = controller.send_command(command)
        print(result)
        time.sleep(1)
    
    print("\n✅ 快速测试完成!")


def main():
    """主函数"""
    controller = ESP32Controller()
    
    print("🎮 ESP32S3-TFT-BS 控制器 v1.0")
    print("支持WS2812灯带、电池管理、LCD显示控制")
    
    while True:
        try:
            print_menu()
            choice = input("\n请选择操作 (0-18): ").strip()
            
            if choice == "0":
                controller.disconnect()
                print("👋 程序已退出")
                break
            
            elif choice == "1":
                controller.connect_serial_with_scan()
            
            elif choice == "2":
                controller.connect_network_with_scan()
            
            elif choice == "3":
                controller.disconnect()
            
            elif choice == "4":
                print(ws2812_single_control(controller))
            
            elif choice == "5":
                print(ws2812_broadcast_control(controller))
            
            elif choice == "6":
                print(ws2812_channel_enable(controller))
            
            elif choice == "7":
                result = controller.send_command({"action": "status"})
                print(result)
            
            elif choice == "8":
                try:
                    channel = int(input("通道号 (0-3): ").strip())
                    duration = int(input("循环持续时间(ms, 1000-60000): ").strip())
                    command = {"channel": channel, "cycle_duration": max(1000, min(60000, duration))}
                    print(controller.send_command(command))
                except ValueError:
                    print("⚠️  输入格式错误")
            
            elif choice == "9":
                print(battery_control(controller))
            
            elif choice == "10":
                print(battery_control(controller))
            
            elif choice == "11":
                print(battery_control(controller))
            
            elif choice == "12":
                print(battery_control(controller))
            
            elif choice == "13":
                print(lcd_control(controller))
            
            elif choice == "14":
                print(lcd_control(controller))
            
            elif choice == "15":
                print(lcd_control(controller))
            
            elif choice == "16":
                print(lcd_control(controller))
            
            elif choice == "17":
                show_mode_info()
            
            elif choice == "18":
                quick_test(controller)
            
            else:
                print("⚠️  选择无效，请重新输入")
            
            # 等待用户查看结果
            if choice not in ["0", "17"]:
                input("\n按回车键继续...")
        
        except KeyboardInterrupt:
            controller.disconnect()
            print("\n\n👋 程序已中断退出")
            break
        
        except Exception as e:
            print(f"\n❌ 发生错误: {e}")
            input("按回车键继续...")


if __name__ == "__main__":
    main()
