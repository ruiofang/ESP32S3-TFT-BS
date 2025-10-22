#!/usr/bin/env python3
"""
ESP32S3 RS485电池通信测试程序
测试新增的RS485电池查询功能
"""

import serial
import json
import time
import sys

class BatteryRS485Tester:
    def __init__(self, port='COM3', baudrate=115200):
        """初始化串口连接"""
        try:
            self.ser = serial.Serial(port, baudrate, timeout=1)
            print(f"已连接到 {port}，波特率 {baudrate}")
            time.sleep(2)  # 等待ESP32启动
        except Exception as e:
            print(f"无法连接到串口 {port}: {e}")
            sys.exit(1)
    
    def send_command(self, command):
        """发送命令并接收响应"""
        print(f"发送: {command}")
        self.ser.write(f"{command}\r\n".encode())
        self.ser.flush()
        
        # 等待响应
        time.sleep(0.5)
        response = ""
        while self.ser.in_waiting:
            response += self.ser.read(self.ser.in_waiting).decode()
            time.sleep(0.1)
        
        print(f"响应: {response.strip()}")
        return response.strip()
    
    def test_basic_battery_query(self):
        """测试基本电池查询"""
        print("\n=== 基本电池查询测试 ===")
        
        # 简单查询
        self.send_command("BATTERY")
        
        # JSON查询
        self.send_command("BATTERY:JSON")
        
        # JSON格式查询
        self.send_command('{"query": "battery"}')
    
    def test_rs485_detailed_query(self):
        """测试RS485详细信息查询"""
        print("\n=== RS485详细信息查询测试 ===")
        
        # 查询详细电池信息
        response = self.send_command('{"query": "battery_detail"}')
        
        try:
            detail_data = json.loads(response)
            print("解析详细信息:")
            print(f"  电压: {detail_data.get('voltage', 'N/A')}V")
            print(f"  电量: {detail_data.get('percentage', 'N/A')}%")
            print(f"  充电状态: {detail_data.get('charging', 'N/A')}")
            print(f"  RS485连接: {detail_data.get('rs485_connected', 'N/A')}")
            print(f"  自动查询: {detail_data.get('auto_query', 'N/A')}")
            
            if 'rs485_data' in detail_data:
                rs485_data = detail_data['rs485_data']
                print("  RS485数据:")
                print(f"    电池组电压: {rs485_data.get('pack_voltage', 'N/A')}V")
                print(f"    电池组电流: {rs485_data.get('pack_current', 'N/A')}A")
                print(f"    SOC: {rs485_data.get('soc', 'N/A')}%")
                print(f"    剩余容量: {rs485_data.get('remain_capacity_mah', 'N/A')}mAh")
                print(f"    满容量: {rs485_data.get('full_capacity_mah', 'N/A')}mAh")
                print(f"    电池串数: {rs485_data.get('battery_strings', 'N/A')}")
                print(f"    保护状态: 0x{rs485_data.get('protect_status', 0):04X}")
                print(f"    MOS状态: 0x{rs485_data.get('fet_status', 0):02X}")
                
                if 'temperatures' in rs485_data:
                    temps = rs485_data['temperatures']
                    print(f"    温度: {temps}")
            
        except json.JSONDecodeError:
            print("无法解析JSON响应")
    
    def test_manual_query(self):
        """测试手动触发查询"""
        print("\n=== 手动查询触发测试 ===")
        
        # 手动触发电池查询
        response = self.send_command('{"trigger_battery_query": true}')
        
        try:
            result = json.loads(response)
            print(f"手动查询结果: {result}")
        except json.JSONDecodeError:
            print("无法解析查询结果")
        
        # 等待一下，然后再次查询详细信息
        time.sleep(2)
        self.test_rs485_detailed_query()
    
    def test_auto_query_control(self):
        """测试自动查询控制"""
        print("\n=== 自动查询控制测试 ===")
        
        # 禁用自动查询
        response = self.send_command('{"battery_auto_query": false}')
        print(f"禁用自动查询: {response}")
        
        time.sleep(1)
        
        # 启用自动查询
        response = self.send_command('{"battery_auto_query": true}')
        print(f"启用自动查询: {response}")
    
    def test_charging_simulation(self):
        """测试充电状态模拟"""
        print("\n=== 充电状态模拟测试 ===")
        
        # 模拟充电状态
        self.send_command('{"simulate_charging": true}')
        time.sleep(1)
        self.send_command('{"query": "battery"}')
        
        time.sleep(2)
        
        # 取消充电状态
        self.send_command('{"simulate_charging": false}')
        time.sleep(1)
        self.send_command('{"query": "battery"}')
    
    def test_battery_level_control(self):
        """测试电池电量控制"""
        print("\n=== 电池电量控制测试 ===")
        
        # 设置不同的电量值
        for level in [25, 50, 75, 90]:
            self.send_command(f'{{"battery": {level}}}')
            time.sleep(1)
            self.send_command('{"query": "battery"}')
            time.sleep(1)
        
        # 恢复自动模式
        self.send_command("BATTERY:AUTO")
    
    def run_all_tests(self):
        """运行所有测试"""
        print("ESP32S3 RS485电池通信功能测试开始")
        print("="*50)
        
        self.test_basic_battery_query()
        self.test_rs485_detailed_query()
        self.test_manual_query()
        self.test_auto_query_control()
        self.test_charging_simulation()
        self.test_battery_level_control()
        
        print("\n" + "="*50)
        print("所有测试完成")
    
    def close(self):
        """关闭串口连接"""
        if self.ser and self.ser.is_open:
            self.ser.close()
            print("串口连接已关闭")

def main():
    """主函数"""
    if len(sys.argv) > 1:
        port = sys.argv[1]
    else:
        port = 'COM3'  # 默认串口
    
    tester = BatteryRS485Tester(port)
    
    try:
        tester.run_all_tests()
    except KeyboardInterrupt:
        print("\n测试被用户中断")
    except Exception as e:
        print(f"测试过程中发生错误: {e}")
    finally:
        tester.close()

if __name__ == "__main__":
    main()