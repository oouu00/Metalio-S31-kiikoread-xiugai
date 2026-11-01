@REM SPDX-FileCopyrightText: 2026 zhuyue
@REM SPDX-License-Identifier: LicenseRef-Kiikoread-Personal-Study-1.0

@echo off
setlocal
REM 双击即可编译，使用官方 ESP-IDF v6.1 安装好的 venv（默认在 %USERPROFILE%\.espressif）。
REM venv 目录名随系统 Python 版本变化（idf6.1_py3.12_env / idf6.1_py3.13_env），用通配探测。
set "PROJ=%~dp0"
set "IDF_TOOLS_PATH=%USERPROFILE%\.espressif"
set "VENV="
for /d %%d in ("%IDF_TOOLS_PATH%\python_env\idf6.1_*_env") do set "VENV=%%d"
if not defined VENV (
    echo [!] 未找到官方 ESP-IDF venv，请先运行 C:\Users\zhuyue\Desktop\gujian (2)\esp-idf-v6.1\install.bat
    pause
    exit /b 1
)
set "PY=%VENV%\Scripts\python.exe"
if not exist "%PY%" (
    echo [!] venv python 未找到: %PY%
    pause
    exit /b 1
)
"%PY%" "%PROJ%build_metalio.py" %*
endlocal
