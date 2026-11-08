@echo off
rem ============================================================
rem  BOX0 只更新应用区(0x10000), 不丢 NVS 配置
rem  仅适用于已经在跑本项目固件的设备；从官方八合一/小智固件刷过来请用 flash_full.bat
rem  用法:  flash_app.bat COM8
rem         flash_app.bat COM8 "D:\tools\esptool.exe"   (自定义 esptool 路径)
rem
rem  ⚠ 本板硬件特性: 烧录完成后 esptool 的软复位不会启动固件,
rem    必须【手动点一下 RESET】(不按任何键)。正常运行时不暴露 COM 口。
rem    日常升级请优先用网页「升级」标签页(OTA), 无需 USB。
rem ============================================================
setlocal
set "PORT=%~1"
if "%PORT%"=="" set /p PORT=请输入 COM 口(例如 COM8): 

set "ESPTOOL=%~2"
if "%ESPTOOL%"=="" set "ESPTOOL=%LOCALAPPDATA%\Arduino15\packages\esp32\tools\esptool_py\5.3.1\esptool.exe"
if not exist "%ESPTOOL%" (
  echo [ERR] 未找到 esptool.exe: %ESPTOOL%
  echo       请把 esptool.exe 完整路径作为第二个参数传入, 例如:
  echo       flash_app.bat COM8 "D:\tools\esptool.exe"
  goto :end
)

set "BIN=%~dp0firmware\box0_stock_v7.1.bin"
if not exist "%BIN%" (
  echo [ERR] 固件不存在: %BIN%
  goto :end
)

echo.
echo  Port : %PORT%
echo  Image: %BIN%  -^> 0x10000
echo.
"%ESPTOOL%" --chip esp32s3 -p %PORT% -b 460800 --before default_reset --after no_reset ^
  write_flash --flash-mode dio --flash-freq 80m --flash-size 16MB 0x10000 "%BIN%"
if errorlevel 1 goto :fail

echo.
echo [OK] 应用已写入并校验通过, 配置保留。
echo [重要] 现在请手动点一下 RESET 键(不要按住任何键), 设备才会启动。
goto :end

:fail
echo.
echo [FAIL] 请按住右键 + 点 RESET 进入下载模式后重试。

:end
echo.
pause
