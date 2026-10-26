# ESP32-P4 固件构建、烧录和串口监视脚本。
#
# 这个脚本统一从 ESP-IDF 的 export.bat 启动构建环境，避免直接在未配置环境的
# PowerShell 中调用 idf.py，造成 Python 环境、工具链和组件管理器不一致。
# 所有构建产物仍由 ESP-IDF 写入当前项目的 build 目录。

[CmdletBinding()]
param(
    # build 只编译；flash 会先编译再烧录；monitor 连接串口；flash-monitor 先烧录再监视。
    [ValidateSet('build', 'flash', 'monitor', 'flash-monitor')]
    [string]$Action = 'build',

    # 默认使用当前项目实际连接的 COM14，也允许通过参数覆盖串口。
    [string]$Port = 'COM14',

    # ESP-IDF 安装目录可通过参数覆盖，便于换电脑或升级版本。
    [string]$IdfPath = 'D:\esp\v6.1-beta1\esp-idf'
)

$ErrorActionPreference = 'Stop'
$projectPath = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$exportBat = Join-Path $IdfPath 'export.bat'

if (-not (Test-Path -LiteralPath $exportBat -PathType Leaf)) {
    throw "找不到 ESP-IDF 环境脚本：$exportBat"
}

if ($Action -ne 'build' -and [string]::IsNullOrWhiteSpace($Port)) {
    throw '烧录或串口监视操作必须提供有效串口，例如 COM14。'
}

# 通过同一个 cmd 进程执行 export.bat 和 idf.py，使 export.bat 设置的环境变量
# 对后续 idf.py 生效；项目路径和参数均使用引号，避免路径中包含空格时解析错误。
$idfArguments = switch ($Action) {
    'build' { 'build' }
    'flash' { "-p $Port flash" }
    'monitor' { "-p $Port monitor" }
    'flash-monitor' { "-p $Port flash monitor" }
}

$command = 'call "{0}" && cd /d "{1}" && idf.py {2}' -f $exportBat, $projectPath, $idfArguments
& cmd.exe /d /s /c $command
if ($LASTEXITCODE -ne 0) {
    throw "ESP-IDF 操作失败，退出码：$LASTEXITCODE"
}
