#!/usr/bin/env python3
"""
WS2812 多通道测试脚本 - 支持无回车符JSON命令
"""

import serial
import json
import time

def test_no_newline_json():
    """测试不带回车符的JSON命令"""
    try:
        ser = serial.Serial('COM12', 115200, timeout=1)  # 修改为您的端口
        print("连接成功，开始测试...")
        time.sleep(2)
        
        # 测试命令
        test_commands = [
            '{"action": "status"}',
            '{"channel": 255, "mode": 1, "color": {"r": 255, "g": 255, "b": 255}, "brightness": 50}',
            '{"channel": 0, "mode": 1, "color": {"r": 255, "g": 0, "b": 0}}',
            '{"channel": 1, "mode": 1, "color": {"r": 0, "g": 255, "b": 0}}',
            '{"channel": 255, "mode": 0}'
        ]
        
        for cmd in test_commands:
            print(f"\n发送命令（无回车符）: {cmd}")
            # 直接发送JSON，不添加回车符
            ser.write(cmd.encode('utf-8'))
            
            # 等待响应
            time.sleep(0.5)
            response = ""
            start_time = time.time()
            while ser.in_waiting > 0 and (time.time() - start_time) < 2:
                response += ser.read(ser.in_waiting).decode('utf-8', errors='ignore')
                time.sleep(0.1)
            
            if response:
                print(f"响应: {response.strip()}")
            else:
                print("无响应")
            
            time.sleep(2)  # 观察效果
        
        print("\n测试完成")
        ser.close()
        
    except Exception as e:
        print(f"错误: {e}")

if __name__ == "__main__":
    test_no_newline_json()
