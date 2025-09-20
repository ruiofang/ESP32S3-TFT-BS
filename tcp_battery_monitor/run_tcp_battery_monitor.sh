#!/bin/bash

set -e  # 在任何命令失败时退出

echo "🌟 === tcp_battery_monitor启动脚本 ==="
echo ""

# 检查配置文件
check_config() {
    echo "🔍 检查配置文件..."
    if [ -f "./tcp_battery_monitor.py" ]; then
        echo "✅ python文件: tcp_battery_monitor.py"
        return 0
    else
        echo "❌ python文件不存在: tcp_battery_monitor.py"
        return 1
    fi
}

# 清理函数
cleanup() {
    echo ""
    echo "🛑 正在退出..."
    # 这里可以添加清理逻辑
    echo "✅ 清理完成"
}

# 主函数
main() {
  
    # 启动准备
    if ! check_config; then
        exit 1
    fi
    
    # 启动程序
    python3 ./tcp_battery_monitor.py
    
    echo ""
    echo "🏁 程序已退出"
}

# 默认启动
main
