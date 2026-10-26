# ESP32-P4 固件编译脚本。
# 本脚本只执行依赖修复和 idf.py build，不包含 flash、monitor 或任何串口操作。

[CmdletBinding()]
param(
    [string]$IdfPath = 'D:\esp\v6.1-beta1\esp-idf'
)

$ErrorActionPreference = 'Stop'
$projectPath = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$pythonPath = 'D:\Espressif\tools\python\v6.1-beta1\venv\Scripts\python.exe'
$idfPyPath = Join-Path $IdfPath 'tools\idf.py'

if (-not (Test-Path -LiteralPath $pythonPath -PathType Leaf)) { throw "找不到 ESP-IDF Python：$pythonPath" }
if (-not (Test-Path -LiteralPath $idfPyPath -PathType Leaf)) { throw "找不到 idf.py：$idfPyPath" }

# 依赖目录已经由 ESP-IDF 解析后，先应用项目固定补丁，再只执行 build。
# 使用独立 PowerShell 子进程执行补丁，并显式绕过脚本执行策略；此处不调用烧录或串口命令。
& powershell.exe -NoProfile -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot 'prepare-managed-components.ps1')
if ($LASTEXITCODE -ne 0) { throw ('第三方组件兼容补丁失败，退出码：' + $LASTEXITCODE) }

$env:IDF_PATH = $IdfPath
$env:IDF_PYTHON_ENV_PATH = Split-Path $pythonPath -Parent | Split-Path -Parent
# 某些未执行 export.ps1 的环境不会提供 ESP_IDF_VERSION；从默认目录名 v6.1-beta1 补齐它。
if (-not $env:ESP_IDF_VERSION) {
    $idfVersionDirectory = Split-Path (Split-Path $IdfPath -Parent) -Leaf
    if ($idfVersionDirectory.StartsWith('v')) { $env:ESP_IDF_VERSION = $idfVersionDirectory.Substring(1) }
}
$env:PYTHONUTF8 = '1'
Push-Location $projectPath
try {
    & $pythonPath $idfPyPath build
    if ($LASTEXITCODE -ne 0) { throw "ESP-IDF build 失败，退出码：$LASTEXITCODE" }
}
finally {
    Pop-Location
}
