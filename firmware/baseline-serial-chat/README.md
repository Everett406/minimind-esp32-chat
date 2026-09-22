# firmware/baseline-serial-chat/

**基线版** — 1.04 MB 串口 chat（无 Web / AP）

来自原始 [zhuhai-esp/ESP32-S3-YuanDi-Board](https://github.com/zhuhai-esp/ESP32-S3-YuanDi-Board) 项目 `YuanDi-S3-MiniMind2` 子项目，未经修改。

## 烧录

```bash
# 用 scripts/burn-baseline.sh 或 .ps1, 或手动:

esptool.py --chip esp32s3 --port COM4 --baud 921600 \
  --before default-reset --after no-reset write-flash 0x0000 bootloader.bin

esptool.py --chip esp32s3 --port COM4 --baud 921600 \
  --before default-reset --after no-reset write-flash 0x8000 partitions.bin

esptool.py --chip esp32s3 --port COM4 --baud 921600 \
  --before default-reset --after no-reset write-flash 0x10000 app.bin

esptool.py --chip esp32s3 --port COM4 --baud 921600 \
  --before default-reset --after hard-reset write-flash 0x120000 model.bin
```

**注意 model 地址是 0x120000（不是 web-chat 版的 0x150000）** —— 旧 partition 布局。

## 烧录后

1. 拔 USB，等 2 秒，重新插入
2. 打开串口监视器（115200 baud, UTF-8）
3. 回车 → 输入消息 → 回车 → 等几秒 → token-by-token 输出

## 特点

- 零网络依赖，烧完即用
- 最小资源占用（1.04 MB app, 13.1 MB model）
- 适合学习 / 调试

## Partition 布局

| 名称 | 类型 | 偏移 | 大小 |
|---|---|---|---|
| nvs | data | 0x9000 | 20 KB |
| factory | app | 0x10000 | 1.06 MB |
| model | data 0x40 | 0x120000 | 14.5 MB |
| coredump | data | 0xFF0000 | 64 KB |

## 想用 Web 版？

请用 `firmware/web-chat/` 目录，partition 布局不同，必须重新烧 partition table。
