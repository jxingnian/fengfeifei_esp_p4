#!/bin/sh
set -eu
test_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
voice_test_lib=$(mktemp /tmp/voice-turn-test.XXXXXX.so)
cc -std=c11 -Wall -Wextra -Werror -ffreestanding -nostdlib -shared -fPIC \
  "$test_dir/voice-turn.test.c" -o "$voice_test_lib"
python3 - "$voice_test_lib" <<'PY'
import ctypes
import sys
result = ctypes.CDLL(sys.argv[1]).main()
print('Voice turn policy:', 'PASS' if result == 0 else f'FAIL at C line {result}')
sys.exit(result)
PY
