@echo off
REM ESP32S3电池监控配置切换批处理文件
echo ESP32S3电池监控系统配置切换工具
echo =======================================

REM 检查Python是否安装
python --version >nul 2>&1
if %errorlevel% neq 0 (
    echo 错误: 未找到Python，请先安装Python
    pause
    exit /b 1
)

echo.
echo 可选操作:
echo 1. 交互式配置
echo 2. 显示当前配置
echo 3. 切换到RS485模式
echo 4. 切换到JSON模式  
echo 5. 切换到混合模式
echo 6. 切换到基本模式
echo 7. 恢复备份
echo.

set /p choice="请选择操作 (1-7): "

if "%choice%"=="1" (
    python config_switcher.py
) else if "%choice%"=="2" (
    python config_switcher.py show
) else if "%choice%"=="3" (
    python config_switcher.py rs485
) else if "%choice%"=="4" (
    python config_switcher.py json
) else if "%choice%"=="5" (
    python config_switcher.py hybrid
) else if "%choice%"=="6" (
    python config_switcher.py basic
) else if "%choice%"=="7" (
    python config_switcher.py restore
) else (
    echo 无效选择
)

echo.
echo 完成！按任意键退出...
pause >nul