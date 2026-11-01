/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * 中文：语音翻页引擎本体。详见 voice_turn.h 的设计要点。
 *
 * English: the voice page-turn engine. See voice_turn.h for the design notes.
 */

#include "voice_turn.h"

#include "bt_host.h"

#include <math.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/idf_additions.h"
#include "nvs.h"

static const char *TAG = "voice_turn";

// 只有 Metalio S31 的板载蓝牙音频模组有麦克风通路；Pico 上本组件整体空转。
// Only the Metalio S31 board module exposes a mic path; on Pico this component idles.
#if KIIKO_BOARD_METALIO47
#define VT_MIC_SUPPORTED 1
#else
#define VT_MIC_SUPPORTED 0
#endif

/* ---- 参数 / Tuning ------------------------------------------------------ */

#define VT_RATE 16000            // 采样率 / sample rate
#define VT_HOP 256               // 每次读 16 ms / one hop is 16 ms
#define VT_WIN 512               // 分析窗 32 ms（50% 重叠）/ 32 ms analysis window, 50% overlap
#define VT_BINS (VT_WIN / 2 + 1)// 功率谱谱线数 / power-spectrum bins
#define VT_MEL 26                // mel 滤波器数 / mel filters
#define VT_MFCC 12               // 保留 c1..c12（丢掉 c0，抗音量）/ keep c1..c12, c0 dropped
#define VT_MAX_FRAMES 200        // 一句话最长 3.2 s / at most 3.2 s per utterance
#define VT_MIN_FRAMES 18         // 太短不当命令（约 0.29 s）/ shorter than ~0.29 s is not a command
#define VT_REPS 3                // 每个词最多存 3 份模板：两两距离撑起自动阈值 / up to three takes per word; pairwise distances drive the auto threshold

#define VT_GATE_DB 8.0f          // 高于噪声底 8 dB 算开口 / 8 dB above the noise floor counts as speech
#define VT_ABS_FLOOR_DB -70.0f   // 绝对静音地板：低于它一律当静音 / absolute silence floor
#define VT_ONSET_HOPS 2          // 连续 2 跳才算起说 / two hops to confirm onset
#define VT_TAIL_HOPS 12          // 连续 12 跳静音（约 190 ms）收尾 / 12 silent hops end the utterance
#define VT_COOLDOWN_MS 700       // 一次判定后的静默期 / ignore window after one decision

#define VT_ACCEPT_DEFAULT 2.0f   // 单份模板时的默认阈值（特征已归一化，下限即 2）/ default threshold with one rep (features normalised; also the floor)
#define VT_ACCEPT_MIN 2.0f
#define VT_ACCEPT_MAX 20.0f
#define VT_ACCEPT_MARGIN 1.55f   // 阈值 = 同词模板两两距离的最大值 × 这个系数 / accept = the worst intra-word pair × this
#define VT_SECOND_MARGIN 1.12f   // 与次优词至少差 12% 才认 / the runner-up must be 12% worse

#define VT_QUANT 32.0f           // NVS int8 量化比例（归一化后 |v|<4，分辨率 0.031）/ int8 scale; |v|<4 after normalise, 0.031 resolution
#define VT_STORE_MAX_FRAMES 128  // 落盘前截断到 2.05 s：3 份 × 2 词约 10 KB，仍能塞进 16 KB NVS 分区 / cap 2.05 s: six takes ≈ 10 KB, fitting the 16 KB NVS partition
#define VT_NVS_NS "vturn2"       // 新命名空间：int8 格式 / new namespace: the int8 format
#define VT_NVS_NS_LEGACY "vturn" // 旧命名空间（int16 旧模板），首帧学习时一次性清掉 / legacy int16 namespace, purged once on first learn
#define VT_BLOB_MAGIC 0x5456u

#define VT_TASK_STACK 5120
#define VT_TASK_PRIO 4
#define VT_TASK_CORE 0

/* ---- 工作区 / Working set（全部在 PSRAM）-------------------------------- */

typedef struct {
    float win[VT_WIN];                    // 当前分析窗 / current analysis window
    float ff_re[VT_WIN];                  // FFT 实部 / FFT real part
    float ff_im[VT_WIN];                  // FFT 虚部 / FFT imaginary part
    float tw_re[VT_WIN / 2];              // 旋转因子 / twiddles
    float tw_im[VT_WIN / 2];
    uint16_t bitrev[VT_WIN];              // 位反转表 / bit-reversal table
    uint16_t mel_bin[VT_MEL + 2];         // mel 三角滤波器边界 / mel triangle edges
    float mel_w[VT_MEL][VT_BINS];         // mel 权重 / mel weights
    float feat[VT_MAX_FRAMES][VT_MFCC];   // 当前这句的特征 / features of the utterance
    float tmpl[VOICE_TURN_WORD_COUNT][VT_REPS][VT_MAX_FRAMES][VT_MFCC];
    uint16_t tmpl_frames[VOICE_TURN_WORD_COUNT][VT_REPS];
    float spec[VT_BINS];                  // 功率谱 / power spectrum
    float dtw_prev[VT_MAX_FRAMES + 2];    // DTW 两行 / DTW rows
    float dtw_cur[VT_MAX_FRAMES + 2];
    int16_t hop[VT_HOP];                  // 最近一跳 / latest hop
    float hp_x1, hp_y1;                   // 直流阻断状态 / DC blocker state
} vt_ctx_t;

static vt_ctx_t *s_ctx;
static TaskHandle_t s_task;
static volatile bool s_inited;
static volatile bool s_listen_request;   // 调用方要求监听 / the caller wants listening
static volatile bool s_learn_active;
static volatile uint8_t s_learn_word;
static volatile uint8_t s_learn_state;
// 手动录音握手：UI 置位请求，采集任务消费后清掉；按钮只发请求，不直接碰缓冲。
// Manual record handshake: the UI raises a request, the capture task consumes and clears it;
// the button posts requests and never touches the buffers itself.
static volatile bool s_rec_req;   // 点「开始录音」/ the start tap
static volatile bool s_rec_stop;  // 点「结束录音」/ the stop tap
// 待落盘的模板：采集任务只挂标记，真正的 NVS 写由 UI 任务在轮询时做。
// 采集任务的栈在 PSRAM，而写 flash 会关 cache——关 cache 期间 PSRAM 不可访问，
// 在那种任务里调 nvs_commit 会直接 panic（"Cache disabled but cached memory region accessed"）。
// A template waiting to be persisted: the capture task only raises a flag and the UI task
// performs the NVS write while polling. The capture task's stack is in PSRAM, and a flash
// write disables the cache; a PSRAM stack is unreachable then, which panics the chip.
static volatile bool s_save_pending;
static volatile uint8_t s_save_word;
static volatile uint8_t s_save_slot;
static volatile float s_accept = VT_ACCEPT_DEFAULT;
static volatile float s_accept_manual;  // >0 手动固定阈值，0 走自动推算 / >0 pins the threshold, 0 means auto
static volatile int s_open_fails;       // 开麦连续失败次数（报错用）/ consecutive open failures, for visible errors
static volatile int s_direction;
static volatile int64_t s_cooldown_until_us;
static voice_turn_match_t s_match;

/* 任务内部状态 / state owned by the capture task */
static bool s_mic_open;
static bool s_noise_inited;
static float s_noise_db;
static int s_onset_hops;
static bool s_utt_active;
static volatile int s_frames;      // 跨任务读（界面显示录音秒数）/ read cross-task for the record timer
static int s_tail_hops;
static volatile bool s_recording;  // 手动录音进行中 / manual recording in progress
static bool s_rec_started;         // 起说已确认，开始取帧 / onset confirmed, frames are being collected
static int s_last_speech;          // 最后一帧有声的位置，用于裁掉尾部静音 / where the last speech frame sits, to trim trailing silence

/* ---- NVS ---------------------------------------------------------------- */

static void nvs_key(char *out, size_t cap, int word, int rep) {
    snprintf(out, cap, "w%dr%d", word, rep);
}

static bool tmpl_save(int word, int rep) {
    if (!s_ctx) return false;
    nvs_handle_t h;
    if (nvs_open(VT_NVS_NS, NVS_READWRITE, &h) != ESP_OK) return false;
    const int frames = s_ctx->tmpl_frames[word][rep];
    const size_t bytes = sizeof(uint16_t) * 2u + (size_t)frames * VT_MFCC;
    uint8_t *buf = heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf) {
        nvs_close(h);
        return false;
    }
    uint16_t hdr[2] = {VT_BLOB_MAGIC, (uint16_t)frames};
    memcpy(buf, hdr, sizeof(hdr));
    int8_t *q = (int8_t *)(buf + sizeof(hdr));
    for (int f = 0; f < frames; ++f)
        for (int d = 0; d < VT_MFCC; ++d) {
            float v = s_ctx->tmpl[word][rep][f][d] * VT_QUANT;
            if (v > 127.0f) v = 127.0f;
            if (v < -127.0f) v = -127.0f;
            q[f * VT_MFCC + d] = (int8_t)lrintf(v);
        }
    char key[12];
    nvs_key(key, sizeof(key), word, rep);
    // 先擦再写：nvs_set_blob 更新时新旧并存会让 16 KB 分区瞬时占用翻倍。
    // Erase before write: an in-place nvs_set_blob keeps old and new copies alive, doubling the
    // transient footprint of the 16 KB partition.
    (void)nvs_erase_key(h, key);
    (void)nvs_commit(h);
    bool ok = nvs_set_blob(h, key, buf, bytes) == ESP_OK;
    if (ok) ok = nvs_commit(h) == ESP_OK;
    nvs_close(h);
    heap_caps_free(buf);
    return ok;
}

static bool tmpl_load(int word, int rep) {
    if (!s_ctx) return false;
    nvs_handle_t h;
    if (nvs_open(VT_NVS_NS, NVS_READONLY, &h) != ESP_OK) return false;
    char key[12];
    nvs_key(key, sizeof(key), word, rep);
    size_t bytes = 0;
    bool ok = false;
    if (nvs_get_blob(h, key, NULL, &bytes) == ESP_OK && bytes > 4) {
        uint8_t *buf = heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (buf && nvs_get_blob(h, key, buf, &bytes) == ESP_OK) {
            uint16_t hdr[2];
            memcpy(hdr, buf, sizeof(hdr));
            int frames = hdr[1];
            if (hdr[0] == VT_BLOB_MAGIC && frames > 0 && frames <= VT_MAX_FRAMES &&
                bytes >= sizeof(hdr) + (size_t)frames * VT_MFCC) {
                const int8_t *q = (const int8_t *)(buf + sizeof(hdr));
                for (int f = 0; f < frames; ++f)
                    for (int d = 0; d < VT_MFCC; ++d)
                        s_ctx->tmpl[word][rep][f][d] = (float)q[f * VT_MFCC + d] / VT_QUANT;
                s_ctx->tmpl_frames[word][rep] = (uint16_t)frames;
                ok = true;
            }
        }
        if (buf) heap_caps_free(buf);
    }
    nvs_close(h);
    return ok;
}

static void tmpl_drop(int word, int rep) {
    if (!s_ctx) return;
    s_ctx->tmpl_frames[word][rep] = 0;
    nvs_handle_t h;
    if (nvs_open(VT_NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    char key[12];
    nvs_key(key, sizeof(key), word, rep);
    (void)nvs_erase_key(h, key);
    (void)nvs_commit(h);
    nvs_close(h);
}

static bool s_legacy_purged;  // 旧命名空间已清 / legacy namespace already purged

// 一次性清掉旧 int16 命名空间：16 KB 分区全系统共用，旧模板占着空间且新 UI 不再读它们。
// 只在 UI 任务里调（learn_poll 首次调用），那里栈在内部 RAM，写 flash 关 cache 安全。
// Purge the legacy int16 namespace once: the 16 KB partition is shared system-wide and the old
// templates only waste space the new UI never reads. Call from the UI task only (first
// learn_poll), whose internal-RAM stack survives the cache-off flash write.
static void purge_legacy_ns(void) {
    nvs_handle_t h;
    if (nvs_open(VT_NVS_NS_LEGACY, NVS_READWRITE, &h) != ESP_OK) return;
    (void)nvs_erase_all(h);
    (void)nvs_commit(h);
    nvs_close(h);
    ESP_LOGI(TAG, "legacy namespace purged");
}

/* ---- 阈值 / accept threshold ------------------------------------------- */

static float dtw_distance(const float *a, int na, const float *b, int nb);

static void recompute_accept(void) {
    if (!s_ctx) return;
    float worst_intra = 0.0f;
    bool have_intra = false;
    // 自动阈值：同一个词的所有模板两两比距离，取最不像的那一对当"人自己说得最散"的参照。
    // 只有一份模板的词跳过；一份都没有时回退 VT_ACCEPT_DEFAULT。
    // Auto threshold: compare every pair of takes of the same word and keep the worst one as the
    // speaker's own spread; a word with one take is skipped, and no pair at all falls back.
    for (int w = 0; w < VOICE_TURN_WORD_COUNT; ++w) {
        for (int a = 0; a < VT_REPS; ++a) {
            const int na = s_ctx->tmpl_frames[w][a];
            if (na < 4) continue;
            for (int b = a + 1; b < VT_REPS; ++b) {
                const int nb = s_ctx->tmpl_frames[w][b];
                if (nb < 4) continue;
                const float d = dtw_distance(&s_ctx->tmpl[w][a][0][0], na, &s_ctx->tmpl[w][b][0][0], nb);
                if (d > worst_intra) worst_intra = d;
                have_intra = true;
            }
        }
    }
    float accept = have_intra ? worst_intra * VT_ACCEPT_MARGIN : VT_ACCEPT_DEFAULT;
    if (accept < VT_ACCEPT_MIN) accept = VT_ACCEPT_MIN;
    if (accept > VT_ACCEPT_MAX) accept = VT_ACCEPT_MAX;
    s_accept = accept;
    if (s_accept_manual > 0.0f) s_accept = s_accept_manual;  // 手动设置优先 / a manual value wins
}

/* ---- FFT / 梅尔 / MFCC -------------------------------------------------- */

static void fft_init(vt_ctx_t *c) {
    int bits = 0;
    while ((1 << bits) < VT_WIN) ++bits;
    for (int i = 0; i < VT_WIN; ++i) {
        int r = 0;
        for (int b = 0; b < bits; ++b)
            if (i & (1 << b)) r |= 1 << (bits - 1 - b);
        c->bitrev[i] = (uint16_t)r;
    }
    for (int i = 0; i < VT_WIN / 2; ++i) {
        const double ang = -2.0 * M_PI * (double)i / (double)VT_WIN;
        c->tw_re[i] = (float)cos(ang);
        c->tw_im[i] = (float)sin(ang);
    }
}

static void fft_run(vt_ctx_t *c) {
    for (int i = 0; i < VT_WIN; ++i) {
        const int j = c->bitrev[i];
        if (j > i) {
            float t = c->ff_re[i];
            c->ff_re[i] = c->ff_re[j];
            c->ff_re[j] = t;
            t = c->ff_im[i];
            c->ff_im[i] = c->ff_im[j];
            c->ff_im[j] = t;
        }
    }
    for (int len = 2; len <= VT_WIN; len <<= 1) {
        const int half = len >> 1;
        const int step = VT_WIN / len;
        for (int i = 0; i < VT_WIN; i += len) {
            for (int k = 0; k < half; ++k) {
                const float wr = c->tw_re[k * step];
                const float wi = c->tw_im[k * step];
                const int a = i + k;
                const int b = a + half;
                const float xr = c->ff_re[b] * wr - c->ff_im[b] * wi;
                const float xi = c->ff_re[b] * wi + c->ff_im[b] * wr;
                c->ff_re[b] = c->ff_re[a] - xr;
                c->ff_im[b] = c->ff_im[a] - xi;
                c->ff_re[a] += xr;
                c->ff_im[a] += xi;
            }
        }
    }
}

/// 把赫兹换成 mel 滤波器的三角边界。/ Triangular mel edges from Hz.
static void mel_init(vt_ctx_t *c) {
    // 100 Hz .. 7600 Hz，等 mel 间隔 / 100 Hz to 7600 Hz, evenly spaced in mel
    const float lo = 100.0f, hi = 7600.0f;
    const float mel_lo = 2595.0f * logf(1.0f + lo / 700.0f);
    const float mel_hi = 2595.0f * logf(1.0f + hi / 700.0f);
    const float bin_hz = (float)VT_RATE / (float)VT_WIN;
    for (int m = 0; m < VT_MEL + 2; ++m) {
        const float mel = mel_lo + (mel_hi - mel_lo) * (float)m / (float)(VT_MEL + 1);
        const float hz = 700.0f * (powf(10.0f, mel / 2595.0f) - 1.0f);
        int b = (int)lrintf(hz / bin_hz);
        if (b < 0) b = 0;
        if (b >= VT_BINS) b = VT_BINS - 1;
        c->mel_bin[m] = (uint16_t)b;
    }
    memset(c->mel_w, 0, sizeof(c->mel_w));
    for (int m = 0; m < VT_MEL; ++m) {
        const int l = c->mel_bin[m], cen = c->mel_bin[m + 1], r = c->mel_bin[m + 2];
        for (int b = l; b < cen; ++b)
            if (cen > l) c->mel_w[m][b] = (float)(b - l) / (float)(cen - l);
        for (int b = cen; b < r; ++b)
            if (r > cen) c->mel_w[m][b] = (float)(r - b) / (float)(r - cen);
    }
}

/// 当前窗 → MFCC，写进 feat[frame]。/ Current window to MFCC, stored in feat[frame].
static void mfcc_frame(vt_ctx_t *c, int frame, float *out) {
    float prev = c->win[0];
    for (int i = 0; i < VT_WIN; ++i) {
        const float x = c->win[i];
        // 预加重 + 汉明窗 / pre-emphasis then Hamming
        const float emph = x - 0.97f * prev;
        prev = x;
        const float w = 0.54f - 0.46f * cosf(2.0f * (float)M_PI * (float)i / (float)(VT_WIN - 1));
        c->ff_re[i] = emph * w;
        c->ff_im[i] = 0.0f;
    }
    fft_run(c);

    // 功率谱 / power spectrum
    for (int b = 0; b < VT_BINS; ++b) c->spec[b] = c->ff_re[b] * c->ff_re[b] + c->ff_im[b] * c->ff_im[b];

    float mel[VT_MEL];
    for (int m = 0; m < VT_MEL; ++m) {
        float acc = 0.0f;
        for (int b = c->mel_bin[m]; b < c->mel_bin[m + 2] && b < VT_BINS; ++b) acc += c->mel_w[m][b] * c->spec[b];
        mel[m] = logf(acc + 1e-6f);
    }

    // DCT-II，取 c1..c12 / DCT-II, keep c1..c12
    for (int k = 1; k <= VT_MFCC; ++k) {
        float acc = 0.0f;
        for (int m = 0; m < VT_MEL; ++m)
            acc += mel[m] * cosf((float)M_PI * (float)k * ((float)m + 0.5f) / (float)VT_MEL);
        out[k - 1] = acc;
    }
    (void)frame;
}

/// 整句归一化：去均值（CMN）再除以整体标准差，使距离与音量/麦克风增益无关。
/// Utterance normalisation: cepstral mean removal, then divide by the overall standard
/// deviation so distances no longer depend on loudness or mic gain.
static void features_normalise(vt_ctx_t *c, int frames) {
    if (frames < 2) return;
    float mean[VT_MFCC] = {0};
    for (int f = 0; f < frames; ++f)
        for (int d = 0; d < VT_MFCC; ++d) mean[d] += c->feat[f][d];
    for (int d = 0; d < VT_MFCC; ++d) mean[d] /= (float)frames;
    float var = 0.0f;
    for (int f = 0; f < frames; ++f)
        for (int d = 0; d < VT_MFCC; ++d) {
            c->feat[f][d] -= mean[d];
            var += c->feat[f][d] * c->feat[f][d];
        }
    var /= (float)(frames * VT_MFCC);
    float scale = 1.0f;
    if (var > 1e-6f) scale = 1.0f / sqrtf(var);
    if (scale > 8.0f) scale = 8.0f;
    for (int f = 0; f < frames; ++f)
        for (int d = 0; d < VT_MFCC; ++d) c->feat[f][d] *= scale;
}

/* ---- DTW ---------------------------------------------------------------- */

static float frame_dist(const float *a, const float *b) {
    float acc = 0.0f;
    for (int d = 0; d < VT_MFCC; ++d) {
        const float t = a[d] - b[d];
        acc += t * t;
    }
    return sqrtf(acc);
}

/// 带 Sakoe-Chiba 约束的 DTW，结果按路径长度归一到"每帧平均距离"。
/// Banded DTW normalised to a per-frame average distance.
static float dtw_distance(const float *a, int na, const float *b, int nb) {
    if (!s_ctx || na < 2 || nb < 2) return 1e9f;
    float *prev = s_ctx->dtw_prev;
    float *cur = s_ctx->dtw_cur;
    const int band = 6 + (na > nb ? na : nb) / 5;
    for (int j = 0; j <= nb; ++j) prev[j] = 1e9f;
    prev[0] = 0.0f;
    for (int i = 1; i <= na; ++i) {
        for (int j = 0; j <= nb; ++j) cur[j] = 1e9f;
        // 带子沿"比例对角线"走：语速差一截、帧数差一半的两段照样能对齐。
        // 沿恒等对角线时 |帧数差| > 带宽就整条无路（判 1e9），等于模板白学——
        // 实测正是次优距离恒为 1e9、自动阈值被顶到上限 20 的原因。
        // The band rides the proportional diagonal so takes whose frame counts differ by half
        // still align. Along the identity diagonal, a length gap beyond the band left no path at
        // all (scored 1e9), rendering a template useless — exactly why the runner-up sat at 1e9
        // and the auto threshold was pinned at the 20 cap.
        const int center = (int)((int64_t)i * nb / na);
        int lo = center - band;
        if (lo < 1) lo = 1;
        int hi = center + band;
        if (hi > nb) hi = nb;
        for (int j = lo; j <= hi; ++j) {
            float m = prev[j];
            if (prev[j - 1] < m) m = prev[j - 1];
            if (cur[j - 1] < m) m = cur[j - 1];
            if (m >= 1e8f) continue;
            cur[j] = frame_dist(a + (i - 1) * VT_MFCC, b + (j - 1) * VT_MFCC) + m;
        }
        float *t = prev;
        prev = cur;
        cur = t;
    }
    const float total = prev[nb];
    if (total >= 1e8f) return 1e9f;
    return total * 2.0f / (float)(na + nb);
}

/* ---- 判定 / decision ---------------------------------------------------- */

static void match_and_emit(void) {
    vt_ctx_t *c = s_ctx;
    const int frames = s_frames;
    features_normalise(c, frames);

    float best[VOICE_TURN_WORD_COUNT];
    for (int w = 0; w < VOICE_TURN_WORD_COUNT; ++w) {
        float d = 1e9f;
        for (int r = 0; r < VT_REPS; ++r) {
            const int nb = c->tmpl_frames[w][r];
            if (nb < 2) continue;
            const float cand = dtw_distance(&c->feat[0][0], frames, &c->tmpl[w][r][0][0], nb);
            if (cand < d) d = cand;
        }
        best[w] = d;
    }

    int win_word = -1;
    float d1 = 1e9f, d2 = 1e9f;
    for (int w = 0; w < VOICE_TURN_WORD_COUNT; ++w) {
        if (best[w] < d1) {
            d2 = d1;
            d1 = best[w];
            win_word = w;
        } else if (best[w] < d2) {
            d2 = best[w];
        }
    }

    const float accept = s_accept;
    const bool known = d1 < 1e8f;
    const bool ok = known && d1 <= accept && (d2 > 1e8f || d1 * VT_SECOND_MARGIN <= d2);
    s_match.best = d1;
    s_match.second = d2;
    s_match.threshold = accept;
    s_match.word = ok ? win_word : -1;
    s_match.accepted = ok;
    ++s_match.seq;  // 界面据此发现新判定 / so the UI can spot a new decision

    if (ok) {
        const int dir = (win_word == VOICE_TURN_WORD_PREV) ? -1 : 1;
        s_direction = dir;
        s_cooldown_until_us = esp_timer_get_time() + (int64_t)VT_COOLDOWN_MS * 1000;
        ESP_LOGI(TAG, "match word=%d dir=%d best=%.2f second=%.2f thr=%.2f frames=%d",
                 win_word, dir, d1, d2, accept, frames);
    } else {
        ESP_LOGD(TAG, "reject best=%.2f second=%.2f thr=%.2f frames=%d", d1, d2, accept, frames);
    }
}

/// 学习：把刚说完的这句存成模板。这里只排进待保存队列，不碰 flash。
/// Learning: store the utterance as a template. This only queues it; flash is untouched here.
static void store_template(int word) {
    vt_ctx_t *c = s_ctx;
    if (!c) return;
    int frames = s_frames;
    // 落盘前截断：模板越长 NVS blob 越大，2.05 s 足够一个词，且 DTW 带宽内匹配不受影响。
    // Truncate before saving: a longer template is a bigger NVS blob; 2.05 s covers a word and
    // matching inside the DTW band is unaffected.
    if (frames > VT_STORE_MAX_FRAMES) frames = VT_STORE_MAX_FRAMES;
    features_normalise(c, frames);
    int slot = -1;
    for (int r = 0; r < VT_REPS; ++r)
        if (c->tmpl_frames[word][r] < 2) {
            slot = r;
            break;
        }
    // 三份都满了就覆盖最后一份，保留前两份做同词参照 / with all slots full, replace the last and keep the first two as intra-word references
    if (slot < 0) slot = VT_REPS - 1;
    memcpy(&c->tmpl[word][slot][0][0], &c->feat[0][0], sizeof(float) * (size_t)frames * VT_MFCC);
    c->tmpl_frames[word][slot] = (uint16_t)frames;
    s_save_word = (uint8_t)word;
    s_save_slot = (uint8_t)slot;
    s_save_pending = true;
    ESP_LOGI(TAG, "learn word=%d slot=%d frames=%d queued", word, slot, frames);
}

/// 把待保存的模板真正写进 NVS。只能在内部 RAM 栈的任务里调用（UI 任务）。
/// Writes a queued template to NVS. Only call it from a task with an internal-RAM stack.
static bool flush_pending(void) {
    if (!s_save_pending) return true;
    const int word = s_save_word;
    const int slot = s_save_slot;
    s_save_pending = false;
    const bool saved = tmpl_save(word, slot);
    recompute_accept();
    ESP_LOGI(TAG, "learn word=%d slot=%d saved=%d accept=%.2f", word, slot, saved ? 1 : 0,
             (double)s_accept);
    s_learn_state = saved ? VOICE_TURN_LEARN_OK : VOICE_TURN_LEARN_FAIL_SAVE;
    return saved;
}

/* ---- 采集任务 / capture task ------------------------------------------- */

// 手动录音定稿：只由采集任务调用（按钮「结束」或 3.2 s 上限）。存模板只排队，不碰 flash。
// Finalise a manual recording: capture task only (the stop tap or the 3.2 s cap). Storing the
// template only queues it; flash stays untouched here.
static void manual_finalize(void) {
    const bool had_request = s_rec_req;
    s_rec_req = false;
    s_rec_stop = false;
    if (!s_recording) {
        // 麦还没送出第一跳就按了结束：判失败让界面提示重录，且不覆盖刚保存的状态。
        // The stop tap beat the first audio hop: fail the take so the UI asks for a re-record,
        // without clobbering a state that was just saved.
        if (had_request && s_learn_active) s_learn_state = VOICE_TURN_LEARN_FAIL;
        return;
    }
    s_recording = false;
    // 运行期按 12 跳静音收尾；手动结束裁到最后一帧有声 + 同样的收尾长度，尾部静音不进模板。
    // Runtime ends after 12 silent hops; manual stop trims to the last speech frame plus the
    // same tail so trailing silence never enters the template.
    int frames = s_last_speech + VT_TAIL_HOPS;
    if (frames > s_frames) frames = s_frames;
    if (frames >= VT_MIN_FRAMES) {
        s_frames = frames;
        s_learn_state = VOICE_TURN_LEARN_SPOKE;
        store_template(s_learn_word);
        s_frames = 0;
    } else {
        ESP_LOGI(TAG, "record too short frames=%d", frames);
        s_learn_state = VOICE_TURN_LEARN_FAIL;
        s_frames = 0;
    }
    s_rec_started = false;
    s_onset_hops = 0;
}

static void hop_process(const int16_t *hop, int n) {
    vt_ctx_t *c = s_ctx;
    if (!c || n <= 0) return;

    // 直流阻断：模组 ADC 可能有偏置，放着不管会让能量门控一直"有人说话"。
    // DC blocker: the module ADC may carry an offset, which would hold the gate open forever.
    for (int i = 0; i < n; ++i) {
        const float x = (float)hop[i];
        const float y = x - c->hp_x1 + 0.995f * c->hp_y1;
        c->hp_x1 = x;
        c->hp_y1 = y;
        const float v = y;
        c->win[(VT_WIN - VT_HOP) + i] = v;
    }

    float energy = 0.0f;
    for (int i = 0; i < n; ++i) {
        const float x = (float)hop[i];
        energy += x * x;
    }
    energy /= (float)n;
    const float db = 10.0f * log10f(energy + 1.0f);

    if (!s_noise_inited) {
        s_noise_db = db;
        s_noise_inited = true;
    }
    const bool speech = db > (s_noise_db + VT_GATE_DB) && db > VT_ABS_FLOOR_DB;
    const float a = (db < s_noise_db) ? 0.25f : (speech ? 0.004f : 0.02f);
    s_noise_db += a * (db - s_noise_db);

    // 手动录音分支：只在点过「开始」后收帧，结束只认按钮（或 3.2 s 上限），不再自动起停。
    // Manual branch: frames are collected only after the start tap, and only the stop tap (or
    // the 3.2 s cap) ends the take; nothing starts or stops on its own any more.
    if (s_rec_req || s_recording) {
        if (!s_recording) {
            // 首跳：清计数，等能量起说确认后再取帧。/ First hop: reset counters, wait for onset.
            s_recording = true;
            s_rec_started = false;
            s_frames = 0;
            s_last_speech = 0;
            s_onset_hops = 0;
            s_utt_active = false;
            s_rec_req = false;
        }
        if (!s_rec_started) {
            // 保留能量起说：从开口那一跳开始取帧，与运行期识别的特征流对齐。
            // Keep the energy onset so frames start at the spoken hop, matching runtime features.
            s_onset_hops = speech ? s_onset_hops + 1 : 0;
            if (s_onset_hops >= VT_ONSET_HOPS) s_rec_started = true;
        }
        if (s_rec_started && s_frames < VT_MAX_FRAMES) {
            mfcc_frame(c, s_frames, c->feat[s_frames]);
            ++s_frames;
            if (speech) s_last_speech = s_frames;
        }
        memmove(c->win, c->win + VT_HOP, sizeof(float) * (VT_WIN - VT_HOP));
        if (s_frames >= VT_MAX_FRAMES) manual_finalize();  // 3.2 s 上限自动收尾 / auto-stop at the 3.2 s cap
        return;
    }

    // 起说判定：连续两跳超过门限。/ Onset: two hops above the gate.
    if (!s_utt_active) {
        s_onset_hops = speech ? s_onset_hops + 1 : 0;
        if (!s_listen_request || s_onset_hops < VT_ONSET_HOPS) return;
        s_utt_active = true;
        s_frames = 0;
        s_tail_hops = 0;
    } else {
        s_tail_hops = speech ? 0 : s_tail_hops + 1;
    }

    // 当前窗（含 50% 前一跳）算一帧特征。/ One frame from the current window.
    if (s_frames < VT_MAX_FRAMES) {
        mfcc_frame(c, s_frames, c->feat[s_frames]);
        ++s_frames;
    }

    const bool done = s_tail_hops >= VT_TAIL_HOPS || s_frames >= VT_MAX_FRAMES;
    if (!done) {
        // 窗往前挪半帧：下一次 hop_process 会填进后半。/ Slide the window by half a frame.
        memmove(c->win, c->win + VT_HOP, sizeof(float) * (VT_WIN - VT_HOP));
        return;
    }

    s_utt_active = false;
    s_onset_hops = 0;
    if (s_frames >= VT_MIN_FRAMES && esp_timer_get_time() >= s_cooldown_until_us) match_and_emit();
    s_frames = 0;
    memmove(c->win, c->win + VT_HOP, sizeof(float) * (VT_WIN - VT_HOP));
}

static void mic_close(void) {
    if (!s_mic_open) return;
#if VT_MIC_SUPPORTED
    bt_mic_close();
#endif
    s_mic_open = false;
    s_noise_inited = false;
    s_utt_active = false;
    s_onset_hops = 0;
    s_frames = 0;
    s_last_speech = 0;  // 重开后一段还没录，判短让用户重录 / after a reopen nothing is recorded yet, so a short take fails and is redone
}

static void capture_task(void *arg) {
    (void)arg;
#if !VT_MIC_SUPPORTED
    for (;;) vTaskDelay(pdMS_TO_TICKS(1000));
#else
    int64_t next_open_us = 0;
    for (;;) {
        // 按钮「结束」优先：先定稿，再决定开关麦。/ The stop tap wins: finalise first, then open or close.
        if (s_rec_stop) manual_finalize();
        const bool want = s_listen_request || s_rec_req || s_recording;
        if (!want) {
            mic_close();
            next_open_us = 0;
            vTaskDelay(pdMS_TO_TICKS(60));
            continue;
        }
        // 耳机在放音时模组归音频通路，麦克风要等它拆掉；正在进行的录音作废。
        // While a headset owns the module the mic waits; any take in progress is aborted.
        if (bt_audio_link_active()) {
            mic_close();
            if (s_recording || s_rec_req) {
                s_recording = false;
                s_rec_started = false;
                s_rec_req = false;
                s_rec_stop = false;
                s_frames = 0;
                s_last_speech = 0;
            }
            if (s_learn_active) s_learn_state = VOICE_TURN_LEARN_BUSY;
            vTaskDelay(pdMS_TO_TICKS(400));
            continue;
        }
        if (!s_mic_open) {
            if (esp_timer_get_time() < next_open_us) {
                vTaskDelay(pdMS_TO_TICKS(120));
                continue;
            }
            const esp_err_t err = bt_mic_open();
            if (err != ESP_OK) {
                // 模组忙（切模式/断链中）：退避后再试，别每秒重开。/ Back off instead of hammering.
                ++s_open_fails;
                ESP_LOGW(TAG, "bt_mic_open failed x%d: %s", s_open_fails, esp_err_to_name(err));
                // 学习等麦时连败三次就转失败态：界面不能永远停在"正在开启麦克风"。
                // After three straight failures during a learning take, fail visibly instead of
                // leaving the UI parked on "opening the mic" forever.
                if (s_open_fails >= 3 && s_learn_active && s_learn_state == VOICE_TURN_LEARN_RECORD) {
                    s_rec_req = false;
                    s_rec_stop = false;
                    s_recording = false;
                    s_rec_started = false;
                    s_frames = 0;
                    s_learn_state = VOICE_TURN_LEARN_FAIL_OPEN;
                }
                next_open_us = esp_timer_get_time() + 3000000;
                vTaskDelay(pdMS_TO_TICKS(250));
                continue;
            }
            s_open_fails = 0;
            s_mic_open = true;
            s_noise_inited = false;
            ESP_LOGI(TAG, "mic open");
        }
        const int got = bt_mic_read(s_ctx->hop, VT_HOP, 150);
        if (got < 0) {
            mic_close();
            next_open_us = esp_timer_get_time() + 2000000;
            vTaskDelay(pdMS_TO_TICKS(200));
            continue;
        }
        if (got == 0) continue;
        hop_process(s_ctx->hop, got);
    }
#endif  // VT_MIC_SUPPORTED
}

/* ---- 对外接口 / public API --------------------------------------------- */

esp_err_t voice_turn_init(void) {
    if (s_inited) return ESP_OK;
#if !VT_MIC_SUPPORTED
    return ESP_ERR_NOT_SUPPORTED;
#endif
    s_ctx = heap_caps_calloc(1, sizeof(vt_ctx_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_ctx) {
        ESP_LOGE(TAG, "no PSRAM for the working set (%u B)", (unsigned)sizeof(vt_ctx_t));
        return ESP_ERR_NO_MEM;
    }
    fft_init(s_ctx);
    mel_init(s_ctx);
    for (int w = 0; w < VOICE_TURN_WORD_COUNT; ++w)
        for (int r = 0; r < VT_REPS; ++r) tmpl_load(w, r);
    recompute_accept();
    if (xTaskCreatePinnedToCoreWithCaps(capture_task, "vt_cap", VT_TASK_STACK, NULL, VT_TASK_PRIO,
                                        &s_task, VT_TASK_CORE,
                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
        ESP_LOGE(TAG, "capture task creation failed");
        heap_caps_free(s_ctx);
        s_ctx = NULL;
        return ESP_ERR_NO_MEM;
    }
    s_inited = true;
    uint8_t reps = 0;
    for (int w = 0; w < VOICE_TURN_WORD_COUNT; ++w) reps += voice_turn_word_reps((voice_turn_word_t)w) ? 1 : 0;
    ESP_LOGI(TAG, "ready, learned %u/%d words, accept=%.2f", (unsigned)reps, VOICE_TURN_WORD_COUNT,
             (double)s_accept);
    return ESP_OK;
}

bool voice_turn_ready(void) { return s_inited; }

uint8_t voice_turn_word_reps(voice_turn_word_t word) {
    if (!s_ctx || (int)word < 0 || (int)word >= VOICE_TURN_WORD_COUNT) return 0;
    uint8_t n = 0;
    for (int r = 0; r < VT_REPS; ++r)
        if (s_ctx->tmpl_frames[(int)word][r] >= 2) ++n;
    return n;
}

bool voice_turn_learned(void) {
    // 只学了一个方向也允许监听：没教的词距离恒为 1e9，不会抢判定。
    // One taught direction is enough to listen: an untaught word stays at 1e9 and never wins.
    for (int w = 0; w < VOICE_TURN_WORD_COUNT; ++w)
        if (voice_turn_word_reps((voice_turn_word_t)w)) return s_inited;
    return false;
}

void voice_turn_set_listen(bool on) { s_listen_request = on; }

bool voice_turn_listening(void) { return s_mic_open && s_listen_request; }

int voice_turn_take_direction(void) {
    const int dir = s_direction;
    if (dir) s_direction = 0;
    return dir;
}

esp_err_t voice_turn_learn_begin(voice_turn_word_t word) {
    if (!s_inited) return ESP_ERR_INVALID_STATE;
    if ((int)word < 0 || (int)word >= VOICE_TURN_WORD_COUNT) return ESP_ERR_INVALID_ARG;
    (void)flush_pending();  // 上一个词还没落盘就先补上 / do not drop a pending template
    s_learn_word = (uint8_t)word;
    s_learn_state = VOICE_TURN_LEARN_WAIT;
    s_utt_active = false;
    s_frames = 0;
    s_onset_hops = 0;
    // 上一次没录完就换词：把挂起的手动录音一起作废。/ Switching words mid-take aborts any live recording.
    s_rec_req = false;
    s_rec_stop = false;
    s_recording = false;
    s_rec_started = false;
    s_last_speech = 0;
    s_learn_active = true;
    return ESP_OK;
}

esp_err_t voice_turn_learn_record_start(void) {
    if (!s_inited || !s_learn_active) return ESP_ERR_INVALID_STATE;
    (void)flush_pending();  // 上一份还没落盘先补上 / persist a still-pending template first
    s_open_fails = 0;       // 每次按「开始」给开麦一个新的重试额度 / each start tap grants a fresh retry budget
    s_rec_stop = false;
    s_rec_req = true;
    s_learn_state = VOICE_TURN_LEARN_RECORD;
    return ESP_OK;
}

void voice_turn_learn_record_stop(void) {
    if (s_recording || s_rec_req) s_rec_stop = true;
}

int voice_turn_learn_record_frames(void) {
    if (!s_inited || !s_mic_open) return -1;
    return s_frames;
}

voice_turn_learn_state_t voice_turn_learn_poll(void) {
    // 采集任务只挂标记，真正的 NVS 写在这里发生：调用方（设置页 on_tick）是 UI 任务，
    // 栈在内部 RAM，写 flash 关 cache 也不会踩到 PSRAM。
    // The capture task only raises a flag; the NVS write happens here because the caller (the
    // settings on_tick) runs on the UI task with an internal-RAM stack.
    if (!s_legacy_purged) {
        s_legacy_purged = true;
        purge_legacy_ns();  // 首次调用清旧空间，给新模板腾地方 / clear the legacy space on first use
    }
    (void)flush_pending();
    if (!s_learn_active) return VOICE_TURN_LEARN_IDLE;
    return (voice_turn_learn_state_t)s_learn_state;
}

void voice_turn_learn_cancel(void) {
    (void)flush_pending();  // 学完就退出也要保住这一段 / keep a just-captured template
    s_learn_active = false;
    s_learn_state = VOICE_TURN_LEARN_IDLE;
    s_utt_active = false;
    s_frames = 0;
    s_onset_hops = 0;
    // 录音中退出：请求全部作废，采集任务下一轮关麦。/ Leaving mid-record cancels every request;
    // the capture task closes the mic on its next pass.
    s_rec_req = false;
    s_rec_stop = false;
    s_recording = false;
    s_rec_started = false;
    s_last_speech = 0;
}

void voice_turn_learn_erase(void) {
    if (!s_ctx) return;
    s_save_pending = false;  // 清空优先：别让队列里的旧模板又写回去 / do not re-save a stale template
    for (int w = 0; w < VOICE_TURN_WORD_COUNT; ++w)
        for (int r = 0; r < VT_REPS; ++r) tmpl_drop(w, r);
    s_accept = s_accept_manual > 0.0f ? s_accept_manual : VT_ACCEPT_DEFAULT;
    s_match.word = -1;
    s_match.accepted = false;
    ESP_LOGI(TAG, "templates erased");
}

void voice_turn_set_accept(float value) {
    if (value <= 0.0f) {
        if (s_accept_manual == 0.0f) return;  // 已经是自动 / already auto
        s_accept_manual = 0.0f;
        recompute_accept();  // 回到按模板内距推算 / back to computing from the templates
        ESP_LOGI(TAG, "accept back to auto (%.2f)", (double)s_accept);
        return;
    }
    if (value < VT_ACCEPT_MIN) value = VT_ACCEPT_MIN;
    if (value > VT_ACCEPT_MAX) value = VT_ACCEPT_MAX;
    if (s_accept_manual == value) return;  // 幂等少刷日志 / idempotent, keep the log quiet
    s_accept_manual = value;
    s_accept = value;
    ESP_LOGI(TAG, "accept manual %.2f", (double)value);
}

float voice_turn_accept(void) { return s_accept; }

void voice_turn_last_match(voice_turn_match_t *out) {
    if (out) *out = s_match;
}
