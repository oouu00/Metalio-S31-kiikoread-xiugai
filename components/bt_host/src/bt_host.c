/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * 中文：蓝牙宿主。两条互不相干的通路：
 *   - 翻页器（BLE HID 主机）用 ESP32 内置 Bluedroid，BLE-only。本组件仍是
 *     `esp_bt_controller` 的唯一生命周期持有者，引用计数供翻页器等消费者共用；
 *   - 音乐 / 听书音频走板载蓝牙音频模组（原厂 BH 方案）：UART1（GPIO39/38，
 *     115200）发 AT 选模式、搜索与配对；I2S0 从机（GPIO42/43/44，16 kHz /
 *     32 bit / 立体声）由模组供时钟，PCM 送进模组后再经蓝牙发给耳机。
 *   模组电源与功放共享 FCA9555 P14（脚号 12），PA 使能 P04（脚号 4）。
 *
 * English: the Bluetooth host. Two independent paths: the page turner (BLE HID host)
 * keeps the on-chip Bluedroid stack in BLE-only mode (this component still owns the sole
 * `esp_bt_controller` lifecycle and its refcount), while music and TTS audio go through
 * the board's Bluetooth audio module (the vendor BH design) — AT commands over UART1
 * (GPIO 39/38, 115200) for mode/scan/pairing, and I2S0 in slave mode (GPIO 42/43/44,
 * 16 kHz / 32-bit / stereo) clocked by the module for the PCM feed. The module shares
 * FCA9555 P14 (pin 12) with the 4G/PA rails; P04 (pin 4) is PA enable.
 *
 * 冻结：不绘制、不做播放策略、不碰 TF 卡。
 * Frozen: no drawing, no playback policy, no SD access.
 */

#include "bt_host.h"

#if KIIKO_BOARD_METALIO47

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "sdkconfig.h"

#include "bt_host_internal.h"
#include "driver/i2s_std.h"
#include "driver/uart.h"
#include "esp_bt.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "metalio_hw.h"

static const char *TAG = "bt_host";

/* ---- 模组接线 / Module wiring ---- */

#define BT_UART UART_NUM_1
#define BT_UART_TX_GPIO 39
#define BT_UART_RX_GPIO 38
#define BT_UART_BAUD 115200
#define BT_UART_RX_BUF 4096

#define BT_PWR_PIN 12  // FCA9555 P1.4：4G + 蓝牙 + 功放共享电源
#define BT_PA_PIN 4    // FCA9555 P0.4：NS4150 PA_EN

#define BT_I2S_PORT I2S_NUM_0
#define BT_I2S_BCLK 42
#define BT_I2S_WS 43
#define BT_I2S_DOUT 44
#define BT_I2S_DIN 45  // 麦 → 主控（模式 1 才有数据）/ mic to the SoC, only in mode 1
#define BT_I2S_RATE 16000

#define BT_MODE_SWITCH_MS 700   // 原厂 AT+RX/TX 与 AT+MODE 之间固定等 700 ms
// 模式应答等待上限。实测模组回显要 660~1600 ms（见下面 BT_SUSPEND_MODE_ACK_MS 的注释），
// 原来的 1500 ms 正好压在范围上沿：回显稍慢一点就判成“没应答”，接着重发 AT+MODE=1 重试，
// 每次重试都要再走 700 ms 的 RX/TX 间隔，最后干脆放弃 → 切到扬声器后一声不响。
// Upper bound for the mode reply. Field echoes take 660-1600 ms, so the old 1500 ms sat exactly on
// the top of that range: a slightly slow echo counted as "no reply" and the code re-sent
// AT+MODE=1 (another 700 ms RX/TX gap each round) until it gave up — silence after switching to
// the speaker.
#define BT_MODE_ACK_MS 3000
#define BT_POWER_RESET_MS 3000  // P14 拉低多久算一次断电复位
#define BT_BOOT_SETTLE_MS 800   // 复电后模组启动的稳定时间
#define BT_WAKE_SETTLE_MS 2000  // 浅睡唤醒后模组重新上电的自举等待 / boot settle after a light-sleep wake
// 链路刚建立后的「重播窗口」：模组会把同一次连接报成 CONNECT SUCCESS 之后又补一条
// CONNECTING/RECONNECT（rc114 日志 12465→12466 只差 1 ms）。这条重播过去被当成断链，
// 于是刚连上的链路被拆掉、600 ms 后再连回来——用户听到的正是「连上马上断开、又自己连上」。
// Post-link replay window: the module announces one connection with CONNECT SUCCESS and then
// repeats CONNECTING/RECONNECT (1 ms later in the rc114 log). That replay used to count as a
// drop, tearing down a link that had just come up and restoring it 600 ms later — the user's
// "connected, dropped, reconnected".
#define BT_LINK_SETTLE_MS 1500
// 入睡前等模组回 `SET MODE 1` 的上限。实测回显要 660~1600 ms，固定等 400 ms 会落在它前面，
// 模组没把「模式 1」写进自己的配置，醒来仍按模式 2 抢先自连。
// Upper bound for waiting on `SET MODE 1` before the rail drops. The echo takes 660-1600 ms in
// the field; a fixed 400 ms lands before it, so mode 1 never reaches the module's own config and
// it wakes up racing us in mode 2.
#define BT_SUSPEND_MODE_ACK_MS 1200
// 硬复位兜底（用户 2026-10-31 提议）：断链时除了现在的「软重建」（AT+RX=2 → AT+MODE=1 →
// 重建 UART/I2S/任务 → 冷启动），再加一级「彻底断电重启模组」。用户手动关开开关屡试不爽，
// 靠的就是那一半断电——软重建只重建了主机侧，模组自己的状态机没被清。分级使用，因为断电
// 3 s 会连 4G 与功放一起掉（P14 是三者共用的），不能一断就来：
//   • 偶发断链（耳机走远、被别的设备抢走）→ 只软重建，快，且断电也救不了不在场的耳机；
//   • 60 s 内断到第 3 次，或软重建连两次都没连回来 → 判定模组状态机半死 → 硬复位一次。
// 硬复位有 120 s 冷却与每会话 3 次上限，耳机长期不在场时不会反复断电。
// Hard-reset fallback (user's suggestion): besides the existing soft rebuild (AT+RX=2 →
// AT+MODE=1 → rebuild UART/I2S/tasks → cold boot), add a full power cycle of the module. The
// user's manual switch toggle works because its "off" half really cuts the rail — the soft
// rebuild only restores the host side and leaves the module's own state machine untouched. It is
// graded because the 3 s cut also drops 4G and the PA (P14 is shared): an occasional drop (headset
// out of range, taken by another device) only gets the fast soft rebuild; a third drop within
// 60 s, or two soft rebuilds that never relinked, means a wedged module → one hard reset, with a
// 120 s cooldown and a 3-per-session cap.

// 原厂「音乐模式」时序（bluetooth_uart.cc: Bluetooth_music_mode_task）：
//   AT+BTSCO=0 → 200 ms → AT+PP=1
// 缺这两条时模组可能停在 HFP/SCO 通路，耳机侧显示已连接、A2DP 音乐却不出声——正是
// 「显示已连接实际没连上、耳机没声音」的形态。连上链路后补发一遍即可。
// The vendor's music-mode sequence: AT+BTSCO=0 → 200 ms → AT+PP=1. Without it the module can
// stay on the HFP/SCO path: the headset looks connected while A2DP music is silent. Replay it
// right after a link comes up.
// AT+PP 的语义到此定案：**播放/暂停切换**，不是「开始播放」。rc114 日志给出了直接证据：
//   34022 TX AT+PP=1（当时播放器刚供上第一块数据）
//   34400~35618 FEED got>0 但 i2s_channel_write 连续失败（tx_err=5）
//   35618 HEAL (i2s stall) —— 模组在 PP 之后**不再拉 I2S**，也就是被按成了暂停。
// 同一天其它五段（41099 / 16066 / 70016 等）PP 之后都能正常出声，说明它是翻转语义：
// 按在「模组已经在放」的时刻 = 按停，按在「模组还没开始」的时刻 = 按开，全看运气。
// A2DP 是 source 推流，数据一到耳机自己就会播，PP 并不是出声的必要条件，所以默认关掉：
// 只要不再翻这个开关，「连上有链路、进度在走、耳机没声音」这条最隐蔽的形态就没了。
// AT+PP is settled now: it toggles play/pause rather than "start playing". rc114 proves it —
// 34022 sends PP right as the player feeds its first block, then 34400-35618 the writes fail
// (tx_err=5) and HEAL fires: the module stopped pulling I2S, i.e. it got paused. The other five
// stretches that day stayed audible, so it is a toggle: press it while playing and you pause.
// A2DP is a source-push protocol and the headset starts on its own once data flows, so PP is not
// needed to make sound. Keep it off by default; set to 1 only to test a paused module.
#define BT_MUSIC_SEND_PP 0
// AT+PP 的发送时机：原厂在 AT+BTSCO=0 之后固定 200 ms 就发，而那一刻播放器往往还没出声。
// 真机日志（rc111，56645 → 56846 → 57332）正好是这个形状：PP 发出去 486 ms 后播放器才
// 从状态 3 走到状态 1。若 AT+PP 是播放/暂停切换，这一下就是把模组按成暂停，之后 PCM 一直
// 在送（write=2048/2048 err=ESP_OK）却不出声——用户看到「自动连上没声音，手动重连就正常」
// （重连时音频已经在流，那一下恰好按成播放）。所以改到播放器真的开始供数据（got > 0）时再发。
// 不做超时兜底：播放器空闲时发 PP 没有任何意义，实测还会跟着出现 I2S 写超时。
// PP firing: the vendor sends it a fixed 200 ms after AT+BTSCO=0, when the player is usually
// still silent. rc111 shows exactly that shape (PP at 56846, player reaches PLAY at 57332). If
// AT+PP toggles play/pause, that press pauses the module and PCM keeps flowing silently.
// Fire it once the player actually feeds data (got > 0). No timeout fallback: pressing PP at an
// idle player means nothing and the field log shows I2S write timeouts right after it.

// SCO 兜底（2026-10-31）：AT+BTSCO=0 之后，模组确认切到 A2DP 的唯一外在证据是 `DISC SCO`。
// rc111 里出声的每一段都有 RX DISC SCO，不出声的那段没有。于是「A2DP 已通、音频在送、
// 但这么久都没等到 DISC SCO」就再补一次 BTSCO=0（不重发 PP，免得又按一次），封顶两次
// —— 这就是把用户「手动重连一下就好」的动作自动做掉。
// SCO fallback: after AT+BTSCO=0 the only outward proof that A2DP took over is `DISC SCO`.
// Every audible stretch in rc111 logs one; the silent one did not. So when A2DP is up and audio
// is flowing but DISC SCO never arrived within this window, re-send BTSCO=0 (without PP, which
// would toggle again), capped at two tries: the user's manual relink, automated.
#define BT_SCO_FALLBACK_MS 2500
#define BT_SCO_RETRY_MAX 2

// 数据面看门狗（2026-10-30）：模组偶尔不再上报断链，界面就一直显示“已连接”却没声音，
// 只能靠用户关开蓝牙开关恢复。下面这几个阈值把“PCM 送不进去”当成断链证据，自动重走
// 模式 1 → 2（与关开开关等价），并限制次数与间隔，保证无耳机时不会无限打转。
// Data-path watchdog: the module sometimes stops reporting a drop, so the UI keeps showing
// "connected" while nothing is audible until the user toggles the switch. These thresholds
// treat "PCM no longer reaches the module" as proof of a dead link and rerun mode 1 → 2,
// with a bounded budget and a minimum gap so it cannot spin when no headset is around.
#define BT_TX_STALL_MS 1200       // 供着数据却整块写不进去的容忍时长 / stall tolerance while feeding
#define BT_TX_LINK_GRACE_MS 2500  // 刚连上给模组开流的时间 / grace right after a fresh link
#define BT_TX_OK_RESET_MS 20000   // 连续正常这么久才认为自愈成功 / healthy run that proves recovery
#define BT_FEED_IDLE_MS 1500      // 有链路却不送数据的容忍时长 / linked-but-not-feeding tolerance

// 断链后主动重连的预算。真机日志里模组自己回连会连报 CONNECT TIMEOUT（每次都 ~5 s），
// 只靠它等于把耳机交给运气；这里用 AT+CONNECT 补，封顶避免耳机不在场时无限重试。
// Budget for active relinks. In the field log the module's own retry just keeps timing out
// (~5 s each), so AT+CONNECT backs it up; the cap stops endless retries with no headset.
#define BT_RELINK_MAX 5
#define BT_IDLE_CLOSE_MS 300000   // 蓝牙开启但一直没连上：闲置 5 分钟自动给模组断电省电 / power down after 5 min idle

#define FEED_FRAMES 256  // 每块 16 ms @16 kHz / 16 ms per block at 16 kHz
#define FEED_TASK_STACK 4096
#define FEED_TASK_PRIO 6
#define FEED_TASK_CORE 1
#define RX_TASK_STACK 3072
#define WORK_TASK_STACK 3072
#define WORK_TASK_PRIO 5

// 模式任务的目标状态：0 = 模组回模式 1（拆链路），1 = 模式 2（可连耳机），
// 2 = 模式 1 + I2S 收（本地麦克风，语音翻页用；同样拆掉耳机链路），
// 3 = 模式 1 + I2S 发 + 功放（本机扬声器外放；同样拆掉耳机链路）。
// Target state for the mode worker: 0 = back to mode 1 (drop the link), 1 = mode 2 (headset),
// 2 = mode 1 with I2S capture (local mic for voice page-turn, also drops the headset link),
// 3 = mode 1 with I2S playback and the PA on (local speaker out; also drops the headset link).
#define WANT_OFF 0
#define WANT_MODE2 1
#define WANT_MIC 2
#define WANT_SPK 3

/* ---- 宿主生命周期 / Host lifecycle ---- */

static SemaphoreHandle_t s_lock;
static uint32_t s_users;
static bool s_stack_up;
static bool s_suspended;

/* ---- 音频模组状态 / Module state ---- */

static SemaphoreHandle_t s_cmd_lock;  // 串行化多步 AT 序列 / serialises multi-step AT sequences
static SemaphoreHandle_t s_work_sem;  // 模式任务信号量 / mode-worker signal
static volatile int s_want_state = WANT_OFF;  // 目标状态，任务按它重试 / target state the worker retries toward
static volatile uint32_t s_seq;           // 生命周期代数：旧序列据此中止 / generation; stale sequences abort
static volatile bool s_audio_wanted;      // bt_audio_init 之后为真 / true after bt_audio_init
static volatile bool s_ready;             // 模组已在模式 2 / module is in mode 2
static volatile bool s_first_init = true; // 本次上电还没做断电复位 / no power reset yet this boot
// 唤醒后的自举等待单独加长：浅睡切掉 P14，模组是真正重新上电，800ms 常赶在自检完成之前。
// Wake gets a longer boot settle: light sleep cuts P14, so the module really reboots and 800 ms
// often lands before its self-test finishes.
static volatile uint32_t s_wake_settle_ms;
static bt_pcm_pull_cb_t s_pull;
// 系统自动放弃蓝牙（硬复位预算耗尽）时回调上层，让设置页开关翻 off 与真实状态一致。
// Registered by the app; fired only from the two give-up sites, never from bt_audio_deinit()
// itself (music_player_stop also calls deinit and must not flip the switch).
static void (*s_auto_disabled_cb)(void) = NULL;
static volatile uint8_t s_volume = 100;   // 0..127
static volatile bool s_mode_ack;
static volatile bool s_cmd_error;

static volatile bool s_pending_connect;  // 进模式 2 后补发的连接 / connect to send once mode 2 is up
static uint8_t s_pending_bda[6];
static volatile bool s_pending_scan;  // 进模式 2 后补发的搜索 / scan to send once mode 2 is up
// 从本机扬声器（mode 1）切回耳机时，进模式 2 前先给模组断电重启。
// 现场（10-31 19:05）里模组照样上报 CONNECT SUCCESS 与 DISC SCO、I2S 也一直在被拉、PCM 每块
// 2048/2048 写成功，耳机却全程无声；用户手动关开一次蓝牙开关（=断电重启模组）立刻恢复。
// 软重进模式 2 清不掉模组自己压着的 A2DP/SCO 状态，只有冷启动可以。
// Power-cycle the module before entering mode 2 when coming back from the local speaker (mode 1).
// Field log 10-31 19:05: the module still reported CONNECT SUCCESS and DISC SCO, I2S kept being
// pulled, every PCM block wrote 2048/2048 — and the headset stayed silent. One manual switch
// toggle (a power cut) restored it at once: a soft mode-2 re-entry cannot clear the module's own
// wedged A2DP/SCO state, only a cold boot can.
// 播放页「重连」按钮：整条断电重启模组（等价用户关开蓝牙开关）。由 UI 置位，
// 模式任务在下一轮开头执行，避免在 UI 线程里等 3 s 断电。
// Player's "relink" button: power-cycle the module end to end (the manual switch toggle). Set
// from the UI and executed at the top of the next worker pass, never on the UI thread.
static volatile bool s_force_restart;
// 重连进行中：整条序列要好几秒，期间再按一次不该再排一次（会变成连着重启两遍，
// 用户看到的是「按了没反应，过了十几秒才出声」）。
// A relink is in flight: the whole sequence takes seconds, so extra presses must not queue
// another one — they would restart the module twice and look like "the button does nothing".
static volatile bool s_relinking;

// 待补发原厂音乐模式时序（AT+BTSCO=0 → AT+PP=1）。由 RX 任务置位、模式任务执行：
// 带 200 ms 等待，不能放在 RX 任务里阻塞收包。
// Pending vendor music-mode sequence, set by the RX task and run by the mode worker: it carries a
// 200 ms wait, which must not block packet reception.
static volatile bool s_music_mode_pending;
static volatile uint8_t s_music_retries;  // 补发次数，避免 SCO 反复回退时无限重发 / resend cap

/* ---- 链路与发现 / Link and discovery ---- */

static volatile bool s_link_active;
// 这条链路是我们主动 `AT+CONNECT` 建起来的，还是模组自己抢先连上的。
// 现场（2026-10-31 21:09 复现）：耳机断电再开机后按「重连」，开蓝牙时模组先自连，
// `CONNECT SUCCESS` 比 `module ready (mode 2)` 还早 7 ms，日志里根本没有 `connecting`；
// 随后链路在、PCM 也在送（FEED err=ESP_OK）、`DISC SCO` 也收到了，耳机却全程无声。
// 再按一次「重连」（走我们自己的 AT+CONNECT）立刻恢复。所以「自连的链路」要在链路成立后
// 重新补一次发射使能并重发音乐模式时序，把音频引擎挂上。
// Whether the link was started by our own AT+CONNECT or the module linked on its own.
// Field case (2026-10-31 21:09): after the headset was power-cycled and "relink" pressed, the
// module self-connected first — CONNECT SUCCESS arrived 7 ms before `module ready (mode 2)` and
// there is no `connecting` line at all. The link stayed up, PCM kept flowing (FEED err=ESP_OK)
// and `DISC SCO` even arrived, but the headset stayed silent. A second "relink" (our own
// AT+CONNECT) fixed it. So a self-connected link needs the transmit enable re-sent and the music
// mode sequence replayed once the link is really up.
static volatile bool s_link_by_us;
static volatile bool s_link_self_connect;
// AT+CONNECT 发出的时刻：用来判断这次链路是「真协商」还是「模组自连秒回旧会话」。
// 21:31 现场的两条链路差得很干脆——静音那次 481 ms 就 CONNECT SUCCESS（HFP 抢在 A2DP 前），
// 出声那次 1280 ms、中间还重试过一次。所以 < 700 ms 成立的一律当成自连链处理。
// When AT+CONNECT went out: tells a real handshake from the module instantly replaying a stale
// session. 21:31 field data is blunt — the silent link answered in 481 ms with HFP ahead of A2DP,
// the working one took 1280 ms and retried once. Anything under 700 ms is treated as self-linked.
static volatile bool s_connect_pending_us;
static volatile int64_t s_connect_sent_us;
#define BT_FASTLINK_MS 700
// 本次「开蓝牙」还剩几次「自动补救重连」名额：命中自连链就排一次干净重连，用完即止，
// 绝不循环（否则模组每次都秒回的话会无限重连）。
// How many automatic clean relinks this Bluetooth-on still allows: one, spent on the first
// suspicious self-link. Never loops, otherwise a module that always answers instantly would
// reconnect forever.
static volatile bool s_rearm_left;
static volatile int64_t s_rearm_at_us;   // 到点才动手：等 SCO/音乐模式先跑完 / run once this passes
// 到点叫醒模式任务的那个一次性定时器：work_task 只在信号量上等，没有自己的轮询周期。
// One-shot timer that wakes the mode worker: work_task blocks on its semaphore and never polls.
static esp_timer_handle_t s_rearm_timer;
static void rearm_timer_cb(void *arg) {
    (void)arg;
    if (s_work_sem) xSemaphoreGive(s_work_sem);
}
static volatile bool s_scanning;
static uint8_t s_link_bda[6];
static char s_link_name[BT_AUDIO_NAME_MAX];

static bt_audio_device_t s_devs[BT_AUDIO_DEV_MAX];
static uint8_t s_dev_count;
static portMUX_TYPE s_dev_lock = portMUX_INITIALIZER_UNLOCKED;

/* ---- 数据面健康 / Data-path health ---- */

// 只有喂帧任务与它触发的自愈会写这几个量；诊断接口只读。
// Only the feed task and the self-heal it triggers write these; the diagnostic API only reads.
static volatile int64_t s_tx_ok_from_us;   // 当前连续写成功的起点 / start of the current good run
static volatile int64_t s_tx_bad_us;       // 本轮写失败的起点 / start of the current bad run
static volatile int64_t s_feed_idle_us;    // 有链路却不送数据的起点 / start of "linked but idle"
static volatile int64_t s_idle_no_link_us;  // 开启但无链路的闲置起点 / start of "audio on but no link" idle

static volatile int64_t s_last_feed_us;    // 最近一次真的送出数据的时刻 / last block actually sent
static volatile int64_t s_link_up_us;      // 本次链路建立的时刻 / when this link came up
static volatile int64_t s_heal_us;         // 上次自愈的时刻 / when the last self-heal ran
static volatile uint32_t s_heal_count;     // 自愈总次数 / total self-heals
static volatile uint32_t s_tx_err_count;   // 累计写失败块数 / failed feed blocks

/* ---- 数据面 / Data path ---- */

static i2s_chan_handle_t s_tx;
static i2s_chan_handle_t s_rx;            // 仅麦克风模式存在 / only exists in mic mode
static volatile bool s_i2s_rx_open;
static int32_t *s_mic_raw;                // PSRAM 收缓冲，首次读时分配 / PSRAM capture buffer
#define BT_MIC_MAX_FRAMES 256             // 单次最多读 16 ms / at most 16 ms per read
static volatile bool s_mic_ready;         // 模式 1 + 收通道已就绪 / mode 1 with capture ready
static volatile bool s_mic_error;         // 本次请求已失败，等调用方重试 / the attempt failed
static int s_mic_fails;                   // 开麦 AT 连续失败次数，够数断电自愈 / consecutive mic-AT failures, enough triggers a power reset
static bool s_uart_up;
static bool s_rx_task_created;
static bool s_feed_task_created;
static volatile bool s_rx_quit;    // 让接收任务自己退出 / ask the RX task to exit
static volatile bool s_feed_quit;  // 让喂帧任务自己退出 / ask the feed task to exit
static volatile bool s_feed_go;
static volatile bool s_feed_idle = true;
// 本次链路是否已经补过原厂音乐模式时序。一次连接会被模组报两三行（SLC / CONNECT），
// 逐行都补一遍就会重复下发 AT+BTSCO=0 / AT+PP=1——若 AT+PP 是播放/暂停切换，第二遍正好按停。
// Whether the vendor music-mode sequence already went out for this link. One connection is
// reported over several lines (SLC / CONNECT); replaying on each one doubles AT+BTSCO=0 /
// AT+PP=1, and a toggling AT+PP would pause what just started.
static volatile bool s_music_applied;
// A2DP（音乐）通路是否真的起来了：`SLC CONNECT SUCCESS` 只是 HFP 服务层，音频还没通。
// 只有真正的 `CONNECT SUCCESS` 才算。音乐模式时序必须等它，否则 AT+BTSCO=0 发在只有
// HFP 的时刻，A2DP 随后建立也不会再补——用户看到"连上了、没声音"。
// Whether the A2DP (music) path is really up: `SLC CONNECT SUCCESS` is only the HFP service
// layer. Only a plain `CONNECT SUCCESS` counts. The music-mode sequence must wait for it,
// otherwise AT+BTSCO=0 lands while only HFP exists and never gets replayed.
static volatile bool s_a2dp_up;
// 断链后主动重连：模组自己的回连会失败（真机日志 701 秒段连报 CONNECT TIMEOUT），
// 由模式任务按预算补发 AT+CONNECT。
// Active relink after a drop: the module's own retry keeps timing out in the field log, so the
// mode worker re-issues AT+CONNECT within a budget.
static volatile bool s_relink_due;
static volatile uint8_t s_relink_tries;
// 硬复位兜底的记账：断链时刻、窗口内次数、上次硬复位时刻与会话累计。
// Bookkeeping for the hard-reset fallback: last drop, drops in the window, last hard reset, total.
// 等一个合适的时机发 AT+PP=1（播放器真的开始供数据那一刻）。
// Waiting for the right moment to send AT+PP=1: the block where the player really feeds data.
static volatile bool s_pp_wait;
static volatile bool s_pp_fire;
// 本次链路是否已经被模组确认切到 A2DP（收到 `DISC SCO`），以及 SCO 兜底已重发几次。
// Whether the module confirmed the A2DP takeover (`DISC SCO`) for this link, and how many
// times the SCO fallback already re-sent BTSCO=0.
static volatile bool s_sco_ok;
static volatile uint8_t s_sco_retry;
static volatile int64_t s_music_applied_us;  // 音乐模式时序发出的时刻 / when the sequence went out
// 本次补发要不要连 AT+PP 一起：SCO 兜底重发只补 BTSCO=0，再按一次 PP 可能把播放按停。
// Whether this replay should include AT+PP: the SCO fallback only re-sends BTSCO=0, since a
// second PP could pause what is playing.
static volatile bool s_music_with_pp = true;
// 统计窗口内的 PCM 峰值（0..32767）：没有串口时，用它区分「播放器送的是静音」还是「真在放歌」。
// Peak PCM in the current 2 s window (0..32767): without a console this tells "the player feeds
// silence" apart from "real audio is flowing".
static uint32_t s_stat_peak;
static int16_t s_pcm_in[FEED_FRAMES * 2];
static int32_t s_pcm_out[FEED_FRAMES * 2];

// 发声测试（2026-10-31）：用户报「深睡/关机连回来有声，锁屏浅睡连回来没声」。两条路都切过
// P14，差别不只在蓝牙——播放器在浅睡里是「暂停再续」，在深睡里是「整机重来」。要一刀切开
// 「模组/蓝牙通路」和「播放器/解码」两类原因，就得有一条不经过播放器的出声路径：这里直接
// 往同一条 I2S 灌 1 kHz 正弦，模组、音量、链路全部沿用真实播放的那一套。
// Test tone (2026-10-31): both sleep paths cut P14, so the difference is not Bluetooth alone —
// the player "pauses and resumes" across light sleep but "restarts from scratch" across deep
// sleep. To split "module/link" from "player/decoder" in one shot we need a sound path that
// skips the player: this pushes a 1 kHz sine into the same I2S, same module, same volume.
#define BT_TONE_HZ_SAMPLES 16  // 16 kHz / 1 kHz：正好 16 点一周期 / exactly 16 samples per cycle
static volatile int64_t s_tone_until_us;
static volatile uint8_t s_tone_phase;
// 幅度 8000 ≈ -12 dBFS：够响，又不至于把耳朵震到。/ 8000 is about -12 dBFS: loud, not painful.
static const int16_t k_tone_lut[BT_TONE_HZ_SAMPLES] = {
    0, 3062, 5657, 7391, 8000, 7391, 5657, 3062,
    0, -3062, -5657, -7391, -8000, -7391, -5657, -3062,
};

static bool backend_start(bool with_rx);  // 按需重建 UART/I2S/任务 / rebuild UART, I2S and tasks on demand
static void backend_teardown(void);  // 音频闲置时整条释放 / release everything while audio is idle
static void i2s_down(void);

/* ---- 诊断埋点 / Diagnostics ---- */

// 没有串口时的唯一现场：把 AT 收发与状态切换逐行交给上层（板级实现写 TF 卡）。
// The only evidence field units have without a console: hand AT traffic and state changes
// line by line to the board, which parks them on the TF card.
static bt_host_trace_fn s_trace;

void bt_host_set_trace(bt_host_trace_fn fn) { s_trace = fn; }

// 诊断埋点：只有 METALIO_BT_TRACE 构建才真的格式化并发给接收方。关闭时编成空宏，
// 这样连格式串（以及 at_text 那套中转）都不会留在交付固件里。
// Trace sink: only METALIO_BT_TRACE builds format and hand off a line. Disabled builds expand
// this to a no-op macro so neither the format strings nor the at_text relay survives in the image.
#if defined(METALIO_BT_TRACE) && METALIO_BT_TRACE
static void bt_log(const char *fmt, ...) {
    if (!s_trace) return;
    char buf[160];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    s_trace(buf);
}

// 去掉行尾 CR/LF 再写进诊断日志，保证一条事件只占一行。
// Strip the trailing CR/LF so one event stays one line in the log file.
static const char *at_text(const char *cmd, char *buf, size_t size) {
    size_t n = strlen(cmd);
    while (n && (cmd[n - 1] == '\r' || cmd[n - 1] == '\n')) --n;
    if (n >= size) n = size - 1;
    memcpy(buf, cmd, n);
    buf[n] = 0;
    return buf;
}
#else
#define bt_log(...) ((void)0)
#endif

/* ---- 工具 / Helpers ---- */

static int hex_val(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// 模组的地址就是带内 12 位十六进制，按打印顺序存进 bda，回填时顺序不变即可往返。
// The module's address is the 12 hex digits it prints; store them in printed order so a
// later AT+CONNECT round-trips exactly.
static void bda_text(const uint8_t bda[6], char out[13]) {
    snprintf(out, 13, "%02x%02x%02x%02x%02x%02x", bda[0], bda[1], bda[2], bda[3], bda[4],
             bda[5]);
}

static bool bda_equal(const uint8_t a[6], const uint8_t b[6]) { return memcmp(a, b, 6) == 0; }

static void dev_upsert(const uint8_t bda[6], const char *name) {
    portENTER_CRITICAL(&s_dev_lock);
    int slot = -1;
    for (uint8_t i = 0; i < s_dev_count; ++i) {
        if (bda_equal(s_devs[i].bda, bda)) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        if (s_dev_count >= BT_AUDIO_DEV_MAX) {
            portEXIT_CRITICAL(&s_dev_lock);
            return;
        }
        slot = s_dev_count++;
        memset(&s_devs[slot], 0, sizeof(s_devs[slot]));
        memcpy(s_devs[slot].bda, bda, 6);
    }
    s_devs[slot].rssi = 0;
    s_devs[slot].audio = true;
    if (name && name[0]) {
        snprintf(s_devs[slot].name, sizeof(s_devs[slot].name), "%s", name);
    } else {
        // 名字为空时回落成地址，列表里不出现空白行。
        // Fall back to the address so the list never shows a blank row.
        bda_text(bda, s_devs[slot].name);
    }
    portEXIT_CRITICAL(&s_dev_lock);
}

static void devs_clear(void) {
    portENTER_CRITICAL(&s_dev_lock);
    s_dev_count = 0;
    portEXIT_CRITICAL(&s_dev_lock);
}

// 0..127 映射成 0..100，再按 1.5 次方给软件增益。原厂用的是平方曲线（BTAudioCodec::Write），
// 满档 1.0、22% 只剩 0.048（约 −26 dB）——实机听感就是"只剩一点底噪、像没声音"。
// rc110 日志里那两句抱怨（vol=36 / vol=28，即 28% / 22%）正好落在这个区间，所以改成 pct^1.5：
// 满档仍是 1.0（不会削波），低档位比原厂响约 6 dB。
// Map 0..127 onto 0..100, then apply a 1.5-power software gain. The vendor curve is the square
// one (BTAudioCodec::Write): unity at full scale but only 0.048 (≈ −26 dB) at 22 %, which is what
// the field reports call "just a bit of noise, no music" (vol=28/36 → 22 %/28 %). A 1.5 power
// keeps unity at full scale (no clipping) and is ~6 dB louder in the low range.
static uint32_t isqrt_u32(uint32_t v) {
    uint32_t r = 0;
    while ((r + 1) * (r + 1) <= v) r++;
    return r;
}

static int32_t vol_factor(uint8_t vol) {
    const uint32_t pct = vol > 127 ? 100 : (uint32_t)vol * 100u / 127u;
    const uint32_t root10 = isqrt_u32(pct * 100u);  // sqrt(pct) × 10
    return (int32_t)(pct * root10 * 65536u / 10000u);
}

/* ---- 一次性 AT 能力探测 / One-shot AT capability probe ----
 * 目标：拿到 BH1098G 的真实命令表，尤其是「关掉自动回连」那一条。模组把「模式 2 + 上次设备」
 * 写在自己的 NV 里，进模式 2 / 冷启动就抢先自连，而那条自连链路的 I2S → 蓝牙发送通道不被 arm，
 * 耳机全程静音——主机侧所有补丁都只是在补救。关掉它的自连才是根治。
 * 我们手上没有这颗模组的 AT 手册（工程里只用过 MODE/RX/TX/CONNECT/BTSCO/PP/INQUIRING/GVER/BT），
 * 所以直接问它：把候选命令逐条灌进去，回话由 RX 任务原样打到串口（`rx: ...`）。
 * 只发「读」类命令：不碰配对、不复位、不改模式，探测完照样能正常建链。
 * Goal: the module's real command table, above all the "disable auto reconnect" one. The module
 * keeps "mode 2 + last device" in its own NV and races us from a stale session whose I2S ->
 * Bluetooth transmit path is never armed, so the headset stays silent. Killing that self-link is
 * the real fix; every host-side patch is only a rescue. We have no AT manual for this module, so
 * ask it: pump the candidate commands in and let the RX task print each reply verbatim. Read-only
 * commands only — no pairing change, no reset, no mode change — so the link still comes up after.
 */
// 探测已经做完（2026-10-31 共四轮，见 probe_round1..4.log），结论写在下面：
//   这颗模组（BH1098G, V1.03）的 AT 层只有动作命令，没有查询语法，也没有「关自连」的命令：
//     ✅ AT+GVER → V1.03        ✅ AT+MODE=1/2/3（MODE=0 → SET MODE ERR，即模式只有 1/2/3）
//     ✅ AT+BTSCO=0 → 切 A2DP    ✅ AT+BTSCO=1 → SETUP SCO（切回通话通路）
//     ❌ AT+BT=0/1、AT+RECONNECT=、AT+AUTOCONN=、AT+DELCONN、AT+DISCONNECT、
//        AT+RESET、AT+HELP、所有 `?` 查询 —— 一律 AT ERROR
//   所以「让模组别自己抢连」这条路走不通，主机侧补偿（首次链路后自动补一次重连）就是最终方案。
// Probe is finished (four rounds, see probe_round1..4.log). This module (BH1098G V1.03) only
// accepts action commands: no query syntax, no way to disable its auto-reconnect. Host-side
// compensation (one clean relink after the first link of a session) is therefore the final design.
#define BT_AT_PROBE 0

static void cmd_send(const char *cmd);   // 定义在本块之后 / defined further down
static void request_state(int want);     // 同上 / same

// 探测期间占住 AT 总线：正常流程若在这 15 s 里发 AT+MODE=2 / AT+TX=1 / AT+CONNECT，
// 回话会和探测的回话混在一起，整张判定表就废了（第一轮就混进了这些）。
// Hold the AT bus during the probe: if the normal path fires AT+MODE=2 / AT+TX=1 / AT+CONNECT
// inside these 15 s, its replies mix with the probe's and the table becomes unreadable — which is
// exactly what happened in round 1.
static volatile bool s_probe_busy;
static volatile int s_probe_want = -1;
// 只探测一次：第二轮日志里 “AT probe: 15 commands” 出现两次（audio_start 被再次调用），
// 两条流交错，判读表就废了。
// Probe once only: round 2 printed "AT probe: 15 commands" twice because audio_start ran again,
// and the two interleaved streams made the table unreadable.
static bool s_probe_done;

#if BT_AT_PROBE
// 跑在独立任务里：探测要 20 秒，不能让调用方陪着等。
// Runs in its own task: the probe takes 20 s and must not block the caller.
static void at_probe_task(void *arg) {
    const int want = (int)(intptr_t)arg;
    if (s_probe_done) {  // 已经问过一遍了，别再灌一次 / already done, do not repeat
        s_probe_busy = false;
        request_state(want);
        vTaskDelete(NULL);
        return;
    }
    s_probe_done = true;
    static const char *const cmds[] = {
        // 控制组：已知可用 / control: known-good
        "AT+GVER\r\n",
        // 阴性对照：不存在就必回 AT ERROR / negative control: absent -> AT ERROR
        "AT+NOSUCHCMD=0\r\n",
        // 已确认存在的命令，换别的参数值：找出「闲置 / 关闭」这类能清掉链路的模式。
        // MODE 是确认可用的命令，只是我们一直只用 1 和 2。
        // Known-good command with other arguments: look for an idle/off mode that clears the link.
        // MODE exists; we have only ever used 1 and 2.
        "AT+MODE=0\r\n",
        "AT+MODE=3\r\n",
        // 蓝牙开关本身：若能单独关射频，断链后关掉射频就不会再有自连。
        // The radio switch itself: if it exists, killing the radio after a drop removes self-connect
        // entirely without touching P14 / the 4G supply.
        "AT+BT=0\r\n",
        "AT+BT=1\r\n",
        // 音频通路重挂：我们只用过 BTSCO=0（切 A2DP），从没试过先 =1 再 =0。
        // Re-arming the audio path: only BTSCO=0 (switch to A2DP) has ever been sent, never =1 first.
        "AT+BTSCO=1\r\n",
        // 软复位放最后：真支持的话模组会重启，后面再发就没回话了。
        // Soft reset last: if it exists the module reboots and later commands go unanswered.
        "AT+RESET\r\n",
    };
    const size_t n = sizeof(cmds) / sizeof(cmds[0]);
    ESP_LOGW(TAG, "AT probe: %u commands, 1 s apart", (unsigned)n);
    bt_log("AT PROBE start n=%u", (unsigned)n);
    for (size_t i = 0; i < n; i++) {
        ESP_LOGW(TAG, "PROBE>>> [%u/%u] %s", (unsigned)(i + 1), (unsigned)n, cmds[i]);
        cmd_send(cmds[i]);
        vTaskDelay(pdMS_TO_TICKS(1500));
        ESP_LOGW(TAG, "PROBE--- [%u/%u] window closed", (unsigned)(i + 1), (unsigned)n);
    }
    ESP_LOGW(TAG, "AT probe: done, releasing the bus");
    bt_log("AT PROBE done");
    const int next = (s_probe_want >= 0) ? s_probe_want : want;
    s_probe_want = -1;
    s_probe_busy = false;
    request_state(next);  // 探测完再开始建链 / link only after the probe
    vTaskDelete(NULL);
}
#endif


static void cmd_send(const char *cmd) {
    if (!s_uart_up) return;
    char shown[48];
    bt_log("TX %s", at_text(cmd, shown, sizeof(shown)));
    // 控制台也留一份（debug 档）：以前 TX 只进 SD 的 bt_trace.log，现场只能看到模组的回话，
    // 「到底发没发 AT+CONNECT / AT+TX=1」全靠猜。带上它，一次复现就能看清完整时序。
    // Mirror the command to the console in debug builds: TX used to go only to the SD trace, so
    // field logs showed the module's replies while "did we actually send AT+CONNECT?" was a guess.
    // 注意：at_text 只在 METALIO_BT_TRACE 下存在；关 trace 的 debug 档（--no-bt-trace）这行不能编。
    // Note: at_text exists only under METALIO_BT_TRACE; a no-trace debug build must not compile this.
#if defined(METALIO_BT_TRACE) && METALIO_BT_TRACE
    ESP_LOGD(TAG, "tx: %s", at_text(cmd, shown, sizeof(shown)));
#else
    (void)shown;
#endif
    uart_write_bytes(BT_UART, cmd, strlen(cmd));
    uart_wait_tx_done(BT_UART, pdMS_TO_TICKS(200));
}

/* ---- 断链与自愈 / Drop and self-heal ---- */

// 统一的“链路已断”收口：清链路与喂帧标志，必要时按预算安排重进模式 2。
// 以前每种断链措辞各写一遍，漏一种就留下“显示已连接”的假在线；集中到一处后
// 任何新措辞只要调一次本函数。
// One place for "the link is gone": clear the link/feed flags and, within the retry budget,
// schedule a fresh mode-2 entry. Previously each wording had its own copy, so a missing one
// left the stale "connected" state behind.
static void link_drop(bool rearm, const char *why) {
    const bool was = s_link_active;
    s_link_active = false;
    s_a2dp_up = false;
    // 链路真没了：排着的「自连秒回补救」也就没有意义了，撤掉，别到点了去打一条不存在的链路。
    // The link is really gone, so a queued stale-session rescue is pointless: cancel it.
    s_connect_pending_us = false;
    s_rearm_at_us = 0;
    if (s_rearm_timer) esp_timer_stop(s_rearm_timer);
    // 链路没了，下一次成立要重新判定是谁发起的。只有真断链（rearm）才清：模组在建立连接的
    // 过程中会补一条 `RECONNECT DEVICE NAME`，那是软断开，我们刚发出去的 AT+CONNECT 仍然有效。
    // Re-judge the origin on the next link, but only on a real drop: the module echoes
    // `RECONNECT DEVICE NAME` while our own connect is in flight, and that soft drop must not
    // erase the fact that the pending link is ours.
    if (rearm) {
        s_link_by_us = false;
        s_link_self_connect = false;
    }
    // 目标就是本机扬声器时不要动喂帧开关：那条通路本来就没有蓝牙链路，
    // 切模式过程中模组发来的断开上报不能把正在外放的数据流关掉。
    // Do not touch the feed flag while the target is the local speaker: that path has no
    // Bluetooth link by design, and the disconnect reports the module emits during the mode
    // switch must not cut a stream that is already playing.
    if (s_want_state != WANT_SPK) s_feed_go = false;
    s_tx_bad_us = 0;
    s_feed_idle_us = 0;
    s_last_feed_us = 0;  // 链路没了，别把上一条链路的数据流当成“还在播” / no stale "still playing"
    s_music_mode_pending = false;  // 链路没了就别再补音乐模式 / no music-mode replay without a link
    s_music_retries = 0;
    s_music_applied = false;       // 下一条链路要重新补一遍 / the next link needs the sequence again
    // 下一条链路重新开始：通路确认、SCO 兜底预算、PP 时机一起归零。
    // The next link starts over: A2DP confirmation, SCO budget and the PP timing all reset.
    s_sco_ok = false;
    s_sco_retry = 0;
    s_pp_wait = false;
    s_pp_fire = false;
    s_music_with_pp = true;
    s_music_applied_us = 0;
    if (!was) return;
    ESP_LOGW(TAG, "headset link dropped: %s", why ? why : "?");
    bt_log("LINK down (%s)", why ? why : "?");
    if (!rearm) return;
    // 策略：断链就彻底关蓝牙（等价于用户把设置页的开关拨到 off），**不自动重连、不自动重启
    // 模组**。旧实现在每次断链时排队一次断电重启（P14 与 4G 共用），耳机不在场时每两分钟断
    // 一次电，预算用尽后再自动关蓝牙——现场看到的就是「反复连接、反复断电、最后蓝牙自己消失」。
    // 恢复改由用户显式触发：播放页「重连」，或设置页重新打开开关。
    // Policy: a dropped link turns Bluetooth off completely (the same as flipping the settings
    // switch). No automatic reconnect and no automatic module restart. The old code queued a power
    // cycle on every drop (P14 is shared with the 4G modem), so an absent headset cut the rail
    // every two minutes and, once the budget was spent, Bluetooth vanished by itself. Recovery is
    // explicit now: the player's "relink" button or the settings switch.
    (void)rearm;
    if (s_want_state != WANT_MODE2) return;        // 扬声器/麦克风没有链路，别误关
    if (s_auto_disabled_cb) s_auto_disabled_cb();  // 先把设置页开关翻 off，界面与真状态一致
    bt_audio_deinit();                             // 回模式 1 并释放后端 = 「关蓝牙开关」
}

// 数据面判死：链路说「已连接」，PCM 却已经送不进模组——就是用户看到的「显示连着没声音」。
// 按新策略不再自愈重连，直接关蓝牙，等用户自己按「重连」。
// A dead data path: the link claims "connected" while PCM no longer reaches the module — exactly
// the user-visible "connected but silent". No self-heal any more: turn Bluetooth off and let the
// user press "relink".
static void audio_watchdog_fire(const char *why) {
    if (!s_link_active) return;              // 没连上时没声音是正常的 / expected while unlinked
    if (s_want_state != WANT_MODE2) return;  // 只对「要耳机出声」生效 / headset target only
    const int64_t now = esp_timer_get_time();
    // 刚连上的几秒里模组还在开流，先别判死。
    // Right after a link the module is still opening the stream; do not judge too early.
    if (s_link_up_us && now - s_link_up_us < (int64_t)BT_TX_LINK_GRACE_MS * 1000) return;
    s_heal_us = now;
    s_heal_count++;
    ESP_LOGW(TAG, "audio dead while linked (%s), disabling Bluetooth (tx_err %u)", why,
             (unsigned)s_tx_err_count);
    bt_log("HEAL->DISABLE (%s) tx_err=%u", why, (unsigned)s_tx_err_count);
    s_link_active = false;
    s_feed_go = false;
    s_tx_bad_us = 0;
    s_feed_idle_us = 0;
    if (s_auto_disabled_cb) s_auto_disabled_cb();
    bt_audio_deinit();
}

/* ---- 模组应答解析 / Module reply parsing ---- */

static void handle_line(char *line) {
    char *s = line;
    while (*s == ' ' || *s == '\t') s++;
    size_t n = strlen(s);
    while (n && (s[n - 1] == ' ' || s[n - 1] == '\t' || s[n - 1] == '\r')) s[--n] = 0;
    if (!n) return;
    ESP_LOGD(TAG, "rx: %s", s);
    bt_log("RX %s", s);

    if (strstr(s, "SET MODE 1") || strstr(s, "SET MODE 2")) {
        s_mode_ack = true;
    }
    // 模组回模式 1/3 即拆耳机链路（协议事实：回模式1断开耳机）。链路状态不同步就会"显示已连接却没声音"。
    // 而且模组一旦离开模式 2，`s_ready` 必须跟着落，否则之后的 AT+CONNECT 全发在模式 1 上白白失败。
    // The module drops the headset link on mode 1/3; without syncing, the UI claims connected while
    // silent. It also left mode 2, so `s_ready` must fall too — otherwise later AT+CONNECT commands
    // go out in mode 1 and fail silently.
    if (strstr(s, "SET MODE 1") || strstr(s, "SET MODE 3")) {
        // 目标本来就是模式 1（本机扬声器 / 本地麦克风）时，这条只是我们自己那次 AT+MODE=1 的
        // 回执，不是“模组自己退回模式 1”。以前照旧清 s_ready 并当断链处理，结果就是切到
        // 扬声器后喂帧任务立刻停摆：环填满 → 解码侧卡住 → 一声不响、进度冻住。
        // When mode 1 is the target (local speaker / local mic) this line is the ack of our own
        // AT+MODE=1, not the module falling back on its own. Clearing s_ready here used to stop
        // the feed task the instant the speaker was selected: the ring filled, the decoder
        // stalled, and the result was silence with a frozen progress bar.
        if (s_want_state == WANT_SPK || s_want_state == WANT_MIC) {
            // 只同步“耳机链路已拆”，绝不动 s_ready / s_feed_go。
            // Sync "headset link is gone" only; never touch s_ready / s_feed_go.
            s_link_active = false;
            s_a2dp_up = false;
            s_sco_ok = false;
            s_music_applied = false;
            bt_log("RX mode 1 ack (local target %d)", s_want_state);
            return;
        }
        s_ready = false;
        s_mic_ready = false;
        // 模组自己退回模式 1（例如电源抖动后重启）时按容错重进一次模式 2 并重连。
        // When the module drops back to mode 1 by itself (a rail glitch rebooting it, say),
        // re-enter mode 2 and reconnect under the self-heal budget.
        audio_watchdog_fire("module left mode 2");
        link_drop(false, s);
        return;
    }
    if (strstr(s, "SET MODE 2")) return;
    // 连接失败也带 CONNECT 字样，必须先于通用的 ERROR/FAIL 处理，否则链路会一直停在“已连接”。
    // A failed connect also carries CONNECT, so this must precede the generic ERROR/FAIL branch;
    // otherwise the link stays stuck at "connected".
    if (strstr(s, "CONNECT") && (strstr(s, "FAIL") || strstr(s, "ERROR"))) {
        link_drop(true, s);
        return;
    }
    if (strstr(s, "ERROR") || strstr(s, "FAIL")) {
        s_cmd_error = true;
        return;
    }
    if (strstr(s, "INQUIRING START")) {
        devs_clear();
        s_scanning = true;
        return;
    }
    if (strstr(s, "INQ COMPLETE")) {
        s_scanning = false;
        s_pending_scan = false;
        return;
    }
    // 模组正在重连：先按断链处理，重连成功会再回 CONNECT SUCCESS 把状态纠正。
    // 但刚连上 1.5 s 内的这条是**同一次连接的重播**（CONNECT SUCCESS 之后 1 ms 就来），
    // 拆掉它会绕回「连上马上断开、又自己连上」的观感，所以只记录。
    // The module is re-linking: treat as dropped; CONNECT SUCCESS will restore the state. Within
    // 1.5 s of a fresh link it is the same connection replayed (1 ms after CONNECT SUCCESS), and
    // tearing it down produces the "connected, dropped, reconnected" flicker — log only.
    if (strstr(s, "RECONNECT") || strstr(s, "CONNECTING")) {
        if (s_link_active && s_link_up_us &&
            esp_timer_get_time() - s_link_up_us < (int64_t)BT_LINK_SETTLE_MS * 1000) {
            bt_log("RX replay ignored (%s) fresh link", s);
            return;
        }
        link_drop(false, s);
        return;
    }
    // 断开措辞全收：真机日志里出现过 REMOTE DISCON，其它固件版本还会打 DISCONNECT /
    // ACL LOST / LINK LOSS。以前只认 REMOTE DISCON，漏掉的正是“显示已连接却没声音”的假在线。
    // Catch every disconnect wording: real logs showed REMOTE DISCON, other module builds print
    // DISCONNECT / ACL LOST / LINK LOSS. Matching only REMOTE DISCON was the stale-link bug.
    if (strstr(s, "DISCON") || strstr(s, "ACL LOST") || strstr(s, "LINK LOSS")) {
        link_drop(true, s);
        return;
    }
    // SCO（通话）通路成立时 A2DP 音乐不出声：耳机那头看着是连上了，实际是把音频送到了
    // HFP 窄带通道。原厂在「音乐模式」按钮里发 AT+BTSCO=0 把 SCO 拆掉，这里收到上报就自动补一遍。
    // With SCO up the A2DP music path stays silent: the headset looks connected while audio goes
    // to the narrow-band HFP channel. The vendor drops it with AT+BTSCO=0 from its "music" button;
    // here the module's own report triggers the same sequence.
    if (strstr(s, "SETUP SCO")) {
        ESP_LOGW(TAG, "headset switched to SCO (call path): forcing music mode");
        if (s_music_retries < 3) {
            s_music_retries++;
            s_music_with_pp = true;  // 完整时序：SCO 刚起来，PP 该跟着走 / full sequence: SCO just came up
            s_music_mode_pending = true;
            if (s_work_sem) xSemaphoreGive(s_work_sem);
        }
        return;
    }
    if (strstr(s, "DISC SCO")) {
        s_music_retries = 0;
        // 这是模组确认「已经切到 A2DP 通路」的唯一外在证据，SCO 兜底靠它收手。
        // The only outward proof that the module switched to A2DP; the SCO fallback stops on it.
        s_sco_ok = true;
        ESP_LOGI(TAG, "music mode (SCO down)");
        bt_log("SCO down (A2DP confirmed)");
        return;
    }
    // 真机日志里一次链路建立会连着报两行：SLC CONNECT SUCCESS（HFP 服务层）与
    // CONNECT SUCCESS（A2DP/ACL）。两条都当新链路，就会把 AT+BTSCO=0 / AT+PP=1 各发两遍；
    // 若 AT+PP 是播放/暂停切换语义，第二遍正好把刚起来的播放按停。
    // 链路已经是连着的就只把它当重复上报丢掉。
    // Real logs report one link with two lines: SLC CONNECT SUCCESS (HFP service level) and
    // CONNECT SUCCESS (A2DP/ACL). Treating both as fresh links sends AT+BTSCO=0 / AT+PP=1
    // twice; if AT+PP toggles play/pause, the second one pauses what just started.
    if (strstr(s, "SLC CONNECT SUCCESS") && s_link_active) {
        ESP_LOGI(TAG, "SLC up (HFP), link already counted");
        return;
    }
    if (strstr(s, "CONNECT SUCCESS")) {
        // 真机日志（rc110）里 SLC（HFP 服务层）先到、真正的 CONNECT SUCCESS（A2DP）后到 450 ms
        // 也很常见（343669 → 344120）。两者都算链路通了（界面与自动续播靠它），但原厂音乐模式
        // 时序必须等 A2DP：发早了，AT+BTSCO=0 落在只有 HFP 的时刻，A2DP 起来就不会再补一遍。
        // Field log (rc110) shows SLC/HFP first and the real CONNECT SUCCESS (A2DP) 450 ms later.
        // Either one counts as a link, but the music-mode sequence has to wait for A2DP: sent too
        // early it lands on an HFP-only link and never gets replayed once A2DP comes up.
        const bool slc = (strstr(s, "SLC CONNECT SUCCESS") != NULL);
        const bool dup = s_link_active;
        // 模组抢先自连（我们没有发过 AT+CONNECT）时标记出来：这种链路的音频引擎常常没挂上，
        // 音乐模式时序里会补一次 AT+TX=1。/ Flag a module-initiated link: its audio engine is
        // usually not armed, and the music-mode sequence re-sends AT+TX=1 for it.
        if (!dup && !s_link_by_us) {
            s_link_self_connect = true;
            ESP_LOGW(TAG, "module self-connected (no AT+CONNECT sent)");
        }
        s_pending_connect = false;
        s_relink_due = false;
        s_relink_tries = 0;  // 连上了，重连预算清零 / linked again, reset the relink budget
        s_link_active = true;
        s_feed_go = true;
        if (!slc) s_a2dp_up = true;
        // 21:38 的现场把「按耗时判断」也否掉了：3620 ms 的真协商链路同样静音，520 ms 的也静音，
        // 而 21:31 出声那条是 1280 ms——耗时根本不是判据。现在唯一稳定成立的规律是：
        // 耳机断电重开后，**第一跳连接一定没声音，第二跳一定有**（用户已连续复现五次）。
        // 所以每次「开蓝牙」只发一个名额：见到本会话第一跳 A2DP 链路就排一次干净重连，
        // 等于替用户按一次「重连」。名额用完即止，绝不循环。
        // The 21:38 run killed the timing heuristic too: a 3620 ms genuine handshake was silent and
        // so was a 520 ms one, while the working 21:31 link took 1280 ms. The only stable rule left
        // is: after the headset is power-cycled, the first link of a session is always silent and
        // the second one always has sound (reproduced five times). So hand out one budget per
        // Bluetooth-on and spend it on the session's first A2DP link — that is the user's "relink".
        if (!slc && s_rearm_left) {
            const int64_t dt_us = s_connect_pending_us
                                      ? (esp_timer_get_time() - s_connect_sent_us)
                                      : 0;
            // 时间戳只在这里清：SLC 行常比 A2DP 行先到（21:38 现场 3534 vs 3537）。
            // Clear the stamp here only: the SLC line usually lands before the A2DP line.
            s_connect_pending_us = false;
            s_rearm_left = false;
            s_rearm_at_us = esp_timer_get_time() + 1200 * 1000;
            if (!s_rearm_timer) {  // 懒建：RX 任务可能先于模式任务跑起来
                const esp_timer_create_args_t args = {
                    .callback = rearm_timer_cb,
                    .name = "bt_rearm",
                };
                esp_timer_create(&args, &s_rearm_timer);
            }
            if (s_rearm_timer) esp_timer_start_once(s_rearm_timer, 1200 * 1000);
            ESP_LOGW(TAG, "first link of this session (%d ms): one clean relink queued",
                     (int)(dt_us / 1000));
            bt_log("AUTO rearm queued (%dms)", (int)(dt_us / 1000));
        }
        // 新链路：重置数据面计时，宽限期由 audio_watchdog_fire 里读 s_link_up_us 判断。
        // Fresh link: restart the data-path clocks; the grace window reads s_link_up_us.
        s_link_up_us = esp_timer_get_time();
        s_tx_ok_from_us = 0;
        s_tx_bad_us = 0;
        s_feed_idle_us = 0;
        s_music_retries = 0;
        // 新链路：通路确认与 SCO 兜底预算都从零开始；完整时序（含 PP）重新走一遍。
        // Fresh link: start the A2DP confirmation and the SCO budget from zero, and allow the
        // full sequence (PP included) again.
        s_sco_ok = false;
        s_sco_retry = 0;
        s_music_with_pp = true;
        // 连上就把原厂音乐模式时序补一遍（在模式任务里跑，带 200 ms 等待）。
        // 同一条链路会被模组报两三行（SLC CONNECT SUCCESS / CONNECT SUCCESS），只在第一次补。
        // Replay the vendor music-mode sequence on a fresh link; it runs in the mode worker
        // because it carries a 200 ms wait. The module reports one link over several lines, so
        // only the first A2DP line triggers the replay.
        if (!s_music_applied && s_a2dp_up) {
            s_music_mode_pending = true;
            if (s_work_sem) xSemaphoreGive(s_work_sem);
        }
        if (!dup) {
            ESP_LOGI(TAG, "headset linked: %s", s_link_name);
            bt_log("LINK up name=%s ready=%d a2dp=%d applied=%d", s_link_name, (int)s_ready,
                   (int)s_a2dp_up, (int)s_music_applied);
        } else {
            // 同一条链路的第二/第三行：状态已经是对的，只留一行证据，别再打一次“已连接”。
            // Second/third line of the same link: state is already right, just keep one trace row.
            bt_log("LINK again (%s) ready=%d a2dp=%d applied=%d", slc ? "SLC" : "CONNECT",
                   (int)s_ready, (int)s_a2dp_up, (int)s_music_applied);
        }
        return;
    }
    // SLC CONNECT TIMEOUT 是 HFP 服务层没连上，A2DP 音乐链路照样能通。实机日志（rc109）里
    // 出现过三次：音乐已经在放，一个 SLC 超时就触发整条重建——把好好的链路拆掉重来，
    // 用户听到的是断音又重连。这种上报只记录，不动链路。
    // "SLC CONNECT TIMEOUT" is an HFP service-level failure while the A2DP music link can still
    // be fine. The rc109 field log shows it three times: music was flowing, one SLC timeout tore
    // the whole backend down and the user heard the link drop and come back. Log it, keep the link.
    if (strstr(s, "SLC CONNECT TIMEOUT")) {
        ESP_LOGI(TAG, "SLC (HFP) timeout, music path untouched");
        return;
    }
    if (strstr(s, "CONNECT TIMEOUT")) {
        // 这条上报属于模组自己的一次回连尝试（它断电重启后会自动回连），不代表我们排队的连接
        // 请求失效。以前在这里清 s_pending_connect，把唤醒后紧跟着就绪的那次 AT+CONNECT 吞掉，
        // 于是模式 2 起来了、耳机却再也没被主动连过——rc110 日志 65252 / 238139 两处就是这样，
        // 用户看到的就是「解锁后耳机搜不到」。
        // This report belongs to the module's own relink attempt (it retries after every reboot);
        // it does not cancel a queued connect. Clearing s_pending_connect here swallowed the
        // AT+CONNECT due right after mode 2 came up — exactly rc110's 65252 / 238139 rows, which
        // the user experienced as "after unlock the headset cannot be found".
        link_drop(true, "connect timeout");
        // 没有链路就主动补 AT+CONNECT：只等模组自己的回连，同一份日志里 30 s 都没成功。
        // With no link, back the module's retry with our own AT+CONNECT; in the same log the
        // module's own retries failed for 30 s.
        if (s_audio_wanted && s_want_state == WANT_MODE2 && !s_link_active) {
            s_relink_due = true;
            if (s_work_sem) xSemaphoreGive(s_work_sem);
        }
        return;
    }
    if (!strncmp(s, "AT+BT:", 6)) {
        const char *p = s + 6;
        if (strlen(p) < 12) return;
        uint8_t bda[6];
        for (int i = 0; i < 6; ++i) {
            const int hi = hex_val(p[i * 2]);
            const int lo = hex_val(p[i * 2 + 1]);
            if (hi < 0 || lo < 0) return;
            bda[i] = (uint8_t)((hi << 4) | lo);
        }
        dev_upsert(bda, p + 12);
        return;
    }
    // 未识别行打 INFO 留证据，与原厂一致（原厂把每行 RX 都打出来）；上次"已连接却没声"就是缺这份证据。
    // Log unmatched lines at INFO for evidence, matching the factory firmware; the silent-while-linked bug had none.
    ESP_LOGI(TAG, "unhandled: %s", s);
}

static void rx_task(void *arg) {
    (void)arg;
    char line[160];
    size_t len = 0;
    bool overflow = false;
    uint8_t chunk[64];
    for (;;) {
        const int n = uart_read_bytes(BT_UART, chunk, sizeof(chunk), pdMS_TO_TICKS(200));
        for (int i = 0; i < n; ++i) {
            const uint8_t b = chunk[i];
            if (b == '\r' || b == '\n') {
                line[len] = 0;
                if (!overflow) handle_line(line);
                len = 0;
                overflow = false;
            } else if (b >= 32 && b < 127) {
                if (len + 1 < sizeof(line)) {
                    line[len++] = (char)b;
                } else {
                    overflow = true;
                }
            }
        }
        // 拆除时由自己退出，栈内存随之还给内部堆。
        // Exit on teardown so the stack returns to the internal heap.
        if (s_rx_quit) {
            s_rx_task_created = false;
            vTaskDelete(NULL);
        }
    }
}

/* ---- 模式序列 / Mode sequences ---- */

// 序列是否仍然有效：代数一致且目标状态没变。
// Whether the sequence still applies: same generation and an unchanged target state.
static bool seq_alive(uint32_t gen, int want) { return gen == s_seq && s_want_state == want; }

// 多步 AT 序列；目标状态或代数变了就中止。返回是否收到模式应答。
// A multi-step AT sequence; aborts when the target state or generation changes.
static bool mode_seq(const char *pre, const char *mode_cmd, uint32_t gen, int want) {
    bool ok = false;
    if (!seq_alive(gen, want)) return false;
    if (xSemaphoreTake(s_cmd_lock, portMAX_DELAY) != pdTRUE) return false;
    if (seq_alive(gen, want)) {
        s_mode_ack = false;
        s_cmd_error = false;
        cmd_send(pre);
        vTaskDelay(pdMS_TO_TICKS(BT_MODE_SWITCH_MS));
        if (seq_alive(gen, want)) {
            s_mode_ack = false;
            s_cmd_error = false;
            cmd_send(mode_cmd);
            const int64_t deadline = esp_timer_get_time() + BT_MODE_ACK_MS * 1000;
            while (seq_alive(gen, want) && esp_timer_get_time() < deadline) {
                if (s_cmd_error) break;
                if (s_mode_ack) {
                    ok = true;
                    break;
                }
                vTaskDelay(pdMS_TO_TICKS(10));
            }
        }
    }
    xSemaphoreGive(s_cmd_lock);
#if defined(METALIO_BT_TRACE) && METALIO_BT_TRACE
    char shown[32];
#endif
    bt_log("SEQ %s ack=%d", at_text(mode_cmd, shown, sizeof(shown)), (int)ok);
    return ok;
}

// 断电复位：P14 低 3 s 再拉高，让模组从冷启动进入已知状态。
// Power reset: hold P14 low for 3 s, then release, so the module boots into a known state.
static void power_reset(uint32_t gen, int want) {
    bt_log("MODULE power reset");
    metalio_io_set(BT_PWR_PIN, false);
    for (int i = 0; i < BT_POWER_RESET_MS / 100 && seq_alive(gen, want); ++i)
        vTaskDelay(pdMS_TO_TICKS(100));
    metalio_io_set(BT_PWR_PIN, true);
}

static void settle(uint32_t gen, int want, int ms) {
    for (int i = 0; i < ms / 100 && seq_alive(gen, want); ++i) vTaskDelay(pdMS_TO_TICKS(100));
    const int rest = ms % 100;
    if (rest && seq_alive(gen, want)) vTaskDelay(pdMS_TO_TICKS(rest));
}

// 连续两次开不了麦就断电复位模组：AT 层卡死时唯一能自愈的路径（与浅睡唤醒同款）。
// Two straight open failures power-reset the module: the only self-rescue when the AT layer
// wedges (the same path a wake after light sleep uses).
static void mic_fail_recover(uint32_t gen) {
    if (++s_mic_fails < 2) return;
    s_mic_fails = 0;
    ESP_LOGW(TAG, "mic open kept failing: power-resetting the module");
    power_reset(gen, WANT_MIC);
    s_first_init = true;  // 下一轮先等启动稳定再发 AT / settle for boot before the next AT round
}

// 地址就是模组的 12 位十六进制，按打印顺序回填。
// The address is the module's own 12 hex digits, echoed back in printed order.
static void connect_send(const uint8_t bda[6]) {
    // 探测期间不发 AT+CONNECT：回话会混进探测窗口，且建链会改状态。
    // No AT+CONNECT during the probe: its replies land inside the probe windows and the link
    // would change state mid-table.
    if (s_probe_busy) return;
    char addr[13];
    char cmd[32];
    bda_text(bda, addr);
    snprintf(cmd, sizeof(cmd), "AT+CONNECT=%s\r\n", addr);
    ESP_LOGI(TAG, "connecting %s", addr);
    // 发出去就当作「这次请求已下发」，别留着让模式任务下一轮再发一遍。
    // rc111 日志 44288 / 44290：audio_begin 的补发和 RELINK 检查在同一轮里各发一条，
    // 模组把后一条当过期请求丢掉，于是每次都要等满 ~6 s 超时才轮到下一次重连——
    // 用户看到的就是「解锁后要等十几秒、甚至搜不到」。
    // Mark the request as sent so the worker's next pass does not repeat it. In rc111 (44288 /
    // 44290) the pending path and the RELINK check both fired in the same pass; the module
    // drops the second one as stale, so every attempt waited out the full ~6 s timeout.
    s_pending_connect = false;
    s_relink_due = false;
    s_link_by_us = true;  // 这条链路由我们发起 / this link is ours
    // 记下发出时刻：回得太快就说明模组在拿旧会话秒回，不是真协商。
    // Stamp the request: an instant answer means the module replayed a stale session.
    s_connect_sent_us = esp_timer_get_time();
    s_connect_pending_us = true;
    cmd_send(cmd);
}

// 进模式 2：发射 / 配对，I2S 音频经模组发给蓝牙耳机。原厂顺序 AT+TX=1 → 700 ms → AT+MODE=2。
// Enter mode 2 (transmit/pair, I2S audio to the headset); vendor order is
// AT+TX=1 → 700 ms → AT+MODE=2.
static bool enter_mode2(uint32_t gen) {
    for (int attempt = 0; attempt < 3 && seq_alive(gen, WANT_MODE2); ++attempt) {
        // 链路已经在了就不要再发 AT+MODE=2：模组冷启动会自己先回连，这时切模式等于
        // 把它正在建的连接踢掉，重来一轮后又留下「连上但没声音」的重复会话。
        // Never send AT+MODE=2 once the link is up: a cold-booted module relinks by itself,
        // and switching modes mid-handshake kicks that connection down and leaves the
        // duplicated "connected but silent" session.
        if (s_link_active) return true;
        if (mode_seq("AT+TX=1\r\n", "AT+MODE=2\r\n", gen, WANT_MODE2)) return true;
        ESP_LOGW(TAG, "mode 2 attempt %d failed", attempt + 1);
        vTaskDelay(pdMS_TO_TICKS(500));
    }
    return false;
}

static void audio_begin(uint32_t gen) {
    if (!s_audio_wanted || !seq_alive(gen, WANT_MODE2)) return;
    bt_log("MODE2 begin first_init=%d settle=%u", (int)s_first_init,
           (unsigned)(s_wake_settle_ms ? s_wake_settle_ms : BT_BOOT_SETTLE_MS));
    // 上电或唤醒后模组可能还在自检，先给它稳定时间；这一段不动 P14。
    // After power-up or wake the module may still be booting; wait without touching P14.
    if (s_first_init) {
        const uint32_t settle_ms = s_wake_settle_ms ? s_wake_settle_ms : BT_BOOT_SETTLE_MS;
        s_wake_settle_ms = 0;
        settle(gen, WANT_MODE2, settle_ms);
        s_first_init = false;
        if (!s_audio_wanted || !seq_alive(gen, WANT_MODE2)) return;
    }

    // 模组断电重启后往往比我们先回连：rc110 日志 176697 唤醒 → 178619 就报 CONNECT SUCCESS，
    // 而我们 179460 才发 AT+MODE=2，1 s 后链路被切掉、再过 4 s 才重新连上——用户看到的就是
    // 「解锁后连上、马上断开、又连上」。CONNECT SUCCESS 只可能出现在模式 2，所以此时不用再切模式，
    // 只补发 AT+TX=1（发射使能，冷启动后必须重发，AQ 章的无声就是这个原因）。
    // The module often relinks before we get there: in rc110 the wake at 176697 was followed by
    // CONNECT SUCCESS at 178619 while AT+MODE=2 only went out at 179460 — the link was cut a
    // second later and came back after four more. That is the user's "connected, dropped, then
    // connected again". CONNECT SUCCESS only happens in mode 2, so skip the switch and just
    // re-send AT+TX=1 (transmit enable, mandatory after a cold boot — the AQ chapter's silence).
    if (s_link_active) {
        s_ready = true;
        ESP_LOGI(TAG, "module already linked, mode 2 kept");
        bt_log("MODE2 light (module already linked)");
        // 发射使能是冷启动后必须重发的幂等命令，而这条捷径没有 mode_seq 那样的应答握手：
        // 单发一次若正好落在模组忙着自连/建 profile 的窗口里就会被丢掉，结果就是
        // 「链路在、I2S 在被模组拉、耳机没声音」（AQ 章）。连发两遍，间隔 120 ms。
        // AT+TX=1 is idempotent and mandatory after a cold boot, but this shortcut has no ack
        // handshake like mode_seq: one shot can land while the module is busy relinking and get
        // dropped, which is exactly "linked, I2S being pulled, no sound" (AQ chapter). Send it
        // twice, 120 ms apart.
        cmd_send("AT+TX=1\r\n");
        vTaskDelay(pdMS_TO_TICKS(120));
        cmd_send("AT+TX=1\r\n");
        bt_log("TX enable x2 (light path)");
        // 这条捷径本身就把发射使能补上了，自连标记可以收掉，免得音乐模式里再补一遍。
        // This shortcut already re-sent the transmit enable, so drop the self-connect mark and let
        // the music-mode sequence stay a single pass.
        s_link_self_connect = false;
        if (s_pending_connect) s_pending_connect = false;  // 已经连上了，不用再连 / already linked
        if (s_music_mode_pending && s_work_sem) xSemaphoreGive(s_work_sem);
        return;
    }

    // 不再给模组断电、也不再在进模式 2 前驻留模式 1：21:31 的现场把这两条路都推翻了。
    // 断电（方案 A，21:27 那版）之后模组冷启动抢连更凶：AT+CONNECT 发出 481 ms 就回
    // `SLC CONNECT SUCCESS`，HFP 抢在 A2DP 前面，正是「模组自连秒回旧会话」的指纹，耳机全程静音。
    // 不断电的「重连」要 1280 ms 才 `CONNECT SUCCESS`，中间还重试过一次，那条才有声音。
    // 所以这里回归最朴素的一次进模式 2；自连秒回由下面 CONNECT SUCCESS 分支识别，
    // 命中后自动补一次干净重连（见 s_rearm_*）。
    // No power cut and no mode-1 dwell before mode 2 any more: the 21:31 field run killed both.
    // After the power cut (Plan A) the cold-booted module races harder: `SLC CONNECT SUCCESS`
    // arrived 481 ms after AT+CONNECT with HFP ahead of A2DP, the fingerprint of a link the
    // module made on its own from a stale session, and the headset stayed silent. The relink
    // without a power cut takes 1280 ms and retries once; that one has sound. So go straight to
    // mode 2 here and let the CONNECT SUCCESS branch spot a fast self-link (s_rearm_*).
    // 不再断电重试：进不去就把错误留给上层，由用户按「重连」。
    // No power-cycle retry any more: report the failure; the user presses "relink".
    const bool ok = enter_mode2(gen);
    if (!ok) {
        ESP_LOGE(TAG, "Bluetooth audio module did not enter mode 2");
        bt_log("MODE2 FAILED");
        // 进不去就把挂起的意图清掉，免得界面一直停在“搜索中”。
        // Drop queued intents on failure so the UI does not sit on “scanning”.
        s_pending_connect = false;
        s_pending_scan = false;
        s_scanning = false;
        return;
    }
    s_ready = true;
    ESP_LOGI(TAG, "module ready (mode 2)");
    bt_log("MODE2 ready link=%d", (int)s_link_active);
    // 链路的 CONNECT SUCCESS 可能早于模式 2 应答到达，那一次音乐模式补发被推迟在这里补上。
    // The link's CONNECT SUCCESS can beat the mode-2 ack; replay the deferred music-mode sequence here.
    if (s_music_mode_pending && s_work_sem) xSemaphoreGive(s_work_sem);
    // 每次开机问一次模组固件版本（官方新固件同款查询）：模组换了固件、行为不同时，
    // 日志第一行就能看出来。应答走 RX 埋点，不认识的格式也不会干扰状态机。
    // Ask for the module version once per boot, the same query the newer vendor firmware uses:
    // when a different module firmware changes behaviour, the log says so up front. The reply
    // rides the RX trace; unknown wording cannot upset the state machine.
    static bool s_ver_asked;
    if (!s_ver_asked) {
        s_ver_asked = true;
        cmd_send("AT+GVER=1\r\n");
    }
    // 搜索/连接请求早于模组就绪：现在补发。
    // Scan/connect requests that arrived before the module was ready go out now.
    if (s_pending_connect) {
        s_pending_connect = false;
        connect_send(s_pending_bda);
        // 搜索紧跟着连接发出去，模组会把连接请求当过期丢掉（rc114 日志 25269 / 25271 两条
        // 只差 2 ms，随后 25 s 才连上）。先让连接走完，再放搜索。
        // A scan right behind AT+CONNECT makes the module drop the connect as stale (rc114:
        // 25269/25271 are 2 ms apart and the link only came up 25 s later). Let the connect
        // go first.
        if (s_pending_scan) vTaskDelay(pdMS_TO_TICKS(600));
    }
    if (s_pending_scan) {
        s_pending_scan = false;
        cmd_send("AT+INQUIRING\r\n");
    }
}

static void audio_leave(uint32_t gen, int want) {
    if (!s_uart_up) return;
    // 没有 AT 断开命令：回模式 1 即让模组拆掉耳机链路。尽力而为，不等应答。
    // There is no AT disconnect: returning to mode 1 drops the headset link. Best effort.
    (void)mode_seq("AT+RX=2\r\n", "AT+MODE=1\r\n", gen, want);
}

// 整条重建模组：与用户「关开蓝牙开关」完全同路——先回模式 1 把僵死链路拆干净，再拆后端，
// 可选断电复位，最后重建后端；模式 2 与回连交给下方 WANT_MODE2 分支接着走完。
// 两种兜底共用它：软重建（不断电，仅重建主机侧 UART/I2S）与硬复位（断电重启模组）。
// End-to-end rebuild, identical to the user's switch toggle: drop to mode 1 to clear the wedged
// link, tear the backend, optionally power-cycle, then rebuild; mode 2 and reconnect are finished
// by the WANT_MODE2 branch below. Both fallbacks share it: a soft rebuild (no power cut) and a
// hard reset (power-cycle the module).

// 播放页「重连」：后台完整重放一次设置页「关 → 开蓝牙开关」，**不断电**。
// The player's "relink": replay the settings switch's off/on by hand, without touching the rail.
//
// 为什么不再断电：P14 一断模组就是冷启动，而它醒来第一件事是按上一条链路抢先自连
// （现场 `RECONNECT DEVICE NAME` 比我们的 `AT+MODE=2` 早到），我们随后的模式切换正好插进
// 它建连到一半，于是两轮建连叠加 → `CONNECT SUCCESS` 之后 1.4 s 就被 `REMOTE DISCON` 拆掉，
// 界面显示已连接、PCM 也在送，耳机全程无声。设置页关开开关之所以有效，正是因为它不断电：
// 模组一直活着，只回模式 1 再回模式 2，连接只会成立一次。
// Why no power cut: cutting P14 cold-boots the module, and its first act on wake-up is to relink
// on its own (`RECONNECT DEVICE NAME` arrives before our `AT+MODE=2`). Our mode switch then lands
// mid-handshake, two link-ups overlap, and 1.4 s after `CONNECT SUCCESS` the module reports
// `REMOTE DISCON`: the UI says connected, PCM keeps flowing, the headset stays silent. The
// settings switch works because it never cuts power — the module stays alive and just moves
// mode 2 -> 1 -> 2, so the link comes up exactly once.
static void module_manual_restart(void) {
    const int want = s_want_state;
    // 探测期间一律不重建：这条路径会 backend_teardown()，UART 一拆，后面的探测命令
    // 连发都发不出去（第三轮就是从 [4/9] 起整段静默，探测表废掉一半）。
    // Never rebuild while the probe runs: this path calls backend_teardown(), and once UART is
    // gone the remaining probe commands are never even transmitted — round 3 went silent from
    // [4/9] onward, which wasted half the table.
    if (s_probe_busy) {
        ESP_LOGW(TAG, "relink refused while AT probe is running");
        return;
    }
    if (!s_audio_wanted || (want != WANT_MODE2 && want != WANT_SPK)) return;
    s_relinking = true;   // 进整条序列：期间再按「重连」直接丢掉 / extra presses are dropped
    ESP_LOGW(TAG, "manual relink (%s, no power cut)", want == WANT_SPK ? "speaker" : "headset");
    // 先自己把链路标成断开：下面那条 AT+MODE=1 会让模组回一条 `SET MODE 1` / `REMOTE DISCON`，
    // 而「断链就关蓝牙」的看门狗正盯着 s_link_active——不先落旗，我们自己发的重连会被自己
    // 判成断链，刚拆完就把蓝牙关了。
    // Mark the link down first: the AT+MODE=1 below makes the module answer `SET MODE 1` /
    // `REMOTE DISCON`, and the "a drop disables Bluetooth" watchdog watches s_link_active. Without
    // this the relink we just started would be judged a drop and Bluetooth would be switched off.
    s_link_active = false;
    s_a2dp_up = false;
    // 「关」那半程：回模式 1 并等模组确认，和 switch off 完全一致。
    // The "off" half: back to mode 1 and wait for the ack, exactly like switching it off.
    if (s_uart_up) {
        const bool left = mode_seq("AT+RX=2\r\n", "AT+MODE=1\r\n", s_seq, want);
        bt_log("MANUAL relink: leave mode 1 ack=%d", (int)left);
    }
    metalio_io_set(BT_PA_PIN, false);
    backend_teardown();  // 内部 s_seq++：下面必须用新的 s_seq
    // 拆完再清一遍标志：回模式 1 期间模组可能又发来断开上报，别让它们影响后续进模式 2。
    // Clear the flags again after the teardown: leaving mode 1 may produce more disconnect
    // reports, and they must not disturb the fresh mode-2 entry below.
    s_ready = false;
    s_feed_go = false;
    s_link_active = false;
    s_link_by_us = false;
    s_link_self_connect = false;
    s_mic_ready = false;
    s_a2dp_up = false;
    s_music_applied = false;
    s_music_mode_pending = false;
    s_sco_ok = false;
    s_sco_retry = 0;
    s_music_retries = 0;
    s_relink_tries = 0;
    // 模组一直没断电，不需要冷启动的稳定时间，重建完立刻进模式序列。
    // The module never lost power, so skip the cold-boot settle and go straight to the sequence.
    s_first_init = false;
    s_wake_settle_ms = 0;
    // 名额已在上面的「开蓝牙」入口发放，这里不再动 / budget is granted by the enable path
    // 「开」那半程：本机扬声器由下面的目标分支自己重建后端，耳机这里直接重建完再补连接。
    // The "on" half: the speaker branch rebuilds the backend itself; for the headset rebuild here
    // and queue the explicit reconnect.
    if (want == WANT_MODE2) {
        if (!backend_start(false)) {
            ESP_LOGE(TAG, "backend restart failed");
            s_relinking = false;
            return;
        }
        static const uint8_t zero_bda[6] = {0};
        if (memcmp(s_link_bda, zero_bda, 6) != 0) {
            memcpy(s_pending_bda, s_link_bda, 6);
            s_pending_connect = true;  // 模式 2 就绪后由 audio_begin 补发 AT+CONNECT
        }
        s_relink_due = false;  // 已用 pending_connect 补连，别再叠加一次 AT+CONNECT
    }
    s_relinking = false;  // 序列跑完，重新接受「重连」/ sequence done, accept relink again
}

void bt_audio_restart_module(void) {
    if (!s_audio_wanted) return;
    if (s_relinking) {
        ESP_LOGW(TAG, "relink ignored: one is already running");
        bt_log("RELINK ignored (busy)");
        return;
    }
    s_force_restart = true;
    if (s_work_sem) xSemaphoreGive(s_work_sem);
}

bool bt_audio_relinking(void) { return s_relinking; }



// 原厂音乐模式时序：AT+BTSCO=0 → 200 ms → AT+PP=1。
// 只在「要耳机出声、模组已在模式 2、链路真的在」时发；200 ms 等待放在模式任务里，
// 不能进 RX 任务，否则那 200 ms 的模组上报会丢。
// Vendor music-mode sequence: AT+BTSCO=0 → 200 ms → AT+PP=1, only while the headset is the
// target, the module sits in mode 2 and the link is really up. The 200 ms wait belongs to the
// mode worker; running it in the RX task would drop whatever the module reports meanwhile.
// 返回是否真发出去了：没发出去的请求不能就此丢掉（唤醒后链路往往比模式 2 先到，
// 早消费会让那一次补发凭空消失，耳机就一直停在 SCO/静音）。
// Returns whether the sequence actually went out: a request that did not go out must not be
// dropped, or a wake where the link lands before mode 2 loses the replay entirely and the
// headset stays on the SCO/silent path.
static bool music_mode_apply(bool with_pp) {
    if (!s_uart_up || !s_audio_wanted || s_want_state != WANT_MODE2) return false;
    if (!s_link_active || !s_ready || !s_a2dp_up) {
        bt_log("MUSIC defer link=%d a2dp=%d ready=%d", (int)s_link_active, (int)s_a2dp_up,
               (int)s_ready);
        return false;
    }
    ESP_LOGI(TAG, "music mode: AT+BTSCO=0%s", with_pp ? " (PP after first audio)" : "");
    bt_log("MUSIC apply pp=%d", (int)with_pp);
    // 自连链路补挂音频引擎：模组自己连上时，它的「I2S → 蓝牙」发送通道往往还没使能，
    // 表现就是链路在、PCM 送得进去（FEED err=ESP_OK）、SCO 也拆了，耳机却一声不出。
    // 这里等链路真正建完（模组刚回 CONNECT SUCCESS，还在建 profile），再补两次 AT+TX=1，
    // 然后照常发 AT+BTSCO=0。用户手动再按一次「重连」恢复的就是这个状态，只是要花 5 s。
    // Arm the audio engine for a self-connected link: when the module links on its own, its
    // I2S → Bluetooth transmit path is often still disabled, which looks like "link up, PCM
    // accepted (FEED err=ESP_OK), SCO torn down, headset silent". Wait for the link to really
    // settle, re-send AT+TX=1 twice, then carry on with AT+BTSCO=0.
    if (s_link_self_connect) {
        s_link_self_connect = false;
        ESP_LOGW(TAG, "self-connected link: re-arming TX before music mode");
        bt_log("ARM TX x2 (self-connect)");
        vTaskDelay(pdMS_TO_TICKS(250));
        cmd_send("AT+TX=1\r\n");
        vTaskDelay(pdMS_TO_TICKS(120));
        cmd_send("AT+TX=1\r\n");
        vTaskDelay(pdMS_TO_TICKS(120));
    }
    s_music_applied = true;
    s_music_applied_us = esp_timer_get_time();
    // 时序重新发出去了，通路确认要重新等（若模组马上回 DISC SCO 会立刻把它置真）。
    // The sequence went out again, so the confirmation is awaited anew (an immediate DISC SCO
    // flips it back).
    s_sco_ok = false;
    cmd_send("AT+BTSCO=0\r\n");
    // AT+PP 不再跟着 BTSCO 立刻发：那一刻播放器往往还没出声，按下去等于把模组按成暂停。
    // 交给 feed 任务，等播放器真的开始供数据（got > 0）那一刻再发；播放器空闲就一直不发。
    // Do not send AT+PP right after BTSCO: the player is usually silent at that moment and the
    // press would pause the module. Let the feed task fire it when the player really feeds data
    // (got > 0), and keep it unsent while the player stays idle.
    if (with_pp) {
        s_pp_wait = true;
        bt_log("PP wait (player idle)");
    }
    return true;
}

static void work_task(void *arg) {
    (void)arg;
    for (;;) {
        if (xSemaphoreTake(s_work_sem, portMAX_DELAY) != pdTRUE) continue;
        for (;;) {
            if (s_suspended) break;
            // 链路刚建立 / 模组报告回落 SCO：补一遍原厂音乐模式时序。
            // Fresh link or a SCO fallback: replay the vendor music-mode sequence.
            if (s_music_mode_pending) {
                // 只有真发出去才清标志：唤醒后链路常比模式 2 先到，早清就再也补不上了。
                // Clear the flag only once the sequence really went out: after a wake the link
                // often lands before mode 2, and clearing early means it never gets replayed.
                const bool with_pp = BT_MUSIC_SEND_PP && s_music_with_pp;
                if (music_mode_apply(with_pp)) s_music_mode_pending = false;
            }
            // AT+PP=1 的时机已到（播放器真的开始供数据）。放在模式任务里发，跟模式序列
            // 天然串行，不用抢 UART。
            // The moment for AT+PP=1 has come (the player really feeds data). It is sent from
            // the mode worker so it stays serialised with the mode sequences.
            if (s_pp_fire) {
                s_pp_fire = false;
                if (s_link_active && s_a2dp_up && s_ready && s_audio_wanted &&
                    s_want_state == WANT_MODE2) {
                    bt_log("PP send (first audio)");
                    cmd_send("AT+PP=1\r\n");
                } else {
                    bt_log("PP drop link=%d a2dp=%d", (int)s_link_active, (int)s_a2dp_up);
                }
            }
            // 播放页「重连」：用户按下时重走一遍「关 → 开蓝牙开关」的干净序列（回模式 1 并等
            // 应答 → 拆后端 → 重建 → 进模式 2 → 补 AT+CONNECT），**不断电**。放在工作线程里是
            // 因为它要阻塞几秒，不能占着 UI 线程。
            // The player's "relink": replay the clean "switch off -> on" sequence (leave mode 1
            // with an ack, rebuild, enter mode 2, reconnect) with no power cut. It runs here
            // because it blocks for a few seconds and must not stall the UI thread.
            if (s_force_restart) {
                s_force_restart = false;
                module_manual_restart();
            }
            // 自连秒回补救：链路是连上了，但模组的 I2S → 蓝牙发送通道没开，耳机静音。
            // 现场验证（21:31）用户手工再按一次「重连」就能出声，这里替他按这一次，且只按一次。
            // Stale-session rescue: the link is up but the module's I2S -> Bluetooth transmit path
            // is dead, so the headset is silent. Field-verified at 21:31: one more user "relink"
            // brings sound back. Press it for them once.
            if (s_rearm_at_us && esp_timer_get_time() >= s_rearm_at_us) {
                s_rearm_at_us = 0;
                if (s_audio_wanted && s_want_state == WANT_MODE2 && s_link_active) {
                    // 22:17 现场否掉了「只重挂音频通路」（BTSCO 1→0）这条路：链路不断、命令也发了，
                    // 耳机仍然静音。说明陈旧会话必须整条重建，1.5 s 轻量方案不成立，回退到
                    // 用户现场验证过的整条重连（= 替他按一次「重连」）。
                    // The 22:17 run killed the lightweight path (BTSCO 1->0): the link stayed up and
                    // the commands went out, yet the headset stayed silent. A stale session has to be
                    // rebuilt end to end, so fall back to the field-verified full relink.
                    ESP_LOGW(TAG, "auto relink: stale-session link has no audio path");
                    bt_log("AUTO relink after fastlink");
                    module_manual_restart();
                }
            }
            const int want = s_want_state;
            if (want == WANT_MODE2) {
                // 已在模式 2 就不重发 AT：重发会打扰已连上的耳机。
                // Skip the AT sequence once mode 2 is up; re-sending disturbs a linked headset.
                if (!s_ready) {
                    // 停在模式 1 时后端已释放，这里按需重建再进模式 2。
                    // The backend was released while sitting in mode 1; rebuild it first.
                    if (!s_uart_up && !backend_start(false)) {
                        ESP_LOGE(TAG, "backend start failed");
                    } else {
                        audio_begin(s_seq);
                    }
                }
            } else if (want == WANT_MIC) {
                // 本地麦克风：不进蓝牙链路，只要模组回模式 1 并把 I2S 收通道打开。
                // Local mic: no Bluetooth link, just mode 1 with the I2S capture path open.
                if (!s_mic_ready && !s_mic_error) {
                    // 端口同一时刻只能有一套通道，先把播放通路整条拆掉再按双工重建。
                    // The port holds one channel set at a time: release playback, rebuild duplex.
                    backend_teardown();
                    if (s_want_state == WANT_MIC) {
                        if (!backend_start(true)) {
                            ESP_LOGE(TAG, "mic backend start failed");
                            s_mic_error = true;
                            mic_fail_recover(s_seq);
                        } else {
                            const uint32_t gen = s_seq;
                            if (s_first_init) {
                                settle(gen, WANT_MIC, BT_BOOT_SETTLE_MS);
                                s_first_init = false;
                            }
                            if (mode_seq("AT+RX=2\r\n", "AT+MODE=1\r\n", gen, WANT_MIC)) {
                                s_mic_ready = true;
                                s_mic_fails = 0;
                                ESP_LOGI(TAG, "module mic ready (mode 1), internal heap %u",
                                         (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
                            } else if (s_want_state == WANT_MIC) {
                                ESP_LOGW(TAG, "mic mode 1 did not answer");
                                s_mic_error = true;
                                mic_fail_recover(gen);
                                backend_teardown();
                            }
                        }
                    }
                }
            } else if (want == WANT_SPK) {
                // 本机扬声器：模组回模式 1（只有它会给 I2S 供时钟），只开 I2S 发通道，
                // 再拉高 NS4150 功放。PCM 仍由 feed_task 从 pcm_pull 拉，和耳机通路同一份数据。
                // Local speaker: the module goes back to mode 1 (the only mode that clocks I2S
                // locally); open the transmit path only, then raise the NS4150 PA. PCM still comes
                // from pcm_pull through feed_task — the same ring as the headset path.
                if (!s_ready) {
                    backend_teardown();
                    if (s_want_state == WANT_SPK) {
                        if (!backend_start(false)) {
                            ESP_LOGE(TAG, "spk backend start failed");
                        } else {
                            const uint32_t gen = s_seq;
                            if (s_first_init) {
                                settle(gen, WANT_SPK, BT_BOOT_SETTLE_MS);
                                s_first_init = false;
                            }
                            // 模式 1 也做有限重试：AT 层偶尔应答丢失，一次失败就静默无声太脆。
                            // 不再断电重启模组（P14 与 4G 共用，且冷启动会让模组抢先自连），
                            // 只把 AT 序列多发几轮。
                            // Mode 1 gets a few retries too: an occasional lost AT reply must not
                            // leave the speaker silent. No module power cycle any more (P14 is
                            // shared with the 4G modem, and a cold boot makes the module race
                            // ahead and relink on its own); just replay the AT sequence.
                            bool ok = false;
                            for (int round = 0; round < 3 && !ok && seq_alive(gen, WANT_SPK); ++round) {
                                if (round) vTaskDelay(pdMS_TO_TICKS(300));
                                for (int attempt = 0; attempt < 2 && seq_alive(gen, WANT_SPK); ++attempt) {
                                    if (mode_seq("AT+RX=2\r\n", "AT+MODE=1\r\n", gen, WANT_SPK)) {
                                        ok = true;
                                        break;
                                    }
                                    vTaskDelay(pdMS_TO_TICKS(500));
                                }
                            }
                            if (ok) {
                                metalio_io_set(BT_PA_PIN, true);
                                s_ready = true;
                                s_feed_go = true;
                                ESP_LOGI(TAG, "module local speaker ready (mode 1)");
                                bt_log("SPK ready (mode 1, PA on)");
                            } else if (s_want_state == WANT_SPK) {
                                ESP_LOGW(TAG, "local speaker mode 1 did not answer");
                                bt_log("SPK failed: no mode-1 ack");
                                backend_teardown();
                            }
                        }
                    }
                }
            } else {
                audio_leave(s_seq, WANT_OFF);
                s_ready = false;
                s_feed_go = false;
                s_mic_ready = false;
                s_music_mode_pending = false;
                s_music_retries = 0;
                s_music_applied = false;
                s_a2dp_up = false;
                s_relink_due = false;
                s_relink_tries = 0;
                s_sco_ok = false;
                s_sco_retry = 0;
                s_pp_wait = false;
                s_pp_fire = false;
                s_music_with_pp = true;
                s_music_applied_us = 0;
                // 模式 1 之后就整条拆掉，把内部内存还给 Wi-Fi。
                // Tear the whole backend down after switching to mode 1 and hand the RAM back to Wi-Fi.
                backend_teardown();
            }
            // 断链后主动补 AT+CONNECT：模组自己的回连在真机日志里连报 CONNECT TIMEOUT，
            // 只靠它等于把耳机交给运气。封顶 BT_RELINK_MAX 次；连上、用户动手、离开模式 2 都清零。
            // Back the module's own relink with one AT+CONNECT: in the field log its retries just
            // kept timing out. Capped at BT_RELINK_MAX; cleared on a link, a user action or
            // leaving mode 2.
            if (s_relink_due) {
                s_relink_due = false;
                static const uint8_t zero_bda[6] = {0};
                if (s_audio_wanted && s_want_state == WANT_MODE2 && s_ready && !s_link_active &&
                    !s_scanning && memcmp(s_link_bda, zero_bda, 6) != 0) {
                    if (s_relink_tries < BT_RELINK_MAX) {
                        s_relink_tries++;
                        bt_log("RELINK try %u/%u", (unsigned)s_relink_tries,
                               (unsigned)BT_RELINK_MAX);
                        connect_send(s_link_bda);
                    } else {
                        bt_log("RELINK budget spent");
                    }
                }
            }
            // 目标没变即办完；若期间有新请求（目标变了）再跑一轮。
            // Done when the target is unchanged; rerun when a new request landed meanwhile.
            if (s_want_state == want) break;
        }
    }
}

// 记下目标状态并唤醒模式任务；状态真变了才推进代数，中止在跑的序列。
// Record the target state and wake the worker; bump the generation only on a real change
// so any in-flight sequence aborts.
static void request_state(int want) {
    if (!s_work_sem) return;
    // 探测占着 AT 总线：先记下目标，探测结束后补发，别让 AT 两路交错。
    // The probe owns the AT bus: remember the target and replay it afterwards.
    if (s_probe_busy) {
        s_probe_want = want;
        return;
    }
    if (s_want_state != want) {
        // 切换目标时留一行：0=关 1=蓝牙耳机 2=麦克风 3=本机扬声器。切到扬声器后没声音时，
        // 先看这行有没有出现、以及后面有没有 “local speaker ready”。
        // One line per retarget (0=off 1=headset 2=mic 3=local speaker). When the speaker stays
        // silent, first check that this fired and that "local speaker ready" follows it.
        ESP_LOGI(TAG, "state %d -> %d", s_want_state, want);
        s_want_state = want;
        s_seq++;
        // 每次切到「耳机」都重新发一个自动补救名额：开关拨一次 = 一次机会，
        // 用来替用户按那一下「重连」（耳机断电重开后第一跳必静音，第二跳才有声）。
        // Grant one rescue budget on every retarget to the headset: one switch-on, one chance to
        // press "relink" for the user (the first link after a headset power cycle is always silent).
        if (want == WANT_MODE2) {
            s_rearm_left = true;
            s_rearm_at_us = 0;
            s_connect_pending_us = false;
            if (s_rearm_timer) esp_timer_stop(s_rearm_timer);
        }
        // 目标变了：先停喂、关功放、清就绪，让下面的分支从头重建通路。
        // 不这样做时，从「本机扬声器」切回「蓝牙耳机」会因 s_ready 仍为真而跳过模式 2 重建，
        // 结果模组还停在模式 1，界面上却显示已切到耳机。
        // The target changed: stop feeding, drop the PA and clear readiness so the branch below
        // rebuilds from scratch. Without this, switching speaker -> headset would keep s_ready
        // true, skip the mode-2 rebuild and leave the module in mode 1 while the UI says headset.
        s_feed_go = false;
        s_ready = false;
        s_mic_ready = false;
        metalio_io_set(BT_PA_PIN, false);
    }
    xSemaphoreGive(s_work_sem);
}

/* ---- I2S / 数据面 ---- */

static esp_err_t i2s_up(bool with_rx) {
    if (s_tx && s_i2s_rx_open == with_rx) return ESP_OK;
    // 方向组合变了：端口同一时刻只能持有一套通道，整条重建。
    // The direction set changed: one channel set per port, so rebuild the whole thing.
    i2s_down();
    const i2s_chan_config_t chan_cfg = {
        .id = BT_I2S_PORT,
        .role = I2S_ROLE_SLAVE,
        .dma_desc_num = 4,
        .dma_frame_num = 240,
        .auto_clear_after_cb = true,
        .auto_clear_before_cb = false,
        .allow_pd = false,
        .intr_priority = 0,
        .tx_destination = I2S_DESTINATION_DMA,
        .rx_destination = I2S_DESTINATION_DMA,
    };
    esp_err_t err = i2s_new_channel(&chan_cfg, &s_tx, with_rx ? &s_rx : NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "I2S channel: %s", esp_err_to_name(err));
        s_tx = NULL;
        s_rx = NULL;
        return err;
    }
    // 从机 + 立体声 32 bit，时钟全部由模组提供（原厂 BTAudioCodecDuplex 同配置）。
    // Slave, stereo 32-bit; every clock comes from the module (same as the vendor codec).
    i2s_std_config_t std_cfg = {0};
    std_cfg.clk_cfg.sample_rate_hz = BT_I2S_RATE;
    std_cfg.clk_cfg.clk_src = I2S_CLK_SRC_DEFAULT;
    std_cfg.clk_cfg.ext_clk_freq_hz = 0;
    std_cfg.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_256;
    std_cfg.slot_cfg.data_bit_width = I2S_DATA_BIT_WIDTH_32BIT;
    std_cfg.slot_cfg.slot_bit_width = I2S_SLOT_BIT_WIDTH_AUTO;
    std_cfg.slot_cfg.slot_mode = I2S_SLOT_MODE_STEREO;
    std_cfg.slot_cfg.slot_mask = I2S_STD_SLOT_BOTH;
    std_cfg.slot_cfg.ws_width = I2S_DATA_BIT_WIDTH_32BIT;
    std_cfg.slot_cfg.ws_pol = false;
    std_cfg.slot_cfg.bit_shift = true;
    std_cfg.slot_cfg.left_align = true;
    std_cfg.slot_cfg.big_endian = false;
    std_cfg.slot_cfg.bit_order_lsb = false;
    std_cfg.gpio_cfg.mclk = I2S_GPIO_UNUSED;
    std_cfg.gpio_cfg.bclk = (gpio_num_t)BT_I2S_BCLK;
    std_cfg.gpio_cfg.ws = (gpio_num_t)BT_I2S_WS;
    std_cfg.gpio_cfg.dout = (gpio_num_t)BT_I2S_DOUT;
    std_cfg.gpio_cfg.din = with_rx ? (gpio_num_t)BT_I2S_DIN : I2S_GPIO_UNUSED;

    err = i2s_channel_init_std_mode(s_tx, &std_cfg);
    if (err == ESP_OK && with_rx) err = i2s_channel_init_std_mode(s_rx, &std_cfg);
    if (err == ESP_OK) err = i2s_channel_enable(s_tx);
    if (err == ESP_OK && with_rx) err = i2s_channel_enable(s_rx);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "I2S init: %s", esp_err_to_name(err));
        i2s_down();
    } else {
        s_i2s_rx_open = with_rx;
    }
    return err;
}

static void i2s_down(void) {
    if (s_rx) {
        (void)i2s_channel_disable(s_rx);
        (void)i2s_del_channel(s_rx);
        s_rx = NULL;
    }
    if (s_tx) {
        (void)i2s_channel_disable(s_tx);
        (void)i2s_del_channel(s_tx);
        s_tx = NULL;
    }
    s_i2s_rx_open = false;
}

static void feed_task(void *arg) {
    (void)arg;
    for (;;) {
        // 拆除时由自己退出，栈与 DMA 之前占的内部内存一并放开。
        // Exit on teardown so the stack returns to the internal heap.
        if (s_feed_quit) {
            s_feed_idle = true;
            s_feed_task_created = false;
            vTaskDelete(NULL);
        }
        const int64_t now = esp_timer_get_time();
        // 蓝牙开启但一直没连上：满 5 分钟闲置自动给模组断电省电。开关保持开，用户再连即重新上电
        // （进蓝牙页自动连 / 点扫描都会走 bt_audio_init）。扫描中不算闲置，连上即清零。
        // Bluetooth on but never linking: after 5 min idle, power the module down to save battery.
        // The switch stays ON; the user reconnecting (entering the BT page or tapping scan) re-powers
        // it via bt_audio_init. Scanning is not idle; linking resets the timer.
        // 只在「等蓝牙耳机」时算闲置；本机扬声器模式本来就没有链路，不能因此断掉模组电，
        // 否则外放放到一半被 5 分钟定时器掐掉。
        // Idle only counts while waiting for a headset; the local-speaker mode has no link by
        // design, and powering the module down there would cut playback after five minutes.
        if (s_audio_wanted && !s_link_active && !s_scanning && s_want_state == WANT_MODE2) {
            if (!s_idle_no_link_us) s_idle_no_link_us = now;
            else if (now - s_idle_no_link_us >= (int64_t)BT_IDLE_CLOSE_MS * 1000) {
                s_idle_no_link_us = 0;
                bt_log("BT idle %u min no link; powering down module + switch off",
                       (unsigned)(BT_IDLE_CLOSE_MS / 60000));
                // 「彻底关闭」：除了给模组断电，还要把设置开关翻成 off，否则界面仍显示开启、
                // 用户以为它还在等着连耳机，实际模组已经断电。
                // "Fully off": power the module down *and* flip the switch off, otherwise the UI
                // still claims Bluetooth is on while the module is already unpowered.
                if (s_auto_disabled_cb) s_auto_disabled_cb();
                bt_audio_deinit();  // 只断模组电，不动设置开关
                vTaskDelay(pdMS_TO_TICKS(30));
                continue;
            }
        } else {
            s_idle_no_link_us = 0;
        }
        // 测试音到时收尾：必须在这里清，否则一块 16 ms 的窗口会无限延长。
        // Retire the test tone here, or a 16 ms window would stretch forever.
        if (s_tone_until_us && now >= s_tone_until_us) {
            s_tone_until_us = 0;
            bt_log("TONE done");
        }
        // 本机扬声器没有蓝牙链路：喂帧只取决于 ready/tx，链路相关的开关（s_feed_go）不参与。
        // 否则模组在「模式 2 → 模式 1」的过程中发来的断开上报（REMOTE DISCON / LINK LOST …）
        // 会走 link_drop() 把 s_feed_go 清掉，于是扬声器一声不响、环填满、解码进度冻住。
        // 这正是「测试音能响、播放器没声」的差别：测试音那条路不经过链路。
        // The local speaker has no Bluetooth link, so feeding depends only on ready/tx; the
        // link-driven flag (s_feed_go) must not participate. Otherwise the disconnect reports the
        // module emits while going mode 2 -> mode 1 (REMOTE DISCON / LINK LOST …) run through
        // link_drop(), which clears s_feed_go — the speaker then stays silent, the ring fills and
        // the decode progress freezes. That is exactly the "tone works, music does not" split.
        const bool local_out = (s_want_state == WANT_SPK);
        if ((!s_feed_go && !local_out) || !s_tx || !s_ready) {
            // 进入闲置的那一刻留一行：链路说连着却停喂，是“显示连上没声音”的直接现场。
            // One line at the moment feeding stops: "linked but idle" is the silent-link scene.
            if (!s_feed_idle && s_link_active)
                bt_log("FEED idle go=%d tx=%d ready=%d link=%d", (int)s_feed_go, (int)(s_tx != NULL),
                       (int)s_ready, (int)s_link_active);
            s_feed_idle = true;
            // 链路说连着，喂帧条件却缺一个（s_ready / s_tx / s_feed_go）——结果同样是没声音。
            // Linked while a feed condition is missing (s_ready / s_tx / s_feed_go): also silent.
            if (s_link_active) {
                if (!s_feed_idle_us) {
                    s_feed_idle_us = now;
                } else if (now - s_feed_idle_us > (int64_t)BT_FEED_IDLE_MS * 1000) {
                    s_feed_idle_us = 0;
                    audio_watchdog_fire("feed idle");
                }
            } else {
                s_feed_idle_us = 0;
            }
            vTaskDelay(pdMS_TO_TICKS(30));
            continue;
        }
        s_feed_idle = false;
        s_feed_idle_us = 0;

        int32_t got = 0;
        bt_pcm_pull_cb_t pull = s_pull;
        // 测试音优先：播放器同时也在放的时候，用它盖住播放器的数据，才算隔离实验。
        // The tone wins: when the player is also feeding, masking it is what makes this a
        // controlled experiment.
        const bool tone = (s_tone_until_us != 0 && now < s_tone_until_us);
        if (tone) {
            uint8_t ph = s_tone_phase;
            for (int i = 0; i < FEED_FRAMES; ++i) {
                const int16_t v = k_tone_lut[ph];
                s_pcm_in[i * 2] = v;
                s_pcm_in[i * 2 + 1] = v;
                ph = (uint8_t)((ph + 1) & (BT_TONE_HZ_SAMPLES - 1));
            }
            s_tone_phase = ph;
            got = FEED_FRAMES * 4;
        } else if (pull) {
            got = pull((uint8_t *)s_pcm_in, FEED_FRAMES * 4);
        }
        if (got < 0) got = 0;
        if (got > FEED_FRAMES * 4) got = FEED_FRAMES * 4;
        got &= ~3;
        // 欠载补静音：I2S 时钟不停，宁可持续送零也不让模组饿着。
        // Pad silence on underrun: the I2S clock never stops, so feed zeros instead of starving it.
        if (got < FEED_FRAMES * 4)
            memset((uint8_t *)s_pcm_in + got, 0, (size_t)(FEED_FRAMES * 4) - (size_t)got);

        // 测试音不乘软件音量：诊断必须听得见，用户把音量调得很低时它仍应是满幅。
        // The tone skips the software volume: a diagnostic has to be audible even when the
        // user left the volume low.
        const int32_t factor = tone ? 65536 : vol_factor(s_volume);
        int32_t peak = 0;  // 本块输入峰值（未乘音量）/ block peak before the software gain
        for (int i = 0; i < FEED_FRAMES; ++i) {
            // 环是 16 kHz 立体声交织：左右分别扩到 32 bit 槽。
            // 若按单声道取样会把左右交错塞进同一耳朵并丢掉 3/4 数据 → 杂音+音调错。
            // The ring is interleaved 16 kHz stereo: widen L and R into the 32-bit slots.
            // A mono-style read interleaves channels into one ear and drops 3/4 of the
            // data, which sounds like noise at the wrong pitch.
            const int32_t raw_l = s_pcm_in[i * 2];
            const int32_t raw_r = s_pcm_in[i * 2 + 1];
            const int32_t a_l = raw_l < 0 ? -raw_l : raw_l;
            const int32_t a_r = raw_r < 0 ? -raw_r : raw_r;
            if (a_l > peak) peak = a_l;
            if (a_r > peak) peak = a_r;
            const int32_t l = raw_l * factor;
            const int32_t r = raw_r * factor;
            s_pcm_out[i * 2] = l;
            s_pcm_out[i * 2 + 1] = r;
        }
        if ((uint32_t)peak > s_stat_peak) s_stat_peak = (uint32_t)peak;

        // AT+PP 的时机到了没有：播放器真的开始供数据（got > 0）才算。
        // 用 got 而不是 peak：静音段（曲目开头、淡入）peak 仍可能为 0，但数据已经在流，
        // 该发就得发。反过来，用户还没点播放时 got 恒为 0，一条 PP 都不会发——那才是对的，
        // 原厂那种「链路一建立就按一下」在播放器空闲时发的是一条没有意义的命令。
        // Field log (reset 3 段) 17428 就是这种：播放器还停在 IDLE，PP 按下去之后 2.5 s
        // 起 I2S 写入开始 ESP_ERR_TIMEOUT，最后触发一次整条重建。
        // Is it time for AT+PP: only once the player really feeds data (got > 0). Use got rather
        // than peak so a silent-but-flowing block still counts; conversely nothing is sent while
        // the player is idle, which is exactly right. The field log shows the vendor-style
        // "press on every link" firing into an idle player, with I2S write timeouts right after.
        if (s_pp_wait && BT_MUSIC_SEND_PP && got > 0) {
            s_pp_wait = false;
            s_pp_fire = true;
            if (s_work_sem) xSemaphoreGive(s_work_sem);
        }
        // SCO 兜底：A2DP 已通、音频在送，却始终没等到 DISC SCO —— 模组可能还挂在 SCO 上。
        // 补一次 BTSCO=0（不重发 PP），等于把用户「手动重连一下就好了」自动做掉。
        // SCO fallback: A2DP is up and audio flows, yet DISC SCO never came — the module may
        // still sit on SCO. Re-send BTSCO=0 (without PP), automating the user's manual relink.
        if (s_a2dp_up && s_music_applied && !s_sco_ok && got > 0 && s_music_applied_us &&
            (now - s_music_applied_us) > (int64_t)BT_SCO_FALLBACK_MS * 1000 &&
            s_sco_retry < BT_SCO_RETRY_MAX) {
            s_sco_retry++;
            s_music_applied_us = now;  // 下一次从这个时刻重新计时 / restart the window
            s_music_with_pp = false;
            s_music_mode_pending = true;
            bt_log("MUSIC reapply (no DISC SCO) %u/%u", (unsigned)s_sco_retry,
                   (unsigned)BT_SCO_RETRY_MAX);
            if (s_work_sem) xSemaphoreGive(s_work_sem);
            if (s_sco_retry >= BT_SCO_RETRY_MAX) {
                // 兜底的 BTSCO=0 也补完两次了，模组仍在拉 I2S 却始终没切到 A2DP（没有
                // `DISC SCO`）。这就是「连上、进度在走、耳机没声音」的形态：软重发已经证明
                // 没用，直接升级成硬复位（断电重启模组）；硬复位预算也耗尽就关闭蓝牙。
                // Both BTSCO=0 retries are spent and the module still never reports `DISC SCO`
                // while pulling I2S: "linked, progress moving, no sound". Re-sending proved
                // useless, so escalate to a hard reset; if even that budget is spent, disable BT.
                // 两次 BTSCO=0 都补完仍没有 DISC SCO：模组在拉 I2S 却没切到 A2DP，按策略关蓝牙。
                // Both BTSCO=0 retries spent with no DISC SCO: the module pulls I2S but never
                // switches to A2DP. Turn Bluetooth off.
                bt_log("SCO fallback spent -> disable BT");
                audio_watchdog_fire("no DISC SCO after fallback");
                if (s_work_sem) xSemaphoreGive(s_work_sem);
            }
        }

        size_t written = 0;
        const esp_err_t err = i2s_channel_write(s_tx, s_pcm_out, sizeof(s_pcm_out), &written,
                                                pdMS_TO_TICKS(300));
        // 每 2 s 一行数据面统计：没有串口时，这是「PCM 到底有没有送进模组」的唯一证据。
        // One line every 2 s: without a console this is the only proof PCM actually reached the module.
        static int64_t s_stat_us;
        if (!s_stat_us || now - s_stat_us >= 2000000) {
            s_stat_us = now;
            // pk 是这 2 s 里输入 PCM 的峰值：pk=0 说明播放器在送静音（暂停 / 解码没出数据），
            // pk 有值却听不到歌，才轮到怀疑数据内容或模组侧的 A2DP 通路。
            // pk is the peak input PCM over the last 2 s: pk=0 means the player is feeding
            // silence; a real pk with no music points at the content or the module's A2DP path.
            bt_log("FEED got=%d/%d write=%u/%u err=%s vol=%u pk=%u", (int)got,
                   (int)(FEED_FRAMES * 4), (unsigned)written, (unsigned)sizeof(s_pcm_out),
                   esp_err_to_name(err), (unsigned)s_volume, (unsigned)s_stat_peak);
            // 同一份统计也交给串口：卡不在、没开 bt_trace 时，这是唯一能看到 I2S 到底写没写进去
            // 的窗口。写不满就是模组没在供时钟（模式没生效或功放没开），声音自然没有、进度也停。
            // Mirror the same counters to the console: without a card or bt_trace this is the only
            // window onto whether the I2S write actually landed. A short write means the module is
            // not clocking I2S (mode or PA not really on), which shows up as silence plus a frozen
            // progress bar.
            if (!(err == ESP_OK && written == sizeof(s_pcm_out))) {
                ESP_LOGW(TAG, "FEED short write: got=%d write=%u/%u err=%s state=%d",
                         (int)got, (unsigned)written, (unsigned)sizeof(s_pcm_out),
                         esp_err_to_name(err), s_want_state);
            } else {
                ESP_LOGD(TAG, "FEED got=%d write=%u/%u err=%s state=%d pk=%u", (int)got,
                         (unsigned)written, (unsigned)sizeof(s_pcm_out), esp_err_to_name(err),
                         s_want_state, (unsigned)s_stat_peak);
            }
            s_stat_peak = 0;
        }
        const bool full = (err == ESP_OK && written == sizeof(s_pcm_out));
        if (full) {
            // 只有「真数据 + 真写进去」才记这条时刻：就绪判据靠它区分「在播但音频通路没
            // 成立」与「本来就没在播」——后者不该被报成故障。
            // Only real data that actually went in updates this stamp: the readiness check uses
            // it to tell "playing but the audio path never came up" from "nothing is playing",
            // and the latter must not be reported as a fault.
            if (got > 0) s_last_feed_us = now;
            // 数据面正常：记下这次连写成功的起点，就绪判据用得上。
            // Healthy block: stamp the start of the current good run (used by readiness checks).
            if (!s_tx_ok_from_us) s_tx_ok_from_us = now;
            s_tx_bad_us = 0;
        } else {
            ++s_tx_err_count;
            s_tx_ok_from_us = 0;
            ESP_LOGD(TAG, "i2s write: %s (%u/%u)", esp_err_to_name(err), (unsigned)written,
                     (unsigned)sizeof(s_pcm_out));
            // 只有“确实在供数据”（got > 0）时才判死：暂停时补的静音块写不进去不代表故障。
            // Only judge while real audio is flowing (got > 0); a silent block failing during a
            // pause is not a fault.
            if (got > 0) {
                if (!s_tx_bad_us) {
                    s_tx_bad_us = now;
                } else if (now - s_tx_bad_us > (int64_t)BT_TX_STALL_MS * 1000) {
                    audio_watchdog_fire("i2s stall");
                }
            } else {
                s_tx_bad_us = 0;
            }
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }
}

static bool backend_start(bool with_rx) {
    s_rx_quit = false;
    s_feed_quit = false;
    if (!s_uart_up) {
        const uart_config_t cfg = {
            .baud_rate = BT_UART_BAUD,
            .data_bits = UART_DATA_8_BITS,
            .parity = UART_PARITY_DISABLE,
            .stop_bits = UART_STOP_BITS_1,
            .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
            .source_clk = UART_SCLK_DEFAULT,
        };
        if (uart_driver_install(BT_UART, BT_UART_RX_BUF, 0, 0, NULL, 0) != ESP_OK) {
            ESP_LOGE(TAG, "UART driver install failed");
            return false;
        }
        (void)uart_param_config(BT_UART, &cfg);
        (void)uart_set_pin(BT_UART, BT_UART_TX_GPIO, BT_UART_RX_GPIO, UART_PIN_NO_CHANGE,
                           UART_PIN_NO_CHANGE);
        s_uart_up = true;
    }
    if (!s_rx_task_created) {
        if (xTaskCreate(rx_task, "bt_rx", RX_TASK_STACK, NULL, 5, NULL) == pdPASS) {
            s_rx_task_created = true;
        } else {
            ESP_LOGE(TAG, "rx task create failed");
            return false;
        }
    }
    if (i2s_up(with_rx) != ESP_OK) return false;
    // 麦克风模式不建喂帧任务：那条通路只收不发，多一个任务只是白占内部栈。
    // No feed task in mic mode: that path only captures, so a task would just burn internal stack.
    if (!with_rx && !s_feed_task_created) {
        if (xTaskCreatePinnedToCore(feed_task, "bt_feed", FEED_TASK_STACK, NULL, FEED_TASK_PRIO,
                                    NULL, FEED_TASK_CORE) == pdPASS) {
            s_feed_task_created = true;
        } else {
            ESP_LOGE(TAG, "feed task create failed");
            return false;
        }
    }
    bt_log("BACKEND up rx=%d heap=%u", (int)with_rx, (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    return true;
}

static void backend_teardown(void) {
    s_feed_go = false;
    s_ready = false;
    // 拆除时把测试音一起收掉：否则浅睡醒来重建完，那一块残留窗口会突然响一下。
    // Drop the test tone with the backend: otherwise a leftover window beeps right after a
    // wake rebuild.
    s_tone_until_us = 0;
    s_link_active = false;
    s_scanning = false;
    s_mic_ready = false;
    s_tx_bad_us = 0;
    s_feed_idle_us = 0;
    s_tx_ok_from_us = 0;
    s_link_up_us = 0;
    metalio_io_set(BT_PA_PIN, false);
    s_seq++;
    // 先让喂帧任务退出：它可能正卡在 i2s_channel_write 里，等这一次写完。
    // Let the feed task leave first; it may be parked inside i2s_channel_write.
    s_feed_quit = true;
    for (int i = 0; i < 50 && s_feed_task_created; ++i) vTaskDelay(pdMS_TO_TICKS(20));
    s_rx_quit = true;
    for (int i = 0; i < 25 && s_rx_task_created; ++i) vTaskDelay(pdMS_TO_TICKS(20));
    s_feed_quit = false;
    s_rx_quit = false;
    // 音频闲置时把 I2S 与 UART 一并还给内部堆：Wi-Fi 的接收缓冲就缺这块连续内存。
    // Return I2S and UART to the internal heap while audio is idle: Wi-Fi RX buffers need it.
    i2s_down();
    if (s_uart_up) {
        uart_driver_delete(BT_UART);
        s_uart_up = false;
    }
    ESP_LOGI(TAG, "backend released, internal heap %u",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA));
    bt_log("BACKEND down heap=%u", (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
}

/* ---- 协议栈生命周期 / Stack lifecycle ---- */

static esp_err_t stack_up_locked(void) {
    if (s_stack_up) return ESP_OK;
    esp_bt_controller_config_t cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    esp_err_t err = esp_bt_controller_init(&cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "controller init: %s", esp_err_to_name(err));
        return err;
    }
    // 经典蓝牙已移出（音频走外置模组），控制器只开 BLE。
    // Classic BR/EDR is gone (audio runs on the external module), so enable BLE only.
    err = esp_bt_controller_enable(ESP_BT_MODE_BLE);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "controller enable: %s", esp_err_to_name(err));
        esp_bt_controller_deinit();
        return err;
    }
    s_stack_up = true;
    err = bt_ble_host_start();
    if (err != ESP_OK) ESP_LOGW(TAG, "ble adapter start: %s", esp_err_to_name(err));
    ESP_LOGI(TAG, "host up (%u users)", (unsigned)s_users);
    return ESP_OK;
}

static void stack_down_locked(void) {
    if (!s_stack_up) return;
    s_stack_up = false;
    bt_ble_host_stop();
    esp_bt_controller_disable();
    esp_bt_controller_deinit();
    ESP_LOGI(TAG, "host down");
}

/* ---- 对外接口 / Public API ---- */

// 这把锁不只是给“协议栈用户”用的：睡眠挂起/恢复也靠它串行化，而音乐这条路只叫
// bt_audio_init()、从不叫 bt_host_start()，于是 s_lock 一直是 NULL，
// bt_host_suspend_for_sleep()/resume_after_sleep() 在 `if (!s_lock) return;` 处静默返回——
// 浅睡切断 P14 后模组等于冷启动，却没人给它重发 AT+TX=1/AT+MODE=2，耳机能连上、I2S 也在走，
// 就是没声音。把创建口子提到这里，两条入口都保证锁存在。
// This lock is not only for stack users: sleep suspend/resume serialize on it too. The music
// path only calls bt_audio_init() and never bt_host_start(), so s_lock stayed NULL and both
// sleep hooks returned silently. Light sleep cuts P14 (the module cold-boots) yet nobody
// re-sent AT+TX=1 / AT+MODE=2: the headset links, I2S keeps clocking, and there is no sound.
static bool lock_ensure(void) {
    if (!s_lock) s_lock = xSemaphoreCreateRecursiveMutex();
    return s_lock != NULL;
}

esp_err_t bt_host_start(void) {
    if (!lock_ensure()) return ESP_ERR_NO_MEM;
    xSemaphoreTakeRecursive(s_lock, portMAX_DELAY);
    esp_err_t err = ESP_OK;
    if (s_users == 0 && !s_suspended) err = stack_up_locked();
    if (err == ESP_OK) s_users++;
    xSemaphoreGiveRecursive(s_lock);
    return err;
}

esp_err_t bt_host_stop(void) {
    if (!s_lock) return ESP_OK;
    xSemaphoreTakeRecursive(s_lock, portMAX_DELAY);
    if (s_users) s_users--;
    if (s_users == 0 && s_stack_up) stack_down_locked();
    xSemaphoreGiveRecursive(s_lock);
    return ESP_OK;
}

bool bt_host_running(void) { return s_stack_up; }
uint32_t bt_host_users(void) { return s_users; }

void bt_host_suspend_for_sleep(void) {
    if (!lock_ensure()) {
        bt_log("SUSPEND skip: no lock");
        ESP_LOGE(TAG, "suspend without lock; sleep hooks are dead");
        return;
    }
    xSemaphoreTakeRecursive(s_lock, portMAX_DELAY);
    if (!s_suspended) {
        s_suspended = true;
        bt_log("SUSPEND users=%u audio=%d ready=%d link=%d uart=%d", (unsigned)s_users,
               (int)s_audio_wanted, (int)s_ready, (int)s_link_active, (int)s_uart_up);
        // 浅睡会切断 P14，模组等于重新上电，唤醒后只需再等它启动——但断电之前必须先像
        // 用户「关掉蓝牙开关」那样让它退回模式 1。模组把模式记在自己的配置里：带着一条
        // 半死的耳机链路断电，醒来它会抢在我们前面反复 CONNECT TIMEOUT / 自己回连，
        // 实测这一抢跑就要多花十几秒，还会留下「连上了但没声音」的状态。
        // 手动关开开关之所以一次就好，靠的就是关的那一半先发了 AT+RX=2 → AT+MODE=1。
        // Light sleep cuts P14 so the module reboots; but before that, put it back into mode 1
        // exactly like switching the Bluetooth switch off does. The module keeps its mode in its
        // own config, so powering it down mid-link makes it wake up racing us with repeated
        // CONNECT TIMEOUT / self-relinks — tens of seconds plus the "connected but silent" state.
        // The switch toggle works because its "off" half sends AT+RX=2 → AT+MODE=1 first.
        if (s_audio_wanted && s_uart_up) {
            bt_log("SUSPEND leave (AT+RX=2 -> AT+MODE=1)");
            cmd_send("AT+RX=2\r\n");
            vTaskDelay(pdMS_TO_TICKS(200));
            // AT+MODE=1 必须等模组真的回 `SET MODE 1` 再断电。实测回显要 660~1600 ms
            // （rc114 日志 52431→53187 只等到 REMOTE DISCON，`SET MODE 1` 直到唤醒后 62017
            // 才出现），而旧代码固定等 400 ms 就切电源——模组没把「模式 1」写进自己的配置，
            // 醒来仍按模式 2 抢先自连，于是我们这边还没发 AT+MODE=2 它已经 CONNECT TIMEOUT
            // 好几轮了。等不到就再补一条，最后留 150 ms 让它把切换写完。
            // Wait for the module's `SET MODE 1` before cutting the rail. The echo takes
            // 660-1600 ms in the field (rc114: 52431 only reached REMOTE DISCON; `SET MODE 1`
            // showed up at 62017, after the wake), while the old code cut power after a fixed
            // 400 ms — mode 1 never reached the module's config, so it woke up relinking in
            // mode 2 and burned several CONNECT TIMEOUT rounds before we even sent AT+MODE=2.
            // Re-send once if it stays silent, then leave 150 ms for the switch to settle.
            s_mode_ack = false;
            cmd_send("AT+MODE=1\r\n");
            const int64_t ack_deadline = esp_timer_get_time() + BT_SUSPEND_MODE_ACK_MS * 1000;
            while (!s_mode_ack && esp_timer_get_time() < ack_deadline)
                vTaskDelay(pdMS_TO_TICKS(20));
            bt_log("SUSPEND mode1 ack=%d", (int)s_mode_ack);
            if (!s_mode_ack) {
                cmd_send("AT+MODE=1\r\n");
                vTaskDelay(pdMS_TO_TICKS(400));
            }
            vTaskDelay(pdMS_TO_TICKS(150));  // 让它把模式切换写完再断电 / finish the switch before power drops
        }
        backend_teardown();
        s_first_init = true;
        s_wake_settle_ms = BT_WAKE_SETTLE_MS;
        stack_down_locked();
    }
    xSemaphoreGiveRecursive(s_lock);
}

void bt_host_resume_after_sleep(void) {
    if (!lock_ensure()) return;
    xSemaphoreTakeRecursive(s_lock, portMAX_DELAY);
    if (s_suspended) {
        s_suspended = false;
        if (s_users) stack_up_locked();
        if (s_audio_wanted) {
            // 浅睡切掉 P14，模组等于冷启动：模式（SET MODE 2）还留在它的配置里，AT+TX=1
            // （发射使能）、音量与耳机链路全没了。所以这里必须把冷启动那套完整重走一遍：
            // 重建 UART/I2S/任务 → AT+TX=1 → AT+MODE=2 → AT+CONNECT 回连。
            // 少了 AT+TX=1 就是实机看到的“耳机显示已连接、I2S 也在被模组拉、但没声音”。
            // Light sleep cuts P14, so the module cold-boots: it keeps the mode in its own
            // config, but AT+TX=1 (transmit enable), volume and the headset link are gone.
            // Replay the full cold-boot path here: rebuild UART/I2S/tasks, AT+TX=1,
            // AT+MODE=2, then AT+CONNECT. Skipping AT+TX=1 is exactly the observed
            // "headset says connected, module keeps clocking I2S, yet no sound".
            s_seq++;            // 中止睡眠前残留的序列 / abort any sequence left from before sleep
            const bool had_link = s_link_active;  // 只用于日志 / log-only
            s_ready = false;
            s_feed_go = false;
            s_link_active = false;
            s_a2dp_up = false;
            s_music_mode_pending = false;
            s_music_retries = 0;
            s_music_applied = false;
            s_relink_due = false;
            s_relink_tries = 0;
            s_sco_ok = false;
            s_sco_retry = 0;
            s_pp_wait = false;
            s_pp_fire = false;
            s_music_with_pp = true;
            s_music_applied_us = 0;
            s_feed_idle_us = 0;
            s_tx_bad_us = 0;
            s_link_up_us = 0;
            s_last_feed_us = 0;  // 唤醒后模组是新的一台，旧数据流时刻作废 / fresh module, stale stamp dropped
            bt_log("RESUME audio=%d users=%u was_link=%d uart=%d", (int)s_audio_wanted,
                   (unsigned)s_users, (int)had_link, (int)s_uart_up);
            // 用户实测：出问题后手动关开一次蓝牙开关必定恢复，而增量补丁时好时坏。所以唤醒
            // 不再赌模组能从电源跌落里自己爬起来，而是照手动那条路的顺序重走一遍冷启动：
            // 重建 UART/I2S/任务 → AT+TX=1 → AT+MODE=2 → AT+CONNECT。让模组醒来时安静地
            // 停在模式 1 的那一步放在入睡侧（见 bt_host_suspend_for_sleep 的 AT+MODE=1）。
            // 这里不碰 P14：它与屏电共用，解锁瞬间断电会闪屏，且浅睡已经切断过它了。
            // Field-verified: toggling the switch always recovered it, incremental patches only
            // sometimes. So replay the manual path's cold boot here (rebuild UART/I2S/tasks →
            // AT+TX=1 → AT+MODE=2 → AT+CONNECT). Making the module boot into mode 1 instead of
            // racing us is done on the sleep side (AT+MODE=1 in bt_host_suspend_for_sleep).
            // P14 is left alone here: it is shared with the panel rail, and light sleep already
            // cut it; dropping it during unlock would blink the screen.
            // 先把 UART 建起来，模式任务才发得出 AT+TX=1。
            // Bring UART up first so the mode worker can actually send AT+TX=1.
            if (!s_uart_up && !backend_start(false)) {
                ESP_LOGE(TAG, "backend start failed after sleep");
                bt_log("RESUME backend failed");
            } else if (s_want_state == WANT_SPK) {
                // 睡前走的是本机扬声器：唤醒后重新进模式 1 并开功放，不去回连耳机。
                // Woke up in local-speaker mode: re-enter mode 1 and raise the PA; no headset relink.
                request_state(WANT_SPK);
            } else {
                // 记得上一次连的耳机就主动回连：只等模组自己扫，实测要 ~20 s。
                // Re-connect the remembered headset: waiting for the module's own retry took ~20 s
                // in the field log.
                static const uint8_t zero_bda[6] = {0};
                if (memcmp(s_link_bda, zero_bda, 6) != 0) {
                    memcpy(s_pending_bda, s_link_bda, 6);
                    s_pending_connect = true;
                }
                request_state(WANT_MODE2);
            }
        } else {
            bt_log("RESUME idle audio=0 users=%u", (unsigned)s_users);
        }
    }
    xSemaphoreGiveRecursive(s_lock);
}

bool bt_host_suspended(void) { return s_suspended; }

/* ---- 音频模组（原 A2DP source 的同一套 API）/ Module audio (same API as the old A2DP) ---- */

// 模式任务与它用的两把锁：听音乐和语音翻页都要用，谁先来谁建。
// The mode worker and its two locks: both music and voice page-turn need them, first caller builds.
static bool s_mode_task_up;

static bool mode_worker_ensure(void) {
    if (!s_cmd_lock) s_cmd_lock = xSemaphoreCreateMutex();
    if (!s_work_sem) s_work_sem = xSemaphoreCreateBinary();
    if (!s_cmd_lock || !s_work_sem) return false;
    if (!s_rearm_timer) {
        const esp_timer_create_args_t args = {
            .callback = rearm_timer_cb,
            .name = "bt_rearm",
        };
        esp_timer_create(&args, &s_rearm_timer);  // 失败也不致命：补救只是不会被触发
    }
    if (!s_mode_task_up) {
        if (xTaskCreate(work_task, "bt_mode", WORK_TASK_STACK, NULL, WORK_TASK_PRIO, NULL) !=
            pdPASS) {
            ESP_LOGE(TAG, "mode task create failed");
            return false;
        }
        s_mode_task_up = true;
    }
    return true;
}

// 音频通路的统一入口：want 决定去蓝牙耳机（WANT_MODE2）还是本机扬声器（WANT_SPK）。
// One entry point for the audio path: want picks the headset (WANT_MODE2) or the local speaker
// (WANT_SPK).
static esp_err_t audio_start(bt_pcm_pull_cb_t pull, int want) {
    if (!mode_worker_ensure()) return ESP_ERR_NO_MEM;
    // 没有这把锁，睡眠挂起/恢复就是两个空函数（见 lock_ensure 的说明）。
    // Without this lock the sleep suspend/resume hooks are no-ops (see lock_ensure).
    if (!lock_ensure()) return ESP_ERR_NO_MEM;
    bt_log(want == WANT_SPK ? "AUDIO init (local speaker)" : "AUDIO init");

    if (s_audio_wanted) {
        // 已在音频状态：只切目标，模式任务会重建对应通路。
        // Already running: just retarget; the mode worker rebuilds the matching path.
        s_pull = pull;
        // 扬声器 → 耳机：模组压着自己的 A2DP/SCO 状态，软切不回来，进模式 2 前断电重启。
        // Speaker -> headset: the module holds its own A2DP/SCO state and a soft switch does not
        // clear it, so power-cycle before entering mode 2.
        request_state(want);
        return ESP_OK;
    }

    s_pull = pull;
    s_audio_wanted = true;
    s_volume = 100;
    s_scanning = false;
    s_link_active = false;
    s_link_by_us = false;
    s_link_self_connect = false;
    s_rearm_left = true;   // 新一次「开蓝牙」：给一次自动补救重连名额 / one auto-relink budget
    s_rearm_at_us = 0;
    s_connect_pending_us = false;
    s_link_name[0] = 0;
    s_pending_connect = false;
    s_pending_scan = false;
    s_music_applied = false;
    s_music_mode_pending = false;
    s_music_retries = 0;
    s_a2dp_up = false;
    s_relink_due = false;
    s_relink_tries = 0;
    s_sco_ok = false;
    s_sco_retry = 0;
    s_pp_wait = false;
    s_pp_fire = false;
    s_music_with_pp = true;
    s_music_applied_us = 0;
    s_tx_ok_from_us = 0;
    s_tx_bad_us = 0;
        s_feed_idle_us = 0;
        s_link_up_us = 0;
        s_idle_no_link_us = 0;  // 重新计时，避免旧时间戳触发立刻断电
    s_seq++;
    metalio_io_set(BT_PWR_PIN, true);  // 模组电源常开，复位由 mode 任务做
    if (!backend_start(false)) {
        s_audio_wanted = false;
        backend_teardown();
        return ESP_FAIL;
    }
#if BT_AT_PROBE
    // 探测任务跑完自己调 request_state；这里的 request_state 会被 s_probe_busy 挡掉并排队。
    // The probe task replays request_state itself; the call below is parked by s_probe_busy.
    s_probe_busy = true;
    s_probe_want = -1;
    if (xTaskCreate(at_probe_task, "at_probe", 3072, (void *)(intptr_t)want, 2, NULL) == pdPASS) {
        return ESP_OK;
    }
    s_probe_busy = false;
#endif
    request_state(want);
    return ESP_OK;
}

// 蓝牙耳机输出（原有的行为）。/ Headset output, the original behaviour.
esp_err_t bt_audio_init(bt_pcm_pull_cb_t pull) { return audio_start(pull, WANT_MODE2); }

// 本机扬声器输出（不连蓝牙）：模组进模式 1 供 I2S 时钟，开 NS4150 功放。
// Local speaker output (no Bluetooth): the module runs mode 1 to clock I2S, NS4150 PA on.
esp_err_t bt_audio_init_local(bt_pcm_pull_cb_t pull) { return audio_start(pull, WANT_SPK); }

// deinit 只发请求；工作线程还要跑完 AT 序列才会拆 UART/I2S 与任务。
// 需要“现在就还内存”的调用方（开热点）用这个等它真拆完。
// deinit only queues the request; the worker still has to finish the AT sequence before it
// tears down UART/I2S and the tasks. Callers that need the RAM back now (hotspot start) wait here.
bool bt_audio_wait_released(uint32_t timeout_ms) {
    for (uint32_t waited = 0; waited < timeout_ms; waited += 20) {
        if (!s_audio_wanted && !s_ready && !s_uart_up && !s_feed_task_created && !s_rx_task_created)
            return true;
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    ESP_LOGW(TAG, "audio backend still up after %u ms", (unsigned)timeout_ms);
    return false;
}

void bt_audio_set_auto_disabled_cb(void (*cb)(void)) {
    s_auto_disabled_cb = cb;
}

void bt_audio_deinit(void) {
    if (!s_audio_wanted) return;
    bt_log("AUDIO deinit");
    s_audio_wanted = false;
    s_ready = false;
    s_feed_go = false;
    s_link_active = false;
    s_idle_no_link_us = 0;  // 模组已断电，闲置计时作废
    s_a2dp_up = false;
    s_scanning = false;
    s_pending_connect = false;
    s_pending_scan = false;
    s_relink_due = false;
    s_relink_tries = 0;
    s_link_name[0] = 0;
    s_seq++;
    metalio_io_set(BT_PA_PIN, false);
    request_state(WANT_OFF);
}

bool bt_audio_ready(void) { return s_ready; }

// 用户显式动作（连接 / 搜索）只清计数；不再自动重连，也不再自动重启模组。
// An explicit user action (connect / scan) only resets the counters; there is no automatic
// reconnect and no automatic module restart any more.
static void rearm_for_user(void) {
    s_relink_due = false;
    s_relink_tries = 0;
}

esp_err_t bt_audio_scan_start(uint32_t ms) {
    (void)ms;  // 模组自定搜索时长，ms 只保留接口兼容 / module-owned duration; ms kept for API compatibility
    if (!s_audio_wanted) return ESP_ERR_INVALID_STATE;
    if (s_scanning) return ESP_OK;
    devs_clear();
    s_scanning = true;
    rearm_for_user();
    if (s_ready) {
        ESP_LOGI(TAG, "scan: sending AT+INQUIRING");
        cmd_send("AT+INQUIRING\r\n");
    } else {
        // 模组还在启动：先记住意图，进模式 2 后立刻补发，界面不必让用户再点一次。
        // The module is still booting: queue the intent and send it once mode 2 is up.
        ESP_LOGI(TAG, "scan: queued until mode 2 ready");
        s_pending_scan = true;
        request_state(WANT_MODE2);
    }
    return ESP_OK;
}

void bt_audio_scan_stop(void) {
    // 没有取消搜索的 AT 命令，本地收尾即可（模组会自己发 INQ COMPLETE）。
    // There is no cancel command; stop locally and let the module finish with INQ COMPLETE.
    s_scanning = false;
    s_pending_scan = false;
}

bool bt_audio_scanning(void) { return s_scanning; }

uint8_t bt_audio_device_count(void) { return s_dev_count; }

const bt_audio_device_t *bt_audio_device(uint8_t index) {
    return index < s_dev_count ? &s_devs[index] : NULL;
}

esp_err_t bt_audio_connect(const uint8_t bda[6]) {
    if (!bda) return ESP_ERR_INVALID_ARG;
    if (!s_audio_wanted) {
        // 界面上的「连接/重连」按了没反应，最可能就是撞在这里：音频通路没起来。
        // A dead "connect" button in the UI is most likely this line: the audio path is down.
        bt_log("CONNECT rejected: audio path down");
        return ESP_ERR_INVALID_STATE;
    }
    bt_log("CONNECT request ready=%d", (int)s_ready);
    memcpy(s_link_bda, bda, 6);
    s_link_name[0] = 0;
    for (uint8_t i = 0; i < s_dev_count; ++i) {
        if (bda_equal(s_devs[i].bda, bda) && s_devs[i].name[0]) {
            snprintf(s_link_name, sizeof(s_link_name), "%s", s_devs[i].name);
            break;
        }
    }
    if (!s_link_name[0]) bda_text(bda, s_link_name);
    rearm_for_user();
    if (s_ready) {
        connect_send(bda);
    } else {
        // 模组还没进模式 2：记下要连的耳机，就绪后立刻发 AT+CONNECT。
        // Not in mode 2 yet: remember the headset and send AT+CONNECT as soon as it is.
        memcpy(s_pending_bda, bda, 6);
        s_pending_connect = true;
        request_state(WANT_MODE2);
    }
    return ESP_OK;
}

void bt_audio_disconnect(void) {
    // 没有 AT 断开命令：回模式 1 让模组拆链路，留在模式 2 会一直占着耳机。
    // 之后的连接/搜索会重新要求模式 2，由模式任务补做。
    // There is no AT disconnect: go back to mode 1 so the module drops the link; staying in
    // mode 2 keeps the headset occupied. A later connect/scan asks for mode 2 again.
    s_ready = false;
    s_feed_go = false;
    s_link_active = false;
    s_scanning = false;
    s_pending_connect = false;
    s_pending_scan = false;
    s_link_name[0] = 0;
    request_state(WANT_OFF);
}

bool bt_audio_link_active(void) { return s_link_active; }

bool bt_audio_output_ready(void) {
    // 本机扬声器根本没有蓝牙链路：mode 1 就绪 + 功放已拉高，就是这条通路的「在」。
    // The local speaker has no Bluetooth link at all: mode 1 up plus the PA raised is what
    // "up" means on that route.
    if (s_want_state == WANT_SPK) return s_ready;
    return s_link_active;
}

bool bt_audio_audio_ready(void) {
    // 「链路在」只是模组的单方面说法（CONNECT SUCCESS 一到就置真，没有超时也没有心跳）；
    // 真正能出声还要 A2DP 建立、且模组确认切离 SCO（`DISC SCO`）。
    // "Link up" is only the module's own claim (set on CONNECT SUCCESS, no timeout, no
    // heartbeat); real audio also needs A2DP up and the module's `DISC SCO` confirmation.
    if (!s_link_active || !s_a2dp_up) return false;
    if (s_sco_ok) return true;
    // 没在送数据时模组本来就不会建立 SCO，`DISC SCO` 自然也等不到；这种「本来就不需要出声」
    // 的情况按就绪处理，否则界面在没播歌时会一直显示未就绪。
    // With no data flowing the module never raises SCO, so `DISC SCO` cannot arrive; treat
    // "no audio needed" as ready, or the UI would sit on "not ready" whenever nothing plays.
    const int64_t now = esp_timer_get_time();
    return !s_last_feed_us || (now - s_last_feed_us) > 3000000;
}

bool bt_audio_test_tone_active(void) {
    return s_tone_until_us != 0 && esp_timer_get_time() < s_tone_until_us;
}

void bt_audio_test_tone_stop(void) {
    if (!s_tone_until_us) return;
    s_tone_until_us = 0;
    bt_log("TONE stop");
}

esp_err_t bt_audio_play_test_tone(uint32_t ms) {
    // 没链路时发出去也是白费：模组根本不拉 I2S，也没有耳机可听。
    // 本机扬声器是例外——那条路本来就没有蓝牙链路，声音走的是板载喇叭。
    // Without a link this is pointless: the module would not pull I2S and nobody is listening.
    // The local speaker is the exception: that path has no link by design; sound goes to the
    // on-board speaker.
    const bool local = (s_want_state == WANT_SPK);
    if ((!s_link_active && !local) || !s_ready || !s_uart_up) {
        bt_log("TONE refused link=%d ready=%d uart=%d", (int)s_link_active, (int)s_ready,
               (int)s_uart_up);
        return ESP_ERR_INVALID_STATE;
    }
    if (ms == 0) ms = 2000;
    if (ms > 5000) ms = 5000;
    s_tone_phase = 0;
    s_tone_until_us = esp_timer_get_time() + (int64_t)ms * 1000;
    // 这一行是判读核心：链路 / A2DP / SCO 确认 / 音量 一起打出来。出声与不出声各对应哪一侧，
    // 对照日志就能定性，不用再猜播放器。
    // The judging line: link / A2DP / SCO confirmation / volume at once. Whichever side the
    // sound lands on, this line plus the outcome decides it — no more guessing at the player.
    bt_log("TONE start %ums link=%d a2dp=%d sco=%d ready=%d vol=%u", (unsigned)ms,
           (int)s_link_active, (int)s_a2dp_up, (int)s_sco_ok, (int)s_ready, (unsigned)s_volume);
    return ESP_OK;
}

bool bt_audio_link_addr(uint8_t bda[6]) {
    if (!s_link_active || !bda) return false;
    memcpy(bda, s_link_bda, 6);
    return true;
}

const char *bt_audio_name(void) { return s_link_name; }

uint32_t bt_audio_heal_count(void) { return s_heal_count; }

uint32_t bt_audio_tx_error_count(void) { return s_tx_err_count; }

esp_err_t bt_audio_set_volume(uint8_t volume) {
    if (volume > 0x7F) volume = 0x7F;
    s_volume = volume;
    return s_link_active ? ESP_OK : ESP_ERR_NOT_SUPPORTED;
}

/* ---- 本地麦克风（语音翻页）/ Local mic for voice page-turn ---- */

// 麦克风通路与耳机播放互斥：模组每次只能在一种模式里。
// The mic path and headset playback are mutually exclusive: the module runs one mode at a time.
esp_err_t bt_mic_open(void) {
    // 每种拒绝/超时都打一条 WARN：开麦失败过去只有 LOGD，排查时看不到卡在哪一步。
    // Every refusal or timeout logs a WARN: failures used to be LOGD only, hiding where it wedged.
    if (s_suspended) {
        ESP_LOGW(TAG, "mic open refused: host suspended");
        return ESP_ERR_INVALID_STATE;
    }
    if (s_audio_wanted) {  // 音乐/耳机占用模组 / audio owns the module
        ESP_LOGW(TAG, "mic open refused: audio owns the module");
        return ESP_ERR_INVALID_STATE;
    }
    if (s_mic_ready) return ESP_OK;
    // 从没放过音乐时模式任务还不存在，这里补建（语音翻页可能先用到模组）。
    // Nothing may have played yet, so create the mode worker here; voice can touch the module first.
    if (!mode_worker_ensure()) return ESP_ERR_NO_MEM;
    metalio_io_set(BT_PWR_PIN, true);
    s_mic_error = false;
    if (s_want_state != WANT_MIC) s_seq++;
    request_state(WANT_MIC);
    // 等 AT 序列跑完（原厂固定 700 ms + 应答等待）。
    // Wait for the AT sequence (vendor-fixed 700 ms plus the ack window).
    const int64_t deadline = esp_timer_get_time() + 6000 * 1000;
    while (esp_timer_get_time() < deadline) {
        if (s_mic_ready) return ESP_OK;
        if (s_mic_error) return ESP_FAIL;  // 模组无应答，调用方退避重试 / module silent, caller backs off
        if (s_want_state != WANT_MIC) {
            ESP_LOGW(TAG, "mic open aborted: want became %d", s_want_state);
            return ESP_ERR_INVALID_STATE;
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    ESP_LOGW(TAG, "mic open timeout: ready=%d err=%d want=%d", (int)s_mic_ready, (int)s_mic_error,
             s_want_state);
    return ESP_ERR_TIMEOUT;
}

void bt_mic_close(void) {
    if (s_want_state == WANT_MIC) request_state(WANT_OFF);
    s_mic_ready = false;
    s_mic_error = false;
}

bool bt_mic_ready(void) { return s_mic_ready; }

// 读单声道 PCM（模组 I2S 是立体声交织，声道 0 是麦克风、声道 1 是回采）。
// 返回帧数；0 = 这次超时没数据；-1 = 通路已不可用。
// Read mono PCM. The module's I2S is interleaved stereo: slot 0 is the mic, slot 1 the
// reference. Returns frames; 0 means nothing arrived this time; -1 means the path is gone.
int bt_mic_read(int16_t *dst, int frames, uint32_t timeout_ms) {
    if (dst == NULL || frames <= 0) return 0;
    if (!s_mic_ready || !s_rx) return -1;
    if (frames > BT_MIC_MAX_FRAMES) frames = BT_MIC_MAX_FRAMES;
    if (!s_mic_raw) {
        s_mic_raw = heap_caps_malloc((size_t)BT_MIC_MAX_FRAMES * 2u * sizeof(int32_t),
                                     MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!s_mic_raw) return -1;
    }
    size_t got = 0;
    const size_t want = (size_t)frames * 2u * sizeof(int32_t);
    const esp_err_t err = i2s_channel_read(s_rx, s_mic_raw, want, &got, pdMS_TO_TICKS(timeout_ms));
    if (err != ESP_OK) return err == ESP_ERR_TIMEOUT ? 0 : -1;
    const int n = (int)(got / (2u * sizeof(int32_t)));
    for (int i = 0; i < n; ++i) {
        // 与官方 BTAudioCodec::Read 同一缩放：32 bit 槽右移 12 位得 int16。
        // Same scaling as the vendor codec's Read: shift the 32-bit slot down to int16.
        int32_t v = s_mic_raw[i * 2] >> 12;
        if (v > INT16_MAX) v = INT16_MAX;
        if (v < -INT16_MAX) v = -INT16_MAX;
        dst[i] = (int16_t)v;
    }
    return n;
}

#endif  // KIIKO_BOARD_METALIO47
