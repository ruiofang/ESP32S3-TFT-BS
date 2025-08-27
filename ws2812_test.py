#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
ESP32-S3 WS2812 LED控制测试脚本
通过串口发送命令控制WS2812 LED灯条

使用方法：
1. 安装依赖: pip install pyserial
2. 修改COM口: 将COM_PORT改为你的ESP32-S3对应的串口
3. 运行脚本: python ws2812_test.py

作者: GitHub Copilot
日期: 2025年8月27日
"""

import serial
import time
import sys

# 配置参数
COM_PORT = "COM3"  # 修改为你的ESP32-S3串口
BAUD_RATE = 115200
TIMEOUT = 1

def send_command(ser, command):
    """发送命令并接收响应"""
    try:
        print(f"发送命令: {command}")
        ser.write((command + "\r\n").encode())
        time.sleep(0.1)
        
        response = ""
        while ser.in_waiting > 0:
            response += ser.read(ser.in_waiting).decode(errors='ignore')
            time.sleep(0.1)
        
        if response.strip():
            print(f"响应: {response.strip()}")
        else:
            print("无响应")
        print("-" * 40)
        return response
    except Exception as e:
        print(f"发送命令失败: {e}")
        return ""

def test_ws2812_basic(ser):
    """基本功能测试"""
    print("=== 基本功能测试 ===")
    
    # 获取帮助信息
    send_command(ser, "WS2812:HELP")
    time.sleep(1)
    
    # 查询初始状态
    send_command(ser, "WS2812:STATUS")
    time.sleep(1)
    
    # 测试静态颜色模式
    print("测试静态颜色模式...")
    send_command(ser, "WS2812:MODE:1")  # 静态模式
    send_command(ser, "WS2812:COLOR:255,0,0")  # 红色
    send_command(ser, "WS2812:BRIGHTNESS:100")  # 中等亮度
    time.sleep(3)
    
    # 更改颜色为绿色
    send_command(ser, "WS2812:COLOR:0,255,0")  # 绿色
    time.sleep(2)
    
    # 更改颜色为蓝色
    send_command(ser, "WS2812:COLOR:0,0,255")  # 蓝色
    time.sleep(2)

def test_ws2812_effects(ser):
    """灯光效果测试"""
    print("=== 灯光效果测试 ===")
    
    # 自动循环效果
    print("测试自动循环效果...")
    send_command(ser, "WS2812:MODE:7")  # 自动循环模式
    send_command(ser, "WS2812:CYCLE_DURATION:3000")  # 快速切换，每个效果3秒
    send_command(ser, "WS2812:BRIGHTNESS:150")
    time.sleep(15)  # 观察15秒，可以看到5个效果的切换
    
    # 彩虹效果
    print("测试彩虹效果...")
    send_command(ser, "WS2812:MODE:2")  # 彩虹模式
    send_command(ser, "WS2812:SPEED:100")  # 快速
    send_command(ser, "WS2812:BRIGHTNESS:150")
    time.sleep(5)
    
    # 呼吸灯效果
    print("测试呼吸灯效果...")
    send_command(ser, "WS2812:MODE:3")  # 呼吸灯模式
    send_command(ser, "WS2812:COLOR:255,100,50")  # 橙色
    send_command(ser, "WS2812:SPEED:500")  # 中速
    time.sleep(5)
    
    # 跑马灯效果
    print("测试跑马灯效果...")
    send_command(ser, "WS2812:MODE:4")  # 跑马灯模式
    send_command(ser, "WS2812:COLOR:255,255,255")  # 白色
    send_command(ser, "WS2812:SPEED:200")  # 较快
    time.sleep(5)
    
    # 闪烁效果
    print("测试闪烁效果...")
    send_command(ser, "WS2812:MODE:5")  # 闪烁模式
    send_command(ser, "WS2812:COLOR:255,0,255")  # 紫色
    send_command(ser, "WS2812:SPEED:300")  # 中等速度
    time.sleep(5)
    
    # 波浪效果
    print("测试波浪效果...")
    send_command(ser, "WS2812:MODE:6")  # 波浪模式
    send_command(ser, "WS2812:COLOR:0,255,255")  # 青色
    send_command(ser, "WS2812:SPEED:150")  # 快速
    time.sleep(5)

def test_brightness_levels(ser):
    """亮度测试"""
    print("=== 亮度测试 ===")
    
    send_command(ser, "WS2812:MODE:1")  # 静态模式
    send_command(ser, "WS2812:COLOR:255,255,255")  # 白色
    
    brightness_levels = [50, 100, 150, 200, 255]
    for brightness in brightness_levels:
        print(f"测试亮度: {brightness}")
        send_command(ser, f"WS2812:BRIGHTNESS:{brightness}")
        time.sleep(2)

def test_speed_variations(ser):
    """速度测试"""
    print("=== 速度测试 ===")
    
    send_command(ser, "WS2812:MODE:2")  # 彩虹模式
    send_command(ser, "WS2812:BRIGHTNESS:200")
    
    speeds = [50, 150, 300, 600, 1000]
    for speed in speeds:
        print(f"测试速度: {speed}ms")
        send_command(ser, f"WS2812:SPEED:{speed}")
        time.sleep(4)

def test_auto_cycle(ser):
    """自动循环模式测试"""
    print("=== 自动循环模式测试 ===")
    
    print("设置自动循环模式...")
    send_command(ser, "WS2812:MODE:7")  # 自动循环模式
    send_command(ser, "WS2812:BRIGHTNESS:200")  # 较高亮度
    
    # 测试不同的循环持续时间
    durations = [3000, 5000, 8000]  # 3秒、5秒、8秒
    
    for duration in durations:
        print(f"设置循环持续时间为 {duration}ms...")
        send_command(ser, f"WS2812:CYCLE_DURATION:{duration}")
        print(f"观察 {duration * 5 // 1000} 秒，可以看到效果自动切换...")
        time.sleep(duration * 5 // 1000)  # 观察5个效果周期
    
    print("自动循环测试完成")

def interactive_mode(ser):
    """交互模式"""
    print("=== 交互模式 ===")
    print("输入命令来控制WS2812 LED，输入'quit'退出")
    print("提示：命令不需要WS2812:前缀，程序会自动添加")
    print("例如：输入 'MODE:1' 等效于 'WS2812:MODE:1'")
    print("特殊命令：'battery' 查询电池状态")
    
    while True:
        try:
            user_input = input("\n输入命令> ").strip()
            
            if user_input.lower() == 'quit':
                break
            elif user_input.lower() == 'battery':
                send_command(ser, "BATTERY")
            elif user_input:
                if not user_input.startswith("WS2812:"):
                    user_input = "WS2812:" + user_input
                send_command(ser, user_input)
        except KeyboardInterrupt:
            print("\n退出交互模式...")
            break
        except Exception as e:
            print(f"错误: {e}")

def main():
    """主函数"""
    print("ESP32-S3 WS2812 LED控制测试脚本")
    print("=" * 50)
    
    try:
        # 打开串口
        print(f"正在连接串口 {COM_PORT}...")
        ser = serial.Serial(COM_PORT, BAUD_RATE, timeout=TIMEOUT)
        time.sleep(2)  # 等待ESP32-S3启动
        print("串口连接成功！")
        
        # 清空缓冲区
        ser.flushInput()
        ser.flushOutput()
        
        while True:
            print("\n请选择测试模式：")
            print("1. 基本功能测试")
            print("2. 灯光效果测试")
            print("3. 自动循环模式测试")
            print("4. 亮度测试")
            print("5. 速度测试")
            print("6. 交互模式")
            print("7. 关闭LED并退出")
            
            choice = input("请输入选择 (1-7): ").strip()
            
            if choice == '1':
                test_ws2812_basic(ser)
            elif choice == '2':
                test_ws2812_effects(ser)
            elif choice == '3':
                test_auto_cycle(ser)
            elif choice == '4':
                test_brightness_levels(ser)
            elif choice == '5':
                test_speed_variations(ser)
            elif choice == '6':
                interactive_mode(ser)
            elif choice == '7':
                print("关闭所有LED...")
                send_command(ser, "WS2812:MODE:0")
                break
            else:
                print("无效选择，请重新输入")
    
    except serial.SerialException as e:
        print(f"串口错误: {e}")
        print(f"请检查：")
        print(f"1. ESP32-S3是否正确连接到 {COM_PORT}")
        print(f"2. 串口是否被其他程序占用")
        print(f"3. ESP32-S3是否正常工作")
    except KeyboardInterrupt:
        print("\n程序被用户中断")
    except Exception as e:
        print(f"未知错误: {e}")
    finally:
        try:
            ser.close()
            print("串口已关闭")
        except:
            pass

if __name__ == "__main__":
    main()
