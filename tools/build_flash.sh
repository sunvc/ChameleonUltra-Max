#!/bin/bash
# Build the application, package a DFU zip, enter DFU and flash it.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
APP_DIR="$ROOT/firmware/application"
TOOLCHAIN="${GNU_INSTALL_ROOT:-/Users/lynn/armgnu/expanded/Payload/bin/}"
GNU_VERSION="${GNU_VERSION:-15.3.1}"
OUTPUT="${1:-$ROOT/build/v2.3.0-bravo.zip}"
PYTHON="${PYTHON:-}"
if [ -z "$PYTHON" ]; then
    if [ -x /Users/lynn/cu-venv/bin/python ]; then
        PYTHON=/Users/lynn/cu-venv/bin/python
    else
        PYTHON=python3
    fi
fi

command -v nrfutil >/dev/null 2>&1 || { echo "找不到 nrfutil"; exit 1; }
[ -d "$TOOLCHAIN" ] || { echo "找不到工具链：$TOOLCHAIN"; exit 1; }

echo "==> 编译"
cd "$APP_DIR"
make -j8 GNU_INSTALL_ROOT="$TOOLCHAIN" GNU_VERSION="$GNU_VERSION"

echo "==> 打包 $OUTPUT"
mkdir -p "$(dirname "$OUTPUT")"
nrfutil pkg generate \
    --hw-version 0 \
    --sd-req 0x100 \
    --application "$ROOT/firmware/objects/application.bin" \
    --application-version 1 \
    --key-file "$ROOT/resource/dfu_key/chameleon.pem" \
    "$OUTPUT"

if ! nrfutil device list 2>&1 | grep -q "nordicDfu"; then
    echo "==> 进入 DFU"
    "$PYTHON" "$ROOT/tools/enter_dfu.py"
    for _ in $(seq 1 20); do
        if nrfutil device list 2>&1 | grep -q "nordicDfu"; then
            break
        fi
        sleep 0.5
    done
fi

if ! nrfutil device list 2>&1 | grep -q "nordicDfu"; then
    echo "设备未进入 DFU。可手动操作：拔 USB，按住 B 再插入，直到 LED 4/5 闪烁。"
    exit 1
fi

echo "==> 刷机"
nrfutil device program --firmware "$OUTPUT" --traits nordicDfu

echo "==> 等待设备重启"
for _ in $(seq 1 30); do
    if nrfutil device list 2>&1 | grep -q "fw_v515"; then
        break
    fi
    sleep 0.5
done
nrfutil device list || true

echo "==> 验证"
"$PYTHON" "$ROOT/tools/probe.py" 1017 || true
"$PYTHON" "$ROOT/tools/probe.py" 1034 || true
