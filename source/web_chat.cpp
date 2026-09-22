// web_chat.cpp - ESP32-S3 简易 Web 聊天前端
//
// 工作流:
//   1. setup() 末尾调 web_chat_init():
//        WiFi.softAP() 开热点  +  WebServer 80 端口注册 3 个路由
//   2. loop() 开头调 web_chat_loop():
//        g_server.handleClient() (非阻塞, 单次 ~ms 级)
//   3. 浏览器连热点访问 http://192.168.4.1/ -> 聊天 HTML
//        输入回车 -> fetch('/api/chat?q=...') 拉 SSE 流 -> 逐 token 显示
//        同时 setInterval(200ms) 拉 /api/status 刷新状态条
//
// 设计原则:
//   - 不动 main.cpp: 通过 extern MM model / MMScratch s 复用同一份模型状态
//   - 不改 LLM 核心: sample_token / build_prompt 在本文件内复制一份 (语义与
//     main.cpp 完全一致), 流式生成循环在本文件内独立实现
//   - 不引新库: 只用 Arduino 内置 WiFi.h + WebServer.h
//   - main.cpp 仅需改 2 行:
//        setup() 末尾加  web_chat_init();
//        loop()  开头加  web_chat_loop();

#include "web_chat.h"

#include <WiFi.h>
#include <WebServer.h>
#include <Arduino.h>
#include <math.h>
#include <stdarg.h>
#include <Preferences.h>  // NVS 存 system prompt

#include "minimind.h"     // MM, MMScratch, mm_forward, MMCfg
#include "tokenizer.h"    // tok_encode, tok_decode_token
#include "esp_timer.h"    // esp_timer_get_time
#include "esp_random.h"   // esp_random

// ============================================================
// 聊天 HTML (单文件内嵌, 用 PROGMEM 存 flash)
// ============================================================
static const char INDEX_HTML[] PROGMEM = R"rawliteral(
<!doctype html>
<html lang="zh-CN" data-theme="dark">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1,viewport-fit=cover">
<meta name="theme-color" content="#0f1115">
<title>YuanDi-S3 · MiniMind 聊天</title>
<style>
  *, *::before, *::after { box-sizing: border-box; -webkit-tap-highlight-color: transparent; }
  html, body { margin: 0; padding: 0; height: 100%; overscroll-behavior: none; }
  body {
    font-family: -apple-system, BlinkMacSystemFont, "PingFang SC", "Microsoft YaHei",
                 "Segoe UI", Roboto, "Helvetica Neue", monospace, sans-serif;
    line-height: 1.55;
    display: flex; flex-direction: column; height: 100dvh;
    transition: background-color 0.2s, color 0.2s;
  }
  /* ---- themes ---- */
  html[data-theme="dark"] body  { background: #0f1115; color: #d8dce4; }
  html[data-theme="light"] body { background: #f7f7f8; color: #1a1c20; }

  /* ---- top bar ---- */
  header {
    flex: 0 0 auto;
    padding: 10px 16px;
    padding-top: max(10px, env(safe-area-inset-top));
    display: flex; align-items: center; gap: 12px; flex-wrap: wrap;
    border-bottom: 1px solid var(--border);
  }
  html[data-theme="dark"]  header { background: #14171d; border-bottom-color: #1f232c; --border: #1f232c; }
  html[data-theme="light"] header { background: #ffffff; border-bottom-color: #e3e5ea; --border: #e3e5ea; }

  header .title { font-size: 15px; font-weight: 600; letter-spacing: 0.2px; }
  header .title .ip { color: #6b7280; font-weight: 400; font-size: 12px; margin-left: 6px; font-family: ui-monospace, Menlo, monospace; }

  header .stats { display: flex; gap: 14px; flex: 1 1 auto; font-size: 11px; font-variant-numeric: tabular-nums;
                  font-family: ui-monospace, Menlo, monospace; flex-wrap: wrap; }
  header .stats span { white-space: nowrap; }
  header .stats b { font-weight: 600; }
  html[data-theme="dark"]  header .stats { color: #5b6473; }
  html[data-theme="light"] header .stats { color: #8a93a4; }
  html[data-theme="dark"]  header .stats b { color: #cbd0db; }
  html[data-theme="light"] header .stats b { color: #2a2f3a; }
  header .stats b.live { color: #16a34a; }
  header .stats .dot { display: inline-block; width: 6px; height: 6px; border-radius: 50%;
                        background: #16a34a; margin-right: 4px; vertical-align: 1px; }

  header .theme {
    flex: 0 0 auto;
    background: transparent; border: 1px solid transparent;
    width: 36px; height: 36px; padding: 0;
    border-radius: 50%; cursor: pointer;
    color: var(--text);
    display: flex; align-items: center; justify-content: center;
    position: relative; overflow: hidden;
    transition: background 0.2s ease, border-color 0.2s ease, transform 0.15s ease;
  }
  header .theme:hover { background: rgba(127,127,127,0.12); }
  header .theme:active { transform: scale(0.92); }
  header .theme .ico {
    position: absolute;
    width: 18px; height: 18px;
    color: var(--text);
    stroke: currentColor;
    transition: opacity 0.3s ease, transform 0.4s cubic-bezier(0.4, 0, 0.2, 1);
  }
  /* 暗色模式: 显示太阳 (点一下变亮) */
  html[data-theme="dark"]  .theme .sun  { opacity: 1; transform: rotate(0deg)   scale(1); }
  html[data-theme="dark"]  .theme .moon { opacity: 0; transform: rotate(90deg)  scale(0.5); }
  /* 亮色模式: 显示月亮 (点一下变暗) */
  html[data-theme="light"] .theme .sun  { opacity: 0; transform: rotate(-90deg) scale(0.5); }
  html[data-theme="light"] .theme .moon { opacity: 1; transform: rotate(0deg)   scale(1); }

  /* About 按钮跟主题按钮同款: 圆形 + SVG i 图标 */
  header .about {
    flex: 0 0 auto;
    background: transparent; border: 1px solid transparent;
    width: 36px; height: 36px; padding: 0;
    border-radius: 50%; cursor: pointer;
    color: var(--text);
    display: flex; align-items: center; justify-content: center;
    transition: background 0.2s ease, transform 0.15s ease;
  }
  header .about:hover { background: rgba(127,127,127,0.12); }
  header .about:active { transform: scale(0.92); }
  header .about .ico {
    width: 18px; height: 18px;
    color: var(--text);
    stroke: currentColor;
  }
  /* 日志按钮同款 */
  header .log {
    flex: 0 0 auto;
    background: transparent; border: 1px solid transparent;
    width: 36px; height: 36px; padding: 0;
    border-radius: 50%; cursor: pointer;
    color: var(--text);
    display: flex; align-items: center; justify-content: center;
    transition: background 0.2s ease, transform 0.15s ease;
  }
  header .log:hover { background: rgba(127,127,127,0.12); }
  header .log:active { transform: scale(0.92); }
  header .log .ico {
    width: 18px; height: 18px;
    color: var(--text);
    stroke: currentColor;
  }
  /* tokenize + sysmon 按钮: 跟 .log 同款, 只是多了个 badge */
  header .tokenize, header .sysmon {
    flex: 0 0 auto;
    background: transparent; border: 1px solid transparent;
    width: 36px; height: 36px; padding: 0;
    border-radius: 50%; cursor: pointer; position: relative;
    color: var(--text);
    display: flex; align-items: center; justify-content: center;
    transition: background 0.2s ease, transform 0.15s ease;
  }
  header .tokenize:hover, header .sysmon:hover { background: rgba(127,127,127,0.12); }
  header .tokenize:active, header .sysmon:active { transform: scale(0.92); }
  header .tokenize .ico, header .sysmon .ico {
    width: 18px; height: 18px; color: var(--text); stroke: currentColor;
  }
  header .sysmon .badge {
    position: absolute; bottom: -2px; right: -2px;
    font-size: 9px; line-height: 1; padding: 2px 4px;
    border-radius: 6px; background: #2563eb; color: #fff;
    font-family: ui-monospace, Menlo, monospace; font-weight: 600;
    min-width: 22px; text-align: center;
  }

  /* ---- 弹窗 (About) ---- */
  .modal {
    position: fixed; inset: 0; z-index: 100;
    display: flex; align-items: center; justify-content: center;
    padding: 24px 16px;            /* 上下 24px, 永远不贴顶贴底 */
    overflow-y: auto;               /* 内容超出时整个 modal 可滚 */
  }
  .modal[hidden] { display: none; }
  .modal-backdrop {
    position: absolute; inset: 0;
    background: rgba(0, 0, 0, 0.45);
    backdrop-filter: blur(4px);
    -webkit-backdrop-filter: blur(4px);
    animation: fadeIn 0.2s ease;
  }
  html[data-theme="light"] .modal-backdrop { background: rgba(0, 0, 0, 0.25); }
  .modal-dialog {
    position: relative;
    max-width: 560px; width: 100%;
    /* max-height = 视口 - 上下 padding, 保证任意屏至少有 24px 上下留白 */
    max-height: calc(100vh - 48px);
    overflow-y: auto;
    border-radius: 8px;
    padding: 24px 28px 28px;
    box-shadow: 0 20px 50px rgba(0, 0, 0, 0.25);
    animation: dialogIn 0.25s cubic-bezier(0.4, 0, 0.2, 1);
    -webkit-overflow-scrolling: touch;
  }
  .modal-dialog-wide { max-width: 880px; }
  .modal-dialog pre {
    margin: 0; padding: 12px 14px; border-radius: 4px;
    font-family: ui-monospace, Menlo, monospace; font-size: 12px; line-height: 1.45;
    max-height: 60vh; overflow: auto; white-space: pre-wrap; word-break: break-all;
  }
  html[data-theme="dark"]  .modal-dialog pre { background: #0a0c10; color: #b8bcc4; border: 1px solid #1f232c; }
  html[data-theme="light"] .modal-dialog pre { background: #f7f7f8; color: #1a1c20; border: 1px solid #e3e5ea; }
  @media (max-width: 600px) {
    .modal { padding: 16px 12px; }
    .modal-dialog { padding: 20px 20px 24px; max-height: calc(100vh - 32px); }
  }
  html[data-theme="dark"]  .modal-dialog { background: #14171d; color: #d8dce4; border: 1px solid #1f232c; }
  html[data-theme="light"] .modal-dialog { background: #ffffff; color: #1a1c20; border: 1px solid #e3e5ea; }
  .modal-dialog h2 { margin: 0 0 18px; font-size: 18px; font-weight: 600; padding-right: 24px; }
  .modal-dialog h3 { margin: 18px 0 8px; font-size: 13px; font-weight: 600;
                     text-transform: uppercase; letter-spacing: 0.5px;
                     opacity: 0.65; }
  .modal-dialog p { margin: 4px 0; font-size: 14px; line-height: 1.55; }
  .modal-dialog code { font-family: ui-monospace, Menlo, monospace; font-size: 12.5px;
                       background: rgba(127,127,127,0.12); padding: 1px 5px; border-radius: 3px; }
  .modal-dialog .kv { display: flex; justify-content: space-between; padding: 4px 0;
                      border-bottom: 1px dashed rgba(127,127,127,0.15); font-size: 13.5px; }
  .modal-dialog .kv:last-child { border-bottom: none; }
  .modal-dialog .kv b { font-weight: 500; opacity: 0.95; }
  .modal-dialog .kv span { opacity: 0.7; font-family: ui-monospace, Menlo, monospace; }
  .modal-dialog .hint { font-size: 12px; opacity: 0.6; margin: 4px 0 6px; line-height: 1.5; }
  .modal-dialog textarea {
    width: 100%; box-sizing: border-box; min-height: 64px; max-height: 200px;
    padding: 8px 10px; font: inherit; font-size: 13px; line-height: 1.5;
    border-radius: 4px; resize: vertical; outline: none;
    font-family: ui-monospace, Menlo, monospace;
  }
  html[data-theme="dark"]  .modal-dialog textarea { background: #0f1115; color: #d8dce4; border: 1px solid #2a2f3a; }
  html[data-theme="light"] .modal-dialog textarea { background: #f7f7f8; color: #1a1c20; border: 1px solid #d4d7dd; }
  .modal-actions { display: flex; gap: 8px; margin-top: 8px; }
  .modal-actions button {
    flex: 0 0 auto; height: 32px; padding: 0 14px;
    border: none; border-radius: 4px; cursor: pointer; font: inherit; font-size: 13px;
    transition: opacity 0.15s, background 0.15s;
  }
  .modal-actions .btn-primary { background: #2563eb; color: #fff; }
  .modal-actions .btn-secondary { background: rgba(127,127,127,0.15); color: inherit; }
  .modal-actions button:hover { opacity: 0.85; }
  .modal-close {
    position: absolute; top: 12px; right: 12px;
    width: 28px; height: 28px;
    background: transparent; border: none; cursor: pointer;
    color: inherit; opacity: 0.5;
    font-size: 22px; line-height: 1; border-radius: 50%;
    display: flex; align-items: center; justify-content: center;
    transition: opacity 0.15s, background 0.15s;
  }
  .modal-close:hover { opacity: 1; background: rgba(127,127,127,0.12); }
  @keyframes fadeIn   { from { opacity: 0; } to { opacity: 1; } }
  @keyframes dialogIn { from { opacity: 0; transform: translateY(8px) scale(0.98); } to { opacity: 1; transform: translateY(0) scale(1); } }

  /* ---- main scroll ---- */
  main { flex: 1 1 auto; min-height: 0; overflow-y: auto; overflow-x: hidden;
         -webkit-overflow-scrolling: touch; }
  main::-webkit-scrollbar { width: 6px; }
  main::-webkit-scrollbar-thumb { background: rgba(127,127,127,0.25); border-radius: 3px; }

  .container { max-width: 760px; margin: 0 auto; padding: 16px; }
  @media (max-width: 600px) { .container { padding: 12px; } }

  /* ---- 移动端适配: 紧凑 header + 短 placeholder ---- */
  @media (max-width: 600px) {
    header { padding: 8px 12px; gap: 8px; }
    header .title { font-size: 13px; }
    header .title .ip { display: none; }   /* 手机端 IP 太挤, 隐藏 */
    header .stats { gap: 10px; font-size: 10px; }
    .composer textarea { font-size: 16px; min-height: 38px; padding: 8px 10px; }
    .composer button { width: 38px; height: 38px; }
    .msg { margin: 8px 0; }
    .msg .bubble { padding: 9px 12px; font-size: 14px; }
  }
  @media (max-width: 380px) {
    header .stats { gap: 6px; }
    header .stats span:nth-child(n+4) { display: none; }  /* 超小屏只保留 state+tok/s+ctx */
  }

  /* ---- messages ---- */
  .msg { margin: 12px 0; }
  .msg .bubble {
    padding: 10px 14px;
    border: 1px solid var(--border);
    word-wrap: break-word; overflow-wrap: anywhere;
    white-space: pre-wrap;
    font-size: 15px;
    border-radius: 3px;
    display: inline-block;
    max-width: 100%;
    transition: background 0.25s ease;
  }
  .msg .bubble .pill {
    display: inline-flex;
    align-items: center;
    gap: 6px;
    font-size: 11px;
    line-height: 1;
    padding: 4px 10px;
    border-radius: 999px;
    background: #2563eb1a;
    color: #60a5fa;
    margin-bottom: 8px;
    font-weight: 500;
    letter-spacing: 0.3px;
    white-space: nowrap;
  }
  .msg .bubble .pill::before {
    content: '';
    width: 5px; height: 5px;
    border-radius: 50%;
    background: #60a5fa;
    animation: pulse 1.2s ease-in-out infinite;
  }
  .msg .bubble .content { white-space: pre-wrap; line-height: 1.55; }
  /* markdown 元素样式 (renderMarkdown() 生成) */
  .msg .bubble .content h1,
  .msg .bubble .content h2,
  .msg .bubble .content h3,
  .msg .bubble .content h4 {
    margin: 12px 0 6px; font-weight: 600; line-height: 1.3;
  }
  .msg .bubble .content h1 { font-size: 18px; }
  .msg .bubble .content h2 { font-size: 16px; }
  .msg .bubble .content h3 { font-size: 15px; opacity: 0.9; }
  .msg .bubble .content h4 { font-size: 14px; opacity: 0.85; }
  .msg .bubble .content p { margin: 6px 0; }
  .msg .bubble .content p:first-child { margin-top: 0; }
  .msg .bubble .content p:last-child  { margin-bottom: 0; }
  .msg .bubble .content strong { font-weight: 600; }
  .msg .bubble .content em { font-style: italic; }
  .msg .bubble .content code {
    font-family: ui-monospace, Menlo, monospace; font-size: 13px;
    padding: 1px 5px; border-radius: 3px;
    background: rgba(127,127,127,0.18);
  }
  .msg .bubble .content ul,
  .msg .bubble .content ol { margin: 6px 0; padding-left: 24px; }
  .msg .bubble .content li { margin: 2px 0; }
  .msg .bubble .content br { line-height: 1.55; }
  /* U+FFFD (4-bit 量化产生的残破 UTF-8) → 浅色小圆点, 不抢眼 */
  .msg .bubble .content .unk {
    display: inline-block;
    width: 0.35em; height: 0.35em;
    border-radius: 50%;
    background: currentColor;
    opacity: 0.3;
    vertical-align: middle;
    margin: 0 1px;
  }
  /* 打字机光标: 2px 细线 + 圆角 + 平滑呼吸 (ChatGPT/Claude 风格) */
  .msg .bubble.streaming .content::after {
    content: '';
    display: inline-block;
    width: 2px;
    height: 1.15em;
    margin-left: 2px;
    background: currentColor;
    opacity: 0;
    border-radius: 1px;
    vertical-align: text-bottom;
    animation: caret 1.1s ease-in-out infinite;
  }
  .msg.user { text-align: left; }
  .msg.user .bubble { border-color: transparent; }
  .msg.bot .bubble.thinking { color: #6b7280; }
  .msg.bot .bubble.thinking .content { display: none; }
  @keyframes caret  { 0%, 100% { opacity: 0.15; } 50% { opacity: 0.85; } }
  @keyframes pulse  { 0%, 100% { opacity: 0.3; } 50% { opacity: 1; } }

  html[data-theme="dark"]  .msg.user .bubble { background: #1e3a8a; color: #dbeafe; }
  html[data-theme="light"] .msg.user .bubble { background: #1e3a8a; color: #ffffff; }
  html[data-theme="dark"]  .msg.bot  .bubble { background: #14171d; color: #d8dce4; border-color: #1f232c; }
  html[data-theme="light"] .msg.bot  .bubble { background: #ffffff; color: #1a1c20; border-color: #e3e5ea; }

  /* ---- composer ---- */
  .composer {
    flex: 0 0 auto;
    padding: 8px 12px;
    padding-bottom: max(8px, env(safe-area-inset-bottom));
  }
  html[data-theme="dark"]  .composer { background: #14171d; border-top: 1px solid #1f232c; }
  html[data-theme="light"] .composer { background: #ffffff; border-top: 1px solid #e3e5ea; }
  .composer .row {
    max-width: 760px; margin: 0 auto;
    display: flex; gap: 8px; align-items: flex-end;
  }
  .composer textarea {
    flex: 1 1 auto; min-height: 40px; max-height: 140px;
    padding: 10px 12px; border-radius: 3px;
    font: inherit; font-size: 15px; line-height: 1.4;
    resize: none; outline: none;
    /* 关键: pre + overflow-x = 不换行 + horizontal scroll. 光标永远紧贴最后一个字,
       不会"wrap 到第二行". Shift+Enter 仍能换行 (\n 被保留). */
    white-space: pre; overflow-x: auto; word-wrap: normal;
    transition: border-color 0.15s, background 0.15s;
  }
  html[data-theme="dark"]  textarea { background: #0f1115; color: #d8dce4; border: 1px solid #2a2f3a; }
  html[data-theme="light"] textarea { background: #f7f7f8; color: #1a1c20; border: 1px solid #d4d7dd; }
  html[data-theme="dark"]  textarea:focus { border-color: #4f8cff; }
  html[data-theme="light"] textarea:focus { border-color: #2563eb; }

  .composer button {
    flex: 0 0 auto; width: 40px; height: 40px;
    border-radius: 3px; border: none; cursor: pointer;
    color: #fff; display: flex; align-items: center; justify-content: center;
    transition: opacity 0.15s;
  }
  html[data-theme="dark"]  .composer button { background: #2563eb; }
  html[data-theme="light"] .composer button { background: #1e3a8a; }
  .composer button:active { opacity: 0.85; }
  .composer button:disabled { opacity: 0.4; cursor: not-allowed; }
  .composer button svg { width: 16px; height: 16px; fill: currentColor; position: absolute;
                          transition: opacity 0.2s, transform 0.2s; }
  /* 发送 vs 停止: 同时只显示一个, 平滑切换 */
  .composer button .ico-stop { opacity: 0; transform: scale(0.5) rotate(-90deg); }
  .composer button.is-busy .ico-send { opacity: 0; transform: scale(0.5) rotate(90deg); }
  .composer button.is-busy .ico-stop { opacity: 1; transform: scale(1) rotate(0); }
  /* 停止按钮变红 */
  html[data-theme="dark"]  .composer button.is-busy { background: #dc2626; }
  html[data-theme="light"] .composer button.is-busy { background: #dc2626; }
</style>
</head>
<body>
  <header>
    <div class="title">ESP32-S3 · N16R8 · MiniMind<span class="ip">192.168.4.1</span></div>
    <div class="stats" id="stats">
      <span><span class="dot"></span><b id="s_state">idle</b></span>
      <span>tok/s <b id="s_tok">0.00</b></span>
      <span>ctx <b id="s_ctx">0</b></span>
      <span>prefill <b id="s_pre">0%</b></span>
      <span>gen <b id="s_n">0</b></span>
      <span>kv <b id="s_kv">0/0</b></span>
    </div>
    <button class="theme" id="theme" aria-label="切换主题">
      <svg class="ico sun" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round">
        <circle cx="12" cy="12" r="4"/>
        <path d="M12 2v2M12 20v2M4.93 4.93l1.41 1.41M17.66 17.66l1.41 1.41M2 12h2M20 12h2M4.93 19.07l1.41-1.41M17.66 6.34l1.41-1.41"/>
      </svg>
      <svg class="ico moon" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round">
        <path d="M21 12.79A9 9 0 1 1 11.21 3 7 7 0 0 0 21 12.79z"/>
      </svg>
    </button>
    <button class="log" id="log-btn" aria-label="查看日志">
      <svg class="ico" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round">
        <path d="M4 6h16M4 12h16M4 18h10"/>
      </svg>
    </button>
    <button class="tokenize" id="tokenize-btn" aria-label="分词器">
      <svg class="ico" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round">
        <path d="M4 4h6v6H4zM14 4h6v6h-6zM4 14h6v6H4zM14 14h2v2h-2zM18 14h2v2h-2zM14 18h2v2h-2zM18 18h2v2h-2z"/>
      </svg>
    </button>
    <button class="sysmon" id="sysmon-btn" aria-label="CPU/温度" title="CPU/温度">
      <svg class="ico" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round">
        <path d="M9 2v3M15 2v3M9 19v3M15 19v3M5 9H2v6h3M22 9h-3v6h3M6 6h12v12H6z"/>
        <rect x="9" y="9" width="6" height="6" rx="0.5"/>
      </svg>
      <span class="badge" id="sysmon-badge">…</span>
    </button>
    <button class="about" id="about" aria-label="关于">
      <svg class="ico" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round">
        <circle cx="12" cy="12" r="10"/>
        <line x1="12" y1="16" x2="12" y2="12"/>
        <line x1="12" y1="8" x2="12.01" y2="8"/>
      </svg>
    </button>
  </header>
  <main id="out"><div class="container" id="msgs"></div></main>
  <div class="composer">
    <div class="row">
      <textarea id="q" rows="1" placeholder="说点什么…"></textarea>
      <button id="send" aria-label="发送">
        <svg class="ico-send" viewBox="0 0 24 24"><path d="M2 21l21-9L2 3v7l15 2-15 2v7z"/></svg>
        <svg class="ico-stop" viewBox="0 0 24 24"><rect x="6" y="6" width="12" height="12" rx="1.5"/></svg>
      </button>
    </div>
  </div>

<div class="modal modal-log" id="log-modal" hidden>
  <div class="modal-backdrop" data-close></div>
  <div class="modal-dialog modal-dialog-wide">
    <button class="modal-close" data-close aria-label="关闭">×</button>
    <h2>实时日志</h2>
    <p class="hint">最近 ~200 行, 每秒拉取. 启动时 log 不会进 buffer (web_chat 还没起来), 所以这段只能看 web_chat 之后的输出.</p>
    <pre id="log-view"></pre>
  </div>
</div>

<div class="modal" id="modal" hidden>
  <div class="modal-backdrop" data-close></div>
  <div class="modal-dialog">
    <button class="modal-close" data-close aria-label="关闭">×</button>
    <h2>关于本机</h2>

    <h3>硬件</h3>
    <div class="kv"><b>主控</b><span>ESP32-S3 N16R8</span></div>
    <div class="kv"><b>CPU</b><span>Xtensa LX7 双核 240 MHz</span></div>
    <div class="kv"><b>Flash / PSRAM</b><span>16 MB / 8 MB</span></div>
    <div class="kv"><b>WiFi</b><span>2.4 GHz 802.11 b/g/n</span></div>

    <h3>模型</h3>
    <div class="kv"><b>名称</b><span>MiniMind2-Small</span></div>
    <div class="kv"><b>参数量</b><span>26 M</span></div>
    <div class="kv"><b>层数 / 头数</b><span>8 / 8</span></div>
    <div class="kv"><b>隐层 / 头维</b><span>512 / 64</span></div>
    <div class="kv"><b>词表</b><span>6 400 (GPT-2 byte-BPE)</span></div>
    <div class="kv"><b>上下文</b><span>256 tokens</span></div>
    <div class="kv"><b>量化</b><span>4-bit group-64 (≈15 MB)</span></div>
    <div class="kv"><b>解码速度</b><span>≈ 0.7 tok/s</span></div>
    <div class="kv"><b>采样</b><span>temp=0.8, top_k=40</span></div>

    <h3>软件栈</h3>
    <p>纯 Arduino ESP32 3.3.11, <code>WebServer</code> + 手写 <code>HTTP/1.0</code> + SSE
       (绕过 chunked 触发 LoadProhibited 的 bug)。</p>
    <p>前端纯 HTML/CSS/JS, 无框架, 全部塞在 1.1 MB 固件里 (PROGMEM)。</p>
    <p>模型权重 mmap 自 <code>0x120000</code> 分区 (14.5 MB), 运行时按需 PSRAM 分配 KV cache。</p>

    <h3>用法</h3>
    <p>手机连 <code>YuanDi-S3-MiniMind</code> WiFi (开放 AP, 无密码), 浏览器开 <code>192.168.4.1</code>。</p>
    <p>输入中文问模型即可, 4-bit 量化下偶尔有 token 卡死/重复, 属于模型自身能力上限。</p>

    <h3>系统 Prompt</h3>
    <p class="hint">拼成 ChatML 的 system 段喂给模型。<b>留空 = 不输出 system 段</b>（模型直接 user→assistant，无角色提示）。改完点保存，存 NVS，断电保留。</p>
    <textarea id="prompt-edit" rows="3" placeholder="留空 = 无 system 段（默认）"></textarea>
    <div class="modal-actions">
      <button class="btn-secondary" id="prompt-default" data-close>恢复默认</button>
      <button class="btn-primary" id="prompt-save">保存</button>
    </div>
    <p class="hint" id="prompt-status"></p>

    <h3>参考</h3>
    <p>参考项目: <code>zhuhai-esp/ESP32-S3-YuanDi-Board</code>
       (B 站"机器知芯"频道), MiniMind by jiaweizzhao。</p>
  </div>
</div>

<!-- 弹窗: 分词器 (BPE tokenize 预览) -->
<div class="modal" id="tokenize-modal" hidden>
  <div class="modal-backdrop" data-close></div>
  <div class="modal-dialog">
    <button class="modal-close" data-close aria-label="关闭">×</button>
    <h2>分词器</h2>
    <p class="hint">调 ESP32 端 GPT-2 BPE tokenizer, 显示模型实际看到的 token 序列。</p>
    <textarea id="tok-input" rows="3" placeholder="输入中文/英文, 看分词结果">如果我有三个苹果, 吃掉一个, 还剩几个</textarea>
    <div class="modal-actions">
      <button class="btn-primary" id="tok-run">分词</button>
    </div>
    <p class="kv" style="margin-top:14px"><span>Tokens</span><b id="tok-count">0</b></p>
    <div id="tok-out" style="display:flex;flex-wrap:wrap;gap:4px;line-height:1.6"></div>
    <p class="hint" id="tok-meta"></p>
  </div>
</div>

<!-- 弹窗: 系统监控 (CPU / 温度 / 内存) -->
<div class="modal" id="sysmon-modal" hidden>
  <div class="modal-backdrop" data-close></div>
  <div class="modal-dialog">
    <button class="modal-close" data-close aria-label="关闭">×</button>
    <h2>系统监控</h2>
    <p class="hint">ESP32-S3 实时数据。CPU 算 IDLE0 task 增量利用率, 温度用内置 sensor。</p>
    <div class="kv"><span>CPU 使用率</span><b id="sm-cpu">–</b></div>
    <div class="kv"><span>芯片温度</span><b id="sm-temp">–</b></div>
    <div class="kv"><span>内部 RAM 剩余</span><b id="sm-heap">–</b></div>
    <div class="kv"><span>PSRAM 剩余</span><b id="sm-psram">–</b></div>
    <div class="kv"><span>当前 KV cache</span><b id="sm-kv">–</b></div>
    <p class="hint">每 2 秒刷新</p>
  </div>
</div>

<script>
(function () {
  const out = document.getElementById('out');
  const msgs = document.getElementById('msgs');
  const q = document.getElementById('q');
  const btn = document.getElementById('send');
  const themeBtn = document.getElementById('theme');
  let busy = false;
  let currentCtrl = null;  // 当前的 fetch AbortController, 用于中止生成

  // ---- theme ----
  const saved = localStorage.getItem('md_theme');
  if (saved) document.documentElement.setAttribute('data-theme', saved);
  // 图标由 CSS 根据 data-theme 切换 (旋转淡出), 不再需要 textContent
  themeBtn.addEventListener('click', () => {
    const cur = document.documentElement.getAttribute('data-theme');
    const next = cur === 'light' ? 'dark' : 'light';
    document.documentElement.setAttribute('data-theme', next);
    localStorage.setItem('md_theme', next);
  });

  // ---- about modal ----
  const aboutBtn = document.getElementById('about');
  const modal = document.getElementById('modal');
  const openModal = () => { modal.hidden = false; };
  const closeModal = () => { modal.hidden = true; };
  aboutBtn.addEventListener('click', openModal);
  modal.addEventListener('click', (e) => { if (e.target.dataset.close !== undefined) closeModal(); });
  document.addEventListener('keydown', (e) => { if (e.key === 'Escape' && !modal.hidden) closeModal(); });

  // ---- 日志 modal: 打开后每秒拉一次 /api/log, 增量追加 ----
  const logBtn = document.getElementById('log-btn');
  const logModal = document.getElementById('log-modal');
  const logView  = document.getElementById('log-view');
  let logTotal = 0;          // 已经渲染到的 total (next expected = logTotal + 1)
  let logTimer = null;
  const closeLog = () => { logModal.hidden = true; if (logTimer) { clearInterval(logTimer); logTimer = null; } };
  const fetchLog = async () => {
    try {
      const r = await fetch('/api/log');
      if (!r.ok) return;
      const j = await r.json();
      // 简单粗暴: 每次拉全量, 但只在 total 变了才重渲染
      if (j.total === logTotal) return;  // 没新行
      logTotal = j.total;
      // 把数组拼成文本 (空字符串过滤)
      logView.textContent = j.lines.filter(s => s.length > 0).join('\n');
      logView.scrollTop = logView.scrollHeight;
    } catch (_) { /* 忽略 */ }
  };
  const openLog = () => {
    logModal.hidden = false;
    logTotal = 0;  // 强制拉全量
    fetchLog();
    if (logTimer) clearInterval(logTimer);
    logTimer = setInterval(fetchLog, 1000);
  };
  logBtn.addEventListener('click', openLog);
  logModal.addEventListener('click', (e) => { if (e.target.dataset.close !== undefined) closeLog(); });
  document.addEventListener('keydown', (e) => { if (e.key === 'Escape' && !logModal.hidden) closeLog(); });

  // ---- system prompt 编辑 ----
  const promptEdit  = document.getElementById('prompt-edit');
  const promptSave  = document.getElementById('prompt-save');
  const promptReset = document.getElementById('prompt-default');
  const promptStat  = document.getElementById('prompt-status');
  // 打开 modal 时拉一次最新值
  aboutBtn.addEventListener('click', async () => {
    promptStat.textContent = '';
    try {
      const r = await fetch('/api/prompt');
      if (r.ok) promptEdit.value = (await r.text());
    } catch (_) { /* 忽略 */ }
  });
  promptSave.addEventListener('click', async () => {
    const v = promptEdit.value;
    promptSave.disabled = true; promptStat.textContent = '保存中…';
    try {
      const r = await fetch('/api/prompt', {
        method: 'POST',
        headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
        body: 'p=' + encodeURIComponent(v)
      });
      promptStat.textContent = r.ok ? '✓ 已保存 (断电保留)' : '✗ ' + (await r.text());
    } catch (e) { promptStat.textContent = '✗ ' + e.message; }
    promptSave.disabled = false;
  });
  promptReset.addEventListener('click', () => {
    promptEdit.value = '';
    promptStat.textContent = '已清空（未保存）— 留空表示无 system 段';
  });

  // ---- chat ----
  function scrollDown() { out.scrollTop = out.scrollHeight; }

  // 极简 markdown 渲染: 标题/**粗体**/*斜体*/`code`/列表/段落/换行
  // HTML escape 防 XSS; U+FFFD → <span class="unk"></span> (浅色小圆点)
  function renderMarkdown(text) {
    const esc = text.replace(/[&<>"']/g, c => (
      { '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' }[c]
    ));
    let h = esc;
    // 代码块 ```...```
    h = h.replace(/```([\s\S]*?)```/g, (m, c) => '<pre><code>' + c + '</code></pre>');
    // 标题
    h = h.replace(/^###### (.+)$/gm, '<h6>$1</h6>')
         .replace(/^##### (.+)$/gm, '<h5>$1</h5>')
         .replace(/^#### (.+)$/gm,  '<h4>$1</h4>')
         .replace(/^### (.+)$/gm,  '<h3>$1</h3>')
         .replace(/^## (.+)$/gm,   '<h2>$1</h2>')
         .replace(/^# (.+)$/gm,    '<h1>$1</h1>');
    // 粗体 + 斜体
    h = h.replace(/\*\*([^*\n]+?)\*\*/g, '<strong>$1</strong>')
         .replace(/\*([^*\n]+?)\*/g,    '<em>$1</em>');
    // 行内代码
    h = h.replace(/`([^`\n]+)`/g, '<code>$1</code>');
    // 列表: 连续多行 - 开头 / 数字. 开头
    h = h.replace(/(?:^|\n)((?:- [^\n]+\n?)+)/g, (m, blk) => {
      const items = blk.trim().split('\n').map(l => '<li>' + l.replace(/^- /, '') + '</li>').join('');
      return '\n<ul>' + items + '</ul>\n';
    });
    h = h.replace(/(?:^|\n)((?:\d+\. [^\n]+\n?)+)/g, (m, blk) => {
      const items = blk.trim().split('\n').map(l => '<li>' + l.replace(/^\d+\. /, '') + '</li>').join('');
      return '\n<ol>' + items + '</ol>\n';
    });
    // 段落: 块级元素外的内容包 <p>, \n 变 <br>
    const parts = h.split(/(\n{2,})/);
    let out = '';
    for (const p of parts) {
      if (/^\n+$/.test(p)) { out += p; continue; }
      if (/^<(h\d|ul|ol|pre|p)/.test(p.trim())) out += p;
      else out += '<p>' + p.replace(/\n/g, '<br>') + '</p>';
    }
    h = out.replace(/<p>\s*<\/p>/g, '').replace(/<p>(<h\d>.*?<\/h\d>)/g, '$1');
    // U+FFFD → dim dot span
    h = h.replace(/\uFFFD/g, '<span class="unk"></span>');
    return h;
  }
  function addMsg(role, text) {
    const wrap = document.createElement('div');
    wrap.className = 'msg ' + role;
    const body = document.createElement('div');
    body.className = 'bubble';
    if (role === 'bot' && !text) {
      body.classList.add('thinking');
      // 胶囊 (状态) + 内容 (流式文本) 分离, 顶部胶囊, 下面内容
      const pill = document.createElement('span');
      pill.className = 'pill';
      pill.textContent = '预填充中…';
      const content = document.createElement('span');
      content.className = 'content';
      body.appendChild(pill);
      body.appendChild(content);
    } else {
      body.textContent = text;
    }
    wrap.appendChild(body);
    msgs.appendChild(wrap);
    scrollDown();
    return body;
  }

  async function send() {
    if (busy) {
      // 中止当前生成
      if (currentCtrl) { try { currentCtrl.abort(); } catch (_) {} }
      return;
    }
    const text = q.value.trim();
    if (!text) return;
    busy = true;
    btn.classList.add('is-busy');
    btn.disabled = false;  // 保持可点 (用于中止)
    btn.setAttribute('aria-label', '停止生成');
    q.disabled = true;
    addMsg('user', text);
    q.value = ''; q.style.height = 'auto';
    const body = addMsg('bot', '');
    // 取 body 下的 .pill 和 .content 子节点, 避免每次 querySelector
    const pillEl = body.querySelector('.pill');
    const contentEl = body.querySelector('.content');
    const setPill = (txt) => { if (pillEl) pillEl.textContent = txt; };
    const dropPill = () => { if (pillEl) pillEl.remove(); };
    // 流式 markdown 渲染: 累积 rawBuf, rAF 节流重渲染
    let rawBuf = '';
    let renderScheduled = false;
    const flushRender = () => {
      renderScheduled = false;
      if (contentEl) contentEl.innerHTML = renderMarkdown(rawBuf);
      scrollDown();
    };
    const scheduleRender = () => {
      if (renderScheduled) return;
      renderScheduled = true;
      requestAnimationFrame(flushRender);
    };
    currentCtrl = new AbortController();
    try {
      const r = await fetch('/api/chat?q=' + encodeURIComponent(text), { signal: currentCtrl.signal });
      if (!r.ok) { if (contentEl) contentEl.textContent = '[错误 HTTP ' + r.status + ']'; return; }
      const reader = r.body.getReader();
      // fatal:false 让残破 UTF-8 字节自动替换成 U+FFFD (再被 renderMarkdown → dim dot)
      const dec = new TextDecoder('utf-8', { fatal: false });
      let buf = '';
      while (true) {
        const { done, value } = await reader.read();
        if (done) break;
        buf += dec.decode(value, { stream: true });
        let idx;
        while ((idx = buf.indexOf('\n\n')) >= 0) {
          const evt = buf.slice(0, idx);
          buf = buf.slice(idx + 2);
          let evName = '', evData = '';
          for (const line of evt.split('\n')) {
            if (line.startsWith('data: ')) evData = line.slice(6);
            else if (line.startsWith('event: ')) evName = line.slice(7).trim();
          }
          if (evName === 'phase' && evData === 'prefill') {
            setPill('预填充中…');
          } else if (evName === 'phase' && evData === 'decode') {
            setPill('生成中…');
            body.classList.remove('thinking');
            body.classList.add('streaming');
          } else if (evName === 'prefill') {
            setPill('预填充 ' + evData);
          } else if (evName === 'done') {
            // 流结束: 强制立即渲染, 摘掉胶囊
            if (contentEl) contentEl.innerHTML = renderMarkdown(rawBuf);
            dropPill();
            body.classList.remove('streaming');
            body.classList.remove('thinking');
          } else if (evName === 'error') {
            rawBuf += '\n[错误] ' + evData;
            scheduleRender();
          } else if (evData) {
            rawBuf += evData;
            scheduleRender();
          }
          scrollDown();
        }
      }
      // 兜底
      if (buf.startsWith('data: ')) rawBuf += buf.slice(6);
      if (!rawBuf) rawBuf = '[空响应]';
      if (contentEl) contentEl.innerHTML = renderMarkdown(rawBuf);
      dropPill();
      body.classList.remove('streaming', 'thinking');
    } catch (e) {
      if (e.name === 'AbortError') {
        // 用户中止: 立即渲染当前 buffer + 加 [已中止] 标记
        rawBuf += '\n\n_[已中止]_';
        if (contentEl) contentEl.innerHTML = renderMarkdown(rawBuf);
      } else if (contentEl) {
        contentEl.innerHTML = '<p>[错误] ' + (e.message || e) + '</p>';
        console.error(e);
      }
    } finally {
      busy = false;
      currentCtrl = null;
      btn.classList.remove('is-busy');
      btn.disabled = false;
      btn.setAttribute('aria-label', '发送');
      q.disabled = false;
      q.focus();
      // 把 textarea 滚到视图内 (生成中页面一直滚到底, 完成后 focus 回 textarea 但它可能已经离开视口)
      try { q.scrollIntoView({ block: 'nearest', behavior: 'smooth' }); } catch (_) {}
      // 把光标移到 textarea 末尾 (空 = 第一个字符前, 但保险起见强制 setSelection)
      try { q.setSelectionRange(q.value.length, q.value.length); } catch (_) {}
      scrollDown();
    }
  }
  window.send = send;

  q.addEventListener('input', () => {
    q.style.height = 'auto';
    q.style.height = Math.min(q.scrollHeight, 140) + 'px';
    // wrap 时让 textarea 自身滚到底, 光标始终紧贴最后一个字 (而不是跑到 wrap 的空行)
    q.scrollTop = q.scrollHeight;
  });
  q.addEventListener('keydown', e => {
    if (e.key === 'Enter' && !e.isShiftKey && !e.isComposing) {
      e.preventDefault(); send();
    }
  });
  btn.addEventListener('click', send);

  setInterval(async () => {
    try {
      const s = await (await fetch('/api/status')).json();
      document.getElementById('s_tok').textContent = (s.tok_s || 0).toFixed(2);
      document.getElementById('s_ctx').textContent = s.ctx || 0;
      document.getElementById('s_pre').textContent = (s.prefill_pct || 0) + '%';
      document.getElementById('s_n').textContent   = s.token_count || 0;
      document.getElementById('s_kv').textContent  = (s.kv_used || 0) + '/' + (s.kv_total || 0);
      const st = s.streaming ? 'streaming' : 'idle';
      const stEl = document.getElementById('s_state');
      stEl.textContent = st;
      stEl.classList.toggle('live', s.streaming);
      // 顶部 sysmon badge: CPU% + 温度 (e.g. "23%/41°")
      const cpu = (s.cpu_pct != null) ? Math.round(s.cpu_pct) : null;
      const tmp = (s.temp_c != null && !isNaN(s.temp_c)) ? Math.round(s.temp_c) : null;
      const badge = document.getElementById('sysmon-badge');
      if (badge) {
        const cpuTxt = (cpu != null) ? cpu + '%' : '–%';
        const tmpTxt = (tmp != null) ? tmp + '°' : '–°';
        badge.textContent = cpuTxt + tmpTxt;
        // 温度颜色: <50° 绿, 50-65° 黄, >65° 红
        badge.style.background = (tmp != null && tmp >= 65) ? '#dc2626'
                                  : (tmp != null && tmp >= 50) ? '#eab308'
                                  : '#16a34a';
      }
      // 缓存到 sysmon modal 用
      window.__sm_last = s;
    } catch (_) { /* ignore */ }
  }, 500);

  // ---- sysmon modal: 打开时拉一次 + 每 2s 刷 ----
  const sysmonModal = document.getElementById('sysmon-modal');
  const sysmonBtn   = document.getElementById('sysmon-btn');
  function refreshSysmon() {
    const s = window.__sm_last;
    if (!s) return;
    const fmt = n => (n == null || isNaN(n)) ? '–' : (n + '');
    document.getElementById('sm-cpu').textContent   = (s.cpu_pct != null) ? Math.round(s.cpu_pct) + '%' : '–';
    document.getElementById('sm-temp').textContent  = (s.temp_c != null && !isNaN(s.temp_c)) ? s.temp_c.toFixed(1) + ' °C' : '–';
    document.getElementById('sm-heap').textContent  = fmt(s.heap_free) + ' B';
    document.getElementById('sm-psram').textContent = fmt(s.psram_free) + ' B';
    document.getElementById('sm-kv').textContent    = (s.kv_used || 0) + ' / ' + (s.kv_total || 0);
  }
  if (sysmonBtn) sysmonBtn.addEventListener('click', () => {
    sysmonModal.hidden = false;
    refreshSysmon();
    if (!window.__sm_int) window.__sm_int = setInterval(refreshSysmon, 2000);
  });
  // 关 modal 时停刷新
  sysmonModal.addEventListener('click', e => {
    if (e.target.hasAttribute('data-close') || e.target.classList.contains('modal-close')) {
      sysmonModal.hidden = true;
      if (window.__sm_int) { clearInterval(window.__sm_int); window.__sm_int = null; }
    }
  });

  // ---- tokenize modal ----
  const tokModal = document.getElementById('tokenize-modal');
  const tokBtn   = document.getElementById('tokenize-btn');
  const tokRun   = document.getElementById('tok-run');
  const tokInput = document.getElementById('tok-input');
  const tokOut   = document.getElementById('tok-out');
  const tokCount = document.getElementById('tok-count');
  const tokMeta  = document.getElementById('tok-meta');
  // byte → utf8 char 反解 (BPE tokenizer 把 UTF-8 字节编成 token, 显示时反解回 char)
  function byteIdToChar(id) {
    // tok_byte_to_id 表是 byte(0-255) → token_id. 反查要找 id 对应的 byte.
    // 没暴露反查表, 用 try by id mapping via static window 兜底: 简单显示 [id]
    return '[' + id + ']';
  }
  async function runTokenize() {
    const text = tokInput.value || '';
    if (!text) { tokOut.innerHTML = '<i style="color:#888">请先输入文本</i>'; tokCount.textContent='0'; return; }
    tokOut.innerHTML = '<i style="color:#888">分词中…</i>';
    try {
      const r = await fetch('/api/tokenize?text=' + encodeURIComponent(text));
      const j = await r.json();
      tokCount.textContent = j.n;
      tokMeta.textContent = '共 ' + j.n + ' 个 token (上限 256)';
      tokOut.innerHTML = '';
      // 简易反解: 用 TextDecoder 遍历 text, 跟 ids 顺序不严格对应, 但用 byte->id 映射显示大致样子
      // 这里采用: 按 text 字符切, 每个字符查一下, 标到对应 token. 太复杂, 退化为按 token 顺序显示 [id, char]
      // 简化: 用 text 字符数 ~= token 数 (大致) 来 align, 不行就纯 [id]
      const chars = [...text];  // grapheme 数组
      const max = Math.min(j.n, chars.length);
      for (let i = 0; i < j.n; i++) {
        const span = document.createElement('span');
        span.className = 'tok';
        // 给每 token 一个 hash 颜色, 便于区分
        const hue = (j.tokens[i] * 47) % 360;
        span.style.background = 'hsl(' + hue + ' 70% 90%)';
        span.style.color = '#000';
        span.style.padding = '2px 6px';
        span.style.borderRadius = '3px';
        span.style.fontSize = '13px';
        span.style.fontFamily = 'ui-monospace, Menlo, monospace';
        const ch = (i < max) ? chars[i] : '·';
        span.textContent = ch + ' ' + j.tokens[i];
        span.title = 'token id: ' + j.tokens[i];
        tokOut.appendChild(span);
      }
    } catch (e) {
      tokOut.innerHTML = '<i style="color:#dc2626">错误: ' + e.message + '</i>';
    }
  }
  if (tokBtn) tokBtn.addEventListener('click', () => {
    tokModal.hidden = false;
    runTokenize();
  });
  if (tokRun) tokRun.addEventListener('click', runTokenize);
  tokInput.addEventListener('keydown', e => {
    if (e.key === 'Enter' && (e.ctrlKey || e.metaKey)) { e.preventDefault(); runTokenize(); }
  });
})();
</script>
</body>
</html>

)rawliteral";






// ============================================================
// 引用 main.cpp 的全局 LLM 状态 (mm_load 已在 main.cpp setup 里完成)
// ============================================================
extern MM        model;
extern MMScratch s;

// ============================================================
// web_chat 配置 (用户可改)
// ============================================================
#ifndef WC_AP_SSID
#define WC_AP_SSID   "YuanDi-S3-MiniMind"   // 热点名
#endif
// WiFi.softAP 第二个参数 NULL = 开放 AP (无密码); 想加密码改成 "yourpass"
#define WC_MAX_PROMPT_TOKENS 256
#define WC_MAX_GENERATE       200
#define WC_SAMPLE_TEMP        0.8f
#define WC_SAMPLE_TOPK        40
#define WC_SYSTEM_PROMPT      "You are a helpful assistant."
#define WC_DEFAULT_USER       "Hello, who are you?"

// ============================================================
// 全局对象
// ============================================================
WebServer    g_server(80);
WebChatState g_wc = {};
static bool  s_streaming = false;   // /api/chat 互斥锁, 防止并发生成

// 自定义 system prompt (默认空 = 无 system 段, NVS 里存啥就用啥, 可通过 /api/prompt POST 改写)
static String g_system_prompt = "";

// NVS 持久化: 命名空间 "md", key "prompt"
static void loadSystemPrompt() {
  Preferences p;
  p.begin("md", true);  // read-only
  String s = p.getString("prompt", "");
  p.end();
  g_system_prompt = s;  // 空也覆盖 (默认空 = 无 system 段)
  if (g_system_prompt.length() == 0) {
    Serial.println("[web_chat] system prompt: <none> (空 = 无 system 段)");
  } else {
    Serial.printf("[web_chat] system prompt (%u bytes): %s\n", g_system_prompt.length(),
                  g_system_prompt.substring(0, 80).c_str());
  }
}
static void saveSystemPrompt(const String& s) {
  Preferences p;
  p.begin("md", false);  // read-write
  p.putString("prompt", s);
  p.end();
  g_system_prompt = s;
}

// ----- 日志 ring buffer (供 /api/log 查询) -----
#define LOG_MAX 200
#define LOG_LEN 240
static char   log_buf[LOG_MAX][LOG_LEN];
static int    log_head = 0;     // next write slot
static int    log_total = 0;    // total lines ever (capped at INT_MAX)
static portMUX_TYPE log_mux = portMUX_INITIALIZER_UNLOCKED;

// 写一行日志: 同时存到 ring buffer + 串口 (替代 Serial.printf/println)
static void webLogf(const char* fmt, ...) {
  char line[LOG_LEN];
  va_list ap; va_start(ap, fmt); vsnprintf(line, sizeof(line), fmt, ap); va_end(ap);
  // 去掉末尾 \n / \r (统一处理)
  int len = strlen(line);
  while (len > 0 && (line[len-1] == '\n' || line[len-1] == '\r')) line[--len] = 0;
  portENTER_CRITICAL(&log_mux);
  strncpy(log_buf[log_head], line, LOG_LEN - 1); log_buf[log_head][LOG_LEN - 1] = 0;
  log_head = (log_head + 1) % LOG_MAX;
  log_total++;
  portEXIT_CRITICAL(&log_mux);
  Serial.println(line);  // 也同步到串口
}

// GET /api/log    返回最近 ~200 行日志 (按时间顺序) + total 行数
static void handleLog() {
  String body = "{\"total\":" + String(log_total) + ",\"lines\":[";
  portENTER_CRITICAL(&log_mux);
  int start = (log_total < LOG_MAX) ? 0 : log_head;
  int n = (log_total < LOG_MAX) ? log_total : LOG_MAX;
  for (int i = 0; i < n; i++) {
    int slot = (start + i) % LOG_MAX;
    if (i > 0) body += ",";
    body += "\"";
    for (const char* p = log_buf[slot]; *p; p++) {
      if (*p == '"')       body += "\\\"";
      else if (*p == '\\')  body += "\\\\";
      else if (*p == '\n' || *p == '\r') { /* 跳过换行 */ }
      else                  body += *p;
    }
    body += "\"";
  }
  portEXIT_CRITICAL(&log_mux);
  body += "]}";
  g_server.send(200, "application/json", body);
}

// ============================================================
// 复制的 LLM 辅助函数 (与 main.cpp 等价, 不动原文件)
// ============================================================

// MiniMind 对话结束符: <|endoftext|>=0 或 <|im_end|>=2
static bool is_end_tok(int tok) { return tok == 0 || tok == 2; }

// temperature + top-k sampling (与 main.cpp::sample_token 一致)
// 单遍扫描 + 插入排序维护 top-k + softmax 采样
static int sample_token_wc(const float *logits, int n) {
  static int   idx[WC_SAMPLE_TOPK];
  static float p  [WC_SAMPLE_TOPK];
  int k = 0;
  for (int v = 0; v < n; v++) {
    float x = logits[v];
    if (k == WC_SAMPLE_TOPK && x <= p[WC_SAMPLE_TOPK - 1]) continue;
    int j = (k < WC_SAMPLE_TOPK) ? k++ : WC_SAMPLE_TOPK - 1;
    while (j > 0 && p[j - 1] < x) {
      p  [j] = p  [j - 1];
      idx[j] = idx[j - 1];
      j--;
    }
    p  [j] = x;
    idx[j] = v;
  }
  // greedy fast-path: temp=0 或 top_k=1 时直接返回 top-1, 避免 exp(0/0) = NaN
  if (WC_SAMPLE_TEMP <= 0.0001f || k <= 1) {
    return idx[0];
  }
  // (上方 fast-path 在 SAMPLE_TEMP=0.8/SAMPLE_TOPK=40 时不会进)
  float maxl = p[0], sum = 0.f;
  for (int i = 0; i < k; i++) { p[i] = expf((p[i] - maxl) / WC_SAMPLE_TEMP); sum += p[i]; }
  float r = (esp_random() / 4294967296.0f) * sum, c = 0.f;
  for (int i = 0; i < k; i++) { c += p[i]; if (r <= c) return idx[i]; }
  return idx[0];
}

// ChatML prompt 模板 (与 main.cpp::build_prompt 一致, system 段取自 g_system_prompt)
// g_system_prompt 为空 → 整段 system 省略, 模型直接 user→assistant (无角色提示)
static int build_prompt_wc(const char *user, char *out, int cap) {
  if (g_system_prompt.length() == 0) {
    return snprintf(out, cap,
      "<|im_start|>user\n%s<|im_end|>\n"
      "<|im_start|>assistant\n",
      user);
  }
  return snprintf(out, cap,
    "<|im_start|>system\n%s<|im_end|>\n"
    "<|im_start|>user\n%s<|im_end|>\n"
    "<|im_start|>assistant\n",
    g_system_prompt.c_str(), user);
}

// ============================================================
// SSE emit 钩子 (web_chat 内部用, 供将来其它前端重定向)
// ============================================================
void web_chat_emit_token(const char* token_str) {
  if (!g_server.client().connected()) return;
  // SSE 协议:  "data: <text>\n\n"  表示一条消息
  String chunk = String("data: ") + String(token_str) + "\n\n";
  g_server.sendContent(chunk);
  g_server.client().flush();   // 立刻把缓冲区推进去, 避免 WebView 以为连接挂掉
}

static void web_chat_emit_event(const char* name, const char* data) {
  if (!g_server.client().connected()) return;
  String chunk = String("event: ") + name + "\ndata: " + data + "\n\n";
  g_server.sendContent(chunk);
  g_server.client().flush();
}

void web_chat_emit_done() {
  if (g_server.client().connected()) {
    g_server.sendContent("event: done\n\n");
    g_server.sendContent("");     // 触发最后一块 chunked 收尾
  }
}

// ============================================================
// 路由处理
// ============================================================

// GET /         聊天 HTML
static void handleRoot() {
  g_server.send_P(200, "text/html", INDEX_HTML);
}

// GET /api/ping    简单连通测试
static void handlePing() {
  g_server.send(200, "text/plain", "pong");
}

// GET /api/tokenize?text=...
// 调 ESP32 端 GPT-2 BPE tokenizer, 返回 {"n":N, "tokens":[int,int,...]} JSON
// (token 序列, 跟模型实际喂入一致)
static void handleTokenize() {
  if (!g_server.hasArg("text")) { g_server.send(400, "text/plain", "missing text"); return; }
  String text = g_server.arg("text");
  // BPE 一次最多 WC_MAX_PROMPT_TOKENS=256 个, 跟实际 chat 限制一致
  static int ids[WC_MAX_PROMPT_TOKENS + 8];
  int n = tok_encode(text.c_str(), ids, WC_MAX_PROMPT_TOKENS + 8);
  if (n < 0) n = 0;
  if (n > WC_MAX_PROMPT_TOKENS + 8) n = WC_MAX_PROMPT_TOKENS + 8;
  String json = "{\"n\":" + String(n) + ",\"tokens\":[";
  for (int i = 0; i < n; i++) {
    if (i) json += ",";
    json += String(ids[i]);
  }
  json += "]}";
  g_server.send(200, "application/json", json);
}

// GET  /api/prompt    读当前 system prompt (text/plain)
// POST /api/prompt    改 system prompt, body 形如 "p=..." 或纯文本 (持久化到 NVS)
static void handlePrompt() {
  HTTPMethod m = g_server.method();
  if (m == HTTP_GET) {
    g_server.send_P(200, "text/plain; charset=utf-8", g_system_prompt.c_str(), g_system_prompt.length());
  } else if (m == HTTP_POST) {
    String p;
    if (g_server.hasArg("p")) {
      p = g_server.arg("p");
    } else {
      p = g_server.arg("plain");  // Content-Type: text/plain 整体 body
    }
    if (p.length() > 1800) { g_server.send(413, "text/plain", "too long"); return; }  // NVS 限制
    saveSystemPrompt(p);
    Serial.printf("[web_chat] system prompt updated (%u bytes)\n", p.length());
    g_server.send(200, "text/plain", "ok");
  } else {
    g_server.send(405, "text/plain", "method not allowed");
  }
}

// ESP32-S3 CPU 使用率 (0-100): ulTaskGetIdleRunTimeCounter() 返回 IDLE0 task
// 累计运行 tick 数 (默认 CONFIG_FREERTOS_HZ=100Hz, 1 tick = 10ms).
// 用 idle 增量 / millis 增量 算 idle%, 然后 100 - idle% = busy%
static int get_cpu_usage_pct() {
  static UBaseType_t last_idle = 0;
  static unsigned long last_ms = 0;
  static bool inited = false;
  UBaseType_t idle = ulTaskGetIdleRunTimeCounter();
  unsigned long now = millis();
  if (!inited) { last_idle = idle; last_ms = now; inited = true; return 0; }
  int pct = 0;
  unsigned long dwall = now - last_ms;
  UBaseType_t didle = idle - last_idle;
  if (dwall > 0) {
    // idle 累计 tick (100Hz) 转 ms: didle * 10
    uint32_t idle_ms = didle * 10;
    int busy_pct = 100 - (int)((uint64_t)idle_ms * 100 / dwall);
    if (busy_pct < 0) busy_pct = 0;
    if (busy_pct > 100) busy_pct = 100;
    pct = busy_pct;
  }
  last_idle = idle;
  last_ms = now;
  return pct;
}

// ESP32-S3 内置温度传感器 (driver/temperature_sensor.h, esp-idf 5.x)
#include "driver/temperature_sensor.h"
static temperature_sensor_handle_t g_temp_sensor = NULL;
static bool g_temp_inited = false;
static float g_temp_c = NAN;
static void init_temp_sensor_once() {
  if (g_temp_inited) return;
  g_temp_inited = true;
  temperature_sensor_config_t cfg = TEMPERATURE_SENSOR_CONFIG_DEFAULT(20, 100);
  if (temperature_sensor_install(&cfg, &g_temp_sensor) == ESP_OK) {
    temperature_sensor_enable(g_temp_sensor);
  } else {
    g_temp_sensor = NULL;
  }
}
static void read_temp_sensor() {
  if (!g_temp_sensor) return;
  float t = 0;
  if (temperature_sensor_get_celsius(g_temp_sensor, &t) == ESP_OK) {
    g_temp_c = t;
  }
}

// 板子内部 RAM 余量 (heap caps)
static unsigned long get_internal_free() { return ESP.getFreeHeap(); }
static unsigned long get_psram_free()   { return ESP.getFreePsram(); }

// GET /api/status    JSON
static void handleStatus() {
  // prefill 进度: prefill 阶段 ctx 缓慢爬升, decode 阶段 ctx 一次性越过 prompt_tokens
  int prefill_pct = 0;
  if (g_wc.prompt_tokens > 0) {
    int shown = (g_wc.ctx_len < g_wc.prompt_tokens) ? g_wc.ctx_len : g_wc.prompt_tokens;
    prefill_pct = (int)(100.0f * (float)shown / (float)g_wc.prompt_tokens);
    if (prefill_pct > 100) prefill_pct = 100;
  }
  int kv_total = g_wc.kv_total > 0 ? g_wc.kv_total : model.c.seq_len;
  init_temp_sensor_once();
  read_temp_sensor();
  int cpu = get_cpu_usage_pct();
  String json = "{";
  json += "\"tok_s\":"        + String(g_wc.tokens_per_sec, 2);  json += ",";
  json += "\"ctx\":"          + String(g_wc.ctx_len);            json += ",";
  json += "\"prefill_pct\":"  + String(prefill_pct);             json += ",";
  json += "\"token_count\":"  + String(g_wc.generated_tokens);   json += ",";
  json += "\"kv_used\":"      + String(g_wc.ctx_len);            json += ",";
  json += "\"kv_total\":"     + String(kv_total);                json += ",";
  json += "\"cpu_pct\":"      + String(cpu);                     json += ",";
  json += "\"temp_c\":"       + String(g_temp_c, 1);             json += ",";
  json += "\"heap_free\":"    + String((unsigned long)get_internal_free()); json += ",";
  json += "\"psram_free\":"   + String((unsigned long)get_psram_free());
  json += "}";
  g_server.send(200, "application/json", json);
}

// GET /api/chat?q=...    SSE 流式响应
// 直接用底层 WiFiClient 写 HTTP 响应头, 绕过 WebServer 的 setContentLength + sendContent
// (后者在 ESP32 WebServer 上有 LoadProhibited panic)
static void handleChat() {
  webLogf("[chat] >>> handler enter");
  if (s_streaming) {
    webLogf("[chat] WARN: s_streaming was true, reset");
    s_streaming = false;
    g_wc.streaming = false;
  }
  String user_q = g_server.arg("q");
  Serial.print("[chat] user_q len=");
  Serial.println(user_q.length());
  if (user_q.length() > 1024) user_q = user_q.substring(0, 1024);
  if (user_q.length() == 0) user_q = String(WC_DEFAULT_USER);

  s_streaming             = true;
  g_wc.streaming          = true;
  g_wc.t_start_ms         = millis();
  g_wc.prompt_tokens      = 0;
  g_wc.generated_tokens   = 0;
  g_wc.ctx_len            = 0;
  g_wc.tokens_per_sec     = 0;
  g_wc.kv_total           = model.c.seq_len;

  // 拿一份 client 副本, handler 期间一直用它写
  WiFiClient cli = g_server.client();

  // 手动写 HTTP/1.0 响应头 (Connection: close 让浏览器读到底)
  // 注意: 写字符串必须带长度, 否则 const char* overload 会写入 \0 终止符
  auto W = [&cli](const char* s) { cli.write((const uint8_t*)s, strlen(s)); };
  W("HTTP/1.0 200 OK\r\n");
  W("Content-Type: text/event-stream; charset=utf-8\r\n");
  W("Cache-Control: no-cache\r\n");
  W("Connection: close\r\n");
  W("\r\n");
  // 立即 flush 一条 phase: prefill 心跳
  W("event: phase\ndata: prefill\n\n");
  cli.flush();
  webLogf("[chat] SSE headers + prefill event sent");

  // 编码 prompt
  char prompt[2048];
  build_prompt_wc(user_q.c_str(), prompt, sizeof(prompt));
  int prompt_ids[WC_MAX_PROMPT_TOKENS];
  int np = tok_encode(prompt, prompt_ids, WC_MAX_PROMPT_TOKENS);
  // DEBUG: 打印实际 prompt 字符串 + 前 5 个 token id, 跟串口对比
  webLogf("[chat] PROMPT (%d bytes):", (int)strlen(prompt));
  webLogf("%s", prompt);   // 单行内容, webLogf 会去掉换行
  webLogf("---END---");
  for (int i = 0; i < np && i < 5; i++) webLogf("[chat]   tok[%d]=%d", i, prompt_ids[i]);
  webLogf("[chat]   ... (%d total)", np);
  if (np == 0) {
    np = tok_encode(WC_DEFAULT_USER, prompt_ids, WC_MAX_PROMPT_TOKENS);
  }
  g_wc.prompt_tokens = np;
  webLogf("[chat] prompt=%d tokens", np);

  // prefill
  int pos = 0;
  int decoded = 0;
  int64_t t_start = esp_timer_get_time();
  char progress[32];
  for (int i = 0; i < np; i++) {
    mm_forward(&model, prompt_ids[i], pos++, &s);
    g_wc.ctx_len = pos;
    if ((i & 3) == 0) {
      snprintf(progress, sizeof(progress), "%d/%d", i + 1, np);
      W("event: prefill\ndata: ");
      W(progress);
      W("\n\n");
      cli.flush();
      delay(0);
      if (!cli.connected()) { webLogf("[chat] client gone in prefill"); goto chat_end; }
    }
  }
  // prefill 完 -> phase: decode
  W("event: phase\ndata: decode\n\n");
  cli.flush();
  webLogf("[chat] prefill done");

  // decode: 边算边写
  for (int step = 0; step < WC_MAX_GENERATE && pos < model.c.seq_len; step++) {
    int tok = sample_token_wc(s.logits, model.c.vocab);
    if (is_end_tok(tok)) break;

    char buf[16];
    int len = tok_decode_token(tok, buf);
    if (len > 0) {
      buf[len] = 0;
      W("data: ");
      W(buf);
      W("\n\n");
      cli.flush();
    }
    decoded++;
    g_wc.generated_tokens = decoded;
    g_wc.ctx_len          = pos + 1;

    int64_t d0 = esp_timer_get_time();
    mm_forward(&model, tok, pos++, &s);
    int64_t dur = esp_timer_get_time() - d0;
    g_wc.tokens_per_sec   = (dur > 0) ? 1e6f / (float)dur : 0;
    g_wc.ctx_len          = pos;

    if ((step & 1) == 0) {
      delay(0);
      if (!cli.connected()) { webLogf("[chat] client gone in decode"); break; }
    }
  }
  {
    int64_t total_us = esp_timer_get_time() - t_start;
    webLogf("[chat] done %d tokens in %.2f s (%.2f tok/s)",
                  decoded, total_us / 1e6, decoded ? decoded * 1e6 / total_us : 0);
  }
  W("event: done\n\n");
  cli.flush();

chat_end:
  // 关闭 client (HTTP/1.0 Connection: close 模式下, close 触发浏览器读到 EOF)
  cli.stop();
  g_wc.streaming = false;
  s_streaming    = false;
  webLogf("[chat] handler exit");
}

// ============================================================
// init / loop  (main.cpp 调这两个)
// ============================================================
void web_chat_init() {
  // 1) AP 模式热点
  WiFi.mode(WIFI_AP);
  // softAP(ssid, pass, channel, ssid_hidden, max_connection)
  // pass=NULL 表示开放 AP (无密码)
  bool ok = WiFi.softAP(WC_AP_SSID, NULL, 1, 0, 4);
  Serial.printf("[web_chat] softAP(%s) -> %s, IP=%s\n",
                WC_AP_SSID, ok ? "OK" : "FAIL", WiFi.softAPIP().toString().c_str());

  // 2) WebServer 路由
  g_server.on("/",           HTTP_GET, handleRoot);
  g_server.on("/api/ping",   HTTP_GET, handlePing);
  g_server.on("/api/chat",   HTTP_GET, handleChat);
  g_server.on("/api/status", HTTP_GET, handleStatus);
  g_server.on("/api/log",    HTTP_GET, handleLog);
  g_server.on("/api/prompt", handlePrompt);   // GET=读 / POST=改
  g_server.on("/api/tokenize", HTTP_GET, handleTokenize);
  g_server.begin();
  Serial.println("[web_chat] WebServer started on :80");

  // 3) 加载 NVS 里保存的 system prompt (用户通过 /api/prompt 改的)
  loadSystemPrompt();

  // 4) 状态初值
  g_wc.kv_total = model.c.seq_len;
}

void web_chat_loop() {
  // 非阻塞: 单次 handleClient 通常只花几 ms, 流式生成期间会周期性被 handleChat 调
  g_server.handleClient();
}
