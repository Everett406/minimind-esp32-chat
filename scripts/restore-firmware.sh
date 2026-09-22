#!/usr/bin/env bash
# 从 backups/<时间戳>/ 还原固件
# Usage: ./restore-firmware.sh backups/2026-08-29-1530 [PORT] [MODEL_OFFSET]

set -e

REPO="$(cd "$(dirname "$0")/.." && pwd)"
BACKUP_DIR="$1"
PORT="${2:-/dev/ttyUSB0}"
MODEL_OFFSET="${3:-0x150000}"  # 默认 web-chat 布局

ESPTOOL="${ESPTOOL:-esptool.py}"

if [ -z "$BACKUP_DIR" ] || [ ! -d "$BACKUP_DIR" ]; then
  echo "Usage: $0 <BACKUP_DIR> [PORT] [MODEL_OFFSET]"
  echo "Available backups:"
  ls "$REPO/backups/" 2>/dev/null || echo "  (none)"
  exit 1
fi

for f in bootloader.bin partitions.bin app.bin model.bin; do
  if [ ! -f "$BACKUP_DIR/$f" ]; then
    echo "Missing: $BACKUP_DIR/$f"
    exit 1
  fi
done

echo -e "\033[36m=== Restore Firmware ===\033[0m"
echo "From: $BACKUP_DIR"
echo "Port: $PORT"
echo "Model: @ $MODEL_OFFSET"

"$ESPTOOL" --chip esp32s3 --port "$PORT" --baud 921600 \
  --before default-reset --after no-reset \
  write-flash 0x0000 "$BACKUP_DIR/bootloader.bin"

"$ESPTOOL" --chip esp32s3 --port "$PORT" --baud 921600 \
  --before default-reset --after no-reset \
  write-flash 0x8000 "$BACKUP_DIR/partitions.bin"

"$ESPTOOL" --chip esp32s3 --port "$PORT" --baud 921600 \
  --before default-reset --after no-reset \
  write-flash 0x10000 "$BACKUP_DIR/app.bin"

"$ESPTOOL" --chip esp32s3 --port "$PORT" --baud 921600 \
  --before default-reset --after hard-reset \
  write-flash "$MODEL_OFFSET" "$BACKUP_DIR/model.bin"

echo ""
echo -e "\033[32m✓ 还原完成!\033[0m"
echo "拔 USB 等 2 秒再插"
