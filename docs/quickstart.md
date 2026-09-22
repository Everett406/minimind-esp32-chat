# 5 分钟快速上手

## 准备工作

1. **硬件**：元旦 ESP32-S3 N16R8 开发板 + USB-C 线
2. **驱动**：CH343 串口驱动（Win10/11 通常自动装好）
3. **工具**（任选一种）：
   - **预编译固件路线**：esptool v4+ (Python) — 推荐，零依赖
   - **源码编译路线**：arduino-cli + ESP32 平台

## 路线 A: 烧预编译固件 (推荐)

### 1. 装 esptool

```bash
pip install esptool
# 或下载 exe: https://github.com/espressif/esptool/releases
```

### 2. 确认串口

Windows: 设备管理器 → 端口 (COM & LPT) → 找 USB-SERIAL CH340 (COM3/COM4...)

### 3. 烧录 (一键脚本)

```bash
# Windows PowerShell (管理员):
cd scripts
.\burn-web-chat.ps1 -Port COM4

# Linux/macOS:
chmod +x burn-web-chat.sh
./burn-web-chat.sh /dev/ttyUSB0
```

**或者手动烧 3 段** (关键 — 顺序不能错):

```bash
# 1. bootloader
esptool.py --chip esp32s3 --port COM4 --baud 921600 \
  --before default-reset --after no-reset \
  write-flash 0x0000 firmware/web-chat/bootloader.bin

# 2. partition table
esptool.py --chip esp32s3 --port COM4 --baud 921600 \
  --before default-reset --after no-reset \
  write-flash 0x8000 firmware/web-chat/partitions.bin

# 3. app (1.14 MB)
esptool.py --chip esp32s3 --port COM4 --baud 921600 \
  --before default-reset --after no-reset \
  write-flash 0x10000 firmware/web-chat/app.bin

# 4. model (13.7 MB) — 这一步要 40-160 秒!
esptool.py --chip esp32s3 --port COM4 --baud 921600 \
  --before default-reset --after hard-reset \
  write-flash 0x150000 firmware/web-chat/model.bin
```

### 4. 断电重启

**拔 USB 等 2 秒再插**（CH343 的 DTR 复位不可靠，必须手动 power cycle）

## 路线 B: 从源码编译

### 1. 装 arduino-cli

```bash
# 见 https://arduino.github.io/arduino-cli/
# 或 winget: winget install ArduinoSA.CLI
```

### 2. 装 ESP32 平台 + 库

```bash
arduino-cli core update-index
arduino-cli core install esp32:esp32@2.0.14
arduino-cli lib install "Adafruit NeoPixel"
```

### 3. 编译

```bash
cd source
arduino-cli compile --fqbn 'esp32:esp32:esp32s3:UploadSpeed=921600,USBMode=default,CDCOnBoot=default,UploadMode=default,CPUFreq=240,FlashMode=dio,FlashSize=16M,PartitionScheme=custom,PSRAM=opi,DebugLevel=none' --build-path ../build .
```

### 4. 烧录

参考路线 A 步骤 3，把 `../build/YuanDiArduino.ino.bin` 烧到 0x10000。

## 5. 验证

打开串口监视器（115200 baud），应该看到：

```
ESP-ROM:esp32s3-20210327
...
S1: serial init done
=== YuanDi-S3 MiniMind2-Small (serial chat) ===
S4: loading model from flash (mmap + mm_load, ~5-10s)... done
[web_chat] softAP(YuanDi-S3-MiniMind) -> OK, IP=192.168.4.1
[web_chat] WebServer started on :80
输入消息后回车开始对话
```

回车 → 输入消息 → 回车 → 等几秒 → token-by-token 输出。

WiFi 列表出现 `YuanDi-S3-MiniMind`（无密码）→ 连上 → 浏览器开 `http://192.168.4.1`。

## 下一步

- 烧录失败 / boot 循环 → [`flash-and-backup.md`](flash-and-backup.md)
- Web UI 详细用法 → [`web-ui.md`](web-ui.md)
- 想改代码 / 调架构 → [`architecture.md`](architecture.md)
