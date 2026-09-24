# 参考资料与致谢 + License 状态

## ⚠️ License 状态总览

| 项目 | License | 商用 | 二次分发 | 备注 |
|---|---|---|---|---|
| **zhuhai-esp/ESP32-S3-YuanDi-Board** | ⚠️ **无 License** | ❌ 需授权 | ❌ 需授权 | 教学项目, 联系作者获得 |
| **MiniMind (jingyaogong)** | ✅ Apache 2.0 | ✅ | ✅ | 模型权重 + 训练代码 |
| **本项目原创部分** (web_chat / scripts / docs) | ✅ MIT | ✅ | ✅ | 我们的代码 |
| **ESP-IDF / Arduino-ESP32** | ✅ Apache 2.0 | ✅ | ✅ | 工具链 |
| **GPT-2 BPE 词表** | ✅ MIT (OpenAI) | ✅ | ✅ | 公开 |

## 原始 MiniMind 项目

- **GitHub**: https://github.com/jingyaogong/minimind
- **作者**: jingyaogong
- **类型**: 从零训练 26M-1B 参数中文 LLM
- **许可**: Apache 2.0 (允许分发)
- **用于本项目**: `firmware/*/model.bin`（训练好的 26M int4 量化权重）

## YuanDi ESP32-S3 N16R8 参考项目（无 License）

- **GitHub**: https://github.com/zhuhai-esp/ESP32-S3-YuanDi-Board
- **类型**: YuanDi 板 (B 站"机器知芯") 适配 ESP32-S3 教学项目合集
- **本项目 (Web Chat) 基于**: `YuanDi-S3-MiniMind2` 子项目
- **许可**: ⚠️ **无 LICENSE 文件**（GitHub 默认 = 版权所有）
- **直接复用其代码**:
  - `source/minimind.h` (推理引擎)
  - `source/tokenizer.h` (BPE 编码)
  - `source/tokenizer_data.h` (BPE 词表)
  - `source/vocab.h` (词条文本)
  - `source/llm.h` (备用 LLM 推理, 本项目未用)
- **使用条件**:
  - **仅供学习** — 商用需先联系作者获得书面授权
  - **二次分发** — 需先联系作者
  - **必须保留原作者署名** (zhuhai-esp / 机器知芯)
- **联系方式**:
  - GitHub: https://github.com/zhuhai-esp/ESP32-S3-YuanDi-Board/issues
  - B 站: 搜索"机器知芯"频道

## B 站教程

- 频道: "机器知芯"
- 关键词: ESP32-S3 N16R8 跑大模型 / YuanDi 开发板
- 视频中讲解了串口 chat 版的搭建，本项目是其 Web 化扩展

## ESP-IDF / Arduino-ESP32

- **arduino-esp32**: https://github.com/espressif/arduino-esp32
- **版本**: 本项目固定用 `esp32:esp32@2.0.14` core + `esp32:esp32@3.3.10-cn` SDK
- **完整 fqbn** (编译时必须):
  ```
  esp32:esp32:esp32s3:UploadSpeed=921600,USBMode=default,CDCOnBoot=default,UploadMode=default,CPUFreq=240,FlashMode=dio,FlashSize=16M,PartitionScheme=custom,PSRAM=opi,DebugLevel=none
  ```
- **关键参数**：
  - `FlashMode=dio` — 比 qio 稳定
  - `FlashSize=16M` — 16 MB flash
  - `PartitionScheme=custom` — 配合 `partitions.csv`
  - `PSRAM=opi` — **必需**，否则 PSRAM 不初始化

## 工具链

- **esptool.py** v4+: https://github.com/espressif/esptool
- **arduino-cli** v1+: https://arduino.github.io/arduino-cli/

## 模型量化工具

在 `zhuhai-esp/ESP32-S3-YuanDi-Board/YuanDi-S3-MiniMind2/tools/convert_minimind.py`（**无 License**）：

```bash
# 从 MiniMind2-Small 训练好的 .pth 转 int4 量化 model.bin
python convert_minimind.py \
  --input ../../minimind2-small-sft-26M.pth \
  --output ./data/model.bin \
  --quant int4_group64
```

输出 `data/model.bin` 就是 `firmware/*/model.bin`。

> ⚠️ 此脚本属于参考项目（无 License），本仓库未直接打包；如需使用请自行去原项目 clone 或联系作者获得授权。

## 关于本仓库的"双重许可" 总结

```
本仓库 = MIT 原创部分  +  Apache 2.0 模型权重  +  无 License 衍生 .h
         ↓                       ↓                       ↓
      自由使用              自由使用               仅供学习
```

**简明判断表**：

| 你想做什么？ | 哪些部分可用？ | 注意事项 |
|---|---|---|
| 学习 / 研究 / 跑 demo | 全部 | 无 |
| 个人项目用 web UI 改改 | web_chat.cpp / 脚本 | MIT |
| 改推理引擎 / BPE 代码 | 需先联系原作者 | zhuhai-esp |
| 商用产品 / 二次分发 | 需先联系原作者 | zhuhai-esp |
