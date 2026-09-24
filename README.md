<div align="center">

# MiniMind-ESP32 Chat

**在 ESP32-S3 上运行 26M 参数中文大模型，开箱即烧，手机浏览器直接对话**

[![release](https://img.shields.io/github/v/release/Everett406/minimind-esp32-chat?include_prereleases)](https://github.com/Everett406/minimind-esp32-chat/releases)
[![license](https://img.shields.io/badge/license-MIT-green)](LICENSE)
[![platform](https://img.shields.io/badge/platform-ESP32--S3-blue)](#-硬件要求)
[![model](https://img.shields.io/badge/model-MiniMind2--Small%2026M%20int4-orange)](#-模型)

*串口聊天 · AP 热点 · Web 流式对话 · 分词器预览 · 系统监控*

</div>

---

## 📖 目录

- [简介](#-简介)
- [特性](#-特性)
- [硬件要求](#-硬件要求)
- [快速开始](#-快速开始)
- [从源码编译](#-从源码编译)
- [目录结构](#-目录结构)
- [模型](#-模型)
- [推理原理](#-推理原理)
- [常见问题](#-常见问题)
- [许可证与致谢](#-许可证与致谢)

## 📌 简介

本项目将 [MiniMind2-Small](https://github.com/jingyaogong/minimind)（26M 参数中文对话模型）经过 4-bit 量化后部署到 ESP32-S3 开发板上，提供两种使用方式：

- **串口版** — 上电即用，USB 串口直接对话，零网络依赖
- **Web 版** — 板子开启 WiFi 热点，手机 / 电脑浏览器访问 `192.168.4.1`，流式输出对话

无需云服务、无需联网，所有推理全部在板端完成。

## ✨ 特性

**推理引擎**

- 纯 C 实现的 Llama 风格 Transformer（RMSNorm + SwiGLU + RoPE + GQA），无第三方框架
- 4-bit group 量化（group=64）+ fp16 scale，模型仅 13.1 MB，塞进 16 MB Flash
- KV cache 位于 PSRAM，scratch 位于内部 SRAM，生成速度 0.4–0.8 tok/s

**Web UI**

- SSE 流式输出，Markdown 渲染
- 内置 GPT-2 BPE 分词器预览（按 token 高亮 + 显示 token ID）
- CPU / 温度 / 内存实时监控（ESP32-S3 内置温度传感器）
- System prompt 自定义、Token / 上下文 / 速度状态条
- 日志查看器（ring buffer 200 行）、暗色 / 串口双主题

## 🛠 硬件要求

| 部件 | 规格 |
|---|---|
| MCU | ESP32-S3-WROOM-1（Xtensa LX7 双核 240 MHz） |
| Flash | 16 MB |
| PSRAM | 8 MB（OPI） |
| 开发板 | 元旦 ESP32-S3 N16R8（[zhuhai-esp/ESP32-S3-YuanDi-Board](https://github.com/zhuhai-esp/ESP32-S3-YuanDi-Board)） |
| 连接 | USB-C（CH343 UART，Arduino 自动 reset） |

> 其他 N16R8 规格的 ESP32-S3 开发板理论上也可以运行，但引脚和按键布局以元旦板为准。

## 🚀 快速开始

### 方式 A：烧录预编译固件（推荐）

1. 从 [Releases](https://github.com/Everett406/minimind-esp32-chat/releases) 下载固件包并解压：
   - `minimind-esp32-chat-web-chat-firmware.zip` — Web 版
   - `minimind-esp32-chat-serial-chat-firmware.zip` — 串口版
2. 用 esptool 一条命令烧录（替换 `COM4` 为你的端口）：

```bash
esptool.py --chip esp32s3 --port COM4 --baud 921600 \
  write_flash 0x0 bootloader.bin 0x8000 partitions.bin 0x10000 app.bin 0x410000 model.bin
```

3. 烧录完成后**断电重启**（拔掉 USB 等 2 秒再插）。

或者，克隆本仓库后使用一键脚本：

```bash
git clone https://github.com/Everett406/minimind-esp32-chat.git
cd minimind-esp32-chat/scripts

# Windows PowerShell
.\burn-web-chat.ps1 -Port COM4

# Linux / macOS
./burn-web-chat.sh /dev/ttyUSB0
```

### 方式 B：使用

- **串口版** — 打开 Arduino Serial Monitor / PuTTY（115200 baud），回车开始对话
- **Web 版** — 连接 WiFi `YuanDi-S3-MiniMind`（无密码），浏览器打开 `http://192.168.4.1`（注意是 HTTP 不是 HTTPS）

> 更详细的烧录步骤、备份 / 回退方法见 [`docs/flash-and-backup.md`](docs/flash-and-backup.md)

## 🔨 从源码编译

```bash
git clone https://github.com/Everett406/minimind-esp32-chat.git
cd minimind-esp32-chat/source

arduino-cli compile --fqbn 'esp32:esp32:esp32s3:UploadSpeed=921600,USBMode=default,CDCOnBoot=default,UploadMode=default,CPUFreq=240,FlashMode=dio,FlashSize=16M,PartitionScheme=custom,PSRAM=opi,DebugLevel=none'
```

编译完成后参考 [`scripts/build.sh`](scripts/build.sh) 与 [`scripts/burn-web-chat.sh`](scripts/burn-web-chat.sh) 完成烧录。架构设计与代码导读见 [`docs/architecture.md`](docs/architecture.md)。

## 📂 目录结构

```
minimind-esp32-chat/
├── README.md                        ← 你正在看的文件
├── CHANGELOG.md                     ← 版本变更记录
├── LICENSE                          ← 许可证（含双重许可声明）
├── docs/                            ← 文档
│   ├── quickstart.md                ← 快速上手
│   ├── architecture.md              ← 代码架构 + 推理流程
│   ├── web-ui.md                    ← Web UI 详细说明
│   └── flash-and-backup.md          ← 烧录 + 回退 + 故障排查
├── firmware/                        ← 预编译固件（开箱即用）
│   ├── baseline-serial-chat/        ← 串口版
│   └── web-chat/                    ← Web 版
│       （每个目录含 bootloader / partitions / app / model 四个 bin + 说明）
├── source/                          ← 完整源码
│   ├── YuanDiArduino.ino            ← 主入口（setup / loop）
│   ├── web_chat.cpp / web_chat.h    ← Web 服务器 + SSE + UI
│   ├── minimind.h                   ← 推理引擎（单 header）
│   ├── tokenizer.h / tokenizer_data.h ← GPT-2 BPE 分词器
│   ├── vocab.h                      ← BPE 词表 + merge rank
│   ├── llm.h                        ← 备用 LLM 推理
│   ├── partitions.csv               ← 自定义分区表
│   └── _build_web.bat               ← arduino-cli 编译脚本
├── scripts/                         ← 一键工具（PowerShell / Bash 成对提供）
│   ├── build.sh / .ps1              ← 编译
│   ├── burn-web-chat.sh / .ps1      ← 烧录 Web 版
│   ├── burn-baseline.sh / .ps1      ← 烧录串口基线版
│   ├── backup-board.sh / .ps1       ← 备份当前 Flash
│   └── restore-firmware.sh / .ps1   ← 还原备份
└── reference/                       ← 参考资料
    └── minimind-link.md             ← 原始 MiniMind 项目链接
```

## 🧠 模型

| 项目 | 说明 |
|---|---|
| 架构 | MiniMind2-Small（Llama 风格：RMSNorm + SwiGLU + RoPE + GQA） |
| 参数量 | 26M |
| 量化 | 4-bit group（group=64）+ fp16 scale |
| 词表 | 6,400（GPT-2 byte-level BPE，中文友好） |
| 序列长度 | 256 |
| 生成速度 | 0.4–0.8 tok/s（240 MHz 双核 + 4-bit 矩阵乘优化） |
| 原始模型 | [MiniMind by jingyaogong](https://github.com/jingyaogong/minimind)（PyTorch） |

## 🔬 推理原理

```
[用户输入] → BPE 分词 → [token ids]
         → Embedding lookup（PSRAM 4-bit codes，dequant fp16 → fp32）
         → 8 层 Transformer（KV cache 在 PSRAM，scratch 在内部 SRAM）
         → tied output head（复用 embedding）
         → top-k=40 + temp=0.8 采样
         → 反 tokenize → [文本] → 流式 SSE 输出
```

详细架构与关键代码注释见 [`docs/architecture.md`](docs/architecture.md)。

## ❓ 常见问题

<details>
<summary><b>烧完没看到 AP？</b></summary>

烧完必须**断电重启**（拔 USB 等 2 秒再插），CH343 的 DTR 复位可能不可靠。
</details>

<details>
<summary><b>Web 界面打不开？</b></summary>

先连 WiFi `YuanDi-S3-MiniMind`（**无密码**），再打开 `http://192.168.4.1`。**注意是 HTTP 不是 HTTPS**。
</details>

<details>
<summary><b>中文乱码？</b></summary>

浏览器打开 `http://192.168.4.1` 即可，HTML 内嵌 UTF-8。串口工具请使用 UTF-8 编码，不要用 GBK。
</details>

<details>
<summary><b>模型加载失败 <code>[ps FAIL] requested=4GB</code>？</b></summary>

大概率是 model.bin 没烧完整。用 `esptool read_flash 0x150000 16 model-head.bin` 对比源文件，group 字段应该是 0x40。
</details>

<details>
<summary><b>想回退到旧版本？</b></summary>

运行 `scripts/restore-firmware.sh` 选择 backup 目录，或手动用 esptool 写回三段固件。
</details>

## 📜 许可证与致谢

本项目基于 [zhuhai-esp/ESP32-S3-YuanDi-Board](https://github.com/zhuhai-esp/ESP32-S3-YuanDi-Board)（B 站"机器知芯"频道）的 `YuanDi-S3-MiniMind2` 子项目。由于参考项目**没有 LICENSE 文件**，本项目采用**双重许可策略**：

| 内容 | 来源 | 许可 |
|---|---|---|
| 本项目原创（`web_chat.cpp` / `YuanDiArduino.ino` / `scripts/` / `docs/`） | 本项目 | **MIT** — 自由使用 / 修改 / 分发 |
| 模型权重（`firmware/*/model.bin`） | [jingyaogong/minimind](https://github.com/jingyaogong/minimind) | **Apache 2.0** |
| 推理引擎 + BPE 源码（`source/minimind.h` 等） | zhuhai-esp（原项目无 License） | **仅供学习** — 商用 / 再分发需先联系原作者 |

简单说：

- 想用本项目的 **web UI / 烧录脚本 / 文档** → 随意用（MIT）
- 想改 **推理引擎 / BPE 分词器代码** → 先联系原作者（B 站私信"机器知芯"或 GitHub issue）
- 想做 **商用产品** → 还需原作者书面授权

详细许可声明见 [LICENSE](LICENSE)。

### 致谢

- [jingyaogong/minimind](https://github.com/jingyaogong/minimind) — 模型 + 训练代码
- [zhuhai-esp/ESP32-S3-YuanDi-Board](https://github.com/zhuhai-esp/ESP32-S3-YuanDi-Board) — 板子适配、串口基线
- B 站"机器知芯"频道 — 元旦板教程
- ESP-IDF / Arduino-ESP32 团队 — 工具链
