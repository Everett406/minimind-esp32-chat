#!/usr/bin/env bash
# 烧录基线版 (1.04MB 串口 chat) 固件
# 基线版 model @ 0x120000 (不是 0x150000)

set -e

REPO="$(cd "$(dirname "$0")/.." && pwd)"
FRM_DIR="$REPO/firmware/baseline-serial-chat"
ESPTOOL="${ESPTOOL:-esptool.py}"
PORT="${1:-/dev/ttyUSB0}"

echo -e "\033[36m=== MiniMind ESP32-S3 Baseline Flash ===\033[0m"

"$ESPTOOL" --chip esp32s3 --port "$PORT" --baud 921600 \
  --before default-reset --after no-reset \
  write-flash 0x0000 "$FRM_DIR/bootloader.bin"

"$ESPTOOL" --chip esp32s3 --port "$PORT" --baud 921600 \
  --before default-reset --after no-reset \
  write-flash 0x8000 "$FRM_DIR/partitions.bin"

"$ESPTOOL" --chip esp32s3 --port "$PORT" --baud 921600 \
  --before default-reset --after no-reset \
  write-flash 0x10000 "$FRM_DIR/app.bin"

"$ESPTOOL" --chip esp32s3 --port "$PORT" --baud 921600 \
  --before default-reset --after hard-reset \
  write-flash 0x120000 "$FRM_DIR/model.bin"

echo -e "\033[32m✓ 烧录完成!\033[0m"
echo "拔 USB 等 2 秒再插, 串口监视器 (115200) 看输出"
