@echo off
chcp 65001 >nul
title ESP32S3-TFT-BS 控制器
echo.
echo ====================================
echo   ESP32S3-TFT-BS 控制器启动菜单
echo ====================================
echo.
echo 请选择启动模式:
echo   1. 完整交互式控制器 (推荐新手)
echo   2. 简化控制器 (快捷操作)
echo   3. 设备扫描工具 (检测可用设备)
echo   4. 退出
echo.
set /p choice=请输入选择 (1-4): 

if "%choice%"=="1" (
    echo.
    echo 🎮 启动完整交互式控制器...
    python esp32_controller.py
) else if "%choice%"=="2" (
    echo.
    echo ⚡ 启动简化控制器...
    python simple_controller.py
) else if "%choice%"=="3" (
    echo.
    echo 🔍 启动设备扫描工具...
    python device_scanner.py
) else if "%choice%"=="4" (
    echo.
    echo 👋 再见!
    goto end
) else (
    echo.
    echo ❌ 无效选择，请重新运行
    pause
    goto end
)

echo.
pause
:end
