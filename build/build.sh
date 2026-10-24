#!/usr/bin/env bash

# 该脚本必须在已经加载 ESP-IDF 环境的终端中执行，例如先运行 export.sh。
# 脚本固定从项目根目录执行 idf.py，避免在其他工作目录调用时找不到组件。
# 编译产物固定放在 build/esp-idf-output，不会覆盖源码和配置文件。

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"

cd "${PROJECT_DIR}"

if ! command -v idf.py >/dev/null 2>&1; then
    echo "错误：未找到 idf.py，请先加载 ESP-IDF 环境。" >&2
    exit 1
fi

# 将 CMake、Ninja 和固件产物集中放入 build/esp-idf-output，避免与本脚本混放。
# 工程目标已经由 sdkconfig.defaults 固定为 esp32p4，不在每次构建时执行
# set-target，避免该命令清理已有配置和增量编译产物。
idf.py -B "${SCRIPT_DIR}/esp-idf-output" build
