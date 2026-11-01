# SPDX-FileCopyrightText: 2026 zhuyue
# SPDX-License-Identifier: LicenseRef-Kiikoread-Personal-Study-1.0

. "C:\esp\idf-v6.1\export.ps1"
Set-Location "C:\esp\Metalio-S31_0.3.3-rc107.1_src"
$env:PYTHONUTF8="1"
& "$env:USERPROFILE\.espressif\python_env\idf6.1_py3.12_env\Scripts\python.exe" "C:\esp\idf-v6.1\tools\idf.py" "-p","COM4","monitor","--reset" *> "C:\esp\Metalio-S31_0.3.3-rc107.1_src\monitor.log"
