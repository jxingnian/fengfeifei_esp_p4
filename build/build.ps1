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

#
# 将本地 Coze 凭据作为 Kconfig 默认值注入。凭据文件位于 build 目录并被
# .gitignore 忽略，因此不会进入源码或版本库；每次配置构建都会重新使用它，
# 避免直接修改 sdkconfig 后被 Kconfig 重新生成覆盖。
#
$localCozeDefaults = Join-Path $projectPath 'build\coze.local.defaults'
$idfCmakeArguments = @()
if (Test-Path -LiteralPath $localCozeDefaults -PathType Leaf) {
    #
    # sdkconfig 中若保留旧的空字符串，Kconfig 会优先使用它而忽略默认值。
    # 因此在配置前按键名同步本地凭据，确保生成的 sdkconfig.h 与本地配置一致。
    #
    $sdkconfigPath = Join-Path $projectPath 'sdkconfig'
    $sdkconfigLines = @()
    if (Test-Path -LiteralPath $sdkconfigPath -PathType Leaf) {
        $sdkconfigLines = @(Get-Content -LiteralPath $sdkconfigPath -Encoding utf8)
    }
    foreach ($localLine in @(Get-Content -LiteralPath $localCozeDefaults -Encoding utf8)) {
        if ($localLine -match '^(CONFIG_VOICE_AI_BACKEND_COZE_DIRECT|CONFIG_VOICE_AI_COZE_BOT_ID|CONFIG_VOICE_AI_COZE_ACCESS_TOKEN|CONFIG_VOICE_AI_COZE_USER_ID)=') {
            $configKey = $Matches[1]
            $sdkconfigLines = @($sdkconfigLines | Where-Object { $_ -notmatch "^$configKey=" })
            $sdkconfigLines += $localLine
        }
    }
    [System.IO.File]::WriteAllLines(
        $sdkconfigPath,
        $sdkconfigLines,
        (New-Object System.Text.UTF8Encoding -ArgumentList $false))

    $idfCmakeArguments += "-DSDKCONFIG_DEFAULTS=$projectPath\sdkconfig.defaults;$localCozeDefaults"
}

# 组件覆盖源码位于项目 components 目录，脚本禁止修改 managed_components；
# 仅同步被忽略的 sdkconfig 凭据配置，组件管理器自动使用本地覆盖组件。
if (-not $env:ESP_IDF_VERSION) {
    $idfVersionDirectory = Split-Path (Split-Path $IdfPath -Parent) -Leaf
    if ($idfVersionDirectory.StartsWith('v')) {
        $env:ESP_IDF_VERSION = $idfVersionDirectory.Substring(1)
    }
}

Push-Location $projectPath
try {
    & $pythonPath $idfPyPath @idfCmakeArguments build
    if ($LASTEXITCODE -ne 0) {
        throw "ESP-IDF build 失败，退出码：$LASTEXITCODE"
    }
}
finally {
    Pop-Location
}
