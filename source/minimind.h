// Portable single-header inference engine for MiniMind2-Small (Llama-style).
// Same code runs on the host (verified against a PyTorch/numpy golden) and on
// the ESP32-S3 (mmap'd flash for the int4 weights). Dims come from the model
// header, so no arch-specific #ifdefs are needed.
//
// Architecture (matches jingyaogong/MiniMind model_minimind.py op-for-op):
//   * RMSNorm(x) = x * rsqrt(mean(x^2) + eps) * w
//   * RoPE: NeoX "rotate_half" convention, freq_i = rope_theta^(-2i/dim)
//   * Attention: GQA. q/k are RMSNorm-ed per head (q_norm/k_norm) *before* RoPE,
//                then RoPE, then causal attention with n_rep = n_heads/n_kv_heads.
//   * FFN: SwiGLU  silu(gate_proj(x)) * up_proj(x)  ->  down_proj
//   * tied input/output embedding (lm_head.weight == embed_tokens.weight)
//
// Weight layout on flash (all int4 = symmetric, per-group, fp16 scales,
// matching tools/convert_minimind.py):  codes nibble = round(x/scale)+8
// (value-8), ragged row-aligned packing, scale = max_abs/8 per group.
#ifndef MINIMIND_H
#define MINIMIND_H
#include <stdint.h>
#include <math.h>
#include <string.h>

#define MM_MAGIC 0x4D4E4432u   // "MND2" (v2: fused kv/gate_up + fp32 scales)
#define MM_MAX_LAYERS 32

typedef struct {
  int vocab, dim, n_layers, n_heads, n_kv_heads, ffn, head_dim, seq_len, group, qk_norm;
  float rope_theta, rms_eps;
} MMCfg;

// Group-wise int4 tensor (row-aligned ragged nibbles + fp16 group scales).
// fscales 由 mm_load 预分配: fp16 scale 启动时一次性转为 fp32, 推理免转换。
typedef struct {
  const uint8_t  *codes;   // rows*row_bytes, nibble = value+8, row_bytes=ceil(cols/2)
  const uint16_t *scales;  // rows*n_groups fp16 (flash mmap)
  const float    *fscales; // rows*n_groups fp32 (PSRAM, 预转换, matvec 直接用)
  int rows, cols, group, n_groups, row_bytes;
} MQT;

static inline float mm_half2float(uint16_t h) {
  uint32_t sign = (uint32_t)(h & 0x8000) << 16;
  uint32_t exp = (h >> 10) & 0x1F, man = h & 0x3FF, f;
  if (exp == 0) {
    if (man == 0) f = sign;
    else {
      exp = 127 - 15 + 1;
      while (!(man & 0x400)) { man <<= 1; exp--; }
      man &= 0x3FF; f = sign | (exp << 23) | (man << 13);
    }
  } else if (exp == 0x1F) {
    f = sign | 0x7F800000u | (man << 13);
  } else {
    f = sign | ((exp - 15 + 127) << 23) | (man << 13);
  }
  float out; memcpy(&out, &f, 4); return out;
}

// Per-layer weights that are stored as plain fp32 (norms, q/k head norms).
typedef struct {
  MMCfg c;
  MQT tok_emb;                       // [V, D]  (also the tied output head)
  const float *attn_norm[MM_MAX_LAYERS];   // [D]  input_layernorm
  MQT q_proj[MM_MAX_LAYERS];         // [n_heads*head_dim, D]
  MQT kv_proj[MM_MAX_LAYERS];        // [2*n_kv_heads*head_dim, D] (fused k+v)
  MQT o_proj[MM_MAX_LAYERS];         // [D, n_heads*head_dim]
  const float *q_norm[MM_MAX_LAYERS];      // [head_dim]
  const float *k_norm[MM_MAX_LAYERS];      // [head_dim]
  const float *ffn_norm[MM_MAX_LAYERS];    // [D]  post_attention_layernorm
  MQT gate_up_proj[MM_MAX_LAYERS];   // [2*ffn, D] (fused gate+up)
  MQT down_proj[MM_MAX_LAYERS];      // [D, ffn]
  const float *out_norm;             // [D]
  // 可选: 覆盖输出头的 matvec (例如 int8 预解压加速), NULL 则走默认 MM_MV
  void (*tok_emb_matvec)(const MQT *, const float *, float *);
} MM;

static const uint8_t *mm_bind_q(const uint8_t *p, MQT *t, int rows, int cols) {
  int32_t group; memcpy(&group, p, 4); p += 4;
  t->rows = rows; t->cols = cols; t->group = group;
  t->n_groups = (cols + group - 1) / group;
  t->row_bytes = (cols + 1) / 2;
  t->codes = p;  p += (size_t)rows * t->row_bytes;
  t->scales = (const uint16_t *)p;  p += (size_t)rows * t->n_groups * 2;
  return p;
}
static const uint8_t *mm_bind_f(const uint8_t *p, const float **t, int n) {
  *t = (const float *)p;  return p + (size_t)n * sizeof(float);
}

// Dequantize row r of a quant tensor into out[cols].
static inline void mm_deq_row(const MQT *t, int r, float *out) {
  const uint8_t *row = t->codes + (size_t)r * t->row_bytes;
  const uint16_t *sc = t->scales + (size_t)r * t->n_groups;
  for (int gi = 0; gi < t->n_groups; gi++) {
    int begin = gi * t->group;
    int end = begin + t->group;
    if (end > t->cols) end = t->cols;
    float scale = mm_half2float(sc[gi]);
    int j = begin;
    if ((j & 1) && j < end) { out[j] = (float)((row[j >> 1] >> 4) - 8) * scale; j++; }
    for (; j + 1 < end; j += 2) {
      uint8_t byte = row[j >> 1];
      out[j] = (float)((byte & 0xF) - 8) * scale;
      out[j + 1] = (float)((byte >> 4) - 8) * scale;
    }
    if (j < end) {
      uint8_t byte = row[j >> 1];
      int code = (j & 1) ? (byte >> 4) : (byte & 0xF);
      out[j] = (float)(code - 8) * scale;
    }
  }
}

// Matrix-vector product over a row range [r0, r1): y = W·x with int4
// group-quantized weights. Single-threaded default, and the unit of parallel
// dispatch on multi-core builds.
static inline void mm_matvec_range(const MQT *__restrict t, const float *__restrict x, float *__restrict y, int r0, int r1) {
  const float *fsc = t->fscales;   // 预转换 fp32 scale (PSRAM), 可能为 NULL
  for (int r = r0; r < r1; r++) {
    const uint8_t *row = t->codes + (size_t)r * t->row_bytes;
    float acc = 0.f;
    for (int gi = 0; gi < t->n_groups; gi++) {
      float scale = fsc ? fsc[(size_t)r * t->n_groups + gi]
                        : mm_half2float(t->scales[(size_t)r * t->n_groups + gi]);
      int begin = gi * t->group, end = begin + t->group;
      if (end > t->cols) end = t->cols;
      float group_acc = 0.f;   // 每组单独按自己的 scale 缩放后再累加
      // 成对处理: 一个 byte 含两个 nibble (偶索引=低4位, 奇索引=高4位), 只读一次
      int j = begin;
      if ((j & 1) && j < end) { group_acc += (float)((row[j >> 1] >> 4) - 8) * x[j]; j++; }
      // 展开 4 对 (8 元素 / 4 bytes): 减少 loop 开销, 利于 FMA 链
      for (; j + 7 < end; j += 8) {
        const uint8_t *bp = row + (j >> 1);
        uint8_t b0 = bp[0], b1 = bp[1], b2 = bp[2], b3 = bp[3];
        group_acc += (float)((b0 & 0xF) - 8) * x[j]
                   + (float)((b0 >> 4) - 8) * x[j + 1]
                   + (float)((b1 & 0xF) - 8) * x[j + 2]
                   + (float)((b1 >> 4) - 8) * x[j + 3]
                   + (float)((b2 & 0xF) - 8) * x[j + 4]
                   + (float)((b2 >> 4) - 8) * x[j + 5]
                   + (float)((b3 & 0xF) - 8) * x[j + 6]
                   + (float)((b3 >> 4) - 8) * x[j + 7];
      }
      for (; j + 1 < end; j += 2) {
        uint8_t byte = row[j >> 1];
        group_acc += (float)((byte & 0xF) - 8) * x[j];
        group_acc += (float)((byte >> 4) - 8) * x[j + 1];
      }
      if (j < end) {
        uint8_t byte = row[j >> 1];
        int code = (j & 1) ? (byte >> 4) : (byte & 0xF);
        group_acc += (float)(code - 8) * x[j];
      }
      acc += group_acc * scale;
    }
    y[r] = acc;
  }
}

// Full matrix-vector product (all rows) for the single-threaded path.
static inline void mm_matvec(const MQT *t, const float *x, float *y) {
  mm_matvec_range(t, x, y, 0, t->rows);
}

// ---- optional dual-core parallel path (ESP32-S3) -------------------------
// ESP32-S3 has no matrix/NPU hardware; the only available compute speedup is
// its second LX7 core. Each matvec is split row-wise: the calling core
// computes the lower half inline while a pinned worker on the *other* core
// computes the upper half. Rows are independent (disjoint y writes; read-only
// x and weights), so the only synchronization is a start/end barrier.
#if defined(MM_PARALLEL) && !defined(MM_HOST)
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

typedef struct { const MQT *t; const float *x; float *y; int r0, r1; } MMJob;
static MMJob g_job;
static SemaphoreHandle_t g_bar;
static TaskHandle_t g_worker;

static void mm_worker(void *arg) {
  (void)arg;
  for (;;) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    __sync_synchronize();   // 确保 g_job 可见 (跨核内存屏障)
    mm_matvec_range(g_job.t, g_job.x, g_job.y, g_job.r0, g_job.r1);
    __sync_synchronize();
    xSemaphoreGive(g_bar);
  }
}

// Small matmuls (k/v proj, rows < 256) are cheaper inline than paying the
// two-core sync overhead, so they run on the calling core only.
static inline void mm_matvec_par(const MQT *t, const float *x, float *y) {
  int R = t->rows;
  if (!g_worker || R < 128) { mm_matvec(t, x, y); return; }  // 小矩阵/worker 未就绪: 单核
  int mid = R / 2;
  g_job.t = t; g_job.x = x; g_job.y = y; g_job.r0 = mid; g_job.r1 = R;
  __sync_synchronize();                       // 写完 g_job 再发通知
  xTaskNotifyGive(g_worker);                 // worker -> [mid, R) on other core
  mm_matvec_range(t, x, y, 0, mid);          // this core -> [0, mid)
  xSemaphoreTake(g_bar, portMAX_DELAY);      // wait for worker
}

// Call once at startup (after model load) to spawn the worker task.
static inline void mm_parallel_init(void) {
  g_bar = xSemaphoreCreateCounting(1, 0);
  if (!g_bar) return;
  xTaskCreatePinnedToCore(mm_worker, "mmw", 8192, NULL,
                          configMAX_PRIORITIES - 1, &g_worker, 0);
}
#define MM_MV mm_matvec_par
#else
#define MM_MV mm_matvec
static inline void mm_parallel_init(void) {}  // 单核构建: 空操作
#endif

static inline void mm_rmsnorm(const float *x, const float *w, int n, float *out, float eps) {
  float ss = 0.f;
  for (int i = 0; i < n; i++) ss += x[i] * x[i];
  float inv = 1.f / sqrtf(ss / n + eps);
  for (int i = 0; i < n; i++) out[i] = w[i] * x[i] * inv;
}
static inline float mm_silu(float x) { return x / (1.f + expf(-x)); }

// Parse header + bind all tensors. Returns 0 on ok, -1 on bad magic.
// ps_alloc: PSRAM 分配器 (用于预转换 fp16→fp32 scale), 传 NULL 则跳过预转换。
static int mm_load(const uint8_t *base, MM *m, void *(*ps_alloc)(size_t)) {
  const uint8_t *p = base;
  uint32_t magic; memcpy(&magic, p, 4); p += 4;
  if (magic != MM_MAGIC) return -1;
  int32_t hv[10]; memcpy(hv, p, 40); p += 40;
  m->c.vocab = hv[0]; m->c.dim = hv[1]; m->c.n_layers = hv[2]; m->c.n_heads = hv[3];
  m->c.n_kv_heads = hv[4]; m->c.ffn = hv[5]; m->c.head_dim = hv[6];
  m->c.seq_len = hv[7]; m->c.group = hv[8]; m->c.qk_norm = hv[9];
  memcpy(&m->c.rope_theta, p, 4); p += 4;
  memcpy(&m->c.rms_eps, p, 4); p += 4;

  int D = m->c.dim, L = m->c.n_layers, F = m->c.ffn, V = m->c.vocab;
  int Q = m->c.n_heads * m->c.head_dim;          // attn output dim
  int K = m->c.n_kv_heads * m->c.head_dim;       // k/v proj output dim (each)

  // ---- 第一遍: 绑定所有张量 (flash 指针) ----
  const uint8_t *scan = p;
  // 收集每个 MQT 的 (scales_ptr, n_groups) 用于第二遍预转换
  // 每层: tok_emb(1) + q + kv + o + gate_up + down = 6 个 QT
  // 总计 1 + L*6 个 QT
  struct { const uint16_t *s16; float *s32; int n; } sc_map[1 + MM_MAX_LAYERS * 6];
  int n_qt = 0;

  scan = mm_bind_q(scan, &m->tok_emb, V, D);
  sc_map[n_qt++] = (typeof(sc_map[0])){m->tok_emb.scales, NULL, V * m->tok_emb.n_groups};
  for (int i = 0; i < L; i++) {
    scan = mm_bind_f(scan, &m->attn_norm[i], D);
    scan = mm_bind_q(scan, &m->q_proj[i], Q, D);
    sc_map[n_qt++] = (typeof(sc_map[0])){m->q_proj[i].scales, NULL, Q * m->q_proj[i].n_groups};
    scan = mm_bind_q(scan, &m->kv_proj[i], 2 * K, D);   // fused k+v
    sc_map[n_qt++] = (typeof(sc_map[0])){m->kv_proj[i].scales, NULL, 2 * K * m->kv_proj[i].n_groups};
    scan = mm_bind_q(scan, &m->o_proj[i], D, Q);
    sc_map[n_qt++] = (typeof(sc_map[0])){m->o_proj[i].scales, NULL, D * m->o_proj[i].n_groups};
    if (m->c.qk_norm) {
      scan = mm_bind_f(scan, &m->q_norm[i], m->c.head_dim);
      scan = mm_bind_f(scan, &m->k_norm[i], m->c.head_dim);
    }
    scan = mm_bind_f(scan, &m->ffn_norm[i], D);
    scan = mm_bind_q(scan, &m->gate_up_proj[i], 2 * F, D);  // fused gate+up
    sc_map[n_qt++] = (typeof(sc_map[0])){m->gate_up_proj[i].scales, NULL, 2 * F * m->gate_up_proj[i].n_groups};
    scan = mm_bind_q(scan, &m->down_proj[i], D, F);
    int dn = D * m->down_proj[i].n_groups;
    sc_map[n_qt++] = (typeof(sc_map[0])){m->down_proj[i].scales, NULL, dn};
  }
  scan = mm_bind_f(scan, &m->out_norm, D);
  (void)scan;

  // ---- 第二遍: 预转换所有 fp16 scale → fp32 (一次大块 PSRAM 分配) ----
  if (ps_alloc) {
    size_t total_scales = 0;
    for (int i = 0; i < n_qt; i++) total_scales += sc_map[i].n;
    float *all_fscales = (float *)ps_alloc(total_scales * sizeof(float));
    if (all_fscales) {
      float *wp = all_fscales;
      for (int i = 0; i < n_qt; i++) {
        sc_map[i].s32 = wp;
        for (int j = 0; j < sc_map[i].n; j++)
          wp[j] = mm_half2float(sc_map[i].s16[j]);
        wp += sc_map[i].n;
      }
    }
    // 回填每个 MQT 的 fscales 指针
    int idx = 0;
    m->tok_emb.fscales = sc_map[idx++].s32;
    for (int i = 0; i < L; i++) {
      m->q_proj[i].fscales = sc_map[idx++].s32;
      m->kv_proj[i].fscales = sc_map[idx++].s32;
      m->o_proj[i].fscales = sc_map[idx++].s32;
      m->gate_up_proj[i].fscales = sc_map[idx++].s32;
      m->down_proj[i].fscales = sc_map[idx++].s32;
    }
  } else {
    // 无分配器: fscales 为 NULL, matvec 回退到 half2float
    m->tok_emb.fscales = NULL;
    for (int i = 0; i < L; i++) {
      m->q_proj[i].fscales = NULL;
      m->kv_proj[i].fscales = NULL;
      m->o_proj[i].fscales = NULL;
      m->gate_up_proj[i].fscales = NULL;
      m->down_proj[i].fscales = NULL;
    }
  }

  m->tok_emb_matvec = NULL;
  return 0;
}

// Scratch buffers, caller-allocated (host: malloc; device: PSRAM).
typedef struct {
  float *x, *h;          // [D]
  float *q, *k, *v;      // q:[Q]  k,v:[K]
  float *att;            // [Q]
  float *g1, *g2;        // [ffn]
  float *logits;         // [vocab]
  float *scores;         // [seq_len]
  float *kcache, *vcache; // [L * n_kv_heads * head_dim * seq_len]
} MMScratch;

// One decode step: token at position pos -> logits[vocab]. KV cache persists.
static void mm_forward(MM *m, int token, int pos, MMScratch *s) {
  int D = m->c.dim, L = m->c.n_layers, F = m->c.ffn;
  int H = m->c.n_heads, HK = m->c.n_kv_heads, Hd = m->c.head_dim;
  int Q = H * Hd, K = HK * Hd, S = m->c.seq_len, n_rep = H / HK;
  float eps = m->c.rms_eps;

  mm_deq_row(&m->tok_emb, token, s->x);            // embedding

  // RoPE cos/sin for this position (head_dim pairs, identical across heads).
  float rope_c[Hd / 2], rope_s[Hd / 2];
  for (int i = 0; i < Hd / 2; i++) {
    float freq = powf(m->c.rope_theta, -2.f * i / Hd);
    rope_c[i] = cosf(pos * freq);
    rope_s[i] = sinf(pos * freq);
  }

  for (int l = 0; l < L; l++) {
    // ---- attention (pre-norm)
    mm_rmsnorm(s->x, m->attn_norm[l], D, s->h, eps);
    MM_MV(&m->q_proj[l], s->h, s->q);             // [Q]
    MM_MV(&m->kv_proj[l], s->h, s->k);            // [2K] fused: first K=k, next K=v

    // per-head RMSNorm (q_norm / k_norm) BEFORE RoPE (only if present)
    if (m->c.qk_norm) {
      for (int hh = 0; hh < H; hh++)
        mm_rmsnorm(s->q + hh * Hd, m->q_norm[l], Hd, s->q + hh * Hd, eps);
      for (int hh = 0; hh < HK; hh++)
        mm_rmsnorm(s->k + hh * Hd, m->k_norm[l], Hd, s->k + hh * Hd, eps);
    }

    // RoPE (rotate_half) per head on q and k
    for (int hh = 0; hh < H; hh++) {
      float *qh = s->q + hh * Hd;
      for (int i = 0; i < Hd / 2; i++) {
        float c = rope_c[i], sn = rope_s[i];
        float a = qh[i], b = qh[i + Hd / 2];
        qh[i] = a * c - b * sn; qh[i + Hd / 2] = b * c + a * sn;
      }
    }
    for (int hh = 0; hh < HK; hh++) {
      float *kh = s->k + hh * Hd;
      for (int i = 0; i < Hd / 2; i++) {
        float c = rope_c[i], sn = rope_s[i];
        float a = kh[i], b = kh[i + Hd / 2];
        kh[i] = a * c - b * sn; kh[i + Hd / 2] = b * c + a * sn;
      }
    }

    // store k/v into cache (per kv head); k=[0..K), v=[K..2K) from fused kv_proj
    float *kc = s->kcache + (size_t)l * K * S + (size_t)pos * K;
    float *vc = s->vcache + (size_t)l * K * S + (size_t)pos * K;
    memcpy(kc, s->k, K * sizeof(float));
    memcpy(vc, s->k + K, K * sizeof(float));

    // causal attention: query head hq -> kv head (hq / n_rep)
    float scale = 1.f / sqrtf((float)Hd);
    for (int hq = 0; hq < H; hq++) {
      int kvh = hq / n_rep;
      float *qh = s->q + hq * Hd;
      float *ao = s->att + hq * Hd;
      const float *klayer = s->kcache + (size_t)l * K * S;
      const float *vlayer = s->vcache + (size_t)l * K * S;
      for (int i = 0; i < Hd; i++) ao[i] = 0.f;
      float maxs = -1e30f;
      for (int t = 0; t <= pos; t++) {
        const float *kt = klayer + (size_t)t * K + (size_t)kvh * Hd;
        float dot = 0.f;
        for (int i = 0; i < Hd; i++) dot += qh[i] * kt[i];
        dot *= scale;
        s->scores[t] = dot;
        if (dot > maxs) maxs = dot;
      }
      float denom = 0.f;
      for (int t = 0; t <= pos; t++) {
        float w = expf(s->scores[t] - maxs); denom += w;
        const float *vt = vlayer + (size_t)t * K + (size_t)kvh * Hd;
        for (int i = 0; i < Hd; i++) ao[i] += w * vt[i];
      }
      for (int i = 0; i < Hd; i++) ao[i] /= denom;
    }

    MM_MV(&m->o_proj[l], s->att, s->h);           // [Q] -> [D]
    for (int i = 0; i < D; i++) s->x[i] += s->h[i];
#ifdef MM_DEBUG
    float mx = 0.f; for (int i = 0; i < D; i++) mx = fmaxf(mx, fabsf(s->x[i]));
    float mq = 0.f; for (int i = 0; i < Hd; i++) mq = fmaxf(mq, fabsf(s->q[i]));
    float ms = 0.f; for (int t = 0; t <= pos; t++) ms = fmaxf(ms, fabsf(s->scores[t]));
    printf("  L%d post-attn |x|=%.3f |q0|=%.3f |score|=%.3f\n", l, mx, mq, ms);
#endif

    // ---- SwiGLU FFN (pre-norm)
    mm_rmsnorm(s->x, m->ffn_norm[l], D, s->h, eps);
    MM_MV(&m->gate_up_proj[l], s->h, s->g1);      // [2F] fused: first F=gate, next F=up
    for (int i = 0; i < F; i++) s->g1[i] = mm_silu(s->g1[i]) * s->g1[F + i];
    MM_MV(&m->down_proj[l], s->g1, s->h);
    for (int i = 0; i < D; i++) s->x[i] += s->h[i];
  }

  mm_rmsnorm(s->x, m->out_norm, D, s->x, eps);
  // 输出头: 若平台设置了覆盖函数 (如 int8 预解压), 用它; 否则走默认 MM_MV
  if (m->tok_emb_matvec) m->tok_emb_matvec(&m->tok_emb, s->x, s->logits);
  else MM_MV(&m->tok_emb, s->x, s->logits);            // tied output head
}

#endif
