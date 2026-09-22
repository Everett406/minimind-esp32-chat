#!/usr/bin/env bash
# 烧录 Web Chat 版固件到 ESP32-S3 (元旦 N16R8 板)
# 按顺序烧 4 段: bootloader + partitions + app + model
#
# Usage: ./burn-web-chat.sh [PORT]
#   默认 /dev/ttyUSB0

set -e

REPO="$(cd "$(dirname "$0")/.." && pwd)"
FRM_DIR="$REPO/firmware/web-chat"
ESPTOOL="${ESPTOOL:-esptool.py}"
PORT="${1:-/dev/ttyUSB0}"

echo -e "\033[36m=== MiniMind ESP32-S3 Web Chat Flash ===\033[0m"
echo "Port: $PORT"
echo "Firmware: $FRM_DIR"
echo ""

echo -e "\033[33m[1/4] bootloader @ 0x0000 ...\033[0m"
"$ESPTOOL" --chip esp32s3 --port "$PORT" --baud 921600 \
  --before default-reset --after no-reset \
  write-flash 0x0000 "$FRM_DIR/bootloader.bin"

echo -e "\033[33m[2/4] partitions @ 0x8000 ...\033[0m"
"$ESPTOOL" --chip esp32s3 --port "$PORT" --baud 921600 \
  --before default-reset --after no-reset \
  write-flash 0x8000 "$FRM_DIR/partitions.bin"

echo -e "\033[33m[3/4] app @ 0x10000 (~12s) ...\033[0m"
"$ESPTOOL" --chip esp32s3 --port "$PORT" --baud 921600 \
  --before default-reset --after no-reset \
  write-flash 0x10000 "$FRM_DIR/app.bin"

echo -e "\033[33m[4/4] model @ 0x150000 (~40-160s) ...\033[0m"
"$ESPTOOL" --chip esp32s3 --port "$PORT" --baud 921600 \
  --before default-reset --after hard-reset \
  write-flash 0x150000 "$FRM_DIR/model.bin"

echo ""
echo -e "\033[32m✓ 烧录完成!\033[0m"
echo ""
echo -e "\033[35m下一步: 拔 USB 等 2 秒再插 (CH343 DTR reset 不可靠)\033[0m"
echo -e "\033[35m        连 WiFi 'YuanDi-S3-MiniMind' (无密码)\033[0m"
echo -e "\033[35m        浏览器开 http://192.168.4.1\033[0m"
