# Changelog

## v1.1.1 — 2026-08-29 (License Notice)

### 修改
- **LICENSE**: 加入"双重许可"声明——本项目原创部分 (web_chat / scripts / docs) 走 MIT；模型权重 (model.bin) 走 Apache 2.0 (MiniMind 原始)；**推理引擎 + BPE 源码 (minimind.h / tokenizer.h / tokenizer_data.h / vocab.h / llm.h) 沿用参考项目 zhuhai-esp/ESP32-S3-YuanDi-Board 的"无 License"状态，仅供学习，不得商用 / 再分发，需先联系原作者**
- **README**: 顶部加 ⚠️ 重要声明 + 简明许可表
- **reference/minimind-link.md**: 加上完整的 License 状态总览表 + "双重许可" 总结

### 为什么
参考项目 `zhuhai-esp/ESP32-S3-YuanDi-Board` 仓库**没有 LICENSE 文件**，按 GitHub 默认 = 版权所有。本仓库**直接包含了 5 个 .h 文件**（minimind.h 等），所以必须显著声明这一事实 + 标明使用条件，避免使用者误以为"全 MIT"。

---

## v1.1 — 2026-08-29 (Web Chat 完整版)

### 新增
- **Web Chat UI**：板子开 AP `YuanDi-S3-MiniMind`，手机/电脑连上开 `http://192.168.4.1` 浏览器流式聊天
- **SSE (Server-Sent Events) 流式输出**：token-by-token 推送到前端
- **Markdown 渲染**：标题/粗体/斜体/代码块/列表/段落 + U+FFFD 残字节 dim dot
- **状态条**：tok/s、ctx、prefill%、gen count、kv cache 实时显示
- **分词器（Tokenizer）模态**：调 BPE 编码 API，按 token 高亮 + 显示 ID
- **系统监控（Sysmon）模态**：CPU 使用率（idle task 增量）、ESP32-S3 内部温度、PSRAM/RAM 余量
- **System prompt 自定义**：About 弹窗可改，持久化到 NVS；**留空 = 不输出 system 段**
- **日志查看器（Log）**：ring buffer 200 行，自动跟随最新
- **主题切换**：暗 / 亮 双主题（localStorage 持久化）

### 改进
- **自定义 partition**：`factory 0x10000 size=0x140000` (1.3MB) + `model 0x150000 size=0xEA0000` (14.6MB) — 让 1.14MB 固件能放下
- **PSRAM=opi 修复**：arduino-cli 短 fqbn 不开启 PSRAM，完整 fqbn 加 `PSRAM=opi,FlashSize=16M,...`
- **Token 表 extern 化**：web_chat.cpp 用 extern 引用 .ino 唯一定义，避免每个 .cpp 各自填导致 web chat 路径用未初始化副本
- **Tokenizer 串口路径已修复**（`tok_byte_to_id` 等数组在 .ino 唯一定义）
- **WDT 全关**：mm_load + 1.5MB fp16→fp32 转换会超 8s 默认 WDT

### 修复
- `tok_init` 死循环防御（`tok_m_key` / `tok_m_val` NULL 检查）
- 采样函数加 greedy fast-path
- `[chat] user_q len=27` 等诊断 log 全部接 web log buffer (供 /api/log 查询)

### 已知问题
- ESP32-S3 4-bit 量化 vs PC fp16 输出有少量精度差距（量化损失），某些 prompt 短问答可能输出重复
- 模型 26M 在 240MHz 双核极限速度约 0.4-0.8 tok/s，长回复需要等
- 1.6MB 0xFF padding 写进 model partition (0x150000 起的 14.6MB region)，不影响功能但占空间

---

## v1.0 — 2026-08-28 (串口基线版)

- 原始 `zhuhai-esp/ESP32-S3-YuanDi-Board` 项目 1.04MB 串口 chat 固件
- 串口回车发消息、回包 token-by-token
- 0 网络依赖，烧完即用
