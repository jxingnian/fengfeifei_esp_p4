<#
    文件：build/build.ps1
    作用：在 Windows PowerShell 中使用 ESP-IDF 6.1 构建 ESP32-P4 工程。

    说明：
    1. ESP-IDF 6.1 不支持从 MSYS/Git Bash 直接运行构建流程，因此 Windows
       环境必须使用本脚本或等价的 PowerShell 命令。
    2. RISC-V GCC 的 C++ 头文件包含路径较深时会触发 Windows 路径解析限制。
       本工程使用 C:\EspressifTools 作为 .espressif 工具目录的短路径入口，
       脚本会优先使用该入口，避免再次命中已经验证的路径错误。
    3. 构建产物统一放在 build\esp-idf-output，源码目录不会生成临时构建目录。
#>

[CmdletBinding()]
param(
    # 传入 -Clean 时调用 ESP-IDF 官方 fullclean，只清理构建产物。
    [switch]$Clean
)

$ErrorActionPreference = "Stop"

# 计算项目根目录，确保脚本从任意当前目录调用时都能定位工程。
$scriptDirectory = Split-Path -Parent $MyInvocation.MyCommand.Path
$projectDirectory = (Resolve-Path (Join-Path $scriptDirectory "..")).Path
$buildDirectory = Join-Path $scriptDirectory "esp-idf-output"

# 使用当前工程约定的 ESP-IDF 路径；也允许通过 IDF_PATH 覆盖。
$idfPath = if ($env:IDF_PATH) { $env:IDF_PATH } else { "D:\esp\v6.1-beta1\esp-idf" }
if (-not (Test-Path -LiteralPath (Join-Path $idfPath "export.ps1"))) {
    throw "未找到 ESP-IDF export.ps1：$idfPath。请设置 IDF_PATH。"
}

# 优先使用已经验证过的短路径工具根目录，避免 Windows 长路径头文件失败。
$shortToolsPath = "C:\EspressifTools"
if (Test-Path -LiteralPath (Join-Path $shortToolsPath "tools")) {
    $env:IDF_TOOLS_PATH = $shortToolsPath
} elseif (-not $env:IDF_TOOLS_PATH) {
    throw "未找到 C:\EspressifTools。请先建立指向 ESP-IDF 工具目录的短路径入口，或设置 IDF_TOOLS_PATH。"
}

$env:PYTHONUTF8 = "1"
Set-ExecutionPolicy -Scope Process -ExecutionPolicy Bypass -Force

# 加载 ESP-IDF 官方环境脚本，随后所有 idf.py 命令都使用同一套工具链。
Push-Location $projectDirectory
try {
    . (Join-Path $idfPath "export.ps1")

    if ($Clean) {
        idf.py -B $buildDirectory fullclean
    }

    idf.py -B $buildDirectory build
} finally {
    Pop-Location
}
