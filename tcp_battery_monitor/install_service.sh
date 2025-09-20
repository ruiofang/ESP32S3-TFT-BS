#!/bin/bash

# 检查是否以root权限运行
if [ "$EUID" -ne 0 ]; then 
    echo "请使用sudo运行此脚本"
    exit 1
fi

# 设置工作目录
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SERVICE_NAME="battery-monitor"

# 检查必要文件是否存在
if [ ! -f "$SCRIPT_DIR/run_tcp_battery_monitor.sh" ]; then
    echo "错误：找不到运行脚本 run_tcp_battery_monitor.sh"
    exit 1
fi

if [ ! -f "$SCRIPT_DIR/$SERVICE_NAME.service" ]; then
    echo "错误：找不到服务配置文件 $SERVICE_NAME.service"
    exit 1
fi

# 确保运行脚本具有执行权限
chmod +x "$SCRIPT_DIR/run_tcp_battery_monitor.sh"

# 停止现有服务（如果正在运行）
if systemctl is-active --quiet $SERVICE_NAME.service 2>/dev/null; then
    echo "停止现有服务..."
    systemctl stop $SERVICE_NAME.service
fi

# 复制服务文件到systemd目录
echo "安装服务配置文件..."
cp "$SCRIPT_DIR/$SERVICE_NAME.service" /etc/systemd/system/

# 重新加载systemd配置
echo "重新加载systemd配置..."
systemctl daemon-reload

# 启用并启动服务
echo "启用并启动服务..."
systemctl enable $SERVICE_NAME.service
systemctl start $SERVICE_NAME.service

# 检查服务状态
sleep 2
if systemctl is-active --quiet $SERVICE_NAME.service; then
    echo "✓ 服务安装并启动成功！"
    echo ""
    echo "可以使用以下命令管理服务："
    echo "  查看状态: systemctl status $SERVICE_NAME.service"
    echo "  查看日志: journalctl -u $SERVICE_NAME.service -f"
    echo "  停止服务: sudo systemctl stop $SERVICE_NAME.service"
    echo "  启动服务: sudo systemctl start $SERVICE_NAME.service"
    echo "  重启服务: sudo systemctl restart $SERVICE_NAME.service"
else
    echo "✗ 服务启动失败，请检查配置"
    echo ""
    echo "查看错误信息："
    echo "  systemctl status $SERVICE_NAME.service"
    echo "  journalctl -u $SERVICE_NAME.service --no-pager"
    exit 1
fi
