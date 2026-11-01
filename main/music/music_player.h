/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * 中文：TF 卡音乐播放器（/sdcard/music）。扫描曲库、Simple Decoder 解码、重采样
 * 到 44.1k/2ch/s16 后入 PCM 环，由 A2DP 拉流回调取走；UI 只通过本头文件的
 * API 与它交互，不碰内部状态。
 *
 * English: TF-card music player (/sdcard/music). Scans the library, decodes with
 * the Simple Decoder, resamples to 44.1k/2ch/s16 into a PCM ring that the A2DP
 * pull callback drains; the UI talks to it only through this header.
 *
 * 冻结：不绘制、不管理蓝牙连接（连接在播放页做）、不做网络。
 * Frozen: no drawing, no Bluetooth connection management (the player page owns
 * that), no networking.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MUSIC_MAX_TRACKS 200
#define MUSIC_ROOT "/sdcard/music"

typedef enum {
    MUSIC_IDLE = 0,   // 未选曲 / nothing selected
    MUSIC_LOADING,    // 打开文件/解码器中 / opening file or decoder
    MUSIC_PLAYING,
    MUSIC_PAUSED,
    MUSIC_ERROR,      // music_error() 给原因 / see music_error() for the cause
} music_state_t;

typedef struct {
    char path[240];
    char title[96];
    char artist[64];
    uint32_t duration_ms;  // 0 = 未知（进度条只画时间）/ unknown (progress shows time only)
} music_track_t;

// 一次性初始化：注册解码器、建解码任务、接 A2DP 拉流、恢复上次曲目。幂等。
// One-time setup: register decoders, create the decode task, attach the A2DP pull
// callback, restore the last track. Idempotent.
esp_err_t music_player_start(void);
// 暂停并释放全部资源（含蓝牙宿主引用）。一般只在关机路径用。
// Pause and release everything (including the Bluetooth host ref); shutdown path only.
void music_player_stop(void);

// 曲库根目录（默认 MUSIC_ROOT，可在文件管理里改）。/ Library root (defaults to MUSIC_ROOT, editable from the file manager).
const char *music_root(void);
void music_set_root(const char *path);

// 重扫曲库根目录（深度 2、≤200 首），返回曲目数。/ Rescan the library root (depth 2, ≤200 tracks); returns the count.
int music_scan(void);
int music_track_count(void);
// 越界返回 NULL；曲库在 PSRAM，扫描期间索引不移动。/ NULL when out of range; the library lives in PSRAM and is not reallocated during reads.
const music_track_t *music_track(int index);

void music_play_index(int index);  // 越界忽略 / ignored when out of range
// 按完整路径找曲库下标，找不到返回 -1（文件管理用它判断走曲库还是单曲直放）。
// Library index for a full path, -1 when absent (the file manager uses it to choose library vs direct play).
int music_find_path(const char *path);
// 直接播一个曲库外的音频文件（文件管理点开）：不进曲库、播完即停。返回 0 表示已受理。
// Play an audio file outside the library (opened from the file manager): it never joins the
// library and stops when it ends. Returns 0 when accepted.
int music_play_file(const char *path);
void music_toggle(void);           // 播放 ⇄ 暂停 / play ⇄ pause
void music_next(void);
void music_prev(void);

music_state_t music_state(void);
const char *music_error(void);
const char *music_title(void);   // 当前曲目；无曲目时 "" / current track; "" when none
const char *music_artist(void);
uint32_t music_elapsed_ms(void);
uint32_t music_duration_ms(void);
int music_track_index(void);
// 变化计数：任何可感知的状态变化 +1；UI 用它做节流刷新。
// Change counter: bumped on any observable change; UI throttles redraws with it.
uint32_t music_serial(void);

uint8_t music_volume(void);      // 0..127，NVS rp_music:vol / persisted
void music_set_volume(uint8_t vol);

/// 输出通路：蓝牙耳机 / 本机扬声器（模组模式 1 + NS4150 功放）。
/// Output route: Bluetooth headset / local speaker (module mode 1 + NS4150 PA).
typedef enum {
    MUSIC_OUT_BT = 0,   // 模式 2：I2S PCM 经模组发射到已配对耳机 / mode 2, PCM to the headset
    MUSIC_OUT_SPK = 1,  // 模式 1：PCM 直接走本机喇叭 / mode 1, PCM straight to the speaker
} music_output_t;

/// 当前输出通路（NVS rp_music:out，缺省蓝牙耳机）。/ Current route (persisted; defaults to headset).
music_output_t music_output(void);
/// 切换输出通路并立即生效；模组会在蓝牙耳机与本机喇叭之间重走一遍模式序列（数秒）。
/// Switch the route and apply it now; the module replays its mode sequence between the headset
/// and the local speaker (a few seconds).
void music_set_output(music_output_t out);

/// 手动重连：把蓝牙模组整条断电重启（等于亲手关开一次蓝牙开关），当前输出通路保持不变。
/// 现场里「显示连着、PCM 在送、耳机却全静音」是模组自己的状态机卡死，软重发 AT 救不回来；
/// 播放页的「重连」按钮就是这条兜底。接口未起来时什么也不做。
/// Manual relink: power-cycle the Bluetooth module end to end (the same as toggling its switch by
/// hand) while keeping the current route. The "linked, PCM flowing, headset silent" field failure
/// is the module's own wedged state machine, which re-sending AT cannot clear; the player's
/// Relink button is this fallback. A no-op while the audio path is not up.
void music_restart_output(void);

/// 进度跳转：绝对毫秒（进度条点按）。无曲目或无时长时忽略。
/// Absolute seek in ms (progress-bar tap). Ignored with no track or no known duration.
void music_seek(uint32_t ms);

/// 收藏：按曲目完整路径持久化，重扫后仍跟随文件。/ Favorite: persisted by full track path; survives rescans as long as the file does.
bool music_favorite(int index);
/// 切换收藏并落盘。/ Toggle the favorite flag and persist it.
void music_favorite_toggle(int index);
/// 当前曲库里的收藏数。/ How many tracks in the current library are favorites.
int music_favorite_count(void);

/// 播放模式：列表循环 / 单曲循环 / 随机播放 / 收藏循环。
/// Play mode: list loop / single repeat / shuffle / favorites loop.
typedef enum {
    MUSIC_MODE_LIST = 0,
    MUSIC_MODE_ONE,
    MUSIC_MODE_SHUFFLE,
    MUSIC_MODE_FAV,
} music_mode_t;

/// 当前模式（NVS rp_music:mode，缺省列表循环）。/ Current mode (persisted; defaults to list loop).
music_mode_t music_mode(void);
/// 直接设置并落盘。/ Set a mode and persist it.
void music_set_mode(music_mode_t mode);
/// 顺序切换到下一个模式；没有收藏时跳过「收藏循环」。
/// Cycle to the next mode; skips the favorites loop when nothing is favorited.
void music_cycle_mode(void);
/// 模式的中文短标签，UI 直接显示。/ Short Chinese label for the UI.
const char *music_mode_label(void);

/// 蓝牙音频开关：开 = 模组重新上电并送音量，关 = 停搜索、拆模组通路（播放随之自动
/// 暂停）。选择同时落盘；连接/断开目标由播放页负责。
/// Bluetooth audio switch: on powers the module back up with its volume, off stops scanning
/// and tears the path down (playback then auto-pauses). The choice persists; the play page
/// owns the connect/disconnect target.
void music_bt_set_enabled(bool enabled);

// TF 卡被移除：关文件、停播、状态回 IDLE。/ Card removed: close the file, stop, back to IDLE.
void music_media_lost(void);
// 射频忙（传输/在线内容）与 USB 占用 TF 卡：忙则自动暂停，闲了自动续。
// Radio busy (transfer/online work) and USB storage owning the TF card: auto-pause
// while busy, auto-resume when clear.
void music_set_radio_busy(bool busy);
void music_set_media_busy(bool busy);
// 休眠前后：暂停并挂起/恢复蓝牙宿主（在 ble_pt_suspend 成功后、resume 之前调用）。
// Around sleep: pause and suspend/resume the Bluetooth host (call after a successful
// ble_pt_suspend, before ble_pt_resume).
void music_set_suspended(bool suspended);

/// 睡眠/锁屏入口专用：语义同 music_set_suspended(true)，但标记「唤醒后不自动续播」。
/// 仅 sleep.c 的锁屏/主睡眠路径调用；传输/下载的瞬态忙仍用 music_set_suspended 以保持自动续播。
/// Sleep/lock entry only: same as music_set_suspended(true) but marks "no auto-resume on wake".
/// Only the lock/host-sleep paths in sleep.c call this; transient busy (transfer/download)
/// keeps using music_set_suspended so auto-resume still works.
void music_set_suspended_for_sleep(void);

/// 用户真正解锁后解除「不自动续播」约束：锁屏时置下的标记在这里清掉，否则唤醒后播放器
/// 会一直停在暂停态——蓝牙连上了、PCM 却一直是静音，用户看到的就是「连上了但没声音」。
/// 只在真人解锁（按键/触摸唤醒并完成解锁）后调用；误触唤醒、定时唤醒不要调。
/// Clears the "no auto-resume" latch set on lock after a real user unlock. Without this the
/// player stays paused after wake: the link comes up while PCM keeps carrying silence, which
/// the user sees as "connected but no sound". Call it only after a genuine unlock, not on an
/// accidental or scheduled wake.
void music_allow_resume_after_unlock(void);

// 环已放空（无待播帧）。/ The ring has fully drained (no queued frames).
bool music_ring_drained(void);

#ifdef __cplusplus
}
#endif
