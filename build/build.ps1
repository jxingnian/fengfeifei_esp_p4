# ESP32-P4 固件构建脚本。
# 本脚本只执行正式编译，不执行烧录、串口监视或任何设备操作。

[CmdletBinding()]
param(
    [string]$IdfPath = 'D:\esp\v6.1-beta1\esp-idf'
)

$ErrorActionPreference = 'Stop'
$projectPath = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$pythonPath = 'D:\Espressif\tools\python\v6.1-beta1\venv\Scripts\python.exe'
$idfPyPath = Join-Path $IdfPath 'tools\idf.py'

if (-not (Test-Path -LiteralPath $pythonPath -PathType Leaf)) {
    throw "找不到 ESP-IDF Python：$pythonPath"
}
if (-not (Test-Path -LiteralPath $idfPyPath -PathType Leaf)) {
    throw "找不到 idf.py：$idfPyPath"
}

# 在干净 PowerShell 中显式加入 ESP-IDF 构建工具，避免依赖用户当前 PATH。
$toolBinPaths = @(
    'D:\Espressif\tools\cmake\4.0.3\bin',
    'D:\Espressif\tools\ninja\1.12.1',
    'D:\Espressif\tools\riscv32-esp-elf\esp-15.2.0_20251204\bin',
    'D:\Espressif\tools\xtensa-esp-elf\esp-15.2.0_20251204\bin',
    'D:\Espressif\tools\esp-clang\esp-21.1.3_20260408\bin'
)
$existingToolPaths = $toolBinPaths | Where-Object { Test-Path -LiteralPath $_ }
$env:Path = (($existingToolPaths -join ';') + ';' + $env:Path)
$env:IDF_PATH = $IdfPath
$env:IDF_PYTHON_ENV_PATH = Split-Path $pythonPath -Parent | Split-Path -Parent
$env:PYTHONUTF8 = '1'

# Coze 令牌只从本地 sdkconfig 读取，源码和默认配置不保存敏感凭据。
Push-Location $projectPath
try {
    & $pythonPath $idfPyPath build
    if ($LASTEXITCODE -ne 0) {
        throw "ESP-IDF build 失败，退出码：$LASTEXITCODE"
    }
}
finally {
    Pop-Location
}
