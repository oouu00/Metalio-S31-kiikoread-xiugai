# SPDX-FileCopyrightText: 2026 zhuyue
# SPDX-License-Identifier: LicenseRef-Kiikoread-Personal-Study-1.0

param(
    [ValidateSet("release", "debug")] [string]$Profile = "release",
    # 是否同时生成 TF 卡升级包 Metalio-S31-update.bin（默认不生成，仅单文件线刷镜像即可）
    [switch]$Sd,
    # 工程目录默认取本脚本所在目录，改名/换路径都不受影响
    [string]$ProjectDir = $PSScriptRoot
)

# ESP-IDF 环境：不再 dot-source export.ps1。它会按 PATH 里排在最前的 python 版本去猜虚拟环境目录
# （本机装的是 idf6.1_py3.12_env，PATH 首位若是 3.13 就猜成 idf6.1_py3.13_env），猜错就报
# "ESP-IDF Python virtual environment ... not found"、$idf_exports 为空，双击时一屏看不懂的红字；
# 真正的编译由 build_metalio.py 自己定位 IDF_PATH / venv / 工具链，这里只固定两个环境变量。
# ESP-IDF env: do NOT dot-source export.ps1 here. It guesses the venv from the first python on PATH
# and aborts with "virtual environment ... not found" (a wall of red text on a double click).
# build_metalio.py resolves IDF_PATH / venv / toolchain itself; just pin the two variables.
if (-not $env:IDF_PATH -and (Test-Path "C:\esp\idf-v6.1\tools\idf.py")) {
    $env:IDF_PATH = "C:\esp\idf-v6.1"
}
if (-not $env:IDF_TOOLS_PATH) { $env:IDF_TOOLS_PATH = Join-Path $env:USERPROFILE ".espressif" }

# 切到工程目录：用脚本自身所在目录，保证可移植
Set-Location $ProjectDir

$py      = "$env:USERPROFILE\.espressif\python_env\idf6.1_py3.12_env\Scripts\python.exe"
$buildPy = Join-Path $ProjectDir "build_metalio.py"
$logFile = Join-Path $ProjectDir "build.log"

Write-Host "[i] 工程目录: $ProjectDir"
Write-Host "[i] 编译档:   $Profile"
Write-Host "[i] 日志写入: $logFile"

# 参数一律用「字符串数组」累加再 splat。
# 别写成 `$sdArg = if ($Sd) { @("--sd") } else { @() }`：单元素数组会被 if 展开成标量字符串，
# `@sdArg` 再按【字符】splat，python 收到的是 `- - s d`（这就是之前 debug+SD 模式的报错）。
# Build the argument list as a real string array; a single-element array assigned from an `if`
# collapses to a scalar, and splatting that scalar sends one argument per character.
[string[]]$pyArgs = @($buildPy, "--profile", $Profile)
if ($Sd) { $pyArgs += "--sd" }

# 默认只生成单文件线刷镜像（build/make_combined）；加 -Sd 才额外生成 TF 卡升级包
Write-Host "[i] 构建参数: $($pyArgs -join ' ')"

& $py @pyArgs *> $logFile
$rc = $LASTEXITCODE

# 必须显式 exit：不写 exit 时 PowerShell 子进程一律返回 0，
# 会让 双击编译.bat/.py 在编译失败时仍然打印「编译成功」，接着复制一堆不存在的产物。
# Exit explicitly: without this the wrapper always reports 0, so callers treat a failed
# build as success and then fail to copy artifacts that were never produced.
if ($rc -ne 0) {
    Write-Host "[!!] 编译失败 exit=$rc（详见 $logFile）"
} else {
    Write-Host "[OK] 编译成功（详见 $logFile）"
}
exit $rc
