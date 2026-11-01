@REM SPDX-FileCopyrightText: 2026 zhuyue
@REM SPDX-License-Identifier: LicenseRef-Kiikoread-Personal-Study-1.0

@echo off
REM 双击即可：加载 ESP-IDF v6.1 环境 -> 选择版本 -> 编译 Metalio S31（rc108）-> 出 TF 卡升级包 -> 写 build.log
REM 窗口在出错时停住（pause），方便看报错。
setlocal enabledelayedexpansion
set "PROJ=%~dp0"
REM 去掉末尾反斜杠，方便后面拼接路径
if "%PROJ:~-1%"=="\" set "PROJ=%PROJ:~0,-1%"

echo ============================================================
echo   Metalio S31 一键编译  （版本 rc108）
echo ============================================================
echo.
echo   请选择要编译的固件版本：
echo.
echo     1 = release   正式版（日志最少、体积小，推荐日常使用）
echo     2 = debug     调试版（含全部日志与诊断输出，便于排查问题）
echo.
echo   直接回车默认编译 release。
echo.
set "PROFILE=release"
set /p "SEL=请输入 1 或 2 ："
if /i "%SEL%"=="2" set "PROFILE=debug"
if /i "%SEL%"=="debug" set "PROFILE=debug"

REM 根据版本决定构建产物目录（release->build，debug->build-debug）
set "BDIR=build"
if /i "%PROFILE%"=="debug" set "BDIR=build-debug"

echo.
echo [i] 已选择：%PROFILE%  （构建目录：%BDIR%）
echo [i] 编译入口：%PROJ%\build_wrapper.ps1
echo [i] 产物：%PROJ%\Metalio-S31-update.bin / %BDIR%\Kiikoread_Metalio_S31.bin
echo.
@REM -Sd 必须传：不传就不会生成 TF 卡升级包 Metalio-S31-update.bin，下面复制必然失败。
@REM Always pass -Sd; without it the TF-card package is never produced and the copy below fails.
powershell -NoProfile -ExecutionPolicy Bypass -File "%PROJ%\build_wrapper.ps1" -Profile %PROFILE% -Sd
set "RC=%errorlevel%"
echo.
if not "%RC%"=="0" (
    echo [!!] 编译失败，exit=%RC%
    echo [i] 未复制任何文件，发布目录保持不变。
    goto :done
)
echo [OK] 编译成功，exit=%RC%
set "OUT=%PROJ%\Metalio-Release"
if not exist "!OUT!" mkdir "!OUT!"
echo [i] 复制到发布目录 !OUT! ...
set "MISS=0"
call :cp "%PROJ%\Metalio-S31-update.bin"
call :cp "%PROJ%\%BDIR%\Metalio-S31-combined.bin"
call :cp "%PROJ%\%BDIR%\Kiikoread_Metalio_S31.bin"
call :cp "%PROJ%\%BDIR%\bootloader\bootloader.bin"
call :cp "%PROJ%\%BDIR%\partition_table\partition-table.bin"
call :cp "%PROJ%\%BDIR%\ota_data_initial.bin"
if "!MISS!"=="0" (echo [OK] 已复制到 !OUT!) else (echo [!!] 有 !MISS! 个产物缺失，其余已复制，详见 build.log)
:done
echo [i] 详见 %PROJ%\build.log
pause
endlocal
exit /b 0

:cp
if exist "%~1" (
    copy /Y "%~1" "!OUT!\" >nul
    echo     + %~nx1
) else (
    echo [WARN] 产物缺失，跳过: %~nx1
    set /a MISS+=1
)
exit /b 0
