#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
超高频率串口通信测试脚本
测试优化后的非阻塞UART处理能力

使用方法:
python ultra_high_freq_test.py

确保ESP32设备已连接并且串口通信正常。
"""

import serial
import json
import time
import threading
import sys
from datetime import datetime
import statistics

# 串口配置
SERIAL_PORT = 'COM12'  # 根据实际情况修改
BAUD_RATE = 115200
TIMEOUT = 0.1  # 减少超时时间

class UltraHighFreqTest:
    def __init__(self):
        self.ser = None
        self.running = False
        self.stats = {
            'sent': 0,
            'received': 0,
            'errors': 0,
            'timeouts': 0,
            'response_times': []
        }
        self.lock = threading.Lock()
        
    def connect_serial(self):
        """连接串口"""
        try:
            self.ser = serial.Serial(SERIAL_PORT, BAUD_RATE, timeout=TIMEOUT)
            print(f"串口连接成功: {SERIAL_PORT} @ {BAUD_RATE}")
            self.ser.flushInput()
            self.ser.flushOutput()
            return True
        except Exception as e:
            print(f"串口连接失败: {e}")
            return False
    
    def send_battery_data_timed(self, battery, charging=False, voltage=24.0):
        """发送电池数据并测量响应时间"""
        if not self.ser:
            return False, 0
            
        data = {
            "voltage": voltage,
            "battery": battery,
            "charging": charging
        }
        
        try:
            json_str = json.dumps(data)
            start_time = time.time()
            
            self.ser.write(json_str.encode('utf-8'))
            self.ser.write(b'\r\n')
            
            with self.lock:
                self.stats['sent'] += 1
            
            # 尝试读取响应
            try:
                response = self.ser.readline().decode('utf-8', errors='ignore').strip()
                if response:
                    response_time = (time.time() - start_time) * 1000  # ms
                    with self.lock:
                        self.stats['received'] += 1
                        self.stats['response_times'].append(response_time)
                    return True, response_time
                else:
                    with self.lock:
                        self.stats['timeouts'] += 1
                    return False, 0
                    
            except Exception:
                with self.lock:
                    self.stats['timeouts'] += 1
                return False, 0
                
        except Exception as e:
            with self.lock:
                self.stats['errors'] += 1
            return False, 0
    
    def test_extreme_frequency(self, duration_seconds=30, target_freq=100):
        """极高频率测试"""
        print(f"\n=== 极高频率测试 ===")
        print(f"测试时长: {duration_seconds}秒")
        print(f"目标频率: {target_freq} Hz")
        print(f"预计发送: {duration_seconds * target_freq}条消息")
        print("测试ESP32在极高频率下是否还会阻塞\n")
        
        self.running = True
        
        start_time = time.time()
        battery_level = 50
        direction = 1
        
        try:
            while time.time() - start_time < duration_seconds:
                # 发送不同的电池数据
                battery_level += direction
                if battery_level >= 100:
                    battery_level = 100
                    direction = -1
                elif battery_level <= 1:
                    battery_level = 1
                    direction = 1
                
                charging = (self.stats['sent'] % 10) < 5  # 50%概率充电
                voltage = 20.0 + (battery_level / 100.0) * 9.4  # 20.0V到29.4V
                
                success, response_time = self.send_battery_data_timed(battery_level, charging, voltage)
                
                if not success and self.stats['sent'] % 100 == 0:
                    print(f"警告: 第{self.stats['sent']}条消息无响应")
                
                # 精确控制发送频率
                time.sleep(1.0 / target_freq)
                
                # 每500条显示一次统计
                if self.stats['sent'] % 500 == 0:
                    elapsed = time.time() - start_time
                    actual_freq = self.stats['sent'] / elapsed if elapsed > 0 else 0
                    
                    with self.lock:
                        avg_response_time = statistics.mean(self.stats['response_times']) if self.stats['response_times'] else 0
                        response_rate = (self.stats['received'] / self.stats['sent'] * 100) if self.stats['sent'] > 0 else 0
                    
                    print(f"进度: {self.stats['sent']}/目标{int(duration_seconds * target_freq)} "
                          f"| 实际频率: {actual_freq:.1f}Hz "
                          f"| 响应率: {response_rate:.1f}% "
                          f"| 平均响应时间: {avg_response_time:.1f}ms")
        except KeyboardInterrupt:
            print("\n用户中断测试")
        finally:
            self.running = False
        
        # 显示最终统计
        elapsed = time.time() - start_time
        self.display_final_stats(elapsed)
    
    def test_burst_extreme(self):
        """极限突发测试 - 无延迟连续发送"""
        print(f"\n=== 极限突发测试 ===")
        print("无延迟连续发送1000条消息，测试系统极限")
        
        self.stats = {
            'sent': 0,
            'received': 0,
            'errors': 0,
            'timeouts': 0,
            'response_times': []
        }
        
        start_time = time.time()
        
        try:
            for i in range(1000):
                battery = 1 + (i % 100)
                charging = i % 2 == 0
                voltage = 24.0 + (i % 100) * 0.05
                
                success, response_time = self.send_battery_data_timed(battery, charging, voltage)
                
                if i % 100 == 0:
                    elapsed = time.time() - start_time
                    freq = (i + 1) / elapsed if elapsed > 0 else 0
                    print(f"突发进度: {i+1}/1000, 频率: {freq:.0f}Hz")
                    
        except Exception as e:
            print(f"突发测试错误: {e}")
        
        elapsed = time.time() - start_time
        print(f"\n突发测试完成: {elapsed:.2f}秒")
        print(f"突发频率: {1000/elapsed:.0f}Hz")
        
        self.display_final_stats(elapsed)
    
    def display_final_stats(self, elapsed):
        """显示最终统计信息"""
        with self.lock:
            response_times = self.stats['response_times'][:]
        
        print(f"\n=== 测试结果统计 ===")
        print(f"测试时长: {elapsed:.2f}秒")
        print(f"发送消息: {self.stats['sent']}条")
        print(f"收到响应: {self.stats['received']}条")
        print(f"发送错误: {self.stats['errors']}条")
        print(f"超时次数: {self.stats['timeouts']}条")
        print(f"平均发送频率: {self.stats['sent']/elapsed:.1f} Hz")
        print(f"响应成功率: {self.stats['received']/self.stats['sent']*100:.1f}%")
        
        if response_times:
            print(f"\n=== 响应时间分析 ===")
            print(f"平均响应时间: {statistics.mean(response_times):.1f}ms")
            print(f"最小响应时间: {min(response_times):.1f}ms")
            print(f"最大响应时间: {max(response_times):.1f}ms")
            print(f"响应时间标准差: {statistics.stdev(response_times):.1f}ms")
            
            # 响应时间分布
            fast_count = sum(1 for t in response_times if t < 10)
            medium_count = sum(1 for t in response_times if 10 <= t < 50)
            slow_count = sum(1 for t in response_times if t >= 50)
            
            print(f"\n响应时间分布:")
            print(f"  <10ms:  {fast_count} ({fast_count/len(response_times)*100:.1f}%)")
            print(f"  10-50ms: {medium_count} ({medium_count/len(response_times)*100:.1f}%)")
            print(f"  >=50ms: {slow_count} ({slow_count/len(response_times)*100:.1f}%)")
        
        # 性能评估
        success_rate = self.stats['received']/self.stats['sent']*100 if self.stats['sent'] > 0 else 0
        avg_response_time = statistics.mean(response_times) if response_times else float('inf')
        
        print(f"\n=== 性能评估 ===")
        if success_rate >= 95 and avg_response_time < 20:
            print("🟢 优秀: 系统在高频率下响应良好，无阻塞问题")
        elif success_rate >= 85 and avg_response_time < 50:
            print("🟡 良好: 系统基本正常，偶有延迟")
        elif success_rate >= 70:
            print("🟠 一般: 系统有一定延迟，可能存在轻微阻塞")
        else:
            print("🔴 问题: 系统响应差，存在严重阻塞或错误")
    
    def close(self):
        """关闭连接"""
        self.running = False
        if self.ser:
            self.ser.close()
            print("串口连接已关闭")

def main():
    test = UltraHighFreqTest()
    
    if not test.connect_serial():
        return
    
    try:
        print("超高频率串口通信测试 - 验证优化效果")
        print("1. 极高频率测试 (100 Hz, 30秒)")
        print("2. 超高频率测试 (200 Hz, 15秒)") 
        print("3. 极限频率测试 (500 Hz, 10秒)")
        print("4. 极限突发测试 (1000条消息无延迟)")
        print("5. 自定义频率测试")
        
        choice = input("请选择测试模式 (1-5): ").strip()
        
        if choice == '1':
            test.test_extreme_frequency(duration_seconds=30, target_freq=100)
        elif choice == '2':
            test.test_extreme_frequency(duration_seconds=15, target_freq=200)
        elif choice == '3':
            test.test_extreme_frequency(duration_seconds=10, target_freq=500)
        elif choice == '4':
            test.test_burst_extreme()
        elif choice == '5':
            duration = int(input("测试时长(秒): "))
            freq = int(input("目标频率(Hz): "))
            test.test_extreme_frequency(duration_seconds=duration, target_freq=freq)
        else:
            print("无效选择")
            
    except KeyboardInterrupt:
        print("\n测试被用户中断")
    except Exception as e:
        print(f"测试过程中发生错误: {e}")
    finally:
        test.close()

if __name__ == "__main__":
    main()