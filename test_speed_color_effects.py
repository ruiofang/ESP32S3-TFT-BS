#!/usr/bin/env python3
"""
WS2812 速度和颜色效果测试脚本
测试呼吸灯、跑马灯、闪烁效果的速度和颜色调节功能
"""


import serial
import json
import time
import argparse

# 默认配置
DEFAULT_PORT = "COM12"  # 修改为你的实际端口
DEFAULT_BAUDRATE = 115200
DEFAULT_TIMEOUT = 1.0
DEFAULT_CHANNEL_ID = 255  # 默认广播到所有通道
DEFAULT_BRIGHTNESS = 255  # 默认最大亮度

def test_breathing_effects(ser, channel_id=DEFAULT_CHANNEL_ID, brightness=DEFAULT_BRIGHTNESS):
    """测试呼吸灯效果的速度和颜色控制"""
    print(f"\n=== 测试呼吸灯效果 (通道ID: {channel_id}, 亮度: {brightness}) ===")
    breathing_tests = [
        {"channel": channel_id, "mode": 3, "color": {"r": 255, "g": 0, "b": 0}, "brightness": brightness, "speed": 100, "desc": "红色快速呼吸"},
        {"channel": channel_id, "mode": 3, "color": {"r": 0, "g": 255, "b": 0}, "brightness": brightness, "speed": 300, "desc": "绿色中速呼吸"},
        {"channel": channel_id, "mode": 3, "color": {"r": 0, "g": 0, "b": 255}, "brightness": brightness, "speed": 800, "desc": "蓝色慢速呼吸"},
        {"channel": channel_id, "mode": 3, "color": {"r": 255, "g": 255, "b": 0}, "brightness": brightness, "speed": 1500, "desc": "黄色极慢呼吸"}
    ]
    for test in breathing_tests:
        print(f"\n设置 - {test['desc']}")
        desc = test.pop('desc')
        send_json_command(ser, test)
        time.sleep(3)

def input_int(prompt, default=None, minv=None, maxv=None):
    while True:
        s = input(f"{prompt} [{default}]: ")
        if not s and default is not None:
            return default
        try:
            v = int(s)
            if (minv is not None and v < minv) or (maxv is not None and v > maxv):
                print(f"请输入范围 {minv}~{maxv}")
                continue
            return v
        except ValueError:
            print("请输入整数！")

def input_color(default):
    print(f"输入颜色RGB值 (0~255)，默认: {default}")
    r = input_int("  R", default['r'], 0, 255)
    g = input_int("  G", default['g'], 0, 255)
    b = input_int("  B", default['b'], 0, 255)
    return {"r": r, "g": g, "b": b}

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

def custom_effect(ser):
    print("\n=== 自定义效果参数 ===")
    channel = input_int("通道号(channel)", 0, 0, 255)
    print("可用模式:")
    print("  1=静态, 2=彩虹, 3=呼吸, 4=跑马, 5=闪烁, 6=波浪, 7=自动循环")
    mode = input_int("模式(mode)", 3, 0, 7)
    
    color = None
    if mode in [1, 3, 4, 5]:  # 静态、呼吸、跑马、闪烁需要颜色设置
        color = input_color({"r": 255, "g": 255, "b": 255})
    
    brightness = input_int("亮度(brightness, 0~255)", 255, 0, 255)
    
    speed = None
    if mode in [2, 3, 4, 5, 6, 7]:  # 彩虹、呼吸、跑马、闪烁、波浪、自动循环需要速度设置
        speed = input_int("速度(speed, ms, 越小越快)", 300, 10, 5000)
    
    cmd = {"channel": channel, "mode": mode, "brightness": brightness}
    if color is not None:
        cmd["color"] = color
    if speed is not None:
        cmd["speed"] = speed
        
    send_json_command(ser, cmd)
    print("已发送自定义设置。\n")

def test_running_effects(ser, channel_id=DEFAULT_CHANNEL_ID, brightness=DEFAULT_BRIGHTNESS):
    """测试跑马灯效果的速度和颜色控制"""
    print(f"\n=== 测试跑马灯效果 (通道ID: {channel_id}, 亮度: {brightness}) ===")
    
    # 先关闭呼吸灯
    send_json_command(ser, {"channel": 255, "mode": 0})
    time.sleep(1)
    
    running_tests = [
        {"channel": channel_id, "mode": 4, "color": {"r": 255, "g": 0, "b": 0}, "brightness": brightness, "speed": 50, "desc": "红色超快跑马灯"},
        {"channel": channel_id, "mode": 4, "color": {"r": 0, "g": 255, "b": 0}, "brightness": brightness, "speed": 150, "desc": "绿色快速跑马灯"},
        {"channel": channel_id, "mode": 4, "color": {"r": 0, "g": 0, "b": 255}, "brightness": brightness, "speed": 400, "desc": "蓝色中速跑马灯"},
        {"channel": channel_id, "mode": 4, "color": {"r": 255, "g": 0, "b": 255}, "brightness": brightness, "speed": 800, "desc": "紫色慢速跑马灯"}
    ]
    
    for test in running_tests:
        print(f"\n设置 - {test['desc']}")
        desc = test.pop('desc')
        send_json_command(ser, test)
        time.sleep(4)  # 观察跑马效果

def test_flash_effects(ser, channel_id=DEFAULT_CHANNEL_ID, brightness=DEFAULT_BRIGHTNESS):
    """测试闪烁效果的速度和颜色控制"""
    print(f"\n=== 测试闪烁效果 (通道ID: {channel_id}, 亮度: {brightness}) ===")
    
    # 先关闭跑马灯
    send_json_command(ser, {"channel": 255, "mode": 0})
    time.sleep(1)
    
    flash_tests = [
        {"channel": channel_id, "mode": 5, "color": {"r": 255, "g": 255, "b": 255}, "brightness": brightness, "speed": 100, "desc": "白色快速闪烁"},
        {"channel": channel_id, "mode": 5, "color": {"r": 255, "g": 255, "b": 0}, "brightness": brightness, "speed": 300, "desc": "黄色中速闪烁"},
        {"channel": channel_id, "mode": 5, "color": {"r": 255, "g": 0, "b": 0}, "brightness": brightness, "speed": 600, "desc": "红色慢速闪烁"},
        {"channel": channel_id, "mode": 5, "color": {"r": 0, "g": 255, "b": 255}, "brightness": brightness, "speed": 1000, "desc": "青色极慢闪烁"}
    ]
    
    for test in flash_tests:
        print(f"\n设置 - {test['desc']}")
        desc = test.pop('desc')
        send_json_command(ser, test)
        time.sleep(4)  # 观察闪烁效果

def test_rainbow_effects(ser, channel_id=DEFAULT_CHANNEL_ID, brightness=DEFAULT_BRIGHTNESS):
    """测试彩虹效果的速度控制"""
    print(f"\n=== 测试彩虹效果 (通道ID: {channel_id}, 亮度: {brightness}) ===")
    
    # 先关闭其他效果
    send_json_command(ser, {"channel": 255, "mode": 0})
    time.sleep(1)
    
    rainbow_tests = [
        {"channel": channel_id, "mode": 2, "brightness": brightness, "speed": 50, "desc": "超快彩虹"},
        {"channel": channel_id, "mode": 2, "brightness": brightness, "speed": 150, "desc": "快速彩虹"},
        {"channel": channel_id, "mode": 2, "brightness": brightness, "speed": 400, "desc": "中速彩虹"},
        {"channel": channel_id, "mode": 2, "brightness": brightness, "speed": 800, "desc": "慢速彩虹"}
    ]
    
    for test in rainbow_tests:
        print(f"\n设置 - {test['desc']}")
        desc = test.pop('desc')
        send_json_command(ser, test)
        time.sleep(5)  # 观察彩虹效果

def test_wave_effects(ser, channel_id=DEFAULT_CHANNEL_ID, brightness=DEFAULT_BRIGHTNESS):
    """测试波浪效果的速度控制"""
    print(f"\n=== 测试波浪效果 (通道ID: {channel_id}, 亮度: {brightness}) ===")
    
    # 先关闭彩虹效果
    send_json_command(ser, {"channel": 255, "mode": 0})
    time.sleep(1)
    
    wave_tests = [
        {"channel": channel_id, "mode": 6, "brightness": brightness, "speed": 100, "desc": "快速波浪"},
        {"channel": channel_id, "mode": 6, "brightness": brightness, "speed": 300, "desc": "中速波浪"},
        {"channel": channel_id, "mode": 6, "brightness": brightness, "speed": 600, "desc": "慢速波浪"},
        {"channel": channel_id, "mode": 6, "brightness": brightness, "speed": 1000, "desc": "极慢波浪"}
    ]
    
    for test in wave_tests:
        print(f"\n设置 - {test['desc']}")
        desc = test.pop('desc')
        send_json_command(ser, test)
        time.sleep(5)  # 观察波浪效果

def test_combined_effects(ser, brightness=DEFAULT_BRIGHTNESS):
    """测试组合效果"""
    print(f"\n=== 测试组合效果 (亮度: {brightness}) ===")
    
    # 每个通道不同效果
    combined_tests = [
        {"channel": 0, "mode": 3, "color": {"r": 255, "g": 0, "b": 0}, "brightness": brightness, "speed": 200},    # 红色呼吸
        {"channel": 1, "mode": 4, "color": {"r": 0, "g": 255, "b": 0}, "brightness": brightness, "speed": 120},    # 绿色跑马灯
        {"channel": 2, "mode": 5, "color": {"r": 0, "g": 0, "b": 255}, "brightness": brightness, "speed": 400},    # 蓝色闪烁
        {"channel": 3, "mode": 1, "color": {"r": 255, "g": 255, "b": 0}, "brightness": brightness}                # 黄色静态
    ]
    
    for test in combined_tests:
        send_json_command(ser, test)
        time.sleep(0.5)
    
    print("组合效果运行中，观察10秒...")
    time.sleep(10)

def get_test_parameters():
    """获取用户自定义的测试参数"""
    print("\n=== 测试参数设置 ===")
    print(f"当前默认通道ID: {DEFAULT_CHANNEL_ID} (255=广播到所有通道)")
    print(f"当前默认亮度: {DEFAULT_BRIGHTNESS} (0-255)")
    
    use_custom = input("是否修改默认参数? (y/n) [n]: ").strip().lower() == 'y'
    
    if use_custom:
        channel_id = input_int(f"请输入通道ID (0-3或255广播)", DEFAULT_CHANNEL_ID, 0, 255)
        if channel_id not in [0, 1, 2, 3, 255]:
            print("警告: 通道ID应为0-3或255")
        
        brightness = input_int(f"请输入亮度", DEFAULT_BRIGHTNESS, 0, 255)
        
        return channel_id, brightness
    else:
        return DEFAULT_CHANNEL_ID, DEFAULT_BRIGHTNESS


def get_serial_connection(port, baud):
    """获取串口连接，支持重试"""
    while True:
        try:
            ser = serial.Serial(port, baud, timeout=1)
            print(f"连接成功，端口: {port}, 波特率: {baud}")
            return ser
        except serial.SerialException as e:
            print(f"串口连接失败: {e}")
            print("可能的原因:")
            print("1. 设备未连接或未正确识别")
            print("2. 串口号错误")
            print("3. 设备被其他程序占用")
            
            choice = input("\n选择操作: (r)重试 (c)更换串口 (q)退出 [r]: ").strip().lower()
            if choice == 'q':
                return None
            elif choice == 'c':
                new_port = input(f"请输入新的串口号 (当前: {port}): ").strip()
                if new_port:
                    port = new_port
            # 默认重试

def main():
    parser = argparse.ArgumentParser(description="WS2812 速度和颜色效果测试工具（支持自定义参数）")
    parser.add_argument('-p', '--port', type=str, default='COM12', help='串口号, 如 COM12')
    parser.add_argument('-b', '--baud', type=int, default=115200, help='波特率')
    parser.add_argument('--mode', type=str, choices=['menu', 'custom'], default='menu', help='启动模式: menu=菜单测试, custom=自定义参数')
    parser.add_argument('--no-retry', action='store_true', help='连接失败时不重试，直接退出')
    parser.add_argument('--once', action='store_true', help='执行一次测试后自动退出，不返回菜单')
    parser.add_argument('--test', type=int, choices=[1,2,3,4,5,6,7,8,9], help='直接运行指定测试: 1=呼吸灯, 2=跑马灯, 3=闪烁, 4=彩虹, 5=波浪, 6=组合, 7=自定义, 8=全部, 9=全部(含彩虹波浪)')
    args = parser.parse_args()

    # 获取串口连接
    if args.no_retry:
        try:
            ser = serial.Serial(args.port, args.baud, timeout=1)
            print(f"连接成功，端口: {args.port}, 波特率: {args.baud}")
        except serial.SerialException as e:
            print(f"串口错误: {e}")
            print("请检查串口号和设备连接")
            return
    else:
        ser = get_serial_connection(args.port, args.baud)
        if ser is None:
            print("用户取消连接，程序退出。")
            return

    try:
        time.sleep(1)
        send_json_command(ser, {"action": "status"})
        time.sleep(0.5)

        # 如果指定了直接运行的测试，执行后退出
        if args.test:
            if args.test == 1:
                test_breathing_effects(ser, DEFAULT_CHANNEL_ID, DEFAULT_BRIGHTNESS)
            elif args.test == 2:
                test_running_effects(ser, DEFAULT_CHANNEL_ID, DEFAULT_BRIGHTNESS)
            elif args.test == 3:
                test_flash_effects(ser, DEFAULT_CHANNEL_ID, DEFAULT_BRIGHTNESS)
            elif args.test == 4:
                test_rainbow_effects(ser, DEFAULT_CHANNEL_ID, DEFAULT_BRIGHTNESS)
            elif args.test == 5:
                test_wave_effects(ser, DEFAULT_CHANNEL_ID, DEFAULT_BRIGHTNESS)
            elif args.test == 6:
                test_combined_effects(ser, DEFAULT_BRIGHTNESS)
            elif args.test == 7:
                custom_effect(ser)
            elif args.test == 8:
                print("\n开始运行基础测试...")
                test_breathing_effects(ser, DEFAULT_CHANNEL_ID, DEFAULT_BRIGHTNESS)
                time.sleep(5)
                test_running_effects(ser, DEFAULT_CHANNEL_ID, DEFAULT_BRIGHTNESS)
                time.sleep(5)
                test_flash_effects(ser, DEFAULT_CHANNEL_ID, DEFAULT_BRIGHTNESS)
                time.sleep(5)
                test_combined_effects(ser, DEFAULT_BRIGHTNESS)
            elif args.test == 9:
                print("\n开始运行所有测试（包括彩虹和波浪）...")
                test_breathing_effects(ser, DEFAULT_CHANNEL_ID, DEFAULT_BRIGHTNESS)
                time.sleep(5)
                test_running_effects(ser, DEFAULT_CHANNEL_ID, DEFAULT_BRIGHTNESS)
                time.sleep(5)
                test_flash_effects(ser, DEFAULT_CHANNEL_ID, DEFAULT_BRIGHTNESS)
                time.sleep(5)
                test_rainbow_effects(ser, DEFAULT_CHANNEL_ID, DEFAULT_BRIGHTNESS)
                time.sleep(5)
                test_wave_effects(ser, DEFAULT_CHANNEL_ID, DEFAULT_BRIGHTNESS)
                time.sleep(5)
                test_combined_effects(ser, DEFAULT_BRIGHTNESS)
            print("\n测试完成，关闭所有LED...")
            send_json_command(ser, {"channel": 255, "mode": 0})
            print("程序退出。")
            ser.close()
            return

        if args.mode == 'custom':
            while True:
                custom_effect(ser)
                again = input("继续自定义设置？(y/n): ").strip().lower()
                if again != 'y':
                    break
        else:
            print("\n欢迎使用WS2812测试工具！")
            
            while True:
                try:
                    # 显示菜单
                    print("\n=== WS2812 测试菜单 ===")
                    print("1. 呼吸灯测试")
                    print("2. 跑马灯测试") 
                    print("3. 闪烁测试")
                    print("4. 彩虹效果测试")
                    print("5. 波浪效果测试")
                    print("6. 组合效果")
                    print("7. 自定义参数")
                    print("8. 运行基础测试(1-3,6)")
                    print("9. 运行所有测试(1-6)")
                    print("0. 关闭所有LED并退出")
                    print("========================")
                    
                    sel = input_int("请选择功能 (按Ctrl+C可随时退出)", 1, 0, 9)
                    if sel == 1:
                        channel_id, brightness = get_test_parameters()
                        test_breathing_effects(ser, channel_id, brightness)
                        if args.once:
                            print("\n呼吸灯测试完成，程序退出。")
                            send_json_command(ser, {"channel": 255, "mode": 0})
                            break
                        print("\n呼吸灯测试完成。")
                        time.sleep(1)  # 短暂暂停让用户看到完成消息
                    elif sel == 2:
                        channel_id, brightness = get_test_parameters()
                        test_running_effects(ser, channel_id, brightness)
                        if args.once:
                            print("\n跑马灯测试完成，程序退出。")
                            send_json_command(ser, {"channel": 255, "mode": 0})
                            break
                        print("\n跑马灯测试完成。")
                        time.sleep(1)  # 短暂暂停让用户看到完成消息
                    elif sel == 3:
                        channel_id, brightness = get_test_parameters()
                        test_flash_effects(ser, channel_id, brightness)
                        if args.once:
                            print("\n闪烁测试完成，程序退出。")
                            send_json_command(ser, {"channel": 255, "mode": 0})
                            break
                        print("\n闪烁测试完成。")
                        time.sleep(1)  # 短暂暂停让用户看到完成消息
                    elif sel == 4:
                        channel_id, brightness = get_test_parameters()
                        test_rainbow_effects(ser, channel_id, brightness)
                        if args.once:
                            print("\n彩虹效果测试完成，程序退出。")
                            send_json_command(ser, {"channel": 255, "mode": 0})
                            break
                        print("\n彩虹效果测试完成。")
                        time.sleep(1)  # 短暂暂停让用户看到完成消息
                    elif sel == 5:
                        channel_id, brightness = get_test_parameters()
                        test_wave_effects(ser, channel_id, brightness)
                        if args.once:
                            print("\n波浪效果测试完成，程序退出。")
                            send_json_command(ser, {"channel": 255, "mode": 0})
                            break
                        print("\n波浪效果测试完成。")
                        time.sleep(1)  # 短暂暂停让用户看到完成消息
                    elif sel == 6:
                        channel_id, brightness = get_test_parameters()
                        test_combined_effects(ser, brightness)
                        if args.once:
                            print("\n组合效果测试完成，程序退出。")
                            send_json_command(ser, {"channel": 255, "mode": 0})
                            break
                        print("\n组合效果测试完成。")
                        time.sleep(1)  # 短暂暂停让用户看到完成消息
                    elif sel == 7:
                        custom_effect(ser)
                        if args.once:
                            print("\n自定义参数设置完成，程序退出。")
                            send_json_command(ser, {"channel": 255, "mode": 0})
                            break
                        print("\n自定义参数设置完成。")
                        time.sleep(1)  # 短暂暂停让用户看到完成消息
                    elif sel == 8:
                        print("\n开始运行基础测试...")
                        channel_id, brightness = get_test_parameters()
                        test_breathing_effects(ser, channel_id, brightness)
                        print("等待5秒后继续下一项测试...")
                        time.sleep(5)
                        test_running_effects(ser, channel_id, brightness)
                        print("等待5秒后继续下一项测试...")
                        time.sleep(5)
                        test_flash_effects(ser, channel_id, brightness)
                        print("等待5秒后继续下一项测试...")
                        time.sleep(5)
                        test_combined_effects(ser, brightness)
                        if args.once:
                            print("\n基础测试完成，程序退出。")
                            send_json_command(ser, {"channel": 255, "mode": 0})
                            break
                        print("\n基础测试完成。")
                        time.sleep(1)  # 短暂暂停让用户看到完成消息
                    elif sel == 9:
                        print("\n开始运行所有测试（包括彩虹和波浪）...")
                        channel_id, brightness = get_test_parameters()
                        test_breathing_effects(ser, channel_id, brightness)
                        print("等待5秒后继续下一项测试...")
                        time.sleep(5)
                        test_running_effects(ser, channel_id, brightness)
                        print("等待5秒后继续下一项测试...")
                        time.sleep(5)
                        test_flash_effects(ser, channel_id, brightness)
                        print("等待5秒后继续下一项测试...")
                        time.sleep(5)
                        test_rainbow_effects(ser, channel_id, brightness)
                        print("等待5秒后继续下一项测试...")
                        time.sleep(5)
                        test_wave_effects(ser, channel_id, brightness)
                        print("等待5秒后继续下一项测试...")
                        time.sleep(5)
                        test_combined_effects(ser, brightness)
                        if args.once:
                            print("\n所有测试完成，程序退出。")
                            send_json_command(ser, {"channel": 255, "mode": 0})
                            break
                        print("\n所有测试完成。")
                        time.sleep(1)  # 短暂暂停让用户看到完成消息
                    elif sel == 0:
                        print("关闭所有LED...")
                        send_json_command(ser, {"channel": 255, "mode": 0})
                        break
                except KeyboardInterrupt:
                    print("\n\n用户中断，正在关闭所有LED...")
                    send_json_command(ser, {"channel": 255, "mode": 0})
                    break
        print("\n测试结束，关闭串口。")
        ser.close()
    except serial.SerialException as e:
        print(f"串口错误: {e}")
        print("请检查串口号和设备连接")
    except KeyboardInterrupt:
        print("\n测试被用户中断")
        if 'ser' in locals():
            send_json_command(ser, {"channel": 255, "mode": 0})
            ser.close()
    except Exception as e:
        print(f"测试错误: {e}")

if __name__ == "__main__":
    main()
