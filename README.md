# MiniMind-ESP32 Chat

> 在 ESP32-S3 N16R8 开发板上跑 26M 参数中文 chat 模型（MiniMind2-Small int4 量化），支持串口聊天 + AP 热点 + 手机浏览器流式对话 + 分词器 / 系统监控。

![status](https://img.shields.io/badge/status-working-brightgreen) ![platform](https://img.shields.io/badge/ESP32--S3-N16R8-blue) ![model](https://img.shields.io/badge/MiniMind2--Small-26M-orange)

## ⚠️ 重要声明 / IMPORTANT

**本项目基于 [zhuhai-esp/ESP32-S3-YuanDi-Board](https://github.com/zhuhai-esp/ESP32-S3-YuanDi-Board)（B 站"机器知芯"频道）的 `YuanDi-S3-MiniMind2` 子项目。**

参考项目**没有 LICENSE 文件**，所以本项目采用**双重许可策略**：

| 文件 | 来源 | 许可 |
|---|---|---|
| 本项目原创（`web_chat.cpp` / `YuanDiArduino.ino` / `scripts/` / `docs/`）| 本项目 | ✅ **MIT** — 自由使用 / 修改 / 分发 |
| 模型权重 (`firmware/*/model.bin`) | [jingyaogong/MiniMind](https://github.com/jingyaogong/minimind) | ✅ **Apache 2.0** — 自由使用 / 分发 |
| 推理引擎 + BPE 源码 (`source/minimind.h` / `tokenizer.h` / `tokenizer_data.h` / `vocab.h` / `llm.h`) | **zhuhai-esp**（无 License）| ⚠️ **仅供学习** — 商用 / 再分发需先联系原作者 |

**详细许可声明见 [LICENSE](LICENSE)。**

简单说：
- **想用本项目的 web UI / 烧录脚本 / 文档** → 随意用，MIT
- **想改推理引擎 / BPE 分词器代码** → **先联系原作者**：B 站私信"机器知芯"或 GitHub issue
- **想做商用产品** → 还需要原作者的书面授权

## ✨ 特性

- **轻量推理引擎** — 纯 C 实现的 Llama 风格 transformer，无第三方框架，0 依赖
- **4-bit 量化** — int4 group-quantized + fp16 scales，13.1 MB 模型塞进 16MB flash
- **双模式 UI**：
  - **串口版（基线）** — 启动后串口直接对话，0 网络依赖
  - **Web 版** — 板子开 AP（`YuanDi-S3-MiniMind`），手机连上开 `192.168.4.1` 浏览器流式聊天
- **附加功能**：
  - 内置 GPT-2 BPE **分词器**预览（按 token 高亮 + ID 显示）
  - **CPU/温度/内存** 实时监控（ESP32-S3 内置温度传感器）
  - **System prompt** 可自定义（留空 = 无 system 段）
  - Markdown 渲染、Token/ctx/速度状态条
  - 日志查看器（ring buffer 200 行）
  - 串口/暗色双主题

## 🛠 硬件

| 部件 | 规格 |
|---|---|
| MCU | ESP32-S3-WROOM-1 (Xtensa LX7 双核 240MHz) |
| Flash | 16 MB |
| PSRAM | 8 MB (OPI) |
| 开发板 | 元旦 ESP32-S3 N16R8 (zhuhai-esp/ESP32-S3-YuanDi-Board) |
| USB | CH343 UART (Arduino 自动 reset) |

## 🚀 5 分钟上手

### 1. 硬件连接

- USB-C 接开发板
- 默认无密码（已配置） AP `YuanDi-S3-MiniMind` 启动后即可

### 2. 烧录固件（任选一种）

#### 方式 A：一键烧预编译固件（推荐新手）

```bash
# 解压 release 包后:
cd minimind-esp32-chat/scripts
# Windows PowerShell:
.\burn-web-chat.ps1 -Port COM4
# Linux/macOS:
./burn-web-chat.sh /dev/ttyUSB0
```

#### 方式 B：从源码编译 + 烧录

```bash
cd minimind-esp32-chat/source
# 编译
arduino-cli compile --fqbn 'esp32:esp32:esp32s3:UploadSpeed=921600,USBMode=default,CDCOnBoot=default,UploadMode=default,CPUFreq=240,FlashMode=dio,FlashSize=16M,PartitionScheme=custom,PSRAM=opi,DebugLevel=none'
# 烧录（见 scripts/ 里的 build + burn 流程）
```

> 详细烧录步骤、常见问题、回退方法见 [`docs/flash-and-backup.md`](docs/flash-and-backup.md)

### 3. 使用

- **串口**：打开 Arduino Serial Monitor / PuTTY (115200 baud)，回车开始对话
- **Web**：连上 WiFi `YuanDi-S3-MiniMind` (无密码) → 浏览器开 `http://192.168.4.1`

## 📂 目录结构

```
minimind-esp32-chat/
├── README.md                           ← 你正在看
├── CHANGELOG.md
├── LICENSE
├── docs/
│   ├── quickstart.md                   ← 快速上手
│   ├── architecture.md                 ← 代码架构 + 推理流程
│   ├── web-ui.md                       ← Web UI 详细说明
│   └── flash-and-backup.md             ← 烧录 + 回退 + 故障排查
├── firmware/                            ← 预编译固件 (开箱即用)
│   ├── baseline-serial-chat/          ← 1.04 MB 串口版 (最小)
│   │   ├── bootloader.bin
│   │   ├── partitions.bin
│   │   ├── app.bin
│   │   ├── model.bin
│   │   └── README.md
│   └── web-chat/                       ← 1.14 MB Web 版 (最新)
│       ├── bootloader.bin
│       ├── partitions.bin
│       ├── app.bin
│       ├── model.bin
│       └── README.md
├── source/                              ← 完整源码 (从 .ino 到 header)
│   ├── YuanDiArduino.ino               ← 主入口 (setup/loop)
│   ├── web_chat.cpp / web_chat.h       ← Web 服务器 + SSE + UI
│   ├── minimind.h                      ← 推理引擎 (单 header, 8KB)
│   ├── tokenizer.h / tokenizer_data.h ← GPT-2 BPE 分词器
│   ├── vocab.h                         ← BPE 词表 + merge rank
│   ├── llm.h                           ← 备用 LLM 推理
│   ├── partitions.csv                  ← 自定义 partition (factory 1.3MB + model 14.6MB)
│   └── _build_web.bat                  ← arduino-cli 编译脚本
├── scripts/                             ← 一键工具
│   ├── build.sh / build.ps1            ← 编译
│   ├── burn-web-chat.sh / .ps1         ← 烧 Web 版 (3 段)
│   ├── burn-baseline.sh / .ps1         ← 烧基线版
│   ├── backup-board.sh / .ps1         ← 备份当前 flash → 本地
│   └── restore-firmware.sh / .ps1     ← 还原到指定 backup
└── reference/                           ← 参考资料
    ├── original-readme.md              ← 原始 zhuhai-esp 项目说明
    └── minimind-link.md                ← 原始 MiniMind 项目链接
```

## 🧠 模型

- **架构**：MiniMind2-Small (Llama 风格, RMSNorm + SwiGLU + RoPE + GQA)
- **参数量**：26M
- **量化**：4-bit group (group=64) + fp16 scale
- **词表**：6,400 (GPT-2 byte-level BPE, 中文友好)
- **序列长度**：256
- **生成速度**：0.4-0.8 tok/s (240MHz 双核 + 4-bit 矩阵乘优化)
- **原始模型**：[MiniMind by jingyaogong](https://github.com/jingyaogong/minimind) (PyTorch)
- **转换工具**：`tools/convert_minimind.py` (在原参考项目中)

## 🔬 推理原理（高级）

```
[用户输入] → BPE 分词 → [token ids]
         → Embedding lookup (PSRAM 4-bit codes, dequant fp16→fp32)
         → 8 层 Transformer (KV cache 在 PSRAM, scratch 在内部 SRAM)
         → tied output head (复用 embedding)
         → top-k=40 + temp=0.8 采样
         → 反 tokenize → [文本] → 流式 SSE 输出
```

详细架构 + 关键代码注释见 [`docs/architecture.md`](docs/architecture.md)

## ❓ 常见问题

**Q: 烧完没看到 AP？**
A: 烧完必须**断电重启**（拔 USB 等 2 秒再插），CH343 的 DTR 复位可能不可靠。

**Q: Web 界面打不开？**
A: 先连 WiFi `YuanDi-S3-MiniMind`（**无密码**），再开 `http://192.168.4.1`。**注意是 HTTP 不是 HTTPS**。

**Q: 中文乱码？**
A: 浏览器开 `http://192.168.4.1` 即可，HTML 内嵌 UTF-8。不要用 GBK 串口工具。

**Q: 模型加载失败 `[ps FAIL] requested=4GB`？**
A: 大概率是 model.bin 没烧完整。用 `esptool read_flash 0x150000 16 model-head.bin` 对比源文件，group 字段应该是 0x40。

**Q: 想回退到旧版本？**
A: `scripts/restore-firmware.sh` 选 backup 目录，或手动 esptool 写回 3 段。

## 📜 License

[MIT](LICENSE) — 商业 / 二次开发 / 闭源使用都允许。

## 🙏 致谢

- **MiniMind 原始作者**：[jingyaogong](https://github.com/jingyaogong/minimind) — 模型 + 训练代码
- **参考项目**：[zhuhai-esp/ESP32-S3-YuanDi-Board](https://github.com/zhuhai-esp/ESP32-S3-YuanDi-Board) — 板子适配、串口基线
- **B 站**："机器知芯" 频道 — 元旦板教程
- **ESP-IDF / Arduino-ESP32 团队** — 工具链
