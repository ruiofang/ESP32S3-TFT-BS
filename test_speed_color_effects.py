#!/usr/bin/env python3
"""
WS2812 速度和颜色效果测试脚本
测试呼吸灯、跑马灯、闪烁效果的速度和颜色调节功能
"""

import serial
import json
import time

def send_json_command(ser, command):
    """发送JSON命令"""
    cmd_str = json.dumps(command)
    print(f"发送: {cmd_str}")
    ser.write(cmd_str.encode('utf-8'))
    
    # 等待响应
    time.sleep(0.3)
    response = ""
    while ser.in_waiting > 0:
        response += ser.read(ser.in_waiting).decode('utf-8', errors='ignore')
        time.sleep(0.1)
    
    if response:
        print(f"响应: {response.strip()}")
    return response

def test_breathing_effects(ser):
    """测试呼吸灯效果的速度和颜色控制"""
    print("\n=== 测试呼吸灯效果 ===")
    
    breathing_tests = [
        {"channel": 0, "mode": 3, "color": {"r": 255, "g": 0, "b": 0}, "brightness": 150, "speed": 100, "desc": "红色快速呼吸"},
        {"channel": 1, "mode": 3, "color": {"r": 0, "g": 255, "b": 0}, "brightness": 150, "speed": 300, "desc": "绿色中速呼吸"},
        {"channel": 2, "mode": 3, "color": {"r": 0, "g": 0, "b": 255}, "brightness": 150, "speed": 800, "desc": "蓝色慢速呼吸"},
        {"channel": 3, "mode": 3, "color": {"r": 255, "g": 255, "b": 0}, "brightness": 150, "speed": 1500, "desc": "黄色极慢呼吸"}
    ]
    
    for test in breathing_tests:
        print(f"\n设置 - {test['desc']}")
        desc = test.pop('desc')  # 移除描述字段
        send_json_command(ser, test)
        time.sleep(3)  # 观察效果

def test_running_effects(ser):
    """测试跑马灯效果的速度和颜色控制"""
    print("\n=== 测试跑马灯效果 ===")
    
    # 先关闭呼吸灯
    send_json_command(ser, {"channel": 255, "mode": 0})
    time.sleep(1)
    
    running_tests = [
        {"channel": 0, "mode": 4, "color": {"r": 255, "g": 0, "b": 0}, "brightness": 200, "speed": 50, "desc": "红色超快跑马灯"},
        {"channel": 1, "mode": 4, "color": {"r": 0, "g": 255, "b": 0}, "brightness": 200, "speed": 150, "desc": "绿色快速跑马灯"},
        {"channel": 2, "mode": 4, "color": {"r": 0, "g": 0, "b": 255}, "brightness": 200, "speed": 400, "desc": "蓝色中速跑马灯"},
        {"channel": 3, "mode": 4, "color": {"r": 255, "g": 0, "b": 255}, "brightness": 200, "speed": 800, "desc": "紫色慢速跑马灯"}
    ]
    
    for test in running_tests:
        print(f"\n设置 - {test['desc']}")
        desc = test.pop('desc')
        send_json_command(ser, test)
        time.sleep(4)  # 观察跑马效果

def test_flash_effects(ser):
    """测试闪烁效果的速度和颜色控制"""
    print("\n=== 测试闪烁效果 ===")
    
    # 先关闭跑马灯
    send_json_command(ser, {"channel": 255, "mode": 0})
    time.sleep(1)
    
    flash_tests = [
        {"channel": 0, "mode": 5, "color": {"r": 255, "g": 255, "b": 255}, "brightness": 200, "speed": 100, "desc": "白色快速闪烁"},
        {"channel": 1, "mode": 5, "color": {"r": 255, "g": 255, "b": 0}, "brightness": 200, "speed": 300, "desc": "黄色中速闪烁"},
        {"channel": 2, "mode": 5, "color": {"r": 255, "g": 0, "b": 0}, "brightness": 200, "speed": 600, "desc": "红色慢速闪烁"},
        {"channel": 3, "mode": 5, "color": {"r": 0, "g": 255, "b": 255}, "brightness": 200, "speed": 1000, "desc": "青色极慢闪烁"}
    ]
    
    for test in flash_tests:
        print(f"\n设置 - {test['desc']}")
        desc = test.pop('desc')
        send_json_command(ser, test)
        time.sleep(4)  # 观察闪烁效果

def test_combined_effects(ser):
    """测试组合效果"""
    print("\n=== 测试组合效果 ===")
    
    # 每个通道不同效果
    combined_tests = [
        {"channel": 0, "mode": 3, "color": {"r": 255, "g": 0, "b": 0}, "brightness": 150, "speed": 200},    # 红色呼吸
        {"channel": 1, "mode": 4, "color": {"r": 0, "g": 255, "b": 0}, "brightness": 180, "speed": 120},    # 绿色跑马灯
        {"channel": 2, "mode": 5, "color": {"r": 0, "g": 0, "b": 255}, "brightness": 200, "speed": 400},    # 蓝色闪烁
        {"channel": 3, "mode": 1, "color": {"r": 255, "g": 255, "b": 0}, "brightness": 100}                # 黄色静态
    ]
    
    for test in combined_tests:
        send_json_command(ser, test)
        time.sleep(0.5)
    
    print("组合效果运行中，观察10秒...")
    time.sleep(10)

def main():
    try:
        ser = serial.Serial('COM12', 115200, timeout=1)  # 修改为您的端口
        print("连接成功，开始测试WS2812速度和颜色效果...")
        time.sleep(2)
        
        # 查询初始状态
        send_json_command(ser, {"action": "status"})
        time.sleep(1)
        
        # 依次测试各种效果
        test_breathing_effects(ser)
        test_running_effects(ser)
        test_flash_effects(ser)
        test_combined_effects(ser)
        
        # 最后关闭所有LED
        print("\n关闭所有LED...")
        send_json_command(ser, {"channel": 255, "mode": 0})
        
        print("\n测试完成！")
        ser.close()
        
    except serial.SerialException as e:
        print(f"串口错误: {e}")
        print("请检查串口号和设备连接")
    except KeyboardInterrupt:
        print("\n测试被用户中断")
        if 'ser' in locals():
            # 关闭所有LED
            send_json_command(ser, {"channel": 255, "mode": 0})
            ser.close()
    except Exception as e:
        print(f"测试错误: {e}")

if __name__ == "__main__":
    main()
