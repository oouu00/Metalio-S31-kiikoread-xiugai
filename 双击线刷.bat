@REM SPDX-FileCopyrightText: 2026 zhuyue
@REM SPDX-License-Identifier: LicenseRef-Kiikoread-Personal-Study-1.0

@echo off
chcp 65001 >nul
REM Metalio-S31 一键线刷启动器（刷全部分区）。双击即用，串口自动探测。
setlocal
set "PROJ=%~dp0"
set "PY="
for /d %%d in ("%USERPROFILE%\.espressif\python_env\idf6.1_*_env") do (
    if exist "%%d\Scripts\python.exe" set "PY=%%d\Scripts\python.exe"
)
if not defined PY set "PY=%USERPROFILE%\.espressif\python_env\idf6.1_py3.13_env\Scripts\python.exe"
if not exist "%PY%" (
    echo [!!] 找不到 ESP-IDF v6.1 的 venv python：%PY%
    pause
    exit /b 2
)
"%PY%" "%PROJ%flash_partitions.py" %*
endlocal
