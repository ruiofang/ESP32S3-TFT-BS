#!/usr/bin/env python3
"""
ESP32S3电池监控配置切换工具
自动修改main.c中的配置宏定义
"""

import os
import sys
import re
from pathlib import Path

class BatteryConfigSwitcher:
    def __init__(self, main_c_path="main/main.c"):
        self.main_c_path = main_c_path
        self.backup_path = main_c_path + ".backup"
        
    def backup_file(self):
        """备份原文件"""
        if os.path.exists(self.main_c_path):
            with open(self.main_c_path, 'r', encoding='utf-8') as f:
                content = f.read()
            with open(self.backup_path, 'w', encoding='utf-8') as f:
                f.write(content)
            print(f"已备份原文件到: {self.backup_path}")
    
    def restore_backup(self):
        """恢复备份文件"""
        if os.path.exists(self.backup_path):
            with open(self.backup_path, 'r', encoding='utf-8') as f:
                content = f.read()
            with open(self.main_c_path, 'w', encoding='utf-8') as f:
                f.write(content)
            print(f"已从备份恢复: {self.main_c_path}")
            return True
        else:
            print("未找到备份文件")
            return False
    
    def modify_config(self, rs485_enabled, json_enabled):
        """修改配置宏定义 (RS485 电池查询已改为运行时模式切换，此处仅保留 JSON 开关)"""
        if not os.path.exists(self.main_c_path):
            print(f"错误: 未找到文件 {self.main_c_path}")
            return False

        # 读取文件内容
        with open(self.main_c_path, 'r', encoding='utf-8') as f:
            content = f.read()

        if not rs485_enabled:
            print("提示: RS485 电池查询已为运行时模式切换，无法通过宏禁用；请使用 BOOT 键在 JSON 模式下运行。")

        # 修改JSON配置
        json_pattern = r'#define\s+ENABLE_JSON_PASSIVE_MODE\s+\d+'
        json_replacement = f'#define ENABLE_JSON_PASSIVE_MODE    {1 if json_enabled else 0}'
        content = re.sub(json_pattern, json_replacement, content)

        # 写回文件
        with open(self.main_c_path, 'w', encoding='utf-8') as f:
            f.write(content)

        return True
    
    def show_current_config(self):
        """显示当前配置"""
        if not os.path.exists(self.main_c_path):
            print(f"错误: 未找到文件 {self.main_c_path}")
            return
        
        with open(self.main_c_path, 'r', encoding='utf-8') as f:
            content = f.read()
        
        # 提取当前配置 (RS485 现在由运行时模式决定，此处不再解析)
        json_match = re.search(r'#define\s+ENABLE_JSON_PASSIVE_MODE\s+(\d+)', content)

        if json_match:
            json_enabled = int(json_match.group(1)) == 1

            print("\n当前配置:")
            print("  RS485电池查询: 运行时切换 (BOOT键: JSON / RS485-1 / RS485-2)")
            print(f"  JSON被动控制: {'启用' if json_enabled else '禁用'}")
        else:
            print("无法解析当前配置")
    
    def interactive_config(self):
        """交互式配置"""
        print("ESP32S3电池监控系统配置切换工具")
        print("="*50)
        
        self.show_current_config()
        
        print("\n可选模式:")
        print("1. 仅RS485电池查询模式")
        print("2. 仅JSON被动控制模式")
        print("3. 混合模式 (RS485 + JSON)")
        print("4. 基本模式 (无外部控制)")
        print("5. 显示当前配置")
        print("6. 恢复备份")
        print("0. 退出")
        
        while True:
            try:
                choice = input("\n请选择模式 (0-6): ").strip()
                
                if choice == '0':
                    print("退出配置工具")
                    break
                elif choice == '1':
                    self.backup_file()
                    if self.modify_config(True, False):
                        print("已切换到: 仅RS485电池查询模式")
                        print("波特率: 9600")
                        print("功能: 自动查询电池信息并显示")
                elif choice == '2':
                    self.backup_file()
                    if self.modify_config(False, True):
                        print("已切换到: 仅JSON被动控制模式")
                        print("波特率: 115200")
                        print("功能: 通过串口JSON命令控制显示")
                elif choice == '3':
                    self.backup_file()
                    if self.modify_config(True, True):
                        print("已切换到: 混合模式")
                        print("波特率: 9600")
                        print("功能: RS485电池查询 + JSON控制")
                elif choice == '4':
                    self.backup_file()
                    if self.modify_config(False, False):
                        print("已切换到: 基本模式")
                        print("波特率: 115200")
                        print("功能: 基本显示，无外部控制")
                elif choice == '5':
                    self.show_current_config()
                elif choice == '6':
                    if self.restore_backup():
                        print("已恢复到备份版本")
                    continue
                else:
                    print("无效选择，请重新输入")
                    continue
                
                if choice in ['1', '2', '3', '4']:
                    print("\n配置修改完成！")
                    print("请重新编译并上传程序到ESP32S3")
                    self.show_current_config()
                
            except KeyboardInterrupt:
                print("\n\n用户中断，退出配置工具")
                break
            except Exception as e:
                print(f"错误: {e}")

def main():
    """主函数"""
    # 检查文件路径
    main_c_paths = [
        "main/main.c",
        "../main/main.c", 
        "main.c",
        "ESP32S3-TFT-BS_V1.3/main/main.c"
    ]
    
    main_c_path = None
    for path in main_c_paths:
        if os.path.exists(path):
            main_c_path = path
            break
    
    if not main_c_path:
        print("错误: 未找到main.c文件")
        print("请在项目根目录或main目录下运行此脚本")
        return
    
    switcher = BatteryConfigSwitcher(main_c_path)
    
    if len(sys.argv) > 1:
        # 命令行模式
        if sys.argv[1] == "show":
            switcher.show_current_config()
        elif sys.argv[1] == "rs485":
            switcher.backup_file()
            switcher.modify_config(True, False)
            print("已切换到RS485模式")
        elif sys.argv[1] == "json":
            switcher.backup_file()
            switcher.modify_config(False, True)
            print("已切换到JSON模式")
        elif sys.argv[1] == "hybrid":
            switcher.backup_file()
            switcher.modify_config(True, True)
            print("已切换到混合模式")
        elif sys.argv[1] == "basic":
            switcher.backup_file()
            switcher.modify_config(False, False)
            print("已切换到基本模式")
        elif sys.argv[1] == "restore":
            switcher.restore_backup()
        else:
            print("用法: python config_switcher.py [show|rs485|json|hybrid|basic|restore]")
    else:
        # 交互模式
        switcher.interactive_config()

if __name__ == "__main__":
    main()