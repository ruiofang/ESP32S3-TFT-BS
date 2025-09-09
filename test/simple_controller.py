#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
ESP32S3-TFT-BS 简化控制脚本
支持命令行参数和配置文件控制
"""

import json
import serial
import serial.tools.list_ports
import socket
import time
import argparse
import configparser
import sys
import ipaddress
import threading
from typing import Dict, Any, Optional, List, Tuple


class SimpleController:
    """简化控制器"""
    
    def __init__(self, config_file: str = "config.ini"):
        self.config = configparser.ConfigParser()
        self.connection = None
        self.connection_type = None
        self.load_config(config_file)
    
    def load_config(self, config_file: str):
        """加载配置文件"""
        try:
            self.config.read(config_file, encoding='utf-8')
        except:
            # 创建默认配置
            self.create_default_config(config_file)
    
    def create_default_config(self, config_file: str):
        """创建默认配置文件"""
        self.config['CONNECTION'] = {
            'type': 'serial',  # serial 或 tcp
            'port': 'COM3',    # 串口号或IP地址
            'baudrate': '115200',  # 波特率或TCP端口
        }
        
        self.config['WS2812'] = {
            'default_brightness': '100',
            'default_speed': '200',
        }
        
        self.config['LCD'] = {
            'default_font_size': '16',
            'default_color': 'white',
        }
        
        with open(config_file, 'w', encoding='utf-8') as f:
            self.config.write(f)
        print(f"✅ 已创建默认配置文件: {config_file}")
    
    def connect(self) -> bool:
        """根据配置连接设备"""
        # 首先尝试自动扫描连接
        if self.scan_and_connect():
            return True
        
        # 如果扫描失败，尝试使用配置文件中的设置
        conn_type = self.config.get('CONNECTION', 'type', fallback='serial')
        
        if conn_type == 'serial':
            port = self.config.get('CONNECTION', 'port', fallback='COM3')
            baudrate = int(self.config.get('CONNECTION', 'baudrate', fallback='115200'))
            return self.connect_serial(port, baudrate)
        
        elif conn_type == 'tcp':
            host = self.config.get('CONNECTION', 'port', fallback='192.168.1.100')
            port = int(self.config.get('CONNECTION', 'baudrate', fallback='80'))
            return self.connect_tcp(host, port)
        
        return False
        """自动扫描并连接设备"""
        conn_type = self.config.get('CONNECTION', 'type', fallback='serial')
        
        if conn_type == 'serial':
            return self.scan_serial_and_connect()
        elif conn_type == 'tcp':
            return self.scan_network_and_connect()
        
        return False
    
    def scan_serial_and_connect(self) -> bool:
        """扫描串口并连接"""
        print("🔍 扫描串口设备...")
        try:
            ports = serial.tools.list_ports.comports()
            if not ports:
                print("❌ 未找到串口设备")
                return False
            
            # 优先选择ESP32相关设备
            esp_ports = []
            other_ports = []
            
            for port in ports:
                description = (port.description or "").lower()
                esp_keywords = ['esp32', 'esp', 'ch340', 'ch341', 'cp210', 'ftdi']
                
                if any(keyword in description for keyword in esp_keywords):
                    esp_ports.append(port)
                else:
                    other_ports.append(port)
            
            # 优先尝试ESP32相关设备
            target_ports = esp_ports + other_ports
            
            for port in target_ports:
                print(f"🔌 尝试连接: {port.device} ({port.description})")
                if self.connect_serial(port.device):
                    # 更新配置文件
                    self.config.set('CONNECTION', 'port', port.device)
                    return True
            
            print("❌ 所有串口连接失败")
            return False
            
        except Exception as e:
            print(f"❌ 扫描失败: {e}")
            return False
    
    def scan_network_and_connect(self) -> bool:
        """扫描网络并连接"""
        print("🔍 扫描网络设备...")
        
        # 获取配置的IP，如果是有效IP则直接尝试连接
        configured_host = self.config.get('CONNECTION', 'port', fallback='192.168.1.100')
        configured_port = int(self.config.get('CONNECTION', 'baudrate', fallback='80'))
        
        try:
            ipaddress.IPv4Address(configured_host)
            print(f"🔌 尝试连接配置的设备: {configured_host}:{configured_port}")
            if self.connect_tcp(configured_host, configured_port):
                return True
        except:
            pass
        
        # 快速扫描本地网段
        try:
            s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
            s.connect(("8.8.8.8", 80))
            local_ip = s.getsockname()[0]
            s.close()
            
            ip_parts = local_ip.split('.')
            network_base = '.'.join(ip_parts[:3])
            
            print(f"🔍 扫描网段: {network_base}.1-254:80")
            
            # 并发扫描常见IP
            common_ips = [
                f"{network_base}.100", f"{network_base}.101", f"{network_base}.102",
                f"{network_base}.200", f"{network_base}.201", f"{network_base}.202",
                f"{network_base}.10", f"{network_base}.20", f"{network_base}.30"
            ]
            
            for ip in common_ips:
                try:
                    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
                    sock.settimeout(0.5)
                    result = sock.connect_ex((ip, 80))
                    sock.close()
                    
                    if result == 0:
                        print(f"🔌 尝试连接: {ip}:80")
                        if self.connect_tcp(ip, 80):
                            # 更新配置文件
                            self.config.set('CONNECTION', 'port', ip)
                            return True
                except:
                    continue
            
            print("❌ 网络设备连接失败")
            return False
            
        except Exception as e:
            print(f"❌ 网络扫描失败: {e}")
            return False
    
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
        """连接TCP"""
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
        """发送命令"""
        if not self.connection:
            if not self.connect():
                return "❌ 无法连接设备"
        
        json_str = json.dumps(command, ensure_ascii=False)
        print(f"📤 发送: {json_str}")
        
        try:
            if self.connection_type == "serial":
                self.connection.write(json_str.encode('utf-8'))
                self.connection.flush()
                time.sleep(0.1)
                if self.connection.in_waiting > 0:
                    response = self.connection.read(self.connection.in_waiting).decode('utf-8', errors='ignore')
                    return f"📥 响应: {response.strip()}"
            
            elif self.connection_type == "tcp":
                self.connection.send(json_str.encode('utf-8'))
                response = self.connection.recv(1024).decode('utf-8', errors='ignore')
                return f"📥 响应: {response.strip()}"
                
        except Exception as e:
            return f"❌ 发送失败: {e}"
        
        return "✅ 命令已发送"
    
    # WS2812控制方法
    def ws2812_static(self, channel: int = 0, r: int = 255, g: int = 255, b: int = 255, brightness: int = None):
        """静态颜色"""
        brightness = brightness or int(self.config.get('WS2812', 'default_brightness'))
        command = {
            "channel": channel,
            "mode": 1,
            "color": {"r": r, "g": g, "b": b},
            "brightness": brightness
        }
        return self.send_command(command)
    
    def ws2812_rainbow(self, channel: int = 0, brightness: int = None, speed: int = None):
        """彩虹效果"""
        brightness = brightness or int(self.config.get('WS2812', 'default_brightness'))
        speed = speed or int(self.config.get('WS2812', 'default_speed'))
        command = {
            "channel": channel,
            "mode": 2,
            "brightness": brightness,
            "speed": speed
        }
        return self.send_command(command)
    
    def ws2812_breathing(self, channel: int = 0, r: int = 255, g: int = 255, b: int = 255, brightness: int = None, speed: int = None):
        """呼吸灯"""
        brightness = brightness or int(self.config.get('WS2812', 'default_brightness'))
        speed = speed or int(self.config.get('WS2812', 'default_speed'))
        command = {
            "channel": channel,
            "mode": 3,
            "color": {"r": r, "g": g, "b": b},
            "brightness": brightness,
            "speed": speed
        }
        return self.send_command(command)
    
    def ws2812_running(self, channel: int = 0, r: int = 255, g: int = 255, b: int = 255, brightness: int = None, speed: int = None):
        """跑马灯"""
        brightness = brightness or int(self.config.get('WS2812', 'default_brightness'))
        speed = speed or int(self.config.get('WS2812', 'default_speed'))
        command = {
            "channel": channel,
            "mode": 4,
            "color": {"r": r, "g": g, "b": b},
            "brightness": brightness,
            "speed": speed
        }
        return self.send_command(command)
    
    def ws2812_off(self, channel: int = 0):
        """关闭灯带"""
        command = {"channel": channel, "mode": 0}
        return self.send_command(command)
    
    # 电池控制方法
    def battery_query(self):
        """查询电池状态"""
        command = {"query": "battery"}
        return self.send_command(command)
    
    def battery_set(self, level: int, charging: bool = False):
        """设置电池状态"""
        command = {"battery": level, "charging": charging}
        return self.send_command(command)
    
    def battery_auto(self):
        """恢复自动模式"""
        command = {"auto_mode": True}
        return self.send_command(command)
    
    # LCD控制方法
    def lcd_clear(self, color: str = "black"):
        """清屏"""
        command = {"lcd": "clear", "color": color}
        return self.send_command(command)
    
    def lcd_text(self, text: str, x: int = 10, y: int = 10, color: str = None, size: int = None):
        """显示文字"""
        color = color or self.config.get('LCD', 'default_color')
        size = size or int(self.config.get('LCD', 'default_font_size'))
        command = {
            "lcd": "text",
            "x": x,
            "y": y,
            "content": text,
            "color": color,
            "size": size
        }
        return self.send_command(command)
    
    def lcd_rect(self, x: int, y: int, width: int, height: int, color: str = "white", fill: bool = False):
        """绘制矩形"""
        command = {
            "lcd": "rect",
            "x": x,
            "y": y,
            "width": width,
            "height": height,
            "color": color,
            "fill": fill
        }
        return self.send_command(command)
    
    def lcd_backlight(self, state: bool = True):
        """背光控制"""
        command = {"lcd": "backlight", "state": state}
        return self.send_command(command)
    
    def disconnect(self):
        """断开连接"""
        if self.connection:
            self.connection.close()
            self.connection = None
            self.connection_type = None


def main():
    """命令行主函数"""
    parser = argparse.ArgumentParser(description="ESP32S3-TFT-BS 简化控制器")
    parser.add_argument('--config', '-c', default='config.ini', help='配置文件路径')
    
    # WS2812控制
    ws_group = parser.add_argument_group('WS2812控制')
    ws_group.add_argument('--ws-static', nargs='+', metavar=('CHANNEL', 'R', 'G', 'B'), help='静态颜色: 通道 R G B [亮度]')
    ws_group.add_argument('--ws-rainbow', nargs='+', metavar=('CHANNEL',), help='彩虹效果: 通道 [亮度] [速度]')
    ws_group.add_argument('--ws-breathing', nargs='+', metavar=('CHANNEL', 'R', 'G', 'B'), help='呼吸灯: 通道 R G B [亮度] [速度]')
    ws_group.add_argument('--ws-running', nargs='+', metavar=('CHANNEL', 'R', 'G', 'B'), help='跑马灯: 通道 R G B [亮度] [速度]')
    ws_group.add_argument('--ws-off', type=int, metavar='CHANNEL', help='关闭灯带: 通道')
    
    # 电池控制
    bat_group = parser.add_argument_group('电池控制')
    bat_group.add_argument('--bat-query', action='store_true', help='查询电池状态')
    bat_group.add_argument('--bat-set', nargs=2, metavar=('LEVEL', 'CHARGING'), help='设置电池: 电量 充电状态(true/false)')
    bat_group.add_argument('--bat-auto', action='store_true', help='恢复自动模式')
    
    # LCD控制
    lcd_group = parser.add_argument_group('LCD控制')
    lcd_group.add_argument('--lcd-clear', nargs='?', const='black', metavar='COLOR', help='清屏: [颜色]')
    lcd_group.add_argument('--lcd-text', nargs='+', metavar=('TEXT', 'X', 'Y'), help='显示文字: 文字 X Y [颜色] [大小]')
    lcd_group.add_argument('--lcd-rect', nargs='+', metavar=('X', 'Y', 'W', 'H'), help='绘制矩形: X Y 宽 高 [颜色] [填充true/false]')
    lcd_group.add_argument('--lcd-backlight', choices=['on', 'off'], help='背光控制')
    
    args = parser.parse_args()
    
    if len(sys.argv) == 1:
        # 没有参数时显示交互式菜单
        interactive_mode()
        return
    
    controller = SimpleController(args.config)
    
    try:
        # WS2812控制
        if args.ws_static:
            params = args.ws_static
            channel, r, g, b = int(params[0]), int(params[1]), int(params[2]), int(params[3])
            brightness = int(params[4]) if len(params) > 4 else None
            print(controller.ws2812_static(channel, r, g, b, brightness))
        
        elif args.ws_rainbow:
            params = args.ws_rainbow
            channel = int(params[0])
            brightness = int(params[1]) if len(params) > 1 else None
            speed = int(params[2]) if len(params) > 2 else None
            print(controller.ws2812_rainbow(channel, brightness, speed))
        
        elif args.ws_breathing:
            params = args.ws_breathing
            channel, r, g, b = int(params[0]), int(params[1]), int(params[2]), int(params[3])
            brightness = int(params[4]) if len(params) > 4 else None
            speed = int(params[5]) if len(params) > 5 else None
            print(controller.ws2812_breathing(channel, r, g, b, brightness, speed))
        
        elif args.ws_running:
            params = args.ws_running
            channel, r, g, b = int(params[0]), int(params[1]), int(params[2]), int(params[3])
            brightness = int(params[4]) if len(params) > 4 else None
            speed = int(params[5]) if len(params) > 5 else None
            print(controller.ws2812_running(channel, r, g, b, brightness, speed))
        
        elif args.ws_off is not None:
            print(controller.ws2812_off(args.ws_off))
        
        # 电池控制
        elif args.bat_query:
            print(controller.battery_query())
        
        elif args.bat_set:
            level = int(args.bat_set[0])
            charging = args.bat_set[1].lower() == 'true'
            print(controller.battery_set(level, charging))
        
        elif args.bat_auto:
            print(controller.battery_auto())
        
        # LCD控制
        elif args.lcd_clear is not None:
            print(controller.lcd_clear(args.lcd_clear))
        
        elif args.lcd_text:
            params = args.lcd_text
            text, x, y = params[0], int(params[1]), int(params[2])
            color = params[3] if len(params) > 3 else None
            size = int(params[4]) if len(params) > 4 else None
            print(controller.lcd_text(text, x, y, color, size))
        
        elif args.lcd_rect:
            params = args.lcd_rect
            x, y, w, h = int(params[0]), int(params[1]), int(params[2]), int(params[3])
            color = params[4] if len(params) > 4 else "white"
            fill = params[5].lower() == 'true' if len(params) > 5 else False
            print(controller.lcd_rect(x, y, w, h, color, fill))
        
        elif args.lcd_backlight:
            state = args.lcd_backlight == 'on'
            print(controller.lcd_backlight(state))
        
        else:
            parser.print_help()
    
    finally:
        controller.disconnect()


def interactive_mode():
    """交互模式菜单"""
    controller = SimpleController()
    
    print("\n🎮 ESP32S3-TFT-BS 简化控制器")
    print("=" * 40)
    print("� 自动扫描设备并连接...")
    
    # 尝试自动连接
    if controller.connect():
        print("✅ 设备连接成功!")
    else:
        print("⚠️  自动连接失败，请检查设备连接")
        print("💡 提示: 可以修改 config.ini 文件中的连接设置")
    
    print("\n�📋 快捷操作:")
    print("1. 红色静态   2. 绿色静态   3. 蓝色静态")
    print("4. 彩虹效果   5. 呼吸灯     6. 跑马灯")
    print("7. 关闭灯带   8. 电池查询   9. LCD清屏")
    print("0. 退出程序")
    print("=" * 40)
    
    presets = {
        '1': lambda: controller.ws2812_static(255, 255, 0, 0, 100),      # 红色
        '2': lambda: controller.ws2812_static(255, 0, 255, 0, 100),      # 绿色
        '3': lambda: controller.ws2812_static(255, 0, 0, 255, 100),      # 蓝色
        '4': lambda: controller.ws2812_rainbow(255, 150, 200),           # 彩虹
        '5': lambda: controller.ws2812_breathing(255, 255, 255, 255, 120, 300),  # 呼吸灯
        '6': lambda: controller.ws2812_running(255, 255, 255, 0, 150, 100),      # 黄色跑马灯
        '7': lambda: controller.ws2812_off(255),                         # 关闭
        '8': lambda: controller.battery_query(),                         # 电池查询
        '9': lambda: controller.lcd_clear("black"),                      # LCD清屏
    }
    
    while True:
        try:
            choice = input("\n请选择操作 (0-9): ").strip()
            
            if choice == '0':
                controller.disconnect()
                print("👋 程序已退出")
                break
            
            elif choice in presets:
                result = presets[choice]()
                print(result)
            
            else:
                print("⚠️  选择无效")
        
        except KeyboardInterrupt:
            controller.disconnect()
            print("\n👋 程序已中断退出")
            break
        
        except Exception as e:
            print(f"❌ 错误: {e}")


if __name__ == "__main__":
    main()
