/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * 中文：音乐播放器核心（见 music_player.h）。解码任务（core1、优先级 3）读文件 →
 * esp_audio_simple_dec 解码 → 重采样到 16k/2ch/s16 → 入 384KB PSRAM 环形缓冲；
 * 蓝牙模组的拉流回调只从环里取数，欠载由 bt_host 补静音。单生产者（解码任务）
 * 单消费者（喂帧任务），环位置用「模 2×容量」的原子计数。
 *
 * English: player core (see music_player.h). The decode task (core 1, priority 3)
 * reads the file, decodes via esp_audio_simple_dec, resamples to 16k/2ch/s16 and
 * feeds a 384 KB PSRAM ring; the module pull callback only drains it (bt_host pads
 * silence on underrun). One producer (decode task), one consumer (feed task); ring
 * positions are atomic counts modulo 2×capacity.
 *
 * 冻结：不绘制、不发起蓝牙连接、不递归到 music/ 之外的目录。
 * Frozen: no drawing, no Bluetooth connects, no directory recursion outside music/.
 */

#include "music_player.h"

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include <dirent.h>
#include <sys/stat.h>

#include "bt_host.h"
#include "bt_trace.h"
#include "esp_audio_simple_dec.h"
#include "esp_audio_simple_dec_default.h"
#include "esp_audio_dec_default.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "music_lyrics.h"
#include "music_tags.h"
#include "nvs.h"
#include "settings.h"

static const char *TAG = "music";

/* ---- 常量 / Constants ---- */

#define NVS_NS "rp_music"
#define NVS_KEY_VOL "vol"
#define NVS_KEY_TRACK "track"
#define NVS_KEY_MODE "mode"        // 播放模式 0..3 / play mode 0..3
#define NVS_KEY_FAV_PREFIX "fav_"  // fav_<路径哈希>，键长 12 ≤ NVS 15 / fav_<path hash>, 12 chars ≤ NVS's 15
#define NVS_KEY_POS "pos"          // 上次播放位置（毫秒）/ last position (ms)
#define NVS_KEY_RESUME "resume"    // 关机前是否在播 / was playing before power off
#define NVS_KEY_OUT "out"          // 输出通路：0=蓝牙耳机 1=本机扬声器 / output route 0=BT 1=speaker

#define IN_CAP (8 * 1024)        // 每次读文件 / per file read
#define OUT_CAP (32 * 1024)      // 单次解码输出 / one decode output
#define RING_CAP (384 * 1024)    // ≈6 s @16k/2ch/s16 / about 6 s
#define RING_MOD (2 * RING_CAP)  // 位置模 2×容量，区分满与空 / positions mod 2×capacity tell full from empty
#define OUT_RATE 16000           // 模组 I2S 固定 16 kHz / the module's I2S runs at a fixed 16 kHz
#define TASK_STACK 12288
#define TASK_PRIO 3
#define TASK_CORE 1

#define REQ_NONE (-1)
#define REQ_STOP (-2)
#define REQ_SINGLE (-3)       // 单曲直放：文件管理里点开的音频 / direct play of a file outside the library

/* ---- 模块状态 / Module state ---- */

static music_track_t *s_tracks;  // PSRAM
static int s_count;
static char s_last_path[240];  // NVS 恢复用 / restored from NVS
static char s_resume_path[240];   // 续播位置归属的曲目路径 / the track the saved position belongs to
static uint32_t s_resume_pos;     // 上次播放位置（毫秒）/ last position (ms)
static bool s_resume_was_playing; // 关机前是否在播 / was playing before power off
static bool s_boot_resume_done;   // 本次会话仅自动续播一次 / auto-resume once per session
static TickType_t s_last_save_tick; // 周期落盘节拍 / periodic-save cadence

static atomic_int s_index = ATOMIC_VAR_INIT(-1);
static atomic_int s_state = ATOMIC_VAR_INIT(MUSIC_IDLE);
static atomic_uint s_serial;
static atomic_int s_req = ATOMIC_VAR_INIT(REQ_NONE);
static atomic_bool s_radio_busy;
static atomic_bool s_media_busy;
static atomic_bool s_media_lost;
// 丢卡那一刻是否在播、丢卡时刻与上次探测时刻：唤醒链路的「假丢卡」要能自己长回来。
// Whether we were playing when the card dropped, plus the loss/probe timestamps, so a false
// card-loss from the wake path can heal itself instead of leaving a dead play button.
static atomic_bool s_media_resume;
static int64_t s_media_lost_us;
static int64_t s_media_probe_us;
static atomic_bool s_suspended;
static atomic_bool s_auto_paused;  // 因条件暂停，条件解除自动续 / paused by a condition; auto-resumes
static atomic_bool s_suspend_no_resume;  // 睡眠挂起：唤醒后不自动续播 / sleep suspend: no auto-resume on wake
static atomic_uint s_vol = 100;    // 0..127
static atomic_int s_mode = ATOMIC_VAR_INIT(MUSIC_MODE_LIST);  // music_mode_t，NVS 持久化 / persisted
static atomic_int s_output = ATOMIC_VAR_INIT(MUSIC_OUT_BT);   // music_output_t，NVS 持久化 / persisted
static atomic_bool s_ready;
static bool s_task_created;
// 停播后解码任务自我退出，把 12 KB 栈还给系统；传书起射频时那正是缺的那点余量。
// The decode task exits after a stop so its 12 KB stack goes back to the heap; that stack is
// exactly the margin the S31 radio is missing when the transfer page starts.
static atomic_bool s_task_exit;

static bool music_media_probe(void);  // 丢卡闩锁自愈探测（定义在对外接口段）/ latch probe, defined below

static uint8_t s_fav[MUSIC_MAX_TRACKS];  // 收藏缓存，按曲目下标对齐 s_tracks / favorite cache aligned with s_tracks

// 单曲直放：曲库外的音频（文件管理里点开），不进播放列表，播完即停。
// Direct play: audio outside the library (opened from the file manager) never joins the
// playlist and stops when it ends.
static atomic_bool s_single;
static char s_single_path[256];
static char s_single_title[96];
static char s_single_artist[64];
static uint32_t s_single_dur;

// 曲库根目录可配置（默认 /sdcard/music），NVS 键 root。
// Configurable library root (default /sdcard/music), NVS key "root".
#define NVS_KEY_ROOT "root"
static char s_root[256] = MUSIC_ROOT;

static char s_error[48];
static portMUX_TYPE s_err_lock = portMUX_INITIALIZER_UNLOCKED;

// 进度：pos 为冻结位置，base 为播放中的计时起点（0 = 未在走）。
// Progress: `pos` is the frozen position, `base` the running start (0 = not ticking).
static portMUX_TYPE s_time_lock = portMUX_INITIALIZER_UNLOCKED;
static uint32_t s_pos_ms;
static int64_t s_base_us;

/* ---- 环形缓冲 / PCM ring ---- */

static uint8_t *s_ring;
static atomic_uint s_wr;
static atomic_uint s_rd;

static uint32_t ring_used(void) {
    const uint32_t wr = atomic_load_explicit(&s_wr, memory_order_acquire);
    const uint32_t rd = atomic_load(&s_rd);
    return (wr - rd + RING_MOD) % RING_MOD;
}

// 只把读点追到写点，不清零两个游标：蓝牙任务可能正拿着一份旧读点，若两个都清零，
// 它随后落盘的旧读点会跑到写点前面，环计数从此错位（看起来永远满，声音全丢——跳转后无声）。
// Only move the read cursor up to the write cursor instead of zeroing both: the Bluetooth
// task may still hold an older read position, and a zeroed pair let that store land ahead of
// the write cursor, which desyncs the counters (the ring looks forever full and all audio is
// dropped — the silence right after a seek).
static void ring_reset(void) {
    atomic_store(&s_rd, atomic_load(&s_wr));
}

// 生产者：最多写入全部 n 字节，返回实际写入。/ Producer: writes up to n bytes, returns how many landed.
static uint32_t ring_push(const uint8_t *src, uint32_t n) {
    const uint32_t free = RING_CAP - ring_used();
    if (n > free) n = free;
    if (!n) return 0;
    uint32_t wr = atomic_load(&s_wr);
    uint32_t off = wr % RING_CAP;
    uint32_t first = RING_CAP - off;
    if (first > n) first = n;
    memcpy(s_ring + off, src, first);
    if (n > first) memcpy(s_ring, src + first, n - first);
    atomic_store_explicit(&s_wr, (wr + n) % RING_MOD, memory_order_release);
    return n;
}

// 消费者（蓝牙任务）：最多读 n 字节。/ Consumer (Bluetooth task): reads up to n bytes.
static uint32_t ring_pull(uint8_t *dst, uint32_t n) {
    const uint32_t wr = atomic_load_explicit(&s_wr, memory_order_acquire);
    const uint32_t rd = atomic_load(&s_rd);
    uint32_t avail = (wr - rd + RING_MOD) % RING_MOD;
    if (n > avail) n = avail;
    if (!n) return 0;
    uint32_t off = rd % RING_CAP;
    uint32_t first = RING_CAP - off;
    if (first > n) first = n;
    memcpy(dst, s_ring + off, first);
    if (n > first) memcpy(dst + first, s_ring, n - first);
    atomic_store_explicit(&s_rd, (rd + n) % RING_MOD, memory_order_release);
    return n;
}

/* ---- 进度时间基 / Progress clock ---- */

static void time_reset(void) {
    portENTER_CRITICAL(&s_time_lock);
    s_pos_ms = 0;
    s_base_us = esp_timer_get_time();
    portEXIT_CRITICAL(&s_time_lock);
}

static void time_freeze(void) {
    portENTER_CRITICAL(&s_time_lock);
    if (s_base_us) {
        s_pos_ms += (uint32_t)((esp_timer_get_time() - s_base_us) / 1000);
        s_base_us = 0;
    }
    portEXIT_CRITICAL(&s_time_lock);
}

static void time_resume(void) {
    portENTER_CRITICAL(&s_time_lock);
    if (!s_base_us) s_base_us = esp_timer_get_time();
    portEXIT_CRITICAL(&s_time_lock);
}

static uint32_t time_elapsed(void) {
    portENTER_CRITICAL(&s_time_lock);
    uint32_t ms = s_pos_ms;
    if (s_base_us) ms += (uint32_t)((esp_timer_get_time() - s_base_us) / 1000);
    portEXIT_CRITICAL(&s_time_lock);
    return ms;
}

/* ---- 状态迁移 / State transitions ---- */

static void save_progress(uint32_t pos, bool playing) {
    s_resume_pos = pos;
    s_resume_was_playing = playing;
    if (s_tracks) {
        const int i = atomic_load(&s_index);
        if (i >= 0 && i < s_count) strlcpy(s_resume_path, s_tracks[i].path, sizeof(s_resume_path));
    }
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u32(h, NVS_KEY_POS, pos);
        nvs_set_u8(h, NVS_KEY_RESUME, playing ? 1 : 0);
        nvs_commit(h);
        nvs_close(h);
    }
}

static void set_state(music_state_t s) {
    const int prev = atomic_exchange(&s_state, (int)s);
    if (prev != (int)s) {
        atomic_fetch_add(&s_serial, 1);
        // 播放状态是「有没有声音」的一半：锁屏/唤醒期间的状态迁移必须和蓝牙事件对得上。
        // Playback state is half of "is there sound": its transitions must line up with the
        // Bluetooth events around lock and wake.
        bt_trace("MUSIC state %d -> %d", prev, (int)s);
        // 串口留一份：没有卡的时候，「谁把播放压回暂停」就是靠这几行看出来的。
        // Keep a console copy: without the card, these lines are how "who paused playback" is seen.
        ESP_LOGI(TAG, "state %d -> %d (auto=%d out=%s)", prev, (int)s,
                 (int)atomic_load(&s_auto_paused),
                 music_output() == MUSIC_OUT_SPK ? "spk" : "bt");
        // 进入暂停/停止时落盘进度（不续播）；播放中位置由周期保存覆盖。
        // Persist progress when entering paused/stopped (no auto-resume); the in-play
        // position is overwritten by the periodic save.
        if (s == MUSIC_PAUSED || s == MUSIC_IDLE) save_progress(time_elapsed(), false);
    }
}

static void bump(void) { atomic_fetch_add(&s_serial, 1); }

static void set_error(const char *msg) {
    portENTER_CRITICAL(&s_err_lock);
    strlcpy(s_error, msg, sizeof(s_error));
    portEXIT_CRITICAL(&s_err_lock);
    bump();
}

// 忙则暂停、闲了（且是条件暂停的）自动续。/ Pause while blocked; auto-resume when clear if the pause was conditional.
static bool play_blocked(void) {
    // 用「当前输出通路是否可出声」而不是「蓝牙链路在不在」：本机扬声器这条路没有耳机链路，
    // 链路判据恒为假，会让每一次播放刚起就被 eval_auto 判成「忙」压回暂停（现场 FEED got=0）。
    // Gate on "can the selected route make sound", not "is the BT link up": the local speaker
    // route has no headset link, so the link flag is always false there and eval_auto would
    // pause every playback the moment it starts (field: FEED got=0 pk=0).
    return atomic_load(&s_media_lost) || atomic_load(&s_media_busy) ||
           atomic_load(&s_radio_busy) || atomic_load(&s_suspended) || !bt_audio_output_ready();
}

static void eval_auto(void) {
    const bool blocked = play_blocked();
    const int st = atomic_load(&s_state);
    if (blocked && st == MUSIC_PLAYING) {
        time_freeze();
        atomic_store(&s_auto_paused, true);
        set_state(MUSIC_PAUSED);
    } else if (!blocked && st == MUSIC_PAUSED && atomic_load(&s_auto_paused) &&
               !atomic_load(&s_suspend_no_resume)) {
        time_resume();
        atomic_store(&s_auto_paused, false);
        set_state(MUSIC_PLAYING);
    }
}

/* ---- 扫描 / Library scan ---- */

static bool ext_wanted(const char *name) {
    const char *dot = strrchr(name, '.');
    if (!dot) return false;
    return strcasecmp(dot, ".mp3") == 0 || strcasecmp(dot, ".m4a") == 0 ||
           strcasecmp(dot, ".aac") == 0 || strcasecmp(dot, ".flac") == 0 ||
           strcasecmp(dot, ".wav") == 0 || strcasecmp(dot, ".opus") == 0 ||
           strcasecmp(dot, ".ogg") == 0;  // Opus/Vorbis 走 OGG 容器解析 / Ogg container (Opus/Vorbis)
}

static void scan_dir(const char *path, int depth, music_track_t *out, int *count) {
    DIR *dir = opendir(path);
    if (!dir) return;
    struct dirent *ent;
    while (*count < MUSIC_MAX_TRACKS && (ent = readdir(dir)) != NULL) {
        if (ent->d_name[0] == '.') continue;
        char child[300];
        const int n = snprintf(child, sizeof(child), "%s/%s", path, ent->d_name);
        if (n <= 0 || n >= (int)sizeof(child)) continue;
        struct stat st;
        if (stat(child, &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) {
            if (depth + 1 < 2) scan_dir(child, depth + 1, out, count);  // 深度 2 / depth 2
        } else if (S_ISREG(st.st_mode) && ext_wanted(ent->d_name)) {
            music_track_t *t = &out[*count];
            memset(t, 0, sizeof(*t));
            strlcpy(t->path, child, sizeof(t->path));
            music_tags_read(child, t->title, sizeof(t->title), t->artist, sizeof(t->artist),
                            &t->duration_ms);
            ++(*count);
        }
    }
    closedir(dir);
}

// 收藏键由完整路径哈希而来：与书籍收藏同一手法，命名空间独立（rp_music）。
// The favorite key hashes the full path: same recipe as book favorites, its own namespace (rp_music).
static void fav_key(const char *path, char key[13]) {
    uint32_t hash = UINT32_C(2166136261);
    for (const unsigned char *p = (const unsigned char *)path; *p; ++p)
        hash = (hash ^ *p) * UINT32_C(16777619);
    snprintf(key, 13, NVS_KEY_FAV_PREFIX "%08lx", (unsigned long)hash);
}

/* ---- 解码任务内部 / Decode-task internals ---- */

static FILE *s_file;
static esp_audio_simple_dec_handle_t s_dec;
static uint8_t *s_in;
static uint8_t *s_out;
static uint32_t s_in_len, s_in_off;
static bool s_eos;
static int s_stall;

// 进度跳转：UI 提交请求，解码任务执行（MP3 按字节粗定位，其余格式/粗定位失败则回起点丢帧）。
// Seek: the UI posts the request; the decode task runs it (a byte-offset coarse jump for MP3,
// a rewind-and-discard for other formats or when the coarse landing cannot resync).
static atomic_int s_seek_req = ATOMIC_VAR_INIT(-1);  // 目标毫秒 / target in ms, -1 = none
static bool s_seek_active;                           // 跳转进行中（仅解码任务读写）/ seeking right now (decode task only)
static uint32_t s_seek_left_ms;                      // 还要丢弃的音频毫秒 / audio milliseconds left to discard
static bool s_seek_coarse;                           // 上次跳转走粗定位，落点解码失败要退回精确路径 / last seek landed coarse; a decode failure rewinds
static uint32_t s_seek_target;                       // 最近一次跳转目标 / latest seek target

// 重采样状态：p 是「虚拟帧位置」，-1 对应 hold；n 为当前解码块帧数。
// Resampler state: p is the virtual frame position (-1 = hold); n is the chunk size.
static int16_t s_rs_hold[2];
static double s_rs_pos;
static int s_rs_n;
static bool s_rs_ch_alive;  // hold 是否有效 / hold carries a real frame
static uint32_t s_rs_rate;
static int s_rs_ch;
static bool s_rs_pending;  // s_out 里还有帧没进环 / s_out still has frames to push

static esp_audio_simple_dec_type_t dec_type_for(const char *path) {
    const char *dot = strrchr(path, '.');
    if (!dot) return ESP_AUDIO_SIMPLE_DEC_TYPE_NONE;
    if (strcasecmp(dot, ".mp3") == 0) return ESP_AUDIO_SIMPLE_DEC_TYPE_MP3;
    if (strcasecmp(dot, ".m4a") == 0 || strcasecmp(dot, ".aac") == 0)
        return ESP_AUDIO_SIMPLE_DEC_TYPE_M4A;
    if (strcasecmp(dot, ".flac") == 0) return ESP_AUDIO_SIMPLE_DEC_TYPE_FLAC;
    if (strcasecmp(dot, ".opus") == 0 || strcasecmp(dot, ".ogg") == 0)
        return ESP_AUDIO_SIMPLE_DEC_TYPE_OGG;  // Ogg 容器解析分发到 Opus/Vorbis / OGG demux → Opus or Vorbis
    if (strcasecmp(dot, ".wav") == 0) return ESP_AUDIO_SIMPLE_DEC_TYPE_WAV;
    return ESP_AUDIO_SIMPLE_DEC_TYPE_NONE;
}

static void close_track(void) {
    if (s_dec) {
        esp_audio_simple_dec_close(s_dec);
        s_dec = NULL;
    }
    if (s_file) {
        fclose(s_file);
        s_file = NULL;
    }
    music_lyrics_clear();
    ring_reset();  // 丢掉上一首残留在环里的音频，否则换曲后要先放完旧数据才出声 / drop the previous track's leftover audio so a new track sounds at once
    s_rs_pending = false;
    s_in_len = s_in_off = 0;
    s_eos = false;
}

static void reset_rs(void) {
    s_rs_pos = 0;
    s_rs_n = 0;
    s_rs_pending = false;
    s_rs_ch_alive = false;
    s_rs_rate = 0;
    s_rs_ch = 0;
    s_rs_hold[0] = s_rs_hold[1] = 0;
}

// 把 s_out 里 n 帧线性重采样推进环；环满返回 false（下次续推，状态全在模块变量里）。
// Linearly resample the n frames in s_out into the ring; returns false when the ring is
// full (state lives in module variables, so the push resumes next round).
static bool emit_rs(const int16_t *x, const int in_ch) {
    const double ratio = (double)s_rs_rate / (double)OUT_RATE;
    while (ring_used() + 4 <= RING_CAP) {
        // floor 而非截断：s_rs_pos 可为负（-1 = hold 帧）。
        // floor, not truncation: s_rs_pos can be negative (-1 = the hold frame).
        long i0 = (long)s_rs_pos;
        if (s_rs_pos < 0 && (double)i0 != s_rs_pos) i0--;
        if (i0 + 1 >= s_rs_n) {  // 需要下一块 / need the next chunk
            if (s_rs_n > 0) {
                s_rs_hold[0] = x[(s_rs_n - 1) * in_ch];
                s_rs_hold[1] = in_ch > 1 ? x[(s_rs_n - 1) * in_ch + 1]
                                         : x[(s_rs_n - 1) * in_ch];
                s_rs_ch_alive = true;
            }
            s_rs_pos -= (double)s_rs_n;
            return true;
        }
        int16_t frame[2];
        for (int c = 0; c < 2; ++c) {
            const int sc = (in_ch > 1 && c < in_ch) ? c : 0;  // 单声道双路同源 / mono feeds both
            int16_t a, b;
            if (i0 < 0) {
                a = s_rs_hold[sc == 0 || in_ch == 1 ? 0 : 1];
                b = x[0 * in_ch + sc];
            } else {
                a = x[i0 * in_ch + sc];
                b = x[(i0 + 1) * in_ch + sc];
            }
            if (!s_rs_ch_alive && i0 < 0) a = 0;
            const double frac = s_rs_pos - (double)i0;
            frame[c] = (int16_t)(a + (double)(b - a) * frac);
        }
        uint8_t pair[4];
        memcpy(pair, &frame[0], 2);
        memcpy(pair + 2, &frame[1], 2);
        if (ring_push(pair, 4) != 4) return false;  // 环满 / ring full
        s_rs_pos += ratio;
    }
    return false;
}

bool music_ring_drained(void) { return !s_ring || ring_used() == 0; }

// 收藏循环：从 cur 之后找下一首收藏，绕回开头；一首收藏都没有时返回 -1。
// Favorites loop: the next favorited track after cur, wrapping; -1 when nothing is favorited.
static int next_fav_index(int cur) {
    if (s_count <= 0) return -1;
    for (int step = 1; step <= s_count; ++step) {
        const int i = (cur + step) % s_count;
        if (s_fav[i]) return i;
    }
    return -1;
}

static void finish_track(void) {
    close_track();
    if (atomic_load(&s_single)) {
        // 单曲直放播完即停，不接曲库里的下一首。/ A direct file stops here; no library follow-up.
        atomic_store(&s_single, false);
        time_freeze();
        bt_trace("MUSIC finish (single) -> idle");
        set_state(MUSIC_IDLE);
        bump();
        return;
    }
    const int count = s_count;
    if (count > 0 && atomic_load(&s_state) != MUSIC_IDLE && !atomic_load(&s_media_lost)) {
        // 曲末自动切歌按模式走；手动上一首/下一首始终按完整曲库顺序。
        // Auto-advance at EOF follows the mode; manual prev/next always walks the whole library.
        const int cur = atomic_load(&s_index);
        int next = cur < 0 ? 0 : cur;
        switch ((music_mode_t)atomic_load(&s_mode)) {
        case MUSIC_MODE_ONE:
            next = cur >= 0 ? cur : 0;  // 单曲循环：同一首重播 / repeat the same track
            break;
        case MUSIC_MODE_SHUFFLE:
            // 随机：count>1 时挑一个不是当前的下标。/ Shuffle: pick any other index when possible.
            if (count > 1) {
                int pick = (int)(esp_random() % (uint32_t)count);
                if (pick == cur) pick = (pick + 1 + (int)(esp_random() % (uint32_t)(count - 1))) % count;
                next = pick;
            } else next = 0;
            break;
        case MUSIC_MODE_FAV: {
            // 收藏循环：只在收藏里绕；收藏被清空则退回列表循环，不至于莫名停止。
            // Favorites loop: walk favorites only; with none left, fall back to the list loop
            // instead of stopping unexpectedly.
            const int fav = next_fav_index(cur < 0 ? -1 : cur);
            next = fav >= 0 ? fav : (cur + 1) % count;
            break;
        }
        case MUSIC_MODE_LIST:
        default:
            next = (cur + 1) % count;
            break;
        }
        atomic_store(&s_req, next);
    } else {
        // 曲末走到这里就是「没下一首 / 丢卡」：实机上表现为播放器突然回到 IDLE，
        // 和唤醒链路里的假丢卡长得一模一样，所以把原因一起写下来。
        // Landing here at EOF means "no next track / card gone": on device that looks exactly
        // like a false card-loss at wake, so record which one it was.
        bt_trace("MUSIC finish (count=%d lost=%d state=%d) -> idle", count,
                 (int)atomic_load(&s_media_lost), (int)atomic_load(&s_state));
        time_freeze();
        set_state(MUSIC_IDLE);
    }
}

/* ---- 进度跳转 / Seek ---- */

// 落位：进度从目标点起算，仍在播放且不忙时续走时钟。
// Land: progress counts from the target and the clock resumes when playing and unblocked.
static void seek_finish(void) {
    s_seek_active = false;
    if (atomic_load(&s_state) == MUSIC_PLAYING && !play_blocked()) time_resume();
    bump();
}

// MP3 音频起点：跳过 ID3v2 头（'ID3' + 版本 + 标志 + 4 字节同步安全长度），否则大封面会让按比例
// 换算的落点偏前。没有 ID3 头时返回 0。
// MP3 audio start: skip the ID3v2 header ('ID3', version, flags, 4-byte syncsafe length), otherwise a
// large cover makes the proportional landing early. Returns 0 when there is no ID3 header.
static long mp3_audio_offset(void) {
    uint8_t h[10];
    if (fseek(s_file, 0, SEEK_SET) != 0 || fread(h, 1, 10, s_file) != 10) return 0;
    if (h[0] != 'I' || h[1] != 'D' || h[2] != '3') return 0;
    const long body = ((long)h[6] << 21) | ((long)h[7] << 14) | ((long)h[8] << 7) | (long)h[9];
    long off = 10 + body;
    if (h[5] & 0x10) off += 10;  // 有页脚 / a footer follows the tag
    return off;
}

// 把解码器与缓冲区恢复到"从当前文件位置重新开始"的状态，供两条跳转路径共用。
// Put the decoder and buffers back into a "restart at the current file position" state; shared by both seek paths.
static void seek_rewind_state(uint32_t ms) {
    esp_audio_simple_dec_reset(s_dec);
    ring_reset();  // 丢掉环里旧位置的在途音频，避免点击后还要先放几秒旧内容 / drop stale audio so the new position sounds at once
    s_in_len = s_in_off = 0;
    s_eos = false;
    s_stall = 0;
    s_rs_pending = false;
    portENTER_CRITICAL(&s_time_lock);
    s_pos_ms = ms;
    s_base_us = 0;  // 落位前进度停在目标点 / progress sits on the target until the seek lands
    portEXIT_CRITICAL(&s_time_lock);
}

// 粗定位：按"文件大小 × 时间比例"直接移动读点再让解码器重新同步，MP3 帧可自同步。
// 大跨度跳转若从头丢帧，要按解码速度重放整段音频（几十秒无声音），所以先走这条。
// Coarse landing: move the read position by the file-size/time ratio and let the decoder resync
// (MP3 frames self-sync). Discarding from the start would replay the whole skipped span at decode
// speed — tens of seconds of silence — so this runs first.
static bool seek_coarse_apply(uint32_t ms, uint32_t dur) {
    const int i = atomic_load(&s_index);
    if (!s_tracks || i < 0 || i >= s_count) return false;
    const char *path = s_tracks[i].path;
    const char *dot = strrchr(path, '.');
    if (!dot || strcasecmp(dot, ".mp3") != 0) return false;
    struct stat st;
    if (stat(path, &st) != 0 || st.st_size <= 0 || st.st_size > (off_t)0x7fff0000) return false;
    const long audio = mp3_audio_offset();
    const long total = (long)st.st_size - audio - 128;  // 粗略扣掉尾部 ID3v1 / roughly minus a trailing ID3v1
    if (total <= 1024) return false;
    long off = audio + (long)((double)total * (double)ms / (double)dur);
    if (off < audio) off = audio;
    if (off > audio + total - 4) off = audio + total - 4;
    if (fseek(s_file, off, SEEK_SET) != 0) return false;
    seek_rewind_state(ms);
    s_seek_active = false;
    s_seek_coarse = true;  // 落点若重新同步失败，报错路径会退回从头丢帧 / a resync failure rewinds from the start
    seek_finish();
    return true;
}

// 精确路径：回文件起点，逐块解码丢弃到目标。粗定位失败（解码器在落点找不到帧头）时也走这里。
// Exact path: rewind and discard chunk by chunk. Also the fallback when a coarse landing cannot resync.
static void seek_begin(uint32_t ms) {
    if (!s_file || !s_dec) return;
    const uint32_t dur = music_duration_ms();
    if (!dur) return;
    // 留在末尾之前，别把目标处的 EOF 当成正常放完而跳下一首。
    // Stay off the tail so a target-time EOF is not mistaken for a finish that advances.
    if (ms + 1500 >= dur) ms = dur > 1500 ? dur - 1500 : 0;
    s_seek_target = ms;
    s_seek_coarse = false;
    if (seek_coarse_apply(ms, dur)) return;
    if (fseek(s_file, 0, SEEK_SET) != 0) return;
    seek_rewind_state(ms);
    s_seek_left_ms = ms;
    s_seek_active = ms > 0;
    bump();
    if (!s_seek_active) seek_finish();
}

// 粗定位没能在落点重新同步：退回从头丢帧，进度保持显示目标点直到真正落位。
// The coarse landing never resynced: rewind and discard, keeping the target on screen until it lands.
static bool seek_fallback(void) {
    s_seek_coarse = false;
    if (!s_file || !s_dec || fseek(s_file, 0, SEEK_SET) != 0) return false;
    ESP_LOGW(TAG, "seek: coarse landing lost the frame sync, rewinding");
    seek_rewind_state(s_seek_target);
    s_seek_left_ms = s_seek_target;
    s_seek_active = s_seek_target > 0;
    if (!s_seek_active) seek_finish();
    return true;
}

static void open_track(int index) {
    close_track();
    atomic_store(&s_single, false);  // 回到曲库：清掉单曲直放标记 / back in the library: drop the direct-play flag
    s_seek_active = false;        // 换曲取消在途跳转 / a track change cancels a pending seek
    s_seek_coarse = false;
    s_seek_target = 0;
    s_stall = 0;
    if (index < 0 || index >= s_count || !s_tracks) {
        set_state(MUSIC_IDLE);
        return;
    }
    atomic_store(&s_index, index);
    set_error("");
    set_state(MUSIC_LOADING);
    bump();

    const music_track_t *t = &s_tracks[index];
    s_file = fopen(t->path, "rb");
    if (!s_file) {
        set_error("文件不存在");
        set_state(MUSIC_ERROR);
        return;
    }
    esp_audio_simple_dec_cfg_t cfg = {0};
    cfg.dec_type = dec_type_for(t->path);
    cfg.use_frame_dec = false;
    if (esp_audio_simple_dec_open(&cfg, &s_dec) != ESP_AUDIO_ERR_OK) {
        s_dec = NULL;
        fclose(s_file);
        s_file = NULL;
        set_error("格式不支持");
        set_state(MUSIC_ERROR);
        return;
    }
    time_reset();
    set_state(MUSIC_PLAYING);
    // 续播：若打开的正是上次曲目且留有进度，跳到上次位置；换曲则清零续播进度。
    // Resume: if this is the saved track and a position is kept, seek there; a new
    // track resets the resume position.
    const bool same_as_resume = strcmp(t->path, s_resume_path) == 0;
    if (same_as_resume && s_resume_pos > 0 && s_resume_pos < t->duration_ms)
        seek_begin(s_resume_pos);
    strlcpy(s_resume_path, t->path, sizeof(s_resume_path));
    if (!same_as_resume) s_resume_pos = 0;
    // 歌词：同目录 .lrc 优先，其次内嵌（在解码任务侧加载，UI 只读查询）。
    // Lyrics: the same-directory .lrc first, then embedded (loaded here on the decode task,
    // read-only from the UI).
    music_lyrics_load(t->path, t->duration_ms);
    // 记住曲目（30 秒级的持久化放到 set；每首一次写入足够便宜）。
    // Persist the track (throttled writes live in set; one write per track is cheap enough).
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_str(h, NVS_KEY_TRACK, t->path);
        nvs_commit(h);
        nvs_close(h);
    }
}

// 单曲直放：曲库外的音频（文件管理里点开）。不进曲库、不写"上次曲目"，播完即停。
// Direct play for audio outside the library (opened from the file manager): it never joins the
// library, never overwrites the saved track, and stops when it ends.
static void open_file(const char *path) {
    close_track();
    s_seek_active = false;        // 换曲取消在途跳转 / a new file cancels a pending seek
    s_seek_coarse = false;
    s_seek_target = 0;
    s_stall = 0;
    atomic_store(&s_index, -1);
    set_error("");
    set_state(MUSIC_LOADING);
    bump();

    s_file = fopen(path, "rb");
    if (!s_file) {
        set_error("文件不存在");
        set_state(MUSIC_ERROR);
        return;
    }
    esp_audio_simple_dec_cfg_t cfg = {0};
    cfg.dec_type = dec_type_for(path);
    cfg.use_frame_dec = false;
    if (esp_audio_simple_dec_open(&cfg, &s_dec) != ESP_AUDIO_ERR_OK) {
        s_dec = NULL;
        fclose(s_file);
        s_file = NULL;
        set_error("格式不支持");
        set_state(MUSIC_ERROR);
        return;
    }
    atomic_store(&s_single, true);
    strlcpy(s_single_path, path, sizeof(s_single_path));
    s_single_title[0] = 0;
    s_single_artist[0] = 0;
    s_single_dur = 0;
    music_tags_read(path, s_single_title, sizeof(s_single_title), s_single_artist,
                    sizeof(s_single_artist), &s_single_dur);
    if (!s_single_title[0]) {  // 没有标签就用文件名 / no tags: fall back to the file name
        const char *slash = strrchr(path, '/');
        strlcpy(s_single_title, slash ? slash + 1 : path, sizeof(s_single_title));
        char *dot = strrchr(s_single_title, '.');
        if (dot) *dot = 0;
    }
    time_reset();
    set_state(MUSIC_PLAYING);
    music_lyrics_load(path, s_single_dur);
}

// 单步解码；返回是否有进展（false = 环满或无输入，等下一轮）。
// One decode step; returns whether it made progress (false = ring full or no input yet).
static bool step_decode(void) {
    if (!s_file || !s_dec) return false;

    // 1) 上一块还没推完，先推完再解码，保证 s_out 只有一份在途数据。
    // Drain the previous chunk before decoding again, so only one chunk is ever in flight.
    if (s_rs_pending) {
        if (!emit_rs((const int16_t *)s_out, s_rs_ch)) return false;
        s_rs_pending = false;
    }

    // 2) 输入不足先读文件。/ Refill the input buffer first.
    if (s_in_off >= s_in_len && !s_eos) {
        const size_t got = fread(s_in, 1, IN_CAP, s_file);
        if (got == 0 && ferror(s_file)) {
            // 读错误（多半是拔卡）：当场按丢卡停机，别把 EOF 当正常放完。
            // A read error (usually card removal): stop as media-lost instead of
            // mistaking it for a clean end of file.
            music_media_lost();
            return false;
        }
        if (got < IN_CAP) s_eos = true;
        s_in_len = (uint32_t)got;
        s_in_off = 0;
    }

    // 3) 解码。/ Decode.
    esp_audio_simple_dec_raw_t raw = {0};
    raw.buffer = s_in + s_in_off;
    raw.len = s_in_len - s_in_off;
    raw.eos = s_eos && s_in_off >= s_in_len;  // 输入耗尽才冲刷解析器缓存 / flush the parser cache only once drained
    esp_audio_simple_dec_out_t out = {0};
    out.buffer = s_out;
    out.len = OUT_CAP;

    const esp_audio_err_t err = esp_audio_simple_dec_process(s_dec, &raw, &out);
    s_in_off += raw.consumed;

    if (err == ESP_AUDIO_ERR_BUFF_NOT_ENOUGH || err == ESP_AUDIO_ERR_FAIL ||
        err == ESP_AUDIO_ERR_INVALID_PARAMETER || err == ESP_AUDIO_ERR_NOT_SUPPORT ||
        err == ESP_AUDIO_ERR_MEM_LACK) {
        // 粗定位落点找不到帧头：退回从头丢帧，别把这一次当成解码失败停机。
        // A coarse landing that cannot find a frame header rewinds instead of stopping as an error.
        if (s_seek_coarse && seek_fallback()) return true;
        set_error("解码失败");
        set_state(MUSIC_ERROR);
        return false;
    }

    if (out.decoded_size) {
        esp_audio_simple_dec_info_t info = {0};
        if (esp_audio_simple_dec_get_info(s_dec, &info) != ESP_AUDIO_ERR_OK ||
            info.sample_rate == 0 || info.channel == 0) {
            set_error("解码失败");
            set_state(MUSIC_ERROR);
            return false;
        }
        if (info.bits_per_sample != 16) {  // 线性重采样只吃 s16 / the resampler takes s16 only
            set_error("格式不支持");
            set_state(MUSIC_ERROR);
            return false;
        }
        if (info.sample_rate != s_rs_rate || info.channel != s_rs_ch) {
            reset_rs();
            s_rs_rate = info.sample_rate;
            s_rs_ch = info.channel;  // 真实声道数做交织步长，emit 只取前两路 / real
                                     // channel count is the interleave stride; emit takes the front pair
        }
        s_rs_n = (int)(out.decoded_size / (2u * info.channel));
        if (s_seek_active) {
            // 跳转中：这一块音频直接丢弃，按解码时长倒扣目标预算，扣完落位。
            // Seeking: this chunk is dropped, counting down the target budget in audio time until it lands.
            s_rs_pending = false;
            const uint32_t ms = (uint32_t)s_rs_n * 1000u / (s_rs_rate ? s_rs_rate : 1);
            if (ms >= s_seek_left_ms) seek_finish();
            else s_seek_left_ms -= ms;
            s_stall = 0;
            return true;
        }
        s_seek_coarse = false;  // 落点已经解出音频，粗定位成功 / the landing decoded audio, so the coarse seek stuck
        s_rs_pending = true;
        s_stall = 0;
        return true;
    }

    // 4) 无输出：要么数据不够（继续读），要么文件结束。
    // No output: either starved (read more) or the file is over.
    if (s_eos && s_in_off >= s_in_len) {
        if (raw.consumed == 0) {
            finish_track();
            return true;
        }
    }
    if (raw.consumed == 0) {
        if (++s_stall > 200) {
            if (s_seek_coarse && seek_fallback()) return true;  // 落点始终没同步上 / the landing never synced
            set_error("解码失败");
            set_state(MUSIC_ERROR);
            return false;
        }
        if (s_in_off < s_in_len) s_in_off++;  // 跳一个字节重新同步 / skip a byte to resync
    } else {
        s_stall = 0;
    }
    return true;
}

static void decode_task(void *arg) {
    (void)arg;
    while (!atomic_load(&s_ready) && !atomic_load(&s_task_exit)) vTaskDelay(pdMS_TO_TICKS(50));
    // 蓝牙一被打开（播放器启动 / 用户拨开开关 / 按「重连」）就自动连上次的耳机：音频栈就绪后
    // 延迟一会儿再连（避开初始化竞态），每次就绪只试一次。断链之后蓝牙会被彻底关掉，那时开关
    // 已经是 off，本段不会去对抗那个策略。
    // As soon as Bluetooth is on (player start, the user flipping the switch, or "relink") the last
    // headset is connected automatically: wait a beat after the audio stack is ready (avoid init
    // races) and try once per readiness. After a drop the switch is off, so this never fights the
    // "a drop disables Bluetooth" policy.
    bool auto_armed = false, was_ready = false;
    TickType_t auto_deadline = 0;
    for (;;) {
        const bool ready = atomic_load(&s_ready);
        if (ready && !was_ready) {
            auto_armed = true;
            auto_deadline = xTaskGetTickCount() + pdMS_TO_TICKS(1500);
        }
        was_ready = ready;
        if (auto_armed && ready && (int32_t)(xTaskGetTickCount() - auto_deadline) >= 0) {
            auto_armed = false;  // 无论结果如何，本次就绪不再重试 / one shot per readiness
            // 模组可能还在启动：bt_audio_connect 会把请求挂到模式 2 就绪后再发。
            // The module may still be booting; bt_audio_connect queues until mode 2 is up.
            // 蓝牙开关关闭时不做自动连接；目标优先取已配对表首项，老用户无配对表
            // 时回退到"上次连过的设备"。
            // No auto-connect while the Bluetooth switch is off; the target prefers the
            // paired table's head, falling back to the last device for existing users.
            // 本机扬声器输出时不找耳机：通路已经就绪，再去 AT+CONNECT 会把模组从 mode 1
            // 拽回 mode 2，扬声器立刻断掉。
            // Never hunt for a headset on the speaker route: the path is already up, and an
            // AT+CONNECT pulls the module back from mode 1 to mode 2, killing the speaker.
            if (app_settings_bt_auto_connect() && app_settings_bt_enabled() &&
                atomic_load(&s_output) == MUSIC_OUT_BT &&
                !bt_audio_link_active() && !bt_audio_scanning()) {
                uint8_t bda[6];
                char name[36];
                bool have = app_settings_bt_paired_count() > 0 &&
                            app_settings_bt_paired_get(0, bda, name, sizeof(name));
                if (!have) have = app_settings_bt_last_device(bda, name, sizeof(name));
                if (have && bt_audio_connect(bda) == ESP_OK) {
                    ESP_LOGI(TAG, "auto-connect to %s", name[0] ? name : "last device");
                }
            }
        }

        const int req = atomic_exchange(&s_req, REQ_NONE);
        if (req == REQ_STOP) {
            close_track();
            atomic_store(&s_single, false);
            time_freeze();
            bt_trace("MUSIC stop req (lost=%d state=%d)", (int)atomic_load(&s_media_lost),
                     (int)atomic_load(&s_state));
            set_state(MUSIC_IDLE);
            if (atomic_load(&s_task_exit)) break;   // 停止并退出：栈归还 / stop-and-exit frees the stack
            continue;
        }
        if (req >= 0) open_track(req);
        else if (req == REQ_SINGLE) open_file(s_single_path);
        const int32_t seek_ms = atomic_exchange(&s_seek_req, -1);
        if (seek_ms >= 0) seek_begin((uint32_t)seek_ms);

        // 丢卡闩锁自愈：卡在位就把闩锁清掉；若这是一次唤醒里的假丢卡（30 s 内）且当时
        // 正在播，就顺手接回原来那首——否则用户看到的是「蓝牙连上了、界面却停在待播放」。
        // Latch self-heal: drop it once the card answers, and resume when the loss was a false
        // one from the wake path (within 30 s) that interrupted playback.
        if (atomic_load(&s_media_lost)) {
            const int64_t now = esp_timer_get_time();
            if (now - s_media_probe_us > 3000000) {
                s_media_probe_us = now;
                if (music_media_probe() && atomic_load(&s_media_resume) &&
                    now - s_media_lost_us < 30 * 1000 * 1000) {
                    atomic_store(&s_media_resume, false);
                    const int idx = atomic_load(&s_index);
                    const int first = idx >= 0 && idx < s_count ? idx : 0;
                    bt_trace("MUSIC resume after media back (idx=%d count=%d)", first, s_count);
                    if (s_count > 0) atomic_store(&s_req, first);
                }
            }
        }

        eval_auto();
        const int state = atomic_load(&s_state);
        // 播放中每 15 秒落盘一次进度+续播标志，防突然断电丢进度。
        // While playing, persist progress and the resume flag every 15 s to survive abrupt power loss.
        if (state == MUSIC_PLAYING) {
            const TickType_t now = xTaskGetTickCount();
            if ((int32_t)(now - s_last_save_tick) >= pdMS_TO_TICKS(15000)) {
                s_last_save_tick = now;
                save_progress(time_elapsed(), true);
            }
        }
        // 跳转丢帧在暂停时也要继续推进（暂停时本来不喂环，静音正确）。
        // The seek's discard loop must run even while paused (paused pulls no ring, so silence is correct).
        if (state == MUSIC_PLAYING || (s_seek_active && state != MUSIC_ERROR)) {
            if (!step_decode()) vTaskDelay(pdMS_TO_TICKS(20));
        } else {
            vTaskDelay(pdMS_TO_TICKS(50));
        }
    }
    // 退出前把自己登记为“已结束”，下次 start 才会重新建任务。
    // Mark the task as gone before self-deleting so the next start recreates it.
    close_track();
    s_task_created = false;
    atomic_store(&s_task_exit, false);
    bt_trace("MUSIC task exit (stack released)");
    vTaskDelete(NULL);
}

/* ---- 模组拉流 / Module pull callback ---- */

static int32_t pcm_pull(uint8_t *dst, int32_t want) {
    if (want <= 0) return 0;
    // 每 2 s 一行的自述：模组在拉流，播放器却给不出数据时，到底是「没在播」还是「环空」。
    // 现场区分这两者只能靠卡上的 trace，改成串口后一眼就能看出断在哪一侧。
    // A line every 2 s: when the module pulls but the player returns nothing, this says whether
    // the player is simply not playing or the ring ran dry — the one thing the SD trace was
    // needed for.
    static int64_t s_pull_log_us;
    static int s_pull_state = -1;
    const int state = atomic_load(&s_state);
    // 暂停/停止时返回 0，bt_host 统一补静音。
    // Return 0 while paused; bt_host pads silence.
    if (state != MUSIC_PLAYING) {
        const int64_t now = esp_timer_get_time();
        if (state != s_pull_state || now - s_pull_log_us >= 5000000) {
            s_pull_log_us = now;
            s_pull_state = state;
            ESP_LOGI(TAG, "pull idle: state=%d idx=%d ready=%d blocked=%d", state,
                     atomic_load(&s_index), (int)atomic_load(&s_ready), (int)play_blocked());
        }
        return 0;
    }
    s_pull_state = state;
    const uint32_t got = ring_pull(dst, (uint32_t)want);
    if (!got) {
        const int64_t now = esp_timer_get_time();
        if (now - s_pull_log_us >= 2000000) {
            s_pull_log_us = now;
            ESP_LOGW(TAG, "pull starved: ring empty while playing (idx=%d)", atomic_load(&s_index));
        }
    }
    return (int32_t)got;
}

/* ---- 对外接口 / Public API ---- */

// 系统自动放弃蓝牙（硬复位预算耗尽）时由 bt_host 回调：把设置页开关翻成 off，
// 让开关显示与“模组已断电”的真实状态一致；用户再拨 on 即走 bt_audio_init 重置计数。
// Fired by bt_host when it auto-disables Bluetooth after the hard-reset budget is spent: flip
// the settings switch off so the UI matches the real state; re-enabling re-arms the budget.
static void bt_auto_disabled_handler(void) {
    app_settings_set_bt_enabled(false);
}

esp_err_t music_player_start(void) {
    if (atomic_load(&s_ready)) return ESP_OK;
    // 一次性把“自动放弃蓝牙”的回调挂上：无论本次是首启还是 stop 后重启，注册幂等。
    // Register the auto-disable callback once (idempotent across re-starts).
    static bool s_auto_cb_registered = false;
    if (!s_auto_cb_registered) {
        bt_audio_set_auto_disabled_cb(bt_auto_disabled_handler);
        s_auto_cb_registered = true;
    }

    // 曲库与工作缓冲进 PSRAM，内部 RAM 留给蓝牙与显示。
    // Library and work buffers go to PSRAM; internal RAM stays for Bluetooth and the display.
    if (!s_tracks) {
        s_tracks = heap_caps_malloc(sizeof(music_track_t) * MUSIC_MAX_TRACKS,
                                    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!s_tracks) s_tracks = malloc(sizeof(music_track_t) * MUSIC_MAX_TRACKS);
        if (!s_tracks) return ESP_ERR_NO_MEM;
        memset(s_tracks, 0, sizeof(music_track_t) * MUSIC_MAX_TRACKS);
    }
    if (!s_ring) s_ring = heap_caps_malloc(RING_CAP, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_ring) s_ring = malloc(RING_CAP);
    if (!s_in) s_in = heap_caps_malloc(IN_CAP, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_in) s_in = malloc(IN_CAP);
    if (!s_out) s_out = heap_caps_malloc(OUT_CAP, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_out) s_out = malloc(OUT_CAP);
    if (!s_ring || !s_in || !s_out) return ESP_ERR_NO_MEM;

    // NVS：音量与上次曲目。/ NVS: volume and the last track.
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        uint8_t vol = 100;
        size_t len = sizeof(s_last_path);
        if (nvs_get_u8(h, NVS_KEY_VOL, &vol) == ESP_OK && vol <= 127)
            atomic_store(&s_vol, vol);
        if (nvs_get_str(h, NVS_KEY_TRACK, s_last_path, &len) != ESP_OK) s_last_path[0] = 0;
        strlcpy(s_resume_path, s_last_path, sizeof(s_resume_path));
        uint32_t pos = 0;
        if (nvs_get_u32(h, NVS_KEY_POS, &pos) == ESP_OK) s_resume_pos = pos;
        uint8_t rp = 0;
        if (nvs_get_u8(h, NVS_KEY_RESUME, &rp) == ESP_OK) s_resume_was_playing = rp != 0;
        uint8_t mode = 0;
        if (nvs_get_u8(h, NVS_KEY_MODE, &mode) != ESP_OK || mode >= (uint8_t)(MUSIC_MODE_FAV + 1))
            mode = MUSIC_MODE_LIST;
        atomic_store(&s_mode, (int)mode);
        uint8_t out = (uint8_t)MUSIC_OUT_BT;
        if (nvs_get_u8(h, NVS_KEY_OUT, &out) != ESP_OK || out > (uint8_t)MUSIC_OUT_SPK)
            out = (uint8_t)MUSIC_OUT_BT;
        atomic_store(&s_output, (int)out);
        size_t root_len = sizeof(s_root);
        if (nvs_get_str(h, NVS_KEY_ROOT, s_root, &root_len) != ESP_OK || !s_root[0])
            strlcpy(s_root, MUSIC_ROOT, sizeof(s_root));
        nvs_close(h);
    }
    s_boot_resume_done = false;
    s_last_save_tick = 0;

    const esp_audio_err_t reg = esp_audio_simple_dec_register_default();
    if (reg != ESP_AUDIO_ERR_OK && reg != ESP_AUDIO_ERR_INVALID_PARAMETER) {
        ESP_LOGE(TAG, "decoder register: %d", (int)reg);
        return ESP_FAIL;
    }
    // MP3/FLAC 走 audio_dec 层，simple 注册表里没有它们；两层都要注册，
    // 否则解码打开报 "Decoder MP3 not registered"（组件自带测试同样是两层都注册）。
    // MP3/FLAC live in the audio_dec layer, absent from the simple table, so both layers
    // must be registered or MP3 opens fail with "Decoder MP3 not registered"
    // (the component's own test registers both the same way).
    const esp_audio_err_t reg2 = esp_audio_dec_register_default();
    if (reg2 != ESP_AUDIO_ERR_OK && reg2 != ESP_AUDIO_ERR_INVALID_PARAMETER) {
        ESP_LOGE(TAG, "decoder register (audio_dec): %d", (int)reg2);
        return ESP_FAIL;
    }

    // 音频通路交给板载模组；音量在送 PCM 前按平方曲线缩放。
    // 开关关闭时不模组上电（音乐页的蓝牙开关负责后续再拉起）。
    // The audio path belongs to the board module; volume scales the PCM on a squared curve.
    // With the switch off the module stays unpowered (the music page's switch brings it up later).
    if (app_settings_bt_enabled()) {
        // 按用户上次选择的输出通路建立音频：蓝牙耳机走模式 2，本机扬声器走模式 1 + 功放。
        // Build the audio path per the last chosen route: headset = mode 2, speaker = mode 1 + PA.
        const bool spk = atomic_load(&s_output) == MUSIC_OUT_SPK;
        esp_err_t err = spk ? bt_audio_init_local(pcm_pull) : bt_audio_init(pcm_pull);
        if (err != ESP_OK)
            ESP_LOGW(TAG, "audio init (%s): %s", spk ? "spk" : "bt", esp_err_to_name(err));
        bt_audio_set_volume((uint8_t)atomic_load(&s_vol));
    }

    atomic_store(&s_ready, true);
    if (!s_task_created) {
        // 上一次停播的任务可能还在退出：等它把栈放开再建，避免两个解码任务并存。
        // The previous task may still be exiting; let it release the stack first.
        for (int i = 0; i < 100 && atomic_load(&s_task_exit); ++i) vTaskDelay(pdMS_TO_TICKS(10));
        s_task_created = true;
        // 解码任务绑 core 1，让 core 0 的 UI/刷屏不受重采样抖动影响。
        // Pin decoding to core 1 so UI/refresh on core 0 never stutters.
        if (xTaskCreatePinnedToCore(decode_task, "music_dec", TASK_STACK, NULL, TASK_PRIO, NULL,
                                    TASK_CORE) != pdPASS) {
            s_task_created = false;   // 下次 start 还要再试，不能永久放弃 / allow a later retry
            atomic_store(&s_ready, false);
            return ESP_ERR_NO_MEM;
        }
    }
    ESP_LOGI(TAG, "ready");
    return ESP_OK;
}

// 蓝牙一打开就自动连上次的耳机（配对表首项，老用户回退到"上次连过的设备"）。断链之后开关
// 已经被翻成 off，本函数不会被调用，所以不会和「断链即关蓝牙」的策略对着干。
// Connect the last headset as soon as Bluetooth is on (paired table head; the last used device
// for older installs). After a drop the switch is already off, so this never fights the
// "a drop disables Bluetooth" policy.
static void connect_last_device(const char *why) {
    if (!app_settings_bt_auto_connect()) return;
    if (atomic_load(&s_output) == MUSIC_OUT_SPK) return;   // 扬声器通路不找耳机 / speaker route
    if (bt_audio_link_active() || bt_audio_scanning()) return;
    uint8_t bda[6];
    char name[36];
    bool have = app_settings_bt_paired_count() > 0 &&
                app_settings_bt_paired_get(0, bda, name, sizeof(name));
    if (!have) have = app_settings_bt_last_device(bda, name, sizeof(name));
    if (have && bt_audio_connect(bda) == ESP_OK)
        ESP_LOGI(TAG, "auto-connect to %s (%s)", name[0] ? name : "last device", why);
}

void music_bt_set_enabled(bool enabled) {
    // 只管音频通路的上下电（pcm_pull 只在这里持有）；连接目标由播放页决定。
    // Owns only the audio path's power cycle (pcm_pull lives here); the play page picks
    // the connect target.
    bt_trace("BT switch -> %d (player_ready=%d)", (int)enabled, (int)atomic_load(&s_ready));
    app_settings_set_bt_enabled(enabled);
    if (enabled) {
        // 播放器被 music_player_stop() 停过（USB 传输模式会走到那条路）时不能静默不做事：
        // 用户看到的是「开关拨回去了、怎么也连不上」。这里补一次启动。
        // Never no-op silently after music_player_stop() (the USB transfer path takes it): the
        // user just sees "switch is back on but nothing connects". Start the player here instead.
        if (!atomic_load(&s_ready) && music_player_start() != ESP_OK) {
            bt_trace("BT switch on: player start failed");
            return;
        }
        const bool spk = atomic_load(&s_output) == MUSIC_OUT_SPK;
        esp_err_t err = spk ? bt_audio_init_local(pcm_pull) : bt_audio_init(pcm_pull);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "audio init (%s): %s", spk ? "spk" : "bt", esp_err_to_name(err));
            bt_trace("BT switch on: audio init failed %d", (int)err);
        }
        bt_audio_set_volume((uint8_t)atomic_load(&s_vol));
        // 开关拨开（含播放页「重连」重新打开）后立即自动连上次的耳机。
        // Right after the switch goes on (including a "relink" that re-enabled it), connect the
        // last headset automatically.
        connect_last_device("switch on");
    } else {
        bt_audio_scan_stop();
        bt_audio_deinit();  // 模组断电、链路与搜索一起收掉 / powers the module down, dropping link and scan
    }
    bump();
}

music_output_t music_output(void) { return (music_output_t)atomic_load(&s_output); }

void music_restart_output(void) {
    // 「重连」是用户手动兜底，按当前状态分两种：
    //  · 蓝牙已被「断链即关闭」策略彻底关掉 → 重新打开（等于在设置页把开关拨回去）
    //  · 还开着但没声音 → 重走一遍模式序列（回模式 1 并等应答 → 进模式 2 → 补 AT+CONNECT）
    // "Relink" is the manual fallback, and it depends on the current state: re-enable Bluetooth
    // when the drop policy turned it off, otherwise replay the mode sequence without a power cut.
    if (!app_settings_bt_enabled()) {
        ESP_LOGW(TAG, "relink: Bluetooth was off, turning it back on");
        music_bt_set_enabled(true);
        bump();
        return;
    }
    // 重连正在跑（整条序列好几秒）：这一下直接丢掉，不要让用户连按几下排出好几次重连。
    // A relink is already running (the sequence takes seconds): drop this press instead of
    // queueing another one on every extra tap.
    if (bt_audio_relinking()) {
        ESP_LOGW(TAG, "relink ignored: already running");
        bump();
        return;
    }
    ESP_LOGW(TAG, "relink (%s)", atomic_load(&s_output) == MUSIC_OUT_SPK ? "spk" : "bt");
    bt_audio_restart_module();
    // 重连完了再把耳机连上（s_link_bda 为空时——比如本次开机还没连过——也要能连）。
    // Reconnect the headset after the sequence; this also covers a boot where s_link_bda is still
    // empty, in which case the module-side relink alone would have nothing to work with.
    connect_last_device("relink");
    bump();
}

void music_set_output(music_output_t out) {
    if (out != MUSIC_OUT_SPK) out = MUSIC_OUT_BT;
    if ((music_output_t)atomic_load(&s_output) == out) return;
    atomic_store(&s_output, (int)out);
    // 落盘：下次开机沿用。/ Persist so the next boot keeps the choice.
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        if (nvs_set_u8(h, NVS_KEY_OUT, (uint8_t)out) == ESP_OK) (void)nvs_commit(h);
        nvs_close(h);
    }
    bt_trace("output -> %s", out == MUSIC_OUT_SPK ? "speaker" : "bt");
    // 两种输出都要模组上电（本机扬声器靠它供 I2S 时钟）；开关若关着就顺手打开，与设置页一致。
    // Both routes need the module powered (the local speaker needs its I2S clock); turn the
    // switch on if it was off so the settings page shows the same state.
    if (!app_settings_bt_enabled()) app_settings_set_bt_enabled(true);
    // 播放器没在跑（传书/USB 那条路把它停了）：只记偏好，别在这里把它拉起来——用户点的是
    // “切输出”，不是“开始播放”，凭空起播会让人以为按钮出错。
    // Player not running (the transfer/USB path stopped it): store the choice only. The user
    // tapped "switch output", not "start playing"; auto-starting looks like a glitch.
    if (!atomic_load(&s_ready)) {
        bump();
        return;
    }
    const bool spk = out == MUSIC_OUT_SPK;
    esp_err_t err = spk ? bt_audio_init_local(pcm_pull) : bt_audio_init(pcm_pull);
    if (err != ESP_OK)
        ESP_LOGW(TAG, "output init (%s): %s", spk ? "spk" : "bt", esp_err_to_name(err));
    bt_audio_set_volume((uint8_t)atomic_load(&s_vol));
    // 从扬声器切到耳机 = 换一条完全不同的通路，按「重连」办：模组侧走干净序列（由 bt_host
    // 在 mode 2 分支里自己完成），这里补上「连上次那副耳机」这一步——只切模式不发 AT+CONNECT
    // 时，耳机停在旧会话上，界面上显示已切到蓝牙却一直没连上。
    // Switching speaker -> headset changes the whole route, so treat it as a relink: bt_host
    // replays the clean mode sequence itself, and this adds the missing "connect the last
    // headset" step. Without it the module enters mode 2 but never sends AT+CONNECT, so the UI
    // says "headset" while nothing connects.
    if (!spk) {
        bt_audio_restart_module();
        connect_last_device("switch to headset");
    }
    bump();
}

void music_player_stop(void) {    if (!atomic_load(&s_ready)) return;
    atomic_store(&s_suspend_no_resume, false);  // 完全停播：解除睡眠挂起的不续播约束
    atomic_store(&s_media_resume, false);       // 停播是有意的，别再自动接回来
    atomic_store(&s_task_exit, true);           // 让解码任务退出，把 12 KB 栈还给系统
    atomic_store(&s_req, REQ_STOP);
    bt_audio_deinit();
    // 任务退出会释放 12 KB 内部栈：USB/传书起射频前这块内存正是缺的余量。
    // The task exits and returns its 12 KB internal stack — the margin the radio start lacks.
    atomic_store(&s_ready, false);
    for (int i = 0; i < 100 && s_task_created; ++i) vTaskDelay(pdMS_TO_TICKS(10));
}

const char *music_root(void) { return s_root; }

void music_set_root(const char *path) {
    if (!path || !path[0]) return;
    strlcpy(s_root, path, sizeof(s_root));
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        if (nvs_set_str(h, NVS_KEY_ROOT, s_root) == ESP_OK) (void)nvs_commit(h);
        nvs_close(h);
    }
    bump();
}

int music_find_path(const char *path) {
    if (!path || !s_tracks) return -1;
    for (int i = 0; i < s_count; ++i)
        if (!strcmp(s_tracks[i].path, path)) return i;
    return -1;
}

// 丢卡闩锁自愈：能打开曲库根目录就说明卡在位，顺手把闩锁清掉。
// 唤醒链路里 app_loop 会在「卡还没恢复」时按丢卡通知一次（为了关掉旧 VFS 句柄），
// 那一次会把闩锁落下；若随后的 music_scan 恰好撞在重挂中间，闩锁就永久留着。
// 后果不是「没声音」而是「点什么都没反应」：music_play_index 见到闩锁直接静默返回。
// Latch self-heal: opening the library root proves the card is back. The wake path notifies a
// card loss once (to close stale VFS handles) and that latches "media lost"; if the following
// music_scan lands in the middle of a remount the latch sticks forever. The symptom is not
// silence but a dead play button: music_play_index returned silently whenever it saw the latch.
static bool music_media_probe(void) {
    DIR *probe = opendir(music_root());
    if (!probe) return false;
    closedir(probe);
    if (atomic_load(&s_media_lost)) {
        atomic_store(&s_media_lost, false);
        bt_trace("MUSIC media latch cleared (probe ok)");
        bump();
    }
    return true;
}

int music_play_file(const char *path) {
    if (!path || !path[0] || !ext_wanted(path)) return -1;
    if (music_player_start() != ESP_OK) return -1;
    strlcpy(s_single_path, path, sizeof(s_single_path));
    atomic_store(&s_req, REQ_SINGLE);
    return 0;
}

int music_scan(void) {
    if (music_player_start() != ESP_OK) return s_count;
    // 扫得动根目录就说明卡在位：清掉丢卡闩锁（USB 占用期间目录打不开，闩锁保留）。
    // Opening the root means the card is back: clear the media-lost latch (while USB owns
    // the card the directory does not open, so the latch stays).
    (void)music_media_probe();
    music_track_t *fresh = heap_caps_malloc(sizeof(music_track_t) * MUSIC_MAX_TRACKS,
                                            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!fresh) fresh = malloc(sizeof(music_track_t) * MUSIC_MAX_TRACKS);
    if (!fresh) return s_count;
    memset(fresh, 0, sizeof(music_track_t) * MUSIC_MAX_TRACKS);

    int count = 0;
    scan_dir(music_root(), 0, fresh, &count);

    // 收藏一次读全：单句柄批量查，翻列表时不再碰 NVS。
    // Load every favorite in one handle pass so listing never touches NVS afterwards.
    memset(s_fav, 0, sizeof(s_fav));
    nvs_handle_t fav_h;
    if (nvs_open(NVS_NS, NVS_READONLY, &fav_h) == ESP_OK) {
        for (int i = 0; i < count; ++i) {
            char key[13];
            uint8_t value = 0;
            fav_key(fresh[i].path, key);
            if (nvs_get_u8(fav_h, key, &value) == ESP_OK && value == 1) s_fav[i] = 1;
        }
        nvs_close(fav_h);
    }

    // 播放中重扫：按路径把当前曲目映射到新表，别让标题指错。
    // Rescan while playing: remap the current file by path so the title cannot point elsewhere.
    char playing_path[240] = {0};
    const int cur = atomic_load(&s_index);
    const int st = atomic_load(&s_state);
    if (s_tracks && cur >= 0 && cur < s_count &&
        (st == MUSIC_PLAYING || st == MUSIC_PAUSED || st == MUSIC_LOADING)) {
        strlcpy(playing_path, s_tracks[cur].path, sizeof(playing_path));
    }

    music_track_t *old = s_tracks;
    s_tracks = fresh;
    s_count = count;
    bump();

    int remap = -1;
    if (playing_path[0]) {
        for (int i = 0; i < count; ++i)
            if (strcmp(s_tracks[i].path, playing_path) == 0) {
                remap = i;
                break;
            }
    }
    if (remap >= 0) {
        atomic_store(&s_index, remap);
    } else if (playing_path[0]) {
        atomic_store(&s_index, -1);  // 曲目被删还在播：继续放，结束后回列表头 / deleted while
                                      // playing: keep playing, restart from the top afterwards
    } else if (atomic_load(&s_index) < 0 && s_last_path[0]) {
        for (int i = 0; i < count; ++i)
            if (strcmp(s_tracks[i].path, s_last_path) == 0) {
                atomic_store(&s_index, i);
                break;
            }
    }
    // 开机续播：本会话仅试一次；上次在播且卡在位时，从恢复出的曲目接着放（位置由 open_track 续）。
    // Boot resume: try once per session; if it was playing last time and the card is present,
    // keep playing from the restored track (position resumed by open_track).
    if (!s_boot_resume_done) {
        s_boot_resume_done = true;
        if (s_resume_was_playing && !atomic_load(&s_media_lost)) {
            const int cur = atomic_load(&s_index);
            if (cur >= 0 && atomic_load(&s_state) == MUSIC_IDLE) music_play_index(cur);
        }
    }
    if (old) free(old);
    ESP_LOGI(TAG, "scan: %d track(s)", count);
    return count;
}

int music_track_count(void) { return s_count; }

const music_track_t *music_track(int index) {
    if (!s_tracks || index < 0 || index >= s_count) return NULL;
    return &s_tracks[index];
}

void music_play_index(int index) {
    // 每一条拒绝路径都留痕：以前这里全是静默 return，实机上就是「点播放没反应」，
    // 日志里连一行都没有，根本分不清是没收到点击还是被状态挡住了。
    // Every refusal is traced now: these used to return silently, which on device is exactly
    // "the play button does nothing" with not a single log line to tell why.
    if (!atomic_load(&s_ready)) {
        bt_trace("MUSIC play refused: player not ready");
        return;
    }
    // 开机蓝牙一律是关的：这时点播放等于「我要听」，顺手把开关打开（耳机路上电并自动回连；
    // 扬声器路也要靠模组供 I2S 时钟），否则按钮看起来毫无反应。
    // Bluetooth always starts off at boot: a play tap means "I want to listen", so turn the switch
    // on (headset route powers up and reconnects; the speaker route needs the module's I2S clock
    // too). Without this the button just looks dead.
    if (!app_settings_bt_enabled()) {
        ESP_LOGW(TAG, "play with Bluetooth off: turning the switch on");
        bt_trace("MUSIC play: BT off -> enabling");
        music_bt_set_enabled(true);
    }
    // 用户点播放是明确的「我要听」：先探一次卡，通了就当场清掉丢卡闩锁。
    // A play tap is an explicit intent: probe the card once and drop the lost latch right here.
    if (atomic_load(&s_media_lost) && !music_media_probe()) {
        bt_trace("MUSIC play refused: media lost (no card)");
        return;
    }
    if (index < 0 || index >= s_count) {
        if (s_count <= 0) {
            bt_trace("MUSIC play refused: empty library, rescan");
            music_scan();  // 曲库没扫过就现扫一遍，别让用户对着空列表点 / scan on demand
            if (s_count <= 0) {
                bt_trace("MUSIC play refused: still empty");
                return;
            }
        }
        bt_trace("MUSIC play index %d out of range (count=%d)", index, s_count);
        index = 0;
    }
    atomic_store(&s_auto_paused, false);
    atomic_store(&s_suspend_no_resume, false);  // 用户显式播放：解除睡眠挂起的不续播约束
    atomic_store(&s_media_resume, false);       // 用户自己动了，丢卡自愈不必再补一手
    atomic_store(&s_index, index);
    set_state(MUSIC_LOADING);
    atomic_store(&s_req, index);
}

void music_toggle(void) {
    // 同上：蓝牙关着时点播放，先把开关打开再走正常流程。
    // Same as above: with Bluetooth off, a play tap opens the switch first.
    if (!app_settings_bt_enabled()) {
        ESP_LOGW(TAG, "toggle with Bluetooth off: turning the switch on");
        bt_trace("MUSIC toggle: BT off -> enabling");
        music_bt_set_enabled(true);
    }
    const int st = atomic_load(&s_state);
    ESP_LOGI(TAG, "toggle st=%d idx=%d count=%d ready=%d out=%s blocked=%d", st,
             atomic_load(&s_index), s_count, (int)atomic_load(&s_ready),
             music_output() == MUSIC_OUT_SPK ? "spk" : "bt", (int)play_blocked());
    bt_trace("MUSIC toggle st=%d idx=%d count=%d lost=%d ready=%d", st, atomic_load(&s_index),
             s_count, (int)atomic_load(&s_media_lost), (int)atomic_load(&s_ready));
    if (!atomic_load(&s_ready)) return;
    if (st == MUSIC_PLAYING) {
        time_freeze();
        atomic_store(&s_auto_paused, false);  // 手动暂停不自动续 / a manual pause stays paused
        set_state(MUSIC_PAUSED);
    } else if (st == MUSIC_PAUSED) {
        time_resume();
        atomic_store(&s_suspend_no_resume, false);  // 用户手动续播：解除睡眠挂起的不续播约束
        set_state(MUSIC_PLAYING);  // 仍忙则 eval_auto 立刻再停 / eval_auto re-pauses if still blocked
    } else {
        const int idx = atomic_load(&s_index);
        music_play_index(idx >= 0 ? idx : 0);
    }
}

void music_next(void) {
    const int count = s_count;
    if (!count) return;
    int cur = atomic_load(&s_index);
    // 单曲直放时按下一首 = 回到曲库从头播。/ Next from a direct file starts the library over.
    if (atomic_load(&s_single)) cur = -1;
    music_play_index((cur < 0 ? 0 : (cur + 1) % count));
}

void music_prev(void) {
    const int count = s_count;
    if (!count) return;
    int cur = atomic_load(&s_index);
    // 单曲直放时按上一首 = 从曲库末尾倒着进。/ Prev from a direct file enters at the library's end.
    if (atomic_load(&s_single)) cur = 0;
    music_play_index(cur <= 0 ? count - 1 : cur - 1);
}

music_state_t music_state(void) { return (music_state_t)atomic_load(&s_state); }

const char *music_error(void) {
    return s_error;  // strlcpy 单写者；显示方立刻取用即可 / single writer via strlcpy; readers copy at once
}

const char *music_title(void) {
    if (atomic_load(&s_single)) return s_single_title;
    const int i = atomic_load(&s_index);
    if (!s_tracks || i < 0 || i >= s_count) return "";
    return s_tracks[i].title;
}

const char *music_artist(void) {
    if (atomic_load(&s_single)) return s_single_artist;
    const int i = atomic_load(&s_index);
    if (!s_tracks || i < 0 || i >= s_count) return "";
    return s_tracks[i].artist;
}

uint32_t music_elapsed_ms(void) { return time_elapsed(); }

uint32_t music_duration_ms(void) {
    if (atomic_load(&s_single)) return s_single_dur;
    const int i = atomic_load(&s_index);
    if (!s_tracks || i < 0 || i >= s_count) return 0;
    return s_tracks[i].duration_ms;
}

int music_track_index(void) { return atomic_load(&s_index); }

uint32_t music_serial(void) { return atomic_load(&s_serial); }

uint8_t music_volume(void) { return (uint8_t)atomic_load(&s_vol); }

void music_set_volume(uint8_t vol) {
    if (vol > 127) vol = 127;
    atomic_store(&s_vol, vol);
    bt_audio_set_volume(vol);
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u8(h, NVS_KEY_VOL, vol);
        nvs_commit(h);
        nvs_close(h);
    }
    bump();
}

void music_seek(uint32_t ms) {
    if (!atomic_load(&s_ready)) return;
    atomic_store(&s_seek_req, (int32_t)ms);  // 执行在解码任务，越界由 seek_begin 钳制
                                             // the decode task runs it; seek_begin clamps the range
}

bool music_favorite(int index) {
    if (index < 0 || index >= s_count) return false;
    return s_fav[index] != 0;
}

void music_favorite_toggle(int index) {
    if (index < 0 || index >= s_count || !s_tracks) return;
    const uint8_t value = s_fav[index] ? 0 : 1;
    s_fav[index] = value;
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        char key[13];
        fav_key(s_tracks[index].path, key);
        esp_err_t err = value ? nvs_set_u8(h, key, 1) : nvs_erase_key(h, key);
        if (err == ESP_ERR_NVS_NOT_FOUND && !value) err = ESP_OK;
        if (err == ESP_OK) err = nvs_commit(h);
        nvs_close(h);
    }
    bump();
}

int music_favorite_count(void) {
    int n = 0;
    for (int i = 0; i < s_count; ++i) if (s_fav[i]) ++n;
    return n;
}

music_mode_t music_mode(void) { return (music_mode_t)atomic_load(&s_mode); }

void music_set_mode(music_mode_t mode) {
    if ((int)mode < 0 || (int)mode > MUSIC_MODE_FAV) mode = MUSIC_MODE_LIST;
    atomic_store(&s_mode, (int)mode);
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        if (nvs_set_u8(h, NVS_KEY_MODE, (uint8_t)mode) == ESP_OK) (void)nvs_commit(h);
        nvs_close(h);
    }
    bump();
}

void music_cycle_mode(void) {
    // 列表循环 → 单曲循环 → 随机播放 → 收藏循环 → 列表循环；没有收藏时跳过收藏循环。
    // List → single → shuffle → favorites → list; the favorites mode is skipped with no favorites.
    int mode = ((int)music_mode() + 1) % (MUSIC_MODE_FAV + 1);
    if (mode == MUSIC_MODE_FAV && music_favorite_count() == 0) mode = MUSIC_MODE_LIST;
    music_set_mode((music_mode_t)mode);
}

const char *music_mode_label(void) {
    switch (music_mode()) {
    case MUSIC_MODE_ONE:     return "单曲循环";
    case MUSIC_MODE_SHUFFLE: return "随机播放";
    case MUSIC_MODE_FAV:     return "收藏循环";
    case MUSIC_MODE_LIST:
    default:                 return "列表循环";
    }
}

void music_media_lost(void) {
    // 记下「丢卡那一刻在不在播」与时刻：唤醒链路的假丢卡要能自己长回来。
    // Remember whether we were playing and when, so a false loss at wake can heal itself.
    bt_trace("MUSIC media lost (state=%d)", (int)atomic_load(&s_state));
    atomic_store(&s_media_resume, atomic_load(&s_state) != MUSIC_IDLE);
    s_media_lost_us = esp_timer_get_time();
    atomic_store(&s_media_lost, true);
    time_freeze();
    atomic_store(&s_auto_paused, false);
    atomic_store(&s_req, REQ_STOP);  // 任务里关文件，避免与解码任务并发 fclose / close in the task
    if (!s_task_created) set_state(MUSIC_IDLE);
}

void music_set_radio_busy(bool busy) {
    if (atomic_exchange(&s_radio_busy, busy) != busy) bump();
}

void music_set_media_busy(bool busy) {
    if (atomic_exchange(&s_media_busy, busy) != busy) bump();
}

void music_set_suspended(bool suspended) {
    atomic_store(&s_suspended, suspended);
    bt_trace("MUSIC suspend=%d state=%d no_resume=%d", (int)suspended, (int)atomic_load(&s_state),
             (int)atomic_load(&s_suspend_no_resume));
    if (suspended) {
        // 顺序契约（sleep.c）：ble_pt_suspend 成功后调用；resume 在 ble_pt_resume 之前。
        // Ordering contract (sleep.c): called after a successful ble_pt_suspend; the resume
        // runs before ble_pt_resume, so the stack is back up when the page turner returns.
        if (atomic_load(&s_state) == MUSIC_PLAYING) {
            time_freeze();
            atomic_store(&s_auto_paused, true);
            set_state(MUSIC_PAUSED);
        }
        bt_host_suspend_for_sleep();
    } else {
        bt_host_resume_after_sleep();  // 续播由 eval_auto 判定 / eval_auto decides the resume
    }
    bump();
}

void music_set_suspended_for_sleep(void) {
    // 睡眠/锁屏入口：与 music_set_suspended(true) 相同的栈生命周期语义（先 ble_pt 拆、再 music 收栈），
    // 但额外标记「不自动续播」，使唤醒后 eval_auto 不再后台恢复播放。
    // Same stack-lifecycle semantics as music_set_suspended(true) but additionally marks
    // "no auto-resume" so eval_auto won't restore playback in the background after wake.
    atomic_store(&s_suspend_no_resume, true);
    music_set_suspended(true);
}

void music_allow_resume_after_unlock(void) {
    // 解锁是用户的明确动作：把锁屏时置下的「不续播」解除，让 eval_auto 在链路回来后
    // 自动把 PAUSED 推回 PLAYING。实机日志里这一段是「蓝牙已连、FEED got=0 pk=0」连续
    // 八秒以上——不是链路问题，是播放器根本没被允许续播。
    // An unlock is an explicit user action: drop the "no resume" latch so eval_auto can push
    // PAUSED back to PLAYING once the link returns. The field log shows 8+ seconds of
    // "link up, FEED got=0 pk=0" — the link is fine, the player simply was not allowed to resume.
    if (!atomic_load(&s_suspend_no_resume)) return;
    atomic_store(&s_suspend_no_resume, false);
    bt_trace("MUSIC allow resume (unlock)");
    bump();
}
