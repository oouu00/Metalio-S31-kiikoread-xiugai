/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * 中文：语音翻页引擎。只教两个方向（上一页 / 下一页），词由用户自己定（任意词语均可），
 * 不使用 esp-sr 声学模型：用户说一遍，把 MFCC 特征当模板存进 NVS，运行期用 DTW 比距离。
 *
 * 设计要点（改之前先读）：
 *   - 静音时只算能量门控，不做 FFT / DTW，CPU 占用 <0.1%；
 *   - 麦克风只在调用方要求（书内阅读、设置页的学习/测试）且没有耳机播放时才打开；
 *   - 所有 DSP 缓冲都在 PSRAM，内部 RAM 只剩几 KB 给 Wi-Fi；
 *   - 本模块不绘制、不翻页、不读设置：翻页方向由调用方 take 走后自己决定。
 *
 * English: voice page-turn engine. It teaches only two directions (previous / next) with any
 * user-chosen words, and no esp-sr acoustic model: the user speaks each word once, its MFCC
 * features are stored in NVS as a template, and runtime matching is DTW. Silence costs only
 * an energy gate (no FFT, no DTW); the mic is opened only when the caller asks (the reader,
 * or the settings learn/test screen) and no headset is playing; every DSP buffer lives in
 * PSRAM. No drawing, no page policy, no settings access here.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/// 方向（词由用户自己教，任意词语）。顺序固定参与 NVS 键名，不要重排。
/// Directions (words are user-taught, any phrase). Fixed order; it is part of the NVS keys.
typedef enum {
    VOICE_TURN_WORD_PREV = 0,  ///< 上一页，词由用户教 / previous page, word chosen by the user
    VOICE_TURN_WORD_NEXT = 1,  ///< 下一页，词由用户教 / next page, word chosen by the user
    VOICE_TURN_WORD_COUNT = 2,
} voice_turn_word_t;

/// 学习/监听状态。/ Learn and listen state.
typedef enum {
    VOICE_TURN_LEARN_IDLE = 0,  ///< 没在学 / not learning
    VOICE_TURN_LEARN_WAIT,      ///< 会话已开，等点「开始录音」/ session open, waiting for the record tap
    VOICE_TURN_LEARN_SPOKE,     ///< 收到一段，正在存模板 / an utterance arrived, saving it now
    VOICE_TURN_LEARN_OK,        ///< 学会一份 / a template was stored
    VOICE_TURN_LEARN_FAIL,      ///< 这段太短/太轻，重录 / too short or too quiet, re-record
    VOICE_TURN_LEARN_BUSY,      ///< 麦克风被耳机或音频占用 / mic owned by headset audio
    VOICE_TURN_LEARN_FAIL_SAVE, ///< 模板没写进 NVS（空间不足/IO 错）/ template not written to NVS
    VOICE_TURN_LEARN_RECORD,    ///< 手动录音中（等点「结束录音」）/ manual recording in progress (ends on tap)
    VOICE_TURN_LEARN_FAIL_OPEN, ///< 麦克风连开失败，点按钮重试 / the mic failed to open repeatedly, retry with the button
} voice_turn_learn_state_t;

/// 初始化：载入模板并创建采集任务，但不打开麦克风。可重复调用。
/// Initialise: load templates and create the capture task, without opening the mic. Idempotent.
esp_err_t voice_turn_init(void);

/// 是否初始化成功且引擎可用。/ True when the engine is up.
bool voice_turn_ready(void);

/// 是否至少有一个方向学过模板（只教一个方向也可用）。
/// True when at least one direction has a template; teaching only one direction is enough.
bool voice_turn_learned(void);

/// 某个词已有的模板份数 0..3。/ Template count for one word, 0..3.
uint8_t voice_turn_word_reps(voice_turn_word_t word);

/// 请求开/关麦克风监听。幂等；内部按耳机播放、模块忙碌自行退避重试。
/// Ask for the mic to listen or stop. Idempotent; it backs off by itself while a headset
/// owns the module.
void voice_turn_set_listen(bool on);
bool voice_turn_listening(void);

/// 取走一次识别结果：-1 上一页，+1 下一页，0 没有。取走即清。
/// Take one result: -1 previous, +1 next, 0 nothing. Consumed on read.
int voice_turn_take_direction(void);

/// 打开某个词的学习会话（覆盖上一次未完成的学习）。只开会话，**不会自动开麦**；
/// 录音由 voice_turn_learn_record_start 手动触发。
/// Open a learning session for one word (discarding any unfinished one). Opens the session
/// only and never starts the mic; recording begins via voice_turn_learn_record_start.
esp_err_t voice_turn_learn_begin(voice_turn_word_t word);
/// 开始/结束一次手动录音（须先 learn_begin 打开会话）。结束时按按钮按下时刻定稿。
/// Start/stop one manual recording (open a session with learn_begin first). The stop tap
/// decides where the utterance ends.
esp_err_t voice_turn_learn_record_start(void);
void voice_turn_learn_record_stop(void);
/// 已收帧数；-1 表示麦克风还没开。62.5 帧 = 1 秒。
/// Frames captured so far; -1 when the mic is not open yet. 62.5 frames = one second.
int voice_turn_learn_record_frames(void);
/// 推进学习状态机；界面每次 tick 调一次。
/// **必须在栈位于内部 RAM 的任务里调用**（设置页的 UI 任务即可）：学会的那一份模板在这里
/// 才真正写进 NVS，而写 flash 期间 cache 关闭，PSRAM 上的栈不可访问。
/// Advance the learning state machine; call per UI tick. **Call it from a task whose stack is
/// in internal RAM** (the settings UI task qualifies): the captured template is written to NVS
/// here, and a flash write disables the cache, making a PSRAM stack unreachable.
voice_turn_learn_state_t voice_turn_learn_poll(void);
/// 取消学习并关掉为学习打开的麦克风。/ Cancel learning and release the mic it opened.
void voice_turn_learn_cancel(void);
/// 清空全部模板。/ Erase every template.
void voice_turn_learn_erase(void);

/// 识别接受阈值：<=0 恢复自动（同词模板两两距离的最大值 × 系数推算），否则手动固定（内部夹到 2..20）。
/// 手动值优先于自动推算，学习/保存流程也会尊重它。幂等。
/// Accept threshold: <=0 restores auto (the worst pairwise distance among the takes of one word
/// times a margin), otherwise a manual value clamped internally to 2..20. A manual value wins
/// over the auto computation, including across learn/save. Idempotent.
void voice_turn_set_accept(float value);
/// 当前生效的阈值（识别测试与设置页显示用）。/ The threshold in force, for the test and settings UI.
float voice_turn_accept(void);

/// 最近一次匹配的调试信息（串口调阈值用）。/ Last match diagnostics, for tuning over serial.
typedef struct {
    float best;        ///< 最优距离 / best distance
    float second;      ///< 次优距离（不同词）/ runner-up distance from another word
    float threshold;   ///< 当前接受阈值 / accept threshold in force
    int word;          ///< 命中的词，-1 表示拒识 / matched word, -1 when rejected
    bool accepted;
    uint32_t seq;      ///< 每次判定自增，界面据此发现新结果 / increments on every decision so the UI can spot a new one
} voice_turn_match_t;
void voice_turn_last_match(voice_turn_match_t *out);

#ifdef __cplusplus
}
#endif
