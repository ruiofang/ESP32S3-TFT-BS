@echo off
echo Checking for ESP-IDF environment...

REM Check if IDF_PATH is set
if "%IDF_PATH%"=="" (
    echo ESP-IDF environment not found. Trying to set up...
    if exist "C:\Espressif\frameworks\esp-idf-v5.1.2\export.bat" (
        call "C:\Espressif\frameworks\esp-idf-v5.1.2\export.bat"
    ) else (
        echo Please run this from ESP-IDF Command Prompt
        pause
        exit /b 1
    )
)

echo ESP-IDF environment ready
echo Building project...

idf.py build

if %errorlevel% neq 0 (
    echo Build failed with error code %errorlevel%
    pause
    exit /b %errorlevel%
) else (
    echo Build successful!
    pause
)
