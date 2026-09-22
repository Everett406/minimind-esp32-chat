# 烧录 + 备份 + 回退 + 故障排查

## 1. 烧录前必看

### 1.1 Partition 布局

本项目用**自定义 partition table**（`source/partitions.csv`）：

| 名称 | 类型 | 子类型 | 偏移 | 大小 | 内容 |
|---|---|---|---|---|---|
| nvs | data | nvs | 0x9000 | 20 KB | WiFi 配网 / 系统 prompt 等配置 |
| factory | app | factory | 0x10000 | **1.31 MB** (0x140000) | 应用固件 |
| model | data | 0x40 | 0x150000 | **14.6 MB** (0xEA0000) | int4 量化的 13.1 MB 模型 |
| coredump | data | coredump | 0xFF0000 | 64 KB | 崩溃日志 |

**关键点**：
- factory 比 arduino 默认的 1 MB 大 — 因为 web chat 版有 ~1.14 MB
- model 偏移从 0x150000 开始（旧基线版是 0x120000）
- **如果你的板子之前烧过旧基线版（1.04 MB 串口版）**，必须先重新烧 partition table + bootloader，否则应用不会跑新代码

### 1.2 烧录顺序

```
1. bootloader @ 0x0000   (12 KB,  < 1秒)
2. partitions @ 0x8000  (3 KB,   < 1秒)
3. app @ 0x10000        (1.14 MB, ~12秒)
4. model @ 0x150000     (13.7 MB, ~40-160秒)
```

> ⚠️ 烧 model.bin 是最容易出问题的步骤（文件大、传输慢），**PowerShell 用 background 跑容易假成功**。必须看到 `Hash of data verified` + `EXIT: 0` 才算成功。

### 1.3 烧完必须断电重启

CH343 UART 的 DTR 自动 reset **不可靠**。烧完必须：
1. 拔 USB
2. 等 2 秒
3. 重新插

不重新插，板子还跑着旧代码（或者 boot 循环但 Serial 不输出新 log）。

## 2. 烧录脚本

### Windows PowerShell

```powershell
# Web Chat 版 (默认)
.\scripts\burn-web-chat.ps1 -Port COM4

# 基线串口版
.\scripts\burn-baseline.ps1 -Port COM4
```

### Linux / macOS

```bash
./scripts/burn-web-chat.sh /dev/ttyUSB0
./scripts/burn-baseline.sh /dev/ttyUSB0
```

## 3. 备份当前 flash

```bash
# 备份 (3 段 + model, 写到 release/backups/<时间戳>/)
.\scripts\backup-board.ps1 -Port COM4

# 输出: release/backups/2026-08-29-1530/app.bin etc.
```

> 注意：model.bin 13.7 MB + 1.6 MB padding = 15 MB，写本地大约 30-40 秒。

## 4. 还原到备份

```bash
# 列出已有备份
ls release/backups/

# 还原 (从指定 backup 目录写回 flash)
.\scripts\restore-firmware.ps1 -BackupDir release/backups/2026-08-29-1530 -Port COM4
```

## 5. 故障排查

### 5.1 boot 循环 (一直重启, Serial 看不到 banner)

```
rst:0x1 (POWERON),boot:0x2a (SPI_FAST_FLASH_BOOT)
...
E (243) esp_image: Image length N doesn't fit in partition length M
E (244) boot: No bootable app partitions in the partition table
```

**原因**：app 太大装不进 factory partition。
**修复**：用更大的 factory partition 重新烧 `partitions.bin`（本项目已用 0x140000 = 1.31 MB）。

### 5.2 PSRAM 没初始化 (3rd-party Lib 报 MALLOC_CAP_SPIRAM 失败)

**原因**：arduino-cli 短 fqbn `esp32:esp32:esp32s3` 不带 `PSRAM=opi`，PSRAM 驱动没装上。
**修复**：用完整 fqbn:
```
esp32:esp32:esp32s3:UploadSpeed=921600,USBMode=default,CDCOnBoot=default,UploadMode=default,CPUFreq=240,FlashMode=dio,FlashSize=16M,PartitionScheme=custom,PSRAM=opi,DebugLevel=none
```

### 5.3 串口能跑，Web 跑不起来 (看不到 AP)

**先看 boot log 里 `softAP` 行**：
- 有 → Web 跑了，可能是你 PC 没连对 WiFi
- 没有 → boot 还在 mm_load，或者 model 加载 OOM

### 5.4 Model 加载失败 — `[ps FAIL] requested=4GB`

**99% 是 model.bin 没烧完整**。验证：

```bash
esptool.py --chip esp32s3 --port COM4 read-flash 0x150000 16 /tmp/check.bin
# 看 /tmp/check.bin 前 4 字节: 应该是 0x4D4E4432 (MND2 magic)
# 对比源文件:
xxd -l 16 source/firmware/web-chat/model.bin
# 4 字节 magic 必须一致
```

如果不一致 → 重烧 model.bin（用前台跑，**不要用 background task**）。

### 5.5 中文乱码

- 浏览器开 `http://192.168.4.1` 看 — HTML 内嵌 UTF-8，不会乱码
- **不要用串口看中文**（除非 PuTTY 设 UTF-8）
- 不要用 GBK / 936 编码的串口工具

### 5.6 WebUI 突然刷新后内容乱

清浏览器缓存 (Ctrl+Shift+R 强刷)，或者开 DevTools Console 看错误。

### 5.7 烧 model.bin 假成功 (background task)

PowerShell background 跑 esptool 时偶发提前返回，但 status=succeeded。**结果**：model.bin 烧一半，剩下的全 0xFF，mm_load 读到 0xFFFFFFFF → n_groups 算成 -1406 → 申请 4 GB PSRAM → 失败。

**预防**：
- 烧 >= 10 MB 文件时**前台跑**（timeout 设大）
- 看完整 `Wrote N bytes at 0x...` + `Hash of data verified` + `EXIT: 0` 三件齐全

## 6. 完整 factory reset

如果板子彻底乱码、boot 循环无法恢复：

```bash
# 1. 整片擦除
esptool.py --chip esp32s3 --port COM4 erase_flash

# 2. 重新烧 3 段 (bootloader + partitions + app)
esptool.py --chip esp32s3 --port COM4 --before default-reset write-flash \
  0x0000  firmware/web-chat/bootloader.bin \
  0x8000  firmware/web-chat/partitions.bin \
  0x10000 firmware/web-chat/app.bin

# 3. 烧 model
esptool.py --chip esp32s3 --port COM4 --before default-reset write-flash \
  0x150000 firmware/web-chat/model.bin

# 4. 断电重启
```

## 7. 烧录工具

- **esptool.py** v4+ (推荐) — `pip install esptool`
- **esptool v5.3.1** 已测试可用
- arduino-cli upload 也行，但**必须用完整 fqbn**（不能短）
