# 代码架构

## 1. 整体结构

```
┌────────────────────────────────────────────────────────────┐
│  ESP32-S3                                                    │
│                                                              │
│  ┌────────────┐  ┌────────────┐  ┌──────────────────────┐  │
│  │  串口 (115200)│ │ WiFi AP    │  │ WebServer :80 (SSE)   │  │
│  │  文本协议     │ │ 192.168.4.1│  │ HTML + JS + CSS        │  │
│  └─────┬──────┘  └─────┬──────┘  └──────────┬───────────┘  │
│        │               │                    │              │
│        └───────────────┴────────────────────┘              │
│                          │                                  │
│                ┌─────────▼──────────┐                       │
│                │  Main / loop      │                       │
│                │  - 读 serial      │                       │
│                │  - 读 HTTP        │                       │
│                │  - 调 sample/run  │                       │
│                └─────────┬──────────┘                       │
│                          │                                  │
│         ┌────────────────┼────────────────┐                │
│         ▼                ▼                ▼                │
│  ┌──────────────┐ ┌──────────────┐ ┌──────────────┐      │
│  │ web_chat.cpp │ │ tokenizer.h  │ │ minimind.h   │      │
│  │ HTML+路由    │ │ BPE 编码/解码│ │ 推理引擎     │      │
│  │ SSE 流式输出 │ │ 6.4K 词表    │ │ int4 矩阵乘   │      │
│  └──────┬───────┘ └──────┬───────┘ └──────┬───────┘      │
│         │                │                │              │
│         └────────────────┴────────────────┘              │
│                          │                                  │
│                ┌─────────▼──────────┐                       │
│                │  flash (mmap)     │                       │
│                │  - bootloader    │                       │
│                │  - partitions    │                       │
│                │  - app (1.1MB)   │                       │
│                │  - model (13MB)  │                       │
│                └──────────────────┘                       │
│                          │                                  │
│                ┌─────────▼──────────┐                       │
│                │  PSRAM (8MB)      │                       │
│                │  - 1.5MB fscales  │                       │
│                │  - 1MB   KV cache│                       │
│                │  - 100KB scratch │                       │
│                └──────────────────┘                       │
└────────────────────────────────────────────────────────────┘
```

## 2. 文件职责

| 文件 | 行数 | 职责 |
|---|---|---|
| `YuanDiArduino.ino` | ~330 | 主入口，setup/loop，串口 chat 循环 |
| `web_chat.cpp` | ~1300 | Web 服务器 + HTML/CSS/JS + SSE + 路由处理 + 日志 ring buffer |
| `web_chat.h` | ~50 | web_chat_init() 声明 |
| `minimind.h` | ~430 | 推理引擎 (单 header, inline 全实现) |
| `tokenizer.h` | ~200 | BPE 编码 (tok_encode) + 反 tokenize (tok_decode_token) |
| `tokenizer_data.h` | ~200 | BPE 词表 + merge rank (189 KB 自动生成) |
| `vocab.h` | ~8300 | 反查表 + 词条文本 (800 KB) |
| `llm.h` | ~430 | 备用 LLM 推理 (Pythia 风格, 本项目未使用) |
| `partitions.csv` | 6 | 自定义 partition 布局 |
| `rtc_wdt.h` | ~150 | RTC 看门狗辅助 (绕过 arduino-esp32 3.x 缺 rtc_wdt_disable API) |

## 3. 推理流程（一次 decode step）

```
输入: token_id (int), pos (int)
       │
       ▼
1. mm_deq_row(&tok_emb, token, s->x)        // 取 embedding (V×D int4 → fp32)
       │   s->x[0..D-1] = embedding
       ▼
2. for layer 0..7:
   │  rmsnorm(s->x, attn_norm[l], D, s->h)   // 输入 RMSNorm
   │  mm_matvec(q_proj,  s->h, s->q)          // Q = x @ Q^T (int4)
   │  rmsnorm(q_per_head, q_norm[l], Hd)       // per-head Q norm
   │  mm_matvec(kv_proj, s->h, kv_buf)         // KV = x @ KV^T (fused)
   │  RoPE(q), RoPE(kv)                        // rotary position embed
   │  KV cache: kcache/vcache[l][pos] = kv_buf
   │  Attention: q @ K^T / sqrt(Hd) → softmax → @ V
   │  mm_matvec(o_proj, attn, s->x)            // 残差 + O 投影
   │  rmsnorm(s->x, ffn_norm[l], D, s->h)      // post-attn norm
   │  mm_matvec(gate_up_proj, s->h, g1g2)     // fused gate+up
   │  silu(g1) * g2 → s->h
   │  mm_matvec(down_proj, s->h, s->x)         // 残差 + down
       ▼
3. rmsnorm(s->x, out_norm, D, s->h)            // final norm
       ▼
4. mm_matvec(tok_emb, s->h, logits)            // tied output head
   │  logits[0..V-1]
       ▼
5. top-k=40 + temp=0.8 采样 → next_token
       │
       ▼
返回 next_token
```

## 4. 关键设计决策

### 4.1 int4 量化（PSRAM 友好）

```c
// 权重布局 (flash 中):
//   [group: 4 bytes] [codes: rows * ceil(cols/2)] [scales: rows * n_groups * 2]
//   group: int32, 量化组大小 (e.g. 64)
//   codes: nibble = round(x/scale) + 8, 行内按 ceil(cols/2) 字节对齐
//   scales: fp16, 每 group 一个, value = max_abs/8

// 反量化 (dequant_row):
//   for each group:
//     scale = half2float(scales[gi])
//     for each byte in row:
//       lo = (byte & 0xF) - 8
//       hi = (byte >> 4) - 8
//       x[2k] = lo * scale
//       x[2k+1] = hi * scale
```

### 4.2 fp16 scale → fp32 预转换（PSRAM）

mm_load 第二遍把所有 fp16 scale 一次转 fp32，存到 PSRAM (`fscales` 数组)。
推理时直接用 fp32 scale，避免每个 matvec 都做 half2float 转换。

总共 ~400K 个 scale，PSRAM 1.5 MB 装得下。

### 4.3 Token 表 extern 化

```c
// tokenizer.h
extern int tok_bu[256];
extern char **tok_bl;
...

// YuanDiArduino.ino  — 唯一定义 + tok_init() 填充
int tok_bu[256] = {0};
char **tok_bl = NULL;
...
void tok_init() { /* 从 vocab.h 填 tok_bl 等 */ }

// web_chat.cpp  — 通过 extern 引用同一份
// (注: 头文件里如果加 static, 多个 .cpp include 会各拿到一份未初始化的副本)
```

### 4.4 SSE 流式输出

```cpp
// chat handler 内部:
g_server.sendContent("HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\n\r\n");
while (正在生成) {
  token = mm_sample(logits);
  g_server.sendContent("data: " + utf8(token) + "\n\n");
  g_server.client().flush();  // 关键: 立刻推给浏览器
}

// 事件: event: phase, event: prefill, event: done
```

为什么不用 ESP32 WebServer 的 `setContentLength(UNKNOWN) + sendContent`？
答: **它会触发 LoadProhibited panic**（segmentation fault in LoadProhibited handler）。所以直接绕过 WebServer，用底层 `WiFiClient::write` + 自己写 HTTP/1.0 头。

### 4.5 流式 markdown 渲染 (rAF 节流)

```js
let rawBuf = '';           // 累积原始 token 文本
let renderScheduled = false;
const flushRender = () => {
  if (contentEl) contentEl.innerHTML = renderMarkdown(rawBuf);
  scrollDown();
};
const scheduleRender = () => {
  if (renderScheduled) return;  // 已 schedule, 等下一帧
  renderScheduled = true;
  requestAnimationFrame(flushRender);
};

// 每个 token 来了: rawBuf += token; scheduleRender();
// → 每帧最多重渲染一次 (~60Hz), 不卡顿
```

### 4.6 WebView 防卡死 (flush)

```cpp
g_server.client().flush();  // 每个 SSE event 之后立刻 flush
```

不 flush 的话，TCP send buffer 满了之后 web_chat.cpp 写阻塞，model 推理也被卡住 → 看着像 0.4 tok/s 实际可能更慢。

## 5. 内存布局 (peak)

| 区 | 大小 | 位置 | 用途 |
|---|---|---|---|
| app code | ~1.1 MB | flash @ 0x10000 | 程序 |
| model | ~13.7 MB | flash @ 0x150000 | int4 权重 |
| fscales (fp32) | ~1.5 MB | PSRAM | 预转换 scale |
| KV cache | ~1 MB | PSRAM | 8 layers × 128 KV × 256 ctx × 4 bytes |
| scratch (x/h/q/k/v/...) | ~10 KB | internal SRAM | matvec 中间量 |
| logits | 25.6 KB | internal SRAM | [V=6400] |
| static arrays | ~50 KB | PSRAM/flash | BPE 表 (vocab.h 是 const 数组, 留在 flash) |

## 6. 性能

- **decode 单步** ~150-250 ms / token (4-bit matvec + 8 层 transformer)
- **整体 tok/s**: 0.4-0.8 (单核), 1.0-1.5 (双核，mm_parallel_init 启用 worker)
- **PSRAM 吞吐**: ~50 MB/s (OPI)
- **瓶颈**: matvec 在 240 MHz 跑 4-bit unpack + int8 FMA, 高度依赖 ESP32-S3 的 MULA/MULS 指令
