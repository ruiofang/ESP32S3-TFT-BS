#!/bin/bash

set -e

SERVICE_NAME="claude-wifi"
SERVICE_FILE="/etc/systemd/system/${SERVICE_NAME}.service"
SCRIPT_PATH="$(pwd)/claude_status_wifi_bridge.py"
USER_NAME="$(whoami)"

echo "==> 检查脚本是否存在"
if [ ! -f "$SCRIPT_PATH" ]; then
    echo "❌ 未找到 claude_status_wifi_bridge.py"
    exit 1
fi

echo "==> 创建 systemd 服务文件"
sudo tee "$SERVICE_FILE" > /dev/null <<EOF
[Unit]
Description=Claude WiFi Bridge Service
After=network-online.target
Wants=network-online.target

[Service]
Type=simple
ExecStart=/usr/bin/python3 $SCRIPT_PATH
WorkingDirectory=$(dirname "$SCRIPT_PATH")
Restart=always
RestartSec=5
User=$USER_NAME

[Install]
WantedBy=multi-user.target
EOF

echo "==> 重载 systemd"
sudo systemctl daemon-reload

echo "==> 启用并启动服务"
sudo systemctl enable "$SERVICE_NAME"
sudo systemctl start "$SERVICE_NAME"

echo "✅ 安装完成"
echo "👉 查看状态：sudo systemctl status $SERVICE_NAME"
echo "👉 查看日志：sudo journalctl -u $SERVICE_NAME -f"
