#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
ESP32S3-TFT-BS 设备扫描工具
自动扫描和检测可用的串口和网络设备
"""

import serial
import serial.tools.list_ports
import socket
import time
import ipaddress
import threading
from typing import List, Tuple
import sys
import subprocess


class DeviceScanner:

    """设备扫描器"""
    
    def __init__(self):
        self.scan_results = {
            'serial': [],
            'network': []
        }
    
    def scan_serial_ports(self, detailed: bool = True) -> List[Tuple[str, str, dict]]:
        """扫描串口设备"""
        print("🔍 扫描串口设备...")
        available_ports = []
        
        try:
            ports = serial.tools.list_ports.comports()
            
            for port in ports:
                device_info = {
                    'port': port.device,
                    'description': port.description or "未知设备",
                    'hwid': getattr(port, 'hwid', ''),
                    'vid': getattr(port, 'vid', None),
                    'pid': getattr(port, 'pid', None),
                    'manufacturer': getattr(port, 'manufacturer', ''),
                    'product': getattr(port, 'product', ''),
                    'is_esp_likely': False
                }
                
                # 检测ESP32相关设备
                esp_keywords = ['esp32', 'esp', 'ch340', 'ch341', 'cp210', 'cp2102', 'ftdi', 'silicon labs']
                description_lower = device_info['description'].lower()
                manufacturer_lower = (device_info['manufacturer'] or '').lower()
                product_lower = (device_info['product'] or '').lower()
                
                if any(keyword in description_lower for keyword in esp_keywords) or \
                   any(keyword in manufacturer_lower for keyword in esp_keywords) or \
                   any(keyword in product_lower for keyword in esp_keywords):
                    device_info['is_esp_likely'] = True
                
                # 测试串口连通性
                if detailed:
                    device_info['connectable'] = self.test_serial_connection(port.device)
                
                available_ports.append((port.device, device_info['description'], device_info))
            
            # 按ESP32可能性排序
            available_ports.sort(key=lambda x: x[2]['is_esp_likely'], reverse=True)
            
        except Exception as e:
            print(f"❌ 串口扫描失败: {e}")
        
        self.scan_results['serial'] = available_ports
        return available_ports
    
    def test_serial_connection(self, port: str, timeout: float = 1.0) -> bool:
        """测试串口连接"""
        try:
            ser = serial.Serial(port, 115200, timeout=timeout)
            ser.close()
            return True
        except:
            return False
    
    def scan_network_range(self, network: str, port: int = 80, timeout: float = 0.5) -> List[str]:
        """扫描网络范围"""
        print(f"🔍 扫描网络 {network}:{port}...")
        
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
        
        try:
            network_obj = ipaddress.IPv4Network(network, strict=False)
            threads = []
            
            for ip in network_obj.hosts():
                ip_str = str(ip)
                thread = threading.Thread(target=scan_ip, args=(ip_str,))
                threads.append(thread)
                thread.start()
                
                # 限制并发线程数
                if len(threads) >= 50:
                    for t in threads:
                        t.join()
                    threads = []
            
            # 等待剩余线程
            for thread in threads:
                thread.join()
            
            available_devices = sorted(scan_results, key=lambda x: ipaddress.IPv4Address(x))
            
        except Exception as e:
            print(f"❌ 网络扫描失败: {e}")
        
        return available_devices
    
    def auto_detect_network(self) -> List[str]:
        """自动检测本地网络并扫描"""
        print("🔍 自动检测本地网络...")
        
        try:
            # 获取本机IP
            s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
            s.connect(("8.8.8.8", 80))
            local_ip = s.getsockname()[0]
            s.close()
            
            print(f"📍 本机IP: {local_ip}")
            
            # 确定网络范围
            ip_obj = ipaddress.IPv4Address(local_ip)
            
            # 扫描 /24 网络
            network = f"{'.'.join(str(ip_obj).split('.')[:-1])}.0/24"
            print(f"🔍 扫描网络: {network}")
            
            devices = self.scan_network_range(network, port=80, timeout=0.3)
            self.scan_results['network'] = devices
            
            return devices
            
        except Exception as e:
            print(f"❌ 自动网络检测失败: {e}")
            return []
    
    def scan_common_networks(self) -> List[str]:
        """扫描常见网络段"""
        print("🔍 扫描常见网络段...")
        
        common_networks = [
            "192.168.1.0/24",
            "192.168.0.0/24", 
            "192.168.100.0/24",
            "10.0.0.0/24",
            "172.16.0.0/24"
        ]
        
        all_devices = []
        
        for network in common_networks:
            print(f"   扫描: {network}")
            devices = self.scan_network_range(network, port=80, timeout=0.2)
            all_devices.extend(devices)
        
        # 去重并排序
        unique_devices = list(set(all_devices))
        unique_devices.sort(key=lambda x: ipaddress.IPv4Address(x))
        
        self.scan_results['network'] = unique_devices
        return unique_devices
    
    def test_esp32_device(self, connection_type: str, address: str, port_or_baudrate: int = None) -> dict:
        """测试设备是否为ESP32S3-TFT-BS"""
        result = {
            'is_esp32': False,
            'responds_to_json': False,
            'battery_command_works': False,
            'error': None
        }
        
        try:
            if connection_type == 'serial':
                baudrate = port_or_baudrate or 115200
                ser = serial.Serial(address, baudrate, timeout=2)
                
                # 发送测试命令
                test_command = '{"query": "battery"}'
                ser.write(test_command.encode('utf-8'))
                ser.flush()
                time.sleep(0.5)
                
                if ser.in_waiting > 0:
                    response = ser.read(ser.in_waiting).decode('utf-8', errors='ignore')
                    result['responds_to_json'] = True
                    
                    # 检查是否包含电池相关信息
                    if any(keyword in response.lower() for keyword in ['battery', 'voltage', 'charging']):
                        result['battery_command_works'] = True
                        result['is_esp32'] = True
                
                ser.close()
            
            elif connection_type == 'tcp':
                port = port_or_baudrate or 80
                sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
                sock.connect((address, port))
                sock.settimeout(3)
                
                # 发送测试命令
                test_command = '{"query": "battery"}'
                sock.send(test_command.encode('utf-8'))
                time.sleep(0.5)
                
                try:
                    response = sock.recv(1024).decode('utf-8', errors='ignore')
                    result['responds_to_json'] = True
                    
                    if any(keyword in response.lower() for keyword in ['battery', 'voltage', 'charging']):
                        result['battery_command_works'] = True
                        result['is_esp32'] = True
                except socket.timeout:
                    pass
                
                sock.close()
        
        except Exception as e:
            result['error'] = str(e)
        
        return result
    
    def full_scan(self, test_devices: bool = True) -> dict:
        """完整扫描"""
        print("🚀 开始完整设备扫描...")
        print("=" * 50)
        
        # 扫描串口
        print("\n📱 串口设备扫描:")
        serial_ports = self.scan_serial_ports(detailed=True)
        
        if serial_ports:
            print(f"✅ 找到 {len(serial_ports)} 个串口设备:")
            for port, desc, info in serial_ports:
                status = "⭐ ESP32可能" if info['is_esp_likely'] else "📱 其他设备"
                connectable = "✅ 可连接" if info.get('connectable', False) else "❌ 无法连接"
                print(f"   {port} - {desc} [{status}] [{connectable}]")
                
                if test_devices and info.get('connectable', False):
                    print(f"      🧪 测试ESP32功能...")
                    test_result = self.test_esp32_device('serial', port, 115200)
                    if test_result['is_esp32']:
                        print(f"      ✅ 确认为ESP32S3-TFT-BS设备!")
                    elif test_result['responds_to_json']:
                        print(f"      ⚠️  设备响应JSON但不是ESP32S3-TFT-BS")
                    else:
                        print(f"      ❌ 不是ESP32S3-TFT-BS设备")
        else:
            print("❌ 未找到串口设备")
        
        # 扫描网络
        print(f"\n🌐 网络设备扫描:")
        network_devices = self.auto_detect_network()
        
        if not network_devices:
            print("   本地网络未找到设备，尝试扫描常见网段...")
            network_devices = self.scan_common_networks()
        
        if network_devices:
            print(f"✅ 找到 {len(network_devices)} 个网络设备:")
            for device in network_devices:
                print(f"   {device}:80")
                
                if test_devices:
                    print(f"      🧪 测试ESP32功能...")
                    test_result = self.test_esp32_device('tcp', device, 80)
                    if test_result['is_esp32']:
                        print(f"      ✅ 确认为ESP32S3-TFT-BS设备!")
                    elif test_result['responds_to_json']:
                        print(f"      ⚠️  设备响应但不是ESP32S3-TFT-BS")
                    elif test_result['error']:
                        print(f"      ❌ 测试失败: {test_result['error']}")
                    else:
                        print(f"      ❌ 不是ESP32S3-TFT-BS设备")
        else:
            print("❌ 未找到网络设备")
        
        print("\n" + "=" * 50)
        print("🏁 扫描完成!")
        
        return self.scan_results


def main():
    """主函数"""
    scanner = DeviceScanner()
    
    print("🔍 ESP32S3-TFT-BS 设备扫描工具")
    print("=" * 50)
    print("选择扫描模式:")
    print("1. 快速扫描 (仅检测设备)")
    print("2. 完整扫描 (包含ESP32功能测试)")
    print("3. 仅扫描串口")
    print("4. 仅扫描网络")
    print("0. 退出")
    
    choice = input("\n请选择 (0-4): ").strip()
    
    if choice == "0":
        print("👋 退出扫描")
        return
    
    elif choice == "1":
        print("\n🚀 执行快速扫描...")
        scanner.scan_serial_ports(detailed=False)
        scanner.auto_detect_network()
        
        serial_count = len(scanner.scan_results['serial'])
        network_count = len(scanner.scan_results['network'])
        print(f"\n📊 扫描结果: 串口设备 {serial_count} 个, 网络设备 {network_count} 个")
    
    elif choice == "2":
        scanner.full_scan(test_devices=True)
    
    elif choice == "3":
        print("\n📱 扫描串口设备...")
        ports = scanner.scan_serial_ports(detailed=True)
        
        if ports:
            print(f"\n✅ 找到 {len(ports)} 个串口设备:")
            for port, desc, info in ports:
                status = "⭐ ESP32可能" if info['is_esp_likely'] else "📱 其他设备"
                print(f"   {port} - {desc} [{status}]")
    
    elif choice == "4":
        print("\n🌐 扫描网络设备...")
        devices = scanner.auto_detect_network()
        
        if devices:
            print(f"\n✅ 找到 {len(devices)} 个网络设备:")
            for device in devices:
                print(f"   {device}:80")
    
    else:
        print("⚠️  无效选择")


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        print("\n\n⏹️  扫描已中断")
    except Exception as e:
        print(f"\n❌ 程序错误: {e}")
