#!/bin/bash

set -e

SERVICE_NAME="claude-wifi"
SERVICE_FILE="/etc/systemd/system/${SERVICE_NAME}.service"

echo "==> 停止服务（如果存在）"
if systemctl is-active --quiet "$SERVICE_NAME"; then
    sudo systemctl stop "$SERVICE_NAME"
fi

echo "==> 禁用开机自启"
if systemctl is-enabled --quiet "$SERVICE_NAME" 2>/dev/null; then
    sudo systemctl disable "$SERVICE_NAME"
fi

echo "==> 删除 systemd 服务文件"
if [ -f "$SERVICE_FILE" ]; then
    sudo rm "$SERVICE_FILE"
else
    echo "⚠️  服务文件不存在，跳过"
fi

echo "==> 重载 systemd"
sudo systemctl daemon-reload

echo "✅ 卸载完成"
echo "ℹ️  原脚本 claude_status_wifi_bridge.py 未被删除"
