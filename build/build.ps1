# ESP32-P4 固件正式编译脚本。
# 本脚本只负责配置构建环境并执行 idf.py build，不执行烧录、监视器或串口操作。

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

# ESP-IDF 的 Python 环境不会自动把 CMake、Ninja 和交叉编译器加入当前
# PowerShell 进程；这里显式设置工具链，保证在干净终端中能够重复构建。
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

# 组件覆盖源码位于项目 components 目录，禁止脚本修改 managed_components、
# sdkconfig 或其他生成文件中的源代码；组件管理器会自动使用本地覆盖组件。
if (-not $env:ESP_IDF_VERSION) {
    $idfVersionDirectory = Split-Path (Split-Path $IdfPath -Parent) -Leaf
    if ($idfVersionDirectory.StartsWith('v')) {
        $env:ESP_IDF_VERSION = $idfVersionDirectory.Substring(1)
    }
}

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
