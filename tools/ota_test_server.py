#!/usr/bin/env python3
"""
OTA 测试服务器 - 用于本地WiFi网络上测试OTA更新

用法:
    python3 ota_test_server.py [--port 8000] [--version 2.2] [--firmware panda.bin]

说明:
    这个脚本在本地启动一个HTTP服务器，供ESP32设备下载固件和配置文件。
    适合在隔离网络中或GitHub无法访问时进行OTA测试。
"""

import http.server
import socketserver
import json
import argparse
import os
import sys
from pathlib import Path
import socket

def get_local_ip():
    """获取本机局域网IP地址"""
    try:
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        s.connect(("8.8.8.8", 80))
        ip = s.getsockname()[0]
        s.close()
        return ip
    except Exception:
        return "127.0.0.1"

class OTAHTTPRequestHandler(http.server.SimpleHTTPRequestHandler):
    """自定义HTTP请求处理器"""

    def do_GET(self):
        """处理GET请求"""
        if self.path == '/latest.json':
            self.send_response(200)
            self.send_header('Content-type', 'application/json')
            self.send_header('Access-Control-Allow-Origin', '*')
            self.end_headers()

            manifest = {
                "version": self.server.firmware_version,
                "url": f"http://{self.server.local_ip}:{self.server.server_port}/panda.bin"
            }
            response = json.dumps(manifest, indent=2)
            self.wfile.write(response.encode())

            print(f"✓ 已发送manifest.json:")
            print(f"  版本: {manifest['version']}")
            print(f"  下载地址: {manifest['url']}")
            return

        # 其他请求使用默认处理
        super().do_GET()

    def log_message(self, format, *args):
        """自定义日志输出"""
        if "latest.json" in str(args):
            return  # latest.json已有详细日志
        print(f"[{self.client_address[0]}] {format % args}")

def main():
    parser = argparse.ArgumentParser(
        description='ESP32 OTA 测试服务器',
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
示例:
  python3 ota_test_server.py
  python3 ota_test_server.py --port 8000 --version 2.2 --firmware build/panda.bin

然后在设备的 main/ota_updater.c 中改成:
  #define OTA_MANIFEST_URL "http://YOUR_PC_IP:8000/latest.json"
        """)

    parser.add_argument('--port', type=int, default=8000,
                        help='HTTP服务器端口 (默认: 8000)')
    parser.add_argument('--version', default='2.2',
                        help='固件版本号 (默认: 2.2)')
    parser.add_argument('--firmware', default='build/panda.bin',
                        help='固件文件路径 (默认: build/panda.bin)')
    parser.add_argument('--ip', default=None,
                        help='绑定的IP地址 (默认: 自动检测)')

    args = parser.parse_args()

    # 检查固件文件
    firmware_path = Path(args.firmware)
    if not firmware_path.exists():
        print(f"❌ 错误: 找不到固件文件 {args.firmware}")
        print(f"   请确保已编译项目或指定正确的路径")
        sys.exit(1)

    # 获取本机IP
    local_ip = args.ip or get_local_ip()
    if local_ip == "127.0.0.1":
        print("⚠️  警告: 无法自动检测本机IP地址")
        print("   请手动指定: python3 ota_test_server.py --ip 192.168.x.x")
        local_ip = "127.0.0.1"

    # 显示配置信息
    print("=" * 60)
    print("🚀 ESP32 OTA 测试服务器")
    print("=" * 60)
    print(f"✓ IP地址:      {local_ip}")
    print(f"✓ 端口:        {args.port}")
    print(f"✓ 固件版本:    {args.version}")
    print(f"✓ 固件文件:    {firmware_path} ({firmware_path.stat().st_size} 字节)")
    print()
    print("📝 配置步骤:")
    print(f"1. 编辑 main/ota_updater.c 中的 OTA_MANIFEST_URL:")
    print(f'   #define OTA_MANIFEST_URL "http://{local_ip}:{args.port}/latest.json"')
    print()
    print(f"2. 重新编译固件:")
    print(f"   cmake --build build")
    print()
    print(f"3. 烧录到设备:")
    print(f"   cmake --build build -- flash")
    print()
    print(f"4. 在设备上点击 '立即检查更新' 按钮")
    print()
    print("=" * 60)
    print(f"✓ 服务器运行中... (按 Ctrl+C 停止)")
    print("=" * 60)
    print()

    # 启动服务器
    handler = OTAHTTPRequestHandler
    socketserver.TCPServer.allow_reuse_address = True

    with socketserver.TCPServer(("0.0.0.0", args.port), handler) as httpd:
        httpd.local_ip = local_ip
        httpd.server_port = args.port
        httpd.firmware_version = args.version

        try:
            httpd.serve_forever()
        except KeyboardInterrupt:
            print("\n\n✓ 服务器已停止")
            return 0

if __name__ == '__main__':
    sys.exit(main())
