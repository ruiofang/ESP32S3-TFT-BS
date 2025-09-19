#!/usr/bin/env python3
"""
ESP32 WS2812 LED控制系统测试脚本
支持可配置LED数量和电量显示功能
"""

import serial
import json
import time
import sys
from typing import Dict, Any

class ESP32Controller:
    def __init__(self, port='COM12', baudrate=115200):
        """初始化ESP32控制器"""
        self.ser = None
        self.port = port
        self.baudrate = baudrate
        
    def connect(self):
        """连接到ESP32"""
        try:
            self.ser = serial.Serial(self.port, self.baudrate, timeout=2)
            print(f"✓ 已连接到 {self.port}")
            time.sleep(2)  # 等待连接稳定
            return True
        except Exception as e:
            print(f"✗ 连接失败: {e}")
            return False
    
    def disconnect(self):
        """断开连接"""
        if self.ser and self.ser.is_open:
            self.ser.close()
            print("已断开连接")
    
    def send_json_command(self, command: Dict[str, Any]) -> bool:
        """发送JSON命令"""
        if not self.ser or not self.ser.is_open:
            print("✗ 设备未连接")
            return False
        
        try:
            json_str = json.dumps(command, separators=(',', ':'))
            self.ser.write(json_str.encode() + b'\n')
            print(f"→ 发送: {json_str}")
            
            # 读取响应
            response = self.ser.readline().decode().strip()
            if response:
                print(f"← 响应: {response}")
            
            return True
        except Exception as e:
            print(f"✗ 发送命令失败: {e}")
            return False
    
    def send_legacy_command(self, command: str) -> bool:
        """发送传统格式命令"""
        if not self.ser or not self.ser.is_open:
            print("✗ 设备未连接")
            return False
        
        try:
            self.ser.write(command.encode() + b'\n')
            print(f"→ 发送: {command}")
            
            # 读取响应
            response = self.ser.readline().decode().strip()
            if response:
                print(f"← 响应: {response}")
            
            return True
        except Exception as e:
            print(f"✗ 发送命令失败: {e}")
            return False

def test_led_count_configuration(controller):
    """测试LED数量配置"""
    print("\n=== 测试LED数量配置 ===")
    
    # 为每个通道设置不同的LED数量
    led_counts = [30, 50, 100, 150]
    
    for ch in range(4):
        command = {
            "action": "ws2812_control",
            "channel": ch,
            "led_count": led_counts[ch]
        }
        controller.send_json_command(command)
        time.sleep(0.5)
    
    # 查询设置结果
    for ch in range(4):
        command = {"action": "ws2812_get_status", "channel": ch}
        controller.send_json_command(command)
        time.sleep(0.5)

def test_battery_display(controller):
    """测试电量显示功能（兼容LCD命令格式）"""
    print("\n=== 测试电量显示功能 ===")
    
    # 设置通道0为电量显示通道（简化配置）
    command = {
        "action": "ws2812_control",
        "battery_channel": 0  # 设置电量显示通道
    }
    controller.send_json_command(command)
    time.sleep(1)
    
    # 测试不同电量等级（使用LCD兼容格式）
    battery_levels = [20, 50, 80, 100]
    
    for level in battery_levels:
        print(f"\n--- 设置电量: {level}% ---")
        command = {
            "voltage": 3.7 + (level / 100.0) * 0.5,  # 3.7V到4.2V的模拟电压
            "battery": level,
            "charging": False
        }
        controller.send_json_command(command)
        time.sleep(2)
    
    # 测试充电动画（使用LCD兼容格式）
    print("\n--- 测试充电动画 ---")
    command = {
        "voltage": 3.9,
        "battery": 60,
        "charging": True
    }
    controller.send_json_command(command)
    time.sleep(5)  # 观察充电动画
    
    # 停止充电
    command = {
        "voltage": 3.9,
        "battery": 60,
        "charging": False
    }
    controller.send_json_command(command)
    time.sleep(1)

def test_mixed_configuration(controller):
    """测试混合配置"""
    print("\n=== 测试混合配置 ===")
    
    # 设置通道0为电量显示，其他通道为正常模式
    # 通道0: 电量显示，60个LED
    command = {
        "action": "ws2812_control",
        "channel": 0,
        "led_count": 60,
        "battery_channel": 0  # 设置电量显示通道
    }
    controller.send_json_command(command)
    time.sleep(0.5)
    
    # 设置电量状态（LCD兼容格式）
    command = {
        "voltage": 4.0,
        "battery": 75,
        "charging": False
    }
    controller.send_json_command(command)
    time.sleep(1)
    
    # 通道1: 呼吸灯模式，40个LED
    command = {
        "action": "ws2812_control",
        "channel": 1,
        "led_count": 40,
        "mode": 1,  # 呼吸模式
        "color": {"r": 255, "g": 0, "b": 0},
        "brightness": 150
    }
    controller.send_json_command(command)
    time.sleep(1)
    
    # 通道2: 流水灯模式，80个LED
    command = {
        "action": "ws2812_control",
        "channel": 2,
        "led_count": 80,
        "mode": 2,  # 流水模式
        "color": {"r": 0, "g": 255, "b": 0},
        "speed": 200
    }
    controller.send_json_command(command)
    time.sleep(1)
    
    # 通道3: 彩虹模式，120个LED
    command = {
        "action": "ws2812_control",
        "channel": 3,
        "led_count": 120,
        "mode": 3,  # 彩虹模式
        "speed": 100
    }
    controller.send_json_command(command)
    time.sleep(3)

def test_legacy_compatibility(controller):
    """测试传统命令兼容性"""
    print("\n=== 测试传统命令兼容性 ===")
    
    # 测试传统格式的LED控制命令
    legacy_commands = [
        "LED:0:1:255,0,0:150:200",  # 通道0，开启，红色，亮度150，速度200
        "LED:1:1:0,255,0:180:150",  # 通道1，开启，绿色，亮度180，速度150
        "LED:2:0",                 # 通道2，关闭
        "LED:STATUS"               # 查询状态
    ]
    
    for cmd in legacy_commands:
        controller.send_legacy_command(cmd)
        time.sleep(1)

def test_lcd_compatible_battery(controller):
    """测试LCD兼容的电量命令"""
    print("\n=== 测试LCD兼容电量命令 ===")
    
    # 先设置电量显示通道
    command = {
        "action": "ws2812_control", 
        "battery_channel": 0
    }
    controller.send_json_command(command)
    time.sleep(1)
    
    print("\n--- 测试不同电量等级 ---")
    # 测试不同电量等级
    test_cases = [
        {"voltage": 3.7, "battery": 20, "charging": False},
        {"voltage": 3.8, "battery": 50, "charging": False},
        {"voltage": 4.0, "battery": 80, "charging": False}, 
        {"voltage": 4.2, "battery": 100, "charging": False}
    ]
    
    for case in test_cases:
        print(f"设置: {case['battery']}%, {case['voltage']}V, 充电: {case['charging']}")
        controller.send_json_command(case)
        time.sleep(2)
    
    print("\n--- 测试充电状态 ---")
    # 测试充电状态
    command = {"voltage": 3.9, "battery": 60, "charging": True}
    controller.send_json_command(command)
    time.sleep(3)
    
    # 停止充电
    command = {"voltage": 3.9, "battery": 60, "charging": False}
    controller.send_json_command(command)
    time.sleep(1)

def test_voltage_control(controller):
    """测试电压控制功能"""
    print("\n=== 测试电压控制功能 ===")
    
    # 设置不同的电压值
    voltages = [20.0, 22.5, 25.0, 27.5]
    
    for voltage in voltages:
        command = {
            "voltage": voltage
        }
        controller.send_json_command(command)
        time.sleep(1)
    
    # 恢复自动电压模式
    command = {"auto_voltage": True}
    controller.send_json_command(command)
    time.sleep(1)

def test_config_persistence(controller):
    """测试配置持久化"""
    print("\n=== 测试配置持久化 ===")
    
    # 设置一个特定配置
    command = {
        "action": "ws2812_control",
        "channel": 0,
        "led_count": 88,
        "mode": "solid",
        "color": {"r": 128, "g": 64, "b": 192},
        "brightness": 200
    }
    controller.send_json_command(command)
    time.sleep(1)
    
    # 保存配置
    command = {"action": "ws2812_save_config"}
    controller.send_json_command(command)
    time.sleep(1)
    
    print("配置已保存。请重启设备后验证配置是否保持。")

def main():
    print("ESP32 WS2812 LED控制系统测试")
    print("=" * 50)
    
    # 创建控制器实例
    controller = ESP32Controller()
    
    if not controller.connect():
        return
    
    try:
        while True:
            print("\n可用测试选项:")
            print("1. LED数量配置测试")
            print("2. 电量显示功能测试")
            print("3. 混合配置测试")
            print("4. LCD兼容电量命令测试")
            print("5. 传统命令兼容性测试")
            print("6. 电压控制测试")
            print("7. 配置持久化测试")
            print("8. 全部测试")
            print("0. 退出")
            
            choice = input("\n请选择测试项目 (0-8): ").strip()
            
            if choice == '0':
                break
            elif choice == '1':
                test_led_count_configuration(controller)
            elif choice == '2':
                test_battery_display(controller)
            elif choice == '3':
                test_mixed_configuration(controller)
            elif choice == '4':
                test_lcd_compatible_battery(controller)
            elif choice == '5':
                test_legacy_compatibility(controller)
            elif choice == '6':
                test_voltage_control(controller)
            elif choice == '7':
                test_config_persistence(controller)
            elif choice == '8':
                test_led_count_configuration(controller)
                test_battery_display(controller)
                test_mixed_configuration(controller)
                test_lcd_compatible_battery(controller)
                test_legacy_compatibility(controller)
                test_voltage_control(controller)
                test_config_persistence(controller)
            else:
                print("无效选择，请重新输入")
    
    except KeyboardInterrupt:
        print("\n\n测试被用户中断")
    
    finally:
        controller.disconnect()

if __name__ == "__main__":
    main()