#!/usr/bin/env bash
# 从 source/ 用 arduino-cli 编译
# Usage: ./build.sh [BUILD_DIR]

set -e

REPO="$(cd "$(dirname "$0")/.." && pwd)"
SRC="$REPO/source"
BUILD_DIR="${1:-$REPO/build}"

# 完整 fqbn: 必须含 PSRAM=opi, FlashSize=16M, PartitionScheme=custom
FQBN="esp32:esp32:esp32s3:UploadSpeed=921600,USBMode=default,CDCOnBoot=default,UploadMode=default,CPUFreq=240,FlashMode=dio,FlashSize=16M,PartitionScheme=custom,PSRAM=opi,DebugLevel=none"

if ! command -v arduino-cli >/dev/null 2>&1; then
  echo "ERROR: arduino-cli not found. Install: https://arduino.github.io/arduino-cli/"
  exit 1
fi

echo "=== Build source/ with arduino-cli ==="
echo "FQBN: $FQBN"
echo "Build dir: $BUILD_DIR"
echo ""

arduino-cli compile \
  --fqbn "$FQBN" \
  --build-path "$BUILD_DIR" \
  "$SRC"

echo ""
echo -e "\033[32m✓ 编译完成!\033[0m"
echo "Output: $BUILD_DIR/YuanDiArduino.ino.bin"
ls -lh "$BUILD_DIR"/YuanDiArduino.ino.bin 2>/dev/null || true
