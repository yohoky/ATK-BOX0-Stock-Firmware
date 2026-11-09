@echo off
rem ============================================================
rem  BOX0 首次刷机 / 从官方八合一固件、小智固件刷过来 —— 完整镜像
rem  用法:  flash_full.bat COM8
rem         flash_full.bat COM8 "D:\tools\esptool.exe"   (自定义 esptool 路径)
rem
rem  完整镜像自带 bootloader + 本工程分区表 + otadata + app,
rem  从 0x0 一次写入；会先整片擦除，所以 WiFi/自选股等配置会清空。
rem  已经跑着本项目固件的设备，请改用 flash_app.bat 或网页 OTA。
rem
rem  进下载模式: 按住【右键】不放, 点一下 RESET, 松开右键; 设备管理器出现 COM 口。
rem  ⚠ 本板硬件特性: 烧录完成后必须【手动点一下 RESET】(不按任何键)才会启动,
rem    软件复位/--after hard_reset 一律无效。正常运行时不暴露 COM 口。
rem ============================================================
setlocal
set "PORT=%~1"
if "%PORT%"=="" set /p PORT=请输入 COM 口(例如 COM8):

set "ESPTOOL=%~2"
if "%ESPTOOL%"=="" set "ESPTOOL=%LOCALAPPDATA%\Arduino15\packages\esp32\tools\esptool_py\5.3.1\esptool.exe"
if not exist "%ESPTOOL%" (
  echo [ERR] 未找到 esptool.exe: %ESPTOOL%
  echo       请把 esptool.exe 完整路径作为第二个参数传入。
  goto :end
)

set "BIN=%~dp0firmware\box0_stock_v7.4_full_0x0.bin"
if not exist "%BIN%" (
  echo [ERR] 完整固件不存在: %BIN%
  goto :end
)

echo.
echo  Port : %PORT%
echo  Image: v7.4 完整镜像  -^> 0x0  (3.1 MB, 含 bootloader + 分区表)
echo  这一步会整片擦除 Flash, 配置将恢复出厂。
echo.
choice /C YN /M "确认继续"
if errorlevel 2 goto :end

"%ESPTOOL%" --chip esp32s3 -p %PORT% -b 460800 --before default_reset --after no_reset erase_flash
if errorlevel 1 goto :fail

"%ESPTOOL%" --chip esp32s3 -p %PORT% -b 460800 --before no_reset --after no_reset ^
  write_flash --flash-mode dio --flash-freq 80m --flash-size 16MB 0x0 "%BIN%"
if errorlevel 1 goto :fail

echo.
echo [OK] 完整固件已写入并校验通过。
echo [重要] 现在请手动点一下 RESET 键(不要按住任何键), 设备才会启动。
echo        首次启动会开热点 BOX0-XXXX, 连上后打开 http://192.168.4.1 配网。
goto :end

:fail
echo.
echo [FAIL] 请按住右键 + 点 RESET 重新进入下载模式后重试。

:end
echo.
pause
