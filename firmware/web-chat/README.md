# firmware/web-chat/

**最新 Web Chat 版**（1.14 MB app + 13.1 MB model）

## 烧录

```bash
# 用 scripts/burn-web-chat.sh 或 .ps1, 或手动:

esptool.py --chip esp32s3 --port COM4 --baud 921600 \
  --before default-reset --after no-reset write-flash 0x0000 bootloader.bin

esptool.py --chip esp32s3 --port COM4 --baud 921600 \
  --before default-reset --after no-reset write-flash 0x8000 partitions.bin

esptool.py --chip esp32s3 --port COM4 --baud 921600 \
  --before default-reset --after no-reset write-flash 0x10000 app.bin

esptool.py --chip esp32s3 --port COM4 --baud 921600 \
  --before default-reset --after hard-reset write-flash 0x150000 model.bin
```

## 烧录后

1. 拔 USB，等 2 秒，重新插入
2. WiFi 列表里找 `YuanDi-S3-MiniMind`（无密码）
3. 浏览器开 `http://192.168.4.1`
4. 回车 / 输消息 / 回车，开始聊天

## 文件大小

| 文件 | 大小 | 烧录地址 |
|---|---|---|
| bootloader.bin | 24 KB | 0x0000 |
| partitions.bin | 4 KB | 0x8000 |
| app.bin | 1.3 MB | 0x10000 |
| model.bin | 15 MB (13.1 MB + 1.6 MB padding) | 0x150000 |

## Partition 布局

| 名称 | 类型 | 偏移 | 大小 |
|---|---|---|---|
| nvs | data | 0x9000 | 20 KB |
| factory | app | 0x10000 | 1.3 MB |
| model | data 0x40 | 0x150000 | 14.6 MB |
| coredump | data | 0xFF0000 | 64 KB |

## 特性

- AP 热点 `YuanDi-S3-MiniMind` (无密码)
- 浏览器流式聊天 (SSE)
- 暗 / 亮主题切换
- Tokenizer / System Monitor / System Prompt 编辑
- 日志 ring buffer 200 行
- 状态条：tok/s、ctx、prefill、gen、kv
- Markdown 渲染
