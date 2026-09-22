# Web UI 使用说明

烧好 Web Chat 版固件后，板子开 AP `YuanDi-S3-MiniMind`（无密码），手机/电脑连上后浏览器开 `http://192.168.4.1`。

## 1. 界面布局

```
┌─────────────────────────────────────────┐
│ ESP32-S3 · N16R8 · MiniMind   192.168.4.1 │  ← 标题
│ ●idle  tok/s 0.45  ctx 18  prefill 100%  │  ← 状态条
│ gen 23  kv 18/256                        │
│                              [🌙] [📋] [□]  │  ← 主题/日志/分词/系统
│                              [⚙] [ⓘ]     │     monitor/about
├─────────────────────────────────────────┤
│                                         │
│   [user] 你好                           │  ← 用户消息 (左对齐)
│   [bot]  你好！很高兴见到你～           │  ← bot 消息
│   [bot]  ▓▓▓ 生成中… 0.4 tok/s         │
│                                         │
│                                         │
├─────────────────────────────────────────┤
│  [textarea  输入消息…]              [▶] │  ← composer
└─────────────────────────────────────────┘
```

## 2. 状态条含义

| 字段 | 含义 |
|---|---|
| `●idle / streaming` | 状态指示器，绿色=live |
| `tok/s` | 当前生成速度 |
| `ctx` | 当前 context 长度（prompt + 生成） |
| `prefill` | 预填充进度 % |
| `gen` | 已生成 token 数 |
| `kv` | KV cache used / total |

## 3. 头部按钮

| 图标 | 功能 |
|---|---|
| 🌙 / ☀ | 暗 / 亮主题切换（localStorage 记住） |
| 📋 | 日志查看器（最近 200 行 ring buffer） |
| ▦ | **分词器** — 输入文本，按"分词"按钮看 BPE 切分（每个 token 一个高亮 box + ID） |
| ⏚ + `23%/41°` | **系统监控** badge — 实时显示 CPU% + 芯片温度（颜色：<50° 绿，50-65° 黄，>65° 红） |
| ⓘ | About — 改 system prompt / 看版本信息 |

## 4. 交互细节

- **发送**：Enter 发送，Shift+Enter 换行
- **停止生成**：点 ▶ 按钮变 ⏹，再点中止当前请求
- **暗主题**：localStorage 记住，刷新不丢
- **响应渲染**：bot 回复用 markdown（**粗体**、*斜体*、`code`、列表、标题、段落）
- **多轮对话**：每个 turn 都独立发送给模型（无 KV cache 跨 turn 复用 — 因为 /api/chat 是新连接）
- **Textarea wrap**：超长输入会**水平滚动**（不换行），光标永远紧贴文字

## 5. System prompt 自定义

点 ⓘ (About) → 找到 "System Prompt" 文本框：

- **留空** → 不输出 `<|im_start|>system` 段（默认）
- **填内容** → 拼成 `<|im_start|>system\n{内容}<|im_end|>` 段
- **恢复默认** 按钮（现在叫"清空"）→ 设回空
- 保存后写 NVS，下次启动自动加载

## 6. 分词器 modal

点 ▦ → 输入文本 → 点 **分词** 按钮（Ctrl+Enter 也行）：

```
输入: 如果我有三个苹果, 吃掉一个, 还剩几个
─────────────────────────────────────
输出: [如][果][我] [有] [三][个] [苹][果]
      309 256 287  299 388  342  290  373
      ...
（共 24 个 token）
```

每个 token 一个 hash 颜色 box，hover 看 ID。

> 注：分词器调的是和模型**同一套 BPE**，所以看到的 token 就是模型实际喂入的（保证 100% 一致）。

## 7. 系统监控 modal

点 ⚙ → 弹出大模态：

| 字段 | 含义 |
|---|---|
| CPU 使用率 | IDLE task 空闲率反算（最近 500ms） |
| 芯片温度 | ESP32-S3 内置 sensor |
| 内部 RAM 剩余 | heap_caps_get_free(MALLOC_CAP_INTERNAL) |
| PSRAM 剩余 | heap_caps_get_free(MALLOC_CAP_SPIRAM) |
| 当前 KV cache | 当前 ctx 长度 / 模型 seq_len (256) |

每 2 秒自动刷新（modal 打开时）。

## 8. 日志查看器

点 📋 → 弹窗显示最近 200 行 ring buffer：

- 时间顺序（最新在最下）
- 自动滚到底
- 包含 boot log + chat 诊断（`[chat] user_q len=27` 之类）

## 9. 流式响应 + 状态

bot 回复时，状态条实时变化：

```
[prefill 12%] → [prefill 100% 预填充 28] → [gen 0 0.00 tok/s] → [gen 1 0.5 tok/s] → ...
```

每个 token 浏览器自动滚到底。

## 10. 已知行为

- **模型生成速度 ~0.4-0.8 tok/s**（26M 在 240MHz 双核极限）— 长回复需要等
- **多轮对话不共享 KV cache** — 每次 /api/chat 是新连接 + 重新 prefill
- **生成中途断网 / 关页面** — 板子继续生成到 200 token 上限后丢弃
- **首问慢**（~5-10 秒 mm_load + prefill）— 之后每条 < 1 秒
