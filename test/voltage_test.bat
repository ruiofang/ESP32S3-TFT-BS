@echo off
chcp 65001 > nul
echo 🔋 ESP32电压控制功能测试
echo ========================
echo.

REM 检查Python是否安装
python --version > nul 2>&1
if errorlevel 1 (
    echo ❌ Python未安装或不在PATH中
    echo 请先安装Python 3.x
    pause
    exit /b 1
)

REM 检查pyserial是否安装
python -c "import serial" > nul 2>&1
if errorlevel 1 (
    echo ⚠️ pyserial未安装，正在安装...
    pip install pyserial
    if errorlevel 1 (
        echo ❌ pyserial安装失败
        pause
        exit /b 1
    )
)

echo ✅ 环境检查完成
echo.

REM 运行测试脚本
echo 🚀 启动电压控制测试...
echo.
python "%~dp0voltage_control_test.py" %*

echo.
echo 📊 测试完成
pause