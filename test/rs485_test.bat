@echo off
REM ESP32S3 RS485电池通信测试批处理文件
echo 启动ESP32S3 RS485电池通信测试...
echo.

REM 检查Python是否安装
python --version >nul 2>&1
if %errorlevel% neq 0 (
    echo 错误: 未找到Python，请先安装Python
    pause
    exit /b 1
)

REM 检查pyserial是否安装
python -c "import serial" >nul 2>&1
if %errorlevel% neq 0 (
    echo 安装pyserial库...
    pip install pyserial
)

REM 检查串口参数
set /p PORT="请输入串口号 (默认COM3): "
if "%PORT%"=="" set PORT=COM3

echo.
echo 开始测试RS485电池通信功能...
echo 串口: %PORT%
echo 波特率: 115200
echo.

REM 运行测试
python battery_rs485_test.py %PORT%

echo.
echo 测试完成，按任意键退出...
pause >nul