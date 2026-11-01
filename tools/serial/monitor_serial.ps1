# SPDX-FileCopyrightText: 2026 zhuyue
# SPDX-License-Identifier: LicenseRef-Kiikoread-Personal-Study-1.0

. "C:\esp\idf-v6.1\export.ps1"
Set-Location "C:\esp\Metalio-S31_0.3.3-rc107.1_src"
$py="$env:USERPROFILE\.espressif\python_env\idf6.1_py3.12_env\Scripts\python.exe"
# 复位设备使其重启并打印启动日志
& $py "C:\esp\idf-v6.1\components\esptool_py\esptool\esptool.py" -p COM4 run 2>$null
Start-Sleep -Seconds 1
Add-Type -AssemblyName System.IO.Ports
$port=New-Object System.IO.Ports.SerialPort "COM4",115200,"None",8,"One"
$port.Open()
$sb=New-Object System.Text.StringBuilder
$start=Get-Date
while((Get-Date)-$start -lt [TimeSpan]::FromSeconds(25)){
  if($port.BytesToRead -gt 0){ $b=$port.ReadExisting(); $sb.Append($b) > $null }
  Start-Sleep -Milliseconds 50
}
$port.Close()
$sb.ToString() | Out-File "C:\esp\Metalio-S31_0.3.3-rc107.1_src\serial.log" -Encoding utf8
Write-Host "captured_chars=$($sb.Length)"
