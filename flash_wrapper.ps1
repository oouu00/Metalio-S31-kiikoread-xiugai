# SPDX-FileCopyrightText: 2026 zhuyue
# SPDX-License-Identifier: LicenseRef-Kiikoread-Personal-Study-1.0

# 项目根目录取本脚本所在位置，避免写死路径导致换机器/改名后失效
$ProjectRoot = $PSScriptRoot

# 不 dot-source export.ps1：它会按 PATH 的 python 版本猜虚拟环境目录，猜错就整段报错。
# 下面的 esptool 用固定 venv 里的 python -m esptool 直接跑，不需要它导出的环境。
# Do not dot-source export.ps1 (it guesses the venv from PATH and aborts on a mismatch);
# python -m esptool from the pinned venv needs nothing from it.
if (-not $env:IDF_TOOLS_PATH) { $env:IDF_TOOLS_PATH = Join-Path $env:USERPROFILE ".espressif" }
Set-Location "$ProjectRoot\build"
& "$env:USERPROFILE\.espressif\python_env\idf6.1_py3.12_env\Scripts\python.exe" -m esptool --chip esp32s31 -p COM4 -b 460800 --before default-reset --after hard-reset --no-stub write-flash "@flash_args" *> "$ProjectRoot\flash.log"
"METALIO_FLASH_EXITCODE=$LASTEXITCODE" | Out-File -Append -Encoding utf8 "$ProjectRoot\flash.log"
