/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * 中文：蓝牙宿主。翻页器（BLE HID 主机）走片内 Bluedroid BLE-only，本组件仍是唯一的
 * `esp_bt_controller` 生命周期持有者，引用计数供翻页器等消费者共用；音乐与听书音频走
 * 板载蓝牙音频模组（UART AT 选模式/配对 + I2S 从机送 PCM），不再使用片内经典蓝牙。
 *
 * English: the Bluetooth host. The page turner (BLE HID host) runs on the on-chip Bluedroid
 * in BLE-only mode and this component still owns the sole `esp_bt_controller` lifecycle, its
 * refcount shared by consumers; music and TTS audio go through the board's Bluetooth audio
 * module (AT over UART for mode/pairing, I2S slave for PCM) instead of classic BR/EDR.
 *
 * 冻结：不绘制、不做播放语义、不直接读写音乐文件。
 * Frozen: no drawing, no playback policy, no direct music-file access.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define BT_AUDIO_DEV_MAX 8
#define BT_AUDIO_NAME_MAX 32

typedef struct {
    uint8_t bda[6];
    char name[BT_AUDIO_NAME_MAX];
    int8_t rssi;
    bool audio;  // 设备类别/服务位表明这是音频设备 / class-of-device says it is audio
} bt_audio_device_t;

/* ---- 宿主生命周期（引用计数）/ Host lifecycle (refcounted) ---- */

esp_err_t bt_host_start(void);
esp_err_t bt_host_stop(void);
bool bt_host_running(void);
uint32_t bt_host_users(void);

/* 休眠：暂停射频并在唤醒后恢复；对翻页器与音频都生效。
 * Sleep: suspend the radio and restore it after wake; applies to both consumers. */
void bt_host_suspend_for_sleep(void);
void bt_host_resume_after_sleep(void);
bool bt_host_suspended(void);

/* ---- 诊断埋点 / Diagnostics hook ---- */

// 注册后，组件把关键事件（AT 收发、模式切换、链路、喂帧统计、挂起/恢复）逐行交给回调；
// 不注册时只有一次空指针判断。回调可能在任何任务上下文被调用，必须自己排队、不能阻塞，
// 更不能在里面直接写卡——写卡由调用方自己的低优先级任务完成。
// Once registered, the component hands key events (AT traffic, mode switches, link state, feed
// stats, suspend/resume) to the callback line by line; with no callback the cost is one NULL
// check. The callback may run in any task context, so it must queue and never block — never
// write the card from inside it; that belongs to the caller's own low-priority task.
typedef void (*bt_host_trace_fn)(const char *line);
void bt_host_set_trace(bt_host_trace_fn fn);

/* ---- 音频（板载模组）/ Audio (board module) ---- */

// PCM 拉流回调：把最多 want 字节的 16kHz/2ch/16bit 小端 PCM 写进 dst，
// 返回实际写入的字节数。欠载时由调用方补静音。
// PCM pull callback: write at most `want` bytes of 16kHz/2ch/16-bit little-endian PCM into
// dst and return how many bytes were written. The caller pads silence when short.
typedef int32_t (*bt_pcm_pull_cb_t)(uint8_t *dst, int32_t want);

// 启动音频通路：上电模组、切到模式 2（发射/配对）、建 I2S 从机与喂帧任务。重复调用无副作用。
// Start the audio path: power the module, switch to mode 2 (transmit/pair), bring up the I2S
// slave and the feed task. Idempotent.
esp_err_t bt_audio_init(bt_pcm_pull_cb_t pull);
// 本机扬声器输出（不连蓝牙）：模组进模式 1（由它给 I2S 供时钟），打开 NS4150 功放，
// PCM 直接送到喇叭。与耳机播放互斥；切换时内部会重走一遍模式序列（数秒）。
// Local speaker output (no Bluetooth): the module runs mode 1 (it clocks I2S itself), the
// NS4150 PA is enabled and PCM goes straight to the speaker. Mutually exclusive with headset
// playback; switching replays the mode sequence internally (a few seconds).
esp_err_t bt_audio_init_local(bt_pcm_pull_cb_t pull);
// 手动兜底（播放页「重连」）：**不断电**，重走一遍「关 → 开蓝牙开关」的干净序列——
// 回模式 1 并等应答 → 拆后端 → 重建 → 进模式 2 → 补 AT+CONNECT。断电会让模组冷启动后
// 抢先自连，我们的 AT+MODE=2 插进它建连到一半的过程，两轮连接叠加，1.4 s 后自己掉，
// 表现就是「显示已连接却没声音」，所以这条路径刻意不碰 P14。
// Manual fallback (the player's Relink button), with **no power cut**: replay the clean
// "switch off -> on" sequence (leave mode 1 with an ack, rebuild, enter mode 2, reconnect). A
// power cut makes the module cold-boot and relink on its own while our AT+MODE=2 lands
// mid-handshake, the two links overlap and die 1.4 s later — "connected but silent".
void bt_audio_restart_module(void);
// 重连是否正在进行：整条序列要几秒，界面据此显示「重连中」并丢掉多余的按键。
// Whether a relink is in flight: the sequence takes seconds; the UI shows "relinking" and drops
// extra presses while it runs.
bool bt_audio_relinking(void);
void bt_audio_deinit(void);
// 「断链即关闭蓝牙」策略生效时回调：由上层把设置页开关翻成 off，让开关显示与真实状态一致。
// 用户重新打开开关（bt_audio_init）或按播放页「重连」即可恢复。
// Fired when the "a drop disables Bluetooth" policy turns the radio off, so the UI switch can be
// flipped off to match the real state. Re-enabling it (bt_audio_init) or pressing Relink recovers.
void bt_audio_set_auto_disabled_cb(void (*cb)(void));
// 等后端真拆完（UART/I2S/任务全部退出）；超时返回 false。
// Wait until the backend is really released (UART, I2S and both tasks gone); false on timeout.
bool bt_audio_wait_released(uint32_t timeout_ms);
bool bt_audio_ready(void);

// 搜索耳机。模组尚未就绪时先把请求挂起，进模式 2 后自动补发。
// Scan for headsets. While the module is not ready the request is queued and sent once it is.
esp_err_t bt_audio_scan_start(uint32_t ms);
void bt_audio_scan_stop(void);
bool bt_audio_scanning(void);
uint8_t bt_audio_device_count(void);
const bt_audio_device_t *bt_audio_device(uint8_t index);

// 连接耳机；未就绪时同样挂起，就绪后自动发 AT+CONNECT。
// Connect a headset; while not ready the request is queued and sent once it is.
esp_err_t bt_audio_connect(const uint8_t bda[6]);
void bt_audio_disconnect(void);
bool bt_audio_link_active(void);
/// 当前连接目标地址；未连接返回 false，bda 不变。/ Address of the active link; false when unlinked, bda untouched.
bool bt_audio_link_addr(uint8_t bda[6]);
const char *bt_audio_name(void);

// 音频是否真的能出声：链路在 + A2DP 已建立 + 模组确认切离 SCO（收到 `DISC SCO`）。
// `bt_audio_link_active()` 只是「模组说它连上了」——那是模组单方面的上报，没有任何超时或
// 心跳，模组不说话时它会一直保持为真。真正「能出声」的唯一外在证据是模组自己上报的
// `DISC SCO`：rc111 实机日志里每一次有声都有它，没声的那一段没有。界面用它区分
// 「已连接」与「已连接但没声音」，排障时不必再抓日志猜测。
// Whether audio can actually be heard: the link is up, A2DP is established, and the module
// confirmed it left SCO (`DISC SCO`). `bt_audio_link_active()` only means "the module says it
// is connected" — a unilateral report with no timeout or heartbeat, so it stays true while the
// module says nothing. The only outward proof of real audio is the module's own `DISC SCO`:
// every audible segment in the rc111 field log has it, the silent one does not. The UI uses it
// to tell "connected" apart from "connected but silent" without digging through the SD log.
bool bt_audio_audio_ready(void);

// 当前选定的输出通路能不能出声：本机扬声器看 mode 1 是否就绪（这条路根本没有耳机链路），
// 耳机看蓝牙链路。播放器用它判断「该不该继续播」——用 bt_audio_link_active() 会在扬声器
// 模式下恒为假，用户的每一次播放都被当成「链路不在」立刻自动暂停（现场：FEED got=0 pk=0）。
// Whether the selected route can produce sound: the local speaker only needs mode 1 up (that
// route has no headset link at all), the headset needs its link. The player gates playback on
// this; using bt_audio_link_active() there is always false on the speaker route, so every play
// attempt is auto-paused as "unlinked" (field: FEED got=0 pk=0).
bool bt_audio_output_ready(void);

// 发声测试：绕过播放器与解码，直接往同一条 I2S 送 `ms` 毫秒 1 kHz 正弦（不乘软件音量）。
// 出声 → 模组 / 链路 / I2S 这一侧没问题，问题在播放器与解码；不出声 → 问题在这一侧。
// 这是把「深睡正常、浅睡没声」切成两半的最短路径：深睡让播放器整机重来，浅睡只让它暂停
// 再续播，两者共用同一条蓝牙通路，所以只有绕开播放器才能判断是谁的问题。
// Test tone: bypass the player and decoder and push a 1 kHz sine into the same I2S for `ms`
// milliseconds (software volume skipped). Audible means the module/link/I2S side is fine and
// the fault is in the player/decoder; silence means the opposite. This is the shortest way to
// split "deep sleep works, light sleep does not": deep sleep restarts the player from scratch
// while light sleep only pauses and resumes it, and both share one Bluetooth path — so only a
// player-free tone can tell which side is broken.
esp_err_t bt_audio_play_test_tone(uint32_t ms);
void bt_audio_test_tone_stop(void);
bool bt_audio_test_tone_active(void);

// 数据面自愈统计（诊断用）：自愈次数与写失败块数。恢复动作在组件内部完成，调用方不必处理。
// Data-path self-heal stats, for diagnostics: heal count and failed feed blocks. Recovery is
// handled inside the component, so callers do not have to react.
uint32_t bt_audio_heal_count(void);
uint32_t bt_audio_tx_error_count(void);

// 软件音量 0..127（模组无绝对音量，按平方曲线在送 PCM 前缩放）；未连接返回 ESP_ERR_NOT_SUPPORTED。
// Software volume 0..127 (the module has no absolute volume, so PCM is scaled on a squared
// curve before the I2S write); ESP_ERR_NOT_SUPPORTED when nothing is linked.
esp_err_t bt_audio_set_volume(uint8_t volume);

/* ---- 本地麦克风（语音翻页）/ Local mic (voice page-turn) ---- */

// 让模组进模式 1（本地双工）并打开 I2S 收通道。与耳机播放互斥：音乐/耳机占用模组时返回
// ESP_ERR_INVALID_STATE，模组不应答返回 ESP_FAIL。内部会等 AT 序列跑完（数秒）。
// Put the module into mode 1 (local duplex) and open the I2S capture path. Mutually exclusive
// with headset playback: ESP_ERR_INVALID_STATE while audio owns the module, ESP_FAIL when the
// module does not answer. Internally waits for the AT sequence, which takes a few seconds.
esp_err_t bt_mic_open(void);

// 关掉麦克风并把 UART/I2S 还给内部堆（模式任务异步拆）。/ Close the mic; the worker releases the backend.
void bt_mic_close(void);
bool bt_mic_ready(void);

// 读单声道麦克风 PCM（16 kHz，s16）。返回帧数，0 = 本次超时没数据，-1 = 通路不可用。
// Read mono mic PCM at 16 kHz, signed 16-bit. Returns frames; 0 on timeout, -1 when the path is gone.
int bt_mic_read(int16_t *dst, int frames, uint32_t timeout_ms);

#ifdef __cplusplus
}
#endif
