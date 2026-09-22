// web_chat.h - MiniMind2-Small 简易 Web 聊天前端 (AP + WebServer + SSE)
//
// 设计要点:
//   - 不动 main.cpp 的 LLM 核心 (mm_forward / sample_token / generate 都不改)。
//   - web_chat.cpp 通过 extern 引用 main.cpp 里的全局 MM model / MMScratch s,
//     自己跑一份 prefill + decode + sample 的流式循环, 每个 token 通过
//     web_chat_emit_token() 推给所有 SSE 客户端。
//   - main.cpp 只需要在 setup() 末尾加 1 行 web_chat_init(),
//     loop() 开头加 1 行 web_chat_loop(), 其它代码原样不动。
//
// 路由:
//   GET  /             聊天 HTML 单页
//   GET  /api/chat?q=  SSE 流式响应 (text/event-stream, data: <token>\n\n)
//   GET  /api/status   JSON {tok_s, ctx, prefill_pct, token_count, kv_used, kv_total}
//
// 默认热点:  SSID = "YuanDi-S3-MiniMind"  开放 AP (无密码)
//            用户可在 web_chat.cpp 顶部改 WC_AP_SSID。

#pragma once
#include <Arduino.h>

// ---------------- 状态 (供 /api/status 读, web_chat_loop 内部写) ----------------
struct WebChatState {
  bool          streaming;         // 是否正在生成
  int           prompt_tokens;     // 当前 prompt 编码后的 token 数
  int           generated_tokens;  // 已生成并推送的 token 数
  int           ctx_len;           // 当前 KV cache 占用长度 (prefill+decode)
  int           kv_total;          // 模型 seq_len 上限
  float         tokens_per_sec;    // 最近一个 decode step 的 tok/s
  unsigned long t_start_ms;        // 当前生成开始的 millis()
};

extern WebChatState g_wc;

// ---------------- API ----------------

// 在 main.cpp setup() 末尾调一次: 启动 AP 热点 + WebServer 80 端口。
void web_chat_init();

// 在 main.cpp loop() 开头调: 非阻塞处理 HTTP 请求 (g_server.handleClient())。
// 流式响应期间会被 handleChat() 内部周期性调用, 让出 CPU 给 /api/status。
void web_chat_loop();

// 推一个 token 给当前 SSE 客户端 (web_chat 内部使用)。
// 如果对端断开则静默忽略。
void web_chat_emit_token(const char* token_str);

// 推一个 "event: done" 并关闭当前 SSE 连接 (web_chat 内部使用)。
void web_chat_emit_done();
