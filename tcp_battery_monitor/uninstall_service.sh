#!/bin/bash

# 检查是否以root权限运行
if [ "$EUID" -ne 0 ]; then 
    echo "请使用sudo运行此脚本"
    exit 1
fi

# 设置服务名称
SERVICE_NAME="battery-monitor"

# 检查服务是否存在
if [ ! -f "/etc/systemd/system/$SERVICE_NAME.service" ]; then
    echo "服务 $SERVICE_NAME.service 未安装"
    exit 0
fi

# 停止服务（如果正在运行）
if systemctl is-active --quiet $SERVICE_NAME.service; then
    echo "停止服务..."
    systemctl stop $SERVICE_NAME.service
else
    echo "服务未在运行"
fi

# 禁用服务
if systemctl is-enabled --quiet $SERVICE_NAME.service; then
    echo "禁用服务..."
    systemctl disable $SERVICE_NAME.service
else
    echo "服务未启用"
fi

# 删除服务文件
echo "删除服务配置文件..."
rm -f /etc/systemd/system/$SERVICE_NAME.service

# 重新加载systemd配置
echo "重新加载systemd配置..."
systemctl daemon-reload

echo "✓ 服务已成功卸载！"
