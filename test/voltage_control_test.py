#!/usr/bin/env python3
"""
ESP32 电压控制测试脚本
测试新的电压控制协议功能
"""

import serial
import time
import json
import sys

class VoltageControlTester:
    def __init__(self, port='COM12', baudrate=115200):
        """初始化串口连接"""
        try:
            self.ser = serial.Serial(port, baudrate, timeout=2)
            print(f"✓ 已连接到 {port}，波特率 {baudrate}")
        except Exception as e:
            print(f"✗ 连接失败: {e}")
            sys.exit(1)
    
    def send_command(self, command):
        """发送命令并接收响应"""
        print(f"\n📤 发送: {command}")
        self.ser.write(f"{command}\r\n".encode())
        time.sleep(0.5)
        
        # 读取响应
        response = ""
        while self.ser.in_waiting > 0:
            response += self.ser.read(self.ser.in_waiting).decode('utf-8', errors='ignore')
            time.sleep(0.1)
        
        if response.strip():
            print(f"📥 响应: {response.strip()}")
        return response.strip()
    
    def send_json_command(self, json_data):
        """发送JSON命令"""
        command = json.dumps(json_data, separators=(',', ':'))
        return self.send_command(command)
    
    def test_voltage_commands(self):
        """测试电压控制命令"""
        print("=" * 60)
        print("🔋 测试电压控制功能")
        print("=" * 60)
        
        # 1. 查询当前状态
        print("\n1️⃣ 查询当前电池状态")
        response = self.send_json_command({"query": "battery"})
        
        # 2. 设置外部电压 - JSON格式
        print("\n2️⃣ 设置电压为 3.5V (JSON)")
        self.send_json_command({"voltage": 3.5})
        
        # 3. 查询状态验证
        print("\n3️⃣ 验证电压设置")
        response = self.send_json_command({"query": "battery"})
        
        # 4. 设置不同电压值
        voltage_tests = [4.0, 3.2, 4.2, 3.0]
        for i, voltage in enumerate(voltage_tests, 4):
            print(f"\n{i}️⃣ 设置电压为 {voltage}V")
            self.send_json_command({"voltage": voltage})
            time.sleep(1)
            # 查询状态
            response = self.send_json_command({"query": "battery"})
        
        # 5. 恢复自动电压模式
        print("\n8️⃣ 恢复自动电压模式")
        self.send_json_command({"auto_voltage": True})
        
        # 6. 验证自动模式
        print("\n9️⃣ 验证自动电压模式")
        response = self.send_json_command({"query": "battery"})
    
    def test_traditional_commands(self):
        """测试传统命令格式"""
        print("\n" + "=" * 60)
        print("📟 测试传统电压控制命令")
        print("=" * 60)
        
        # 1. 设置电压
        print("\n1️⃣ 传统命令设置电压 3.8V")
        self.send_command("VOLTAGE:3.8")
        
        # 2. 查询状态
        print("\n2️⃣ 查询电池状态")
        self.send_command("BATTERY:JSON")
        
        # 3. 设置边界值
        print("\n3️⃣ 测试边界值 - 最低电压 3.0V")
        self.send_command("VOLTAGE:3.0")
        
        print("\n4️⃣ 测试边界值 - 最高电压 4.2V") 
        self.send_command("VOLTAGE:4.2")
        
        # 4. 测试无效值
        print("\n5️⃣ 测试无效值 - 2.5V (应该失败)")
        self.send_command("VOLTAGE:2.5")
        
        print("\n6️⃣ 测试无效值 - 5.0V (应该失败)")
        self.send_command("VOLTAGE:5.0")
        
        # 5. 恢复自动模式
        print("\n7️⃣ 恢复自动电压模式")
        self.send_command("VOLTAGE:AUTO")
    
    def test_combined_control(self):
        """测试组合控制"""
        print("\n" + "=" * 60)
        print("🎛️ 测试电压、电量和充电状态组合控制")
        print("=" * 60)
        
        # 1. 同时设置电压和电量
        print("\n1️⃣ 同时设置电压3.9V和电量85%")
        self.send_json_command({
            "voltage": 3.9,
            "battery": 85,
            "charging": True
        })
        
        # 2. 查询状态
        print("\n2️⃣ 查询组合设置结果")
        response = self.send_json_command({"query": "battery"})
        
        # 3. 只恢复电压自动，保持其他设置
        print("\n3️⃣ 只恢复电压自动模式")
        self.send_json_command({"auto_voltage": True})
        
        # 4. 查询部分自动状态
        print("\n4️⃣ 查询部分自动状态")
        response = self.send_json_command({"query": "battery"})
        
        # 5. 全部恢复自动
        print("\n5️⃣ 全部恢复自动模式")
        self.send_json_command({
            "auto_mode": True,
            "auto_charging": True,
            "auto_voltage": True
        })
        
        # 6. 最终状态查询
        print("\n6️⃣ 最终状态查询")
        response = self.send_json_command({"query": "battery"})
    
    def run_all_tests(self):
        """运行所有测试"""
        print("🚀 开始ESP32电压控制功能测试")
        
        try:
            # 基础电压控制测试
            self.test_voltage_commands()
            
            # 传统命令测试
            self.test_traditional_commands()
            
            # 组合控制测试
            self.test_combined_control()
            
            print("\n" + "=" * 60)
            print("✅ 所有测试完成!")
            print("=" * 60)
            
        except KeyboardInterrupt:
            print("\n⚠️ 测试被用户中断")
        except Exception as e:
            print(f"\n❌ 测试过程中发生错误: {e}")
        finally:
            self.ser.close()
            print("🔌 串口连接已关闭")

def main():
    """主函数"""
    import argparse
    parser = argparse.ArgumentParser(description="ESP32电压控制测试脚本")
    parser.add_argument("--port", "-p", default="COM3", help="串口端口 (默认: COM3)")
    parser.add_argument("--baudrate", "-b", type=int, default=115200, help="波特率 (默认: 115200)")
    
    args = parser.parse_args()
    
    print(f"🔧 配置: 端口={args.port}, 波特率={args.baudrate}")
    
    tester = VoltageControlTester(args.port, args.baudrate)
    tester.run_all_tests()

if __name__ == "__main__":
    main()