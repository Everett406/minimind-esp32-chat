// YuanDi-S3 AI Chat: MiniMind2-Small (26M, Llama-style) 串口聊天推理。
//
// - 模型 (int4 量化 ~13MB) 放在 flash 自定义 `model` 分区 (0x110000)，mmap 只读。
//   由 tools/convert_minimind.py 从 MiniMind2-Small (.pth) 转换而来。
// - 分词器 (GPT-2 byte-level BPE, vocab 6400) 由 include/tokenizer_data.h 提供
//   (tools/convert_minimind.py tokgen 由 tokenizer.json 生成)。
// - 输出头与 embedding 共享 (tie)，KV cache / scratch 在 PSRAM。
// - 无显示屏：串口输入提示词，串口流式输出生成文本。
// - 板载 WS2812 (GPIO48) 显示状态：
//     蓝=启动/加载  红=错误  绿=空闲等待输入  黄=编码提示词  紫闪=正在生成

#include <Adafruit_NeoPixel.h>
#include <Arduino.h>

#include "esp_heap_caps.h"
#include "soc/rtc_cntl_reg.h"
#include "soc/timer_group_reg.h"
#include "esp_idf_version.h"
#include "esp_partition.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_log.h"

#include "minimind.h"     // 推理引擎
#include "tokenizer.h"    // GPT-2 BPE 分词器 (含 tokenizer_data.h)
#include "web_chat.h"     // AP + WebServer + SSE 流式聊天前端 (extern MM model/MMScratch s)
#include <Preferences.h>  // NVS 存储 system prompt

// ---------------- token tables 唯一定义 (extern 声明在 tokenizer.h) ----------------
// 必须有这一份! token 表由 tok_init 填充, 串口和 web 两条路径共享
int tok_bu[256];
int tok_byte_to_id[256];
char **tok_bl;
const char **tok_bl_key;
int         *tok_bl_val;
uint32_t *tok_m_key;
uint32_t *tok_m_val;

// ---------------- 配置 ----------------
#define PIN_WS2812 48
#define MAX_PROMPT_TOKENS 256
#define MAX_GENERATE 200
#define SAMPLE_TEMP 0.8f
#define SAMPLE_TOPK 40
#define SYSTEM_PROMPT "You are a helpful assistant."
#define DEFAULT_USER "Hello, who are you?"

// ---------------- WS2812 状态灯 ----------------
Adafruit_NeoPixel led(1, PIN_WS2812, NEO_GRB + NEO_KHZ800);

static void led_set(uint8_t r, uint8_t g, uint8_t b) {
  // led.setPixelColor(0, led.Color(r, g, b));
  // led.show();
}
static void led_boot()  { led_set(0, 0, 60); }    // 蓝：启动/加载
static void led_idle()  { led_set(0, 60, 0); }    // 绿：等待输入
static void led_busy()  { led_set(60, 40, 0); }   // 黄：编码提示词
static void led_error() { led_set(80, 0, 0); }    // 红：错误
static void led_gen(bool phase) {                 // 紫闪：生成中
  led_set(phase ? 50 : 10, 0, phase ? 50 : 10);
}

static void fatal(const char *msg) {
  Serial.println(msg);
  // led_error();
  for (;;) delay(1000);
}

// ---------------- 模型 ----------------
MM model;
MMScratch s;

static void *ps(size_t n) {
  void *p = heap_caps_malloc(n, MALLOC_CAP_SPIRAM);
  if (!p) {
    Serial.printf("\n[ps FAIL] requested=%u bytes (%.1f KB), free PSRAM=%u KB, largest free block=%u KB\n",
                  (unsigned)n, n / 1024.0f,
                  (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024),
                  (unsigned)(heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM) / 1024));
    fatal("PSRAM 分配失败");
  }
  return p;
}
// 内部 SRAM: matvec 高频复用的向量 (x/h/q/k/v/...) 放这里，访问远快于 PSRAM。
// 仅 KV cache (1MB) 因太大留在 PSRAM。
static void *pi(size_t n) {
  void *p = heap_caps_malloc(n, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  if (!p) fatal("内部 RAM 分配失败 (减小 logits/ffn 或关闭其它任务)");
  return p;
}

// 输出头 matvec 使用默认路径 (MM_MV / mm_matvec_par)
// 注: int8 staging 对多 group scale (n_groups>1) 不安全, 已移除

// ---------------- token 解码 ----------------
static void emit(int tok) {
  if (tok_is_special(tok)) return;            // 特殊 token 不输出
  char buf[16];
  int len = tok_decode_token(tok, buf);
  if (len > 0) Serial.write((const uint8_t *)buf, len);
}

// MiniMind 对话结束符：<|endoftext|>=0 或 <|im_end|>=2
static bool is_end(int tok) { return tok == 0 || tok == 2; }

// temperature + top-k 采样（单遍扫描 + 插入排序维护 top-k）。
static int sample_token(const float *logits, int n) {
  static int idx[SAMPLE_TOPK];
  static float p[SAMPLE_TOPK];
  int k = 0;
  for (int v = 0; v < n; v++) {
    float x = logits[v];
    if (k == SAMPLE_TOPK && x <= p[SAMPLE_TOPK - 1]) continue;
    int j = (k < SAMPLE_TOPK) ? k++ : SAMPLE_TOPK - 1;
    while (j > 0 && p[j - 1] < x) {
      p[j] = p[j - 1];
      idx[j] = idx[j - 1];
      j--;
    }
    p[j] = x;
    idx[j] = v;
  }
  float maxl = p[0], sum = 0.f;
  for (int i = 0; i < k; i++) {
    p[i] = expf((p[i] - maxl) / SAMPLE_TEMP);
    sum += p[i];
  }
  float r = (esp_random() / 4294967296.0f) * sum, c = 0.f;
  for (int i = 0; i < k; i++) {
    c += p[i];
    if (r <= c) return idx[i];
  }
  return idx[0];
}

// ---------------- 聊天模板 ----------------
// 组装 MiniMind 的 chat 模板:
//   <|im_start|>system\n{sys}<|im_end|>
//   <|im_start|>user\n{user}<|im_end|>
//   <|im_start|>assistant\n
static void build_prompt(const char *user, char *out, int cap) {
  int n = snprintf(out, cap,
    "<|im_start|>system\n%s<|im_end|>\n"
    "<|im_start|>user\n%s<|im_end|>\n"
    "<|im_start|>assistant\n",
    SYSTEM_PROMPT, user);
  (void)n;
}

// ---------------- 生成 ----------------
static void generate(const char *user_text) {
  // led_busy();
  char prompt[2048];
  build_prompt(user_text, prompt, sizeof(prompt));

  int prompt_ids[MAX_PROMPT_TOKENS];
  int np = tok_encode(prompt, prompt_ids, MAX_PROMPT_TOKENS);
  if (np == 0) {
    Serial.println("(提示词无法编码, 使用默认输入)");
    np = tok_encode(DEFAULT_USER, prompt_ids, MAX_PROMPT_TOKENS);
  }

  Serial.print(">>> ");
  Serial.print("[prefill: ");
  int pos = 0;                 // 每次输入重置上下文 (KV cache 从 0 重写)
  for (int i = 0; i < np; i++) {
    // led_gen(i & 1);            // 编码提示词时闪烁：表示正在"思考"
    if ((i & 3) == 0) Serial.print(".");   // 每 4 token 一个点, 让用户看到进度
    mm_forward(&model, prompt_ids[i], pos++, &s);
    if ((i & 15) == 0) delay(0);   // prefill 期间喂看门狗
  }
  Serial.printf("] done (%d tokens)\n", np);

  int64_t t_start = esp_timer_get_time();
  int64_t decode_us = 0;
  int decoded = 0;

  Serial.print("[generating");
  for (int step = 0; step < MAX_GENERATE && pos < model.c.seq_len; step++) {
    int tok = sample_token(s.logits, model.c.vocab);
    if (is_end(tok)) break;    // 遇到 <|im_end|>/<|endoftext|> 结束本轮回复
    emit(tok);
    // led_gen(step & 1);

    int64_t d0 = esp_timer_get_time();
    mm_forward(&model, tok, pos++, &s);
    decode_us += esp_timer_get_time() - d0;
    decoded++;
    if ((step & 7) == 0) delay(0);   // 喂任务看门狗
    // 每 16 step 报告一次生成进度 (decode 约 1.2s/token, 16 step ≈ 20s)
    if ((step & 15) == 15) {
      int64_t elapsed = esp_timer_get_time() - t_start;
      Serial.printf(" %d/%d, %.1fs", decoded, MAX_GENERATE, elapsed / 1e6);
    }
  }
  Serial.print("] done\n");

  int64_t total_us = esp_timer_get_time() - t_start;
  Serial.printf("\n\n--- %d tokens, %.2f s, %.2f tok/s (%.1f ms/token) ---\n",
                decoded, total_us / 1e6,
                decoded ? decoded * 1e6 / total_us : 0,
                decoded ? decode_us / 1000.0 / decoded : 0);
  // led_idle();
}

// ---------------- setup / loop ----------------
void setup() {
  // 静音 ESP-IDF 内部 log (task_wdt 错误会刷屏抢占 UART, 导致 Serial.println banner flush 不出)
  // ESP_LOG_NONE=6 完全静默, 之前 ESP_LOG_ERROR 阈值包括 ERROR 级别所以 task_wdt 错误还显示
  esp_log_level_set("*", ESP_LOG_NONE);
  // arduino-esp32 3.3.11 默认 task WDT + RTC WDT, mm_load + ps_alloc + 173K fp16→fp32
  // 在某些编译配置下超过 8s 触发 WDT reset. 直接操作寄存器禁掉所有 WDT
  // (arduino-esp32 3.3.11 不链接 rtc_wdt lib, 无法用 rtc_wdt_disable() API)
  // disableLoopWDT() 禁掉 loopTask 自己的 esp_task_wdt_reset 周期调用 (arduino-esp32 公开 API)
  disableLoopWDT();
  disableCore0WDT();
  disableCore1WDT();
  // Task WDT: 清 TIMG_T0_EN 位 (TG0)
  CLEAR_PERI_REG_MASK(TIMG_T0CONFIG_REG(0), TIMG_T0_EN);
  CLEAR_PERI_REG_MASK(TIMG_T0CONFIG_REG(1), TIMG_T0_EN);
  // RTC WDT: 解锁保护 + 清 RTC_CNTL_WDT_EN 位
  CLEAR_PERI_REG_MASK(RTC_CNTL_WDTWPROTECT_REG, RTC_CNTL_WDT_WKEY);
  SET_PERI_REG_MASK(RTC_CNTL_WDTWPROTECT_REG, 0x50d83aa1);  // 任意值 unlock
  CLEAR_PERI_REG_MASK(RTC_CNTL_WDTCONFIG0_REG, RTC_CNTL_WDT_EN);
  Serial.setRxBufferSize(1024);
  Serial.begin(115200);
  Serial.println("S1: serial init done");
  // led.begin();
  // led.setBrightness(40);
  // led_boot();
  delay(1500);
  Serial.println("\n=== YuanDi-S3 MiniMind2-Small (serial chat) ===");
  Serial.println("S2: banner printed");

  const esp_partition_t *part = esp_partition_find_first(
      ESP_PARTITION_TYPE_DATA, (esp_partition_subtype_t)0x40, "model");
  Serial.printf("S3: part = %p\n", (void*)part);
  if (!part) fatal("未找到 model 分区 (检查 partitions.csv 是否生效)");

  const void *base;
#if ESP_IDF_VERSION_MAJOR >= 5
  esp_partition_mmap_handle_t h;
  if (esp_partition_mmap(part, 0, part->size, ESP_PARTITION_MMAP_DATA, &base, &h) != ESP_OK)
    fatal("model 分区 mmap 失败");
#else
  spi_flash_mmap_handle_t h;
  if (esp_partition_mmap(part, 0, part->size, SPI_FLASH_MMAP_DATA, &base, &h) != ESP_OK)
    fatal("model 分区 mmap 失败");
#endif

  Serial.printf("\n[diag] PSRAM total=%u KB free=%u KB largest=%u KB, internal free=%u KB\n",
                (unsigned)(heap_caps_get_total_size(MALLOC_CAP_SPIRAM) / 1024),
                (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024),
                (unsigned)(heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM) / 1024),
                (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024));
  Serial.print("S4: loading model from flash (mmap + mm_load, ~5-10s)... ");
  // PSRAM=opi 修好, 用 ps() 预转换 all_fscales (1.5MB, 8MB 里够)
  if (mm_load((const uint8_t *)base, &model, ps))
    fatal("模型 magic 错误: 请用 esptool 烧录 MiniMind 模型:\n"
          "  esptool.py --chip esp32s3 write_flash 0x110000 data/model.bin");
  Serial.println("done");
  Serial.printf("[diag] AFTER model load: PSRAM free=%u KB largest=%u KB\n",
                (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024),
                (unsigned)(heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM) / 1024));

  MMCfg *c = &model.c;
  Serial.printf("model: V=%d D=%d L=%d H=%d KV=%d F=%d HD=%d S=%d (mapped %.1f MB)\n",
                c->vocab, c->dim, c->n_layers, c->n_heads, c->n_kv_heads,
                c->ffn, c->head_dim, c->seq_len, part->size / 1e6);

  int D = c->dim, L = c->n_layers, F = c->ffn, V = c->vocab, S = c->seq_len;
  int Q = c->n_heads * c->head_dim, K = c->n_kv_heads * c->head_dim;
  // 小型 per-step 缓冲放内部 SRAM (matvec 高频读写, 远快于 PSRAM); 仅 KV cache 留 PSRAM
  s.x = (float *)pi((size_t)D * 4);
  s.h = (float *)pi((size_t)D * 4);
  s.q = (float *)pi((size_t)Q * 4);
  s.k = (float *)pi((size_t)2 * K * 4);   // 融合 kv_proj 输出 2K (前 K=k, 后 K=v)
  s.att = (float *)pi((size_t)Q * 4);
  s.g1 = (float *)pi((size_t)2 * F * 4);  // 融合 gate_up_proj 输出 2F
  s.logits = (float *)pi((size_t)V * 4);
  s.scores = (float *)pi((size_t)S * 4);
  s.kcache = (float *)ps((size_t)L * K * S * 4);
  s.vcache = (float *)ps((size_t)L * K * S * 4);

  tok_init();
  mm_parallel_init();          // 拉起双核并行 worker (core 0) 加速 matvec
  Serial.printf("PSRAM free after alloc: %u KB\n",
                heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024);

  // web_chat 启动 AP + WebServer + 路由; 内部用 extern MM model / MMScratch s
  web_chat_init();
  Serial.println("\n输入消息后回车开始对话 (空行 = 默认问题):");
  // led_idle();
}

void loop() {
  static char line[512];
  static int len = 0;

  // web_chat: 非阻塞处理 HTTP 请求 (流式生成期间会周期性被 handleChat 调)
  web_chat_loop();

  while (Serial.available()) {
    char ch = (char)Serial.read();
    if (ch == '\r') continue;
    if (ch == '\n') {
      line[len] = '\0';
      Serial.println();
      generate(len ? line : DEFAULT_USER);
      Serial.print("\n>> ");
      len = 0;
      return;
    }
    if (ch == 8 || ch == 127) {   // 退格
      if (len > 0) { len--; /* Serial.print("\b \b"); */ }  // 退格不回显 (monitor 自己管)
      continue;
    }
    if (len < (int)sizeof(line) - 1 && (uint8_t)ch >= 32) {  // 允许 UTF-8 高位字节(中文)
      line[len++] = ch;
      // Serial.write(ch);   // 板子不回显 (arduino-cli monitor 自身会 local echo, 避免双重)
    }
  }
  delay(5);
}
