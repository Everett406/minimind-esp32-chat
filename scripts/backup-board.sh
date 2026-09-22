#!/usr/bin/env bash
# 备份当前 ESP32-S3 flash 全部 4 段
# Usage: ./backup-board.sh [PORT] [--baseline]

set -e

REPO="$(cd "$(dirname "$0")/.." && pwd)"
BACKUP_ROOT="$REPO/backups"
STAMP=$(date +%Y-%m-%d-%H%M)
DEST="$BACKUP_ROOT/$STAMP"
mkdir -p "$DEST"

ESPTOOL="${ESPTOOL:-esptool.py}"
PORT="${1:-/dev/ttyUSB0}"

# 默认 web-chat 布局
MODEL_OFFSET="0x150000"
MODEL_SIZE="0xEA0000"
if [ "${2:-}" = "--baseline" ]; then
  MODEL_OFFSET="0x120000"
  MODEL_SIZE="0xED0000"
fi

echo -e "\033[36m=== Backup ESP32-S3 Flash ===\033[0m"
echo "Port: $PORT"
echo "Output: $DEST"
echo "Model: @ $MODEL_OFFSET size $MODEL_SIZE"
echo ""

echo -e "\033[33m[1/4] bootloader @ 0x0000 ...\033[0m"
"$ESPTOOL" --chip esp32s3 --port "$PORT" --baud 921600 \
  --before default-reset --after no-reset \
  read-flash 0x0000 0x6000 "$DEST/bootloader.bin"

echo -e "\033[33m[2/4] partitions @ 0x8000 ...\033[0m"
"$ESPTOOL" --chip esp32s3 --port "$PORT" --baud 921600 \
  --before default-reset --after no-reset \
  read-flash 0x8000 0x1000 "$DEST/partitions.bin"

echo -e "\033[33m[3/4] app @ 0x10000 ...\033[0m"
"$ESPTOOL" --chip esp32s3 --port "$PORT" --baud 921600 \
  --before default-reset --after no-reset \
  read-flash 0x10000 0x140000 "$DEST/app.bin"

echo -e "\033[33m[4/4] model @ $MODEL_OFFSET ...\033[0m"
"$ESPTOOL" --chip esp32s3 --port "$PORT" --baud 921600 \
  --before default-reset --after no-reset \
  read-flash "$MODEL_OFFSET" "$MODEL_SIZE" "$DEST/model.bin"

echo ""
echo -e "\033[32m✓ 备份完成: $DEST\033[0m"
ls -lh "$DEST"
