#!/usr/bin/env python3
"""中文：执行S31启动门禁，真实唤醒不多等一次，无证据的复位一律保留门禁。
English: Execute S31 boot gates: retain real wakes without granting unproven resets an unlock.

中文：本板（metalio47）深睡唤醒实测报 reset=14（ESP_RST_PWR_GLITCH），既不是 DEEPSLEEP 也不是
POWERON，RTC 域（RTC_NOINIT 标记与 bootloader 早期按键记录）都保不住，所以门禁的证据来源已经
换掉：复位原因不再是唯一依据，改为「NVS 命名空间 s31wake 键 deep 上的一次性标记」。
本测试随之改为抽取 sleep.c 里真实的 lock_mark_deep_wake / lock_clear_deep_wake /
lock_wake_mark_read / lock_wake_mark_write / app_lock_resolve_wake_mark，只把 nvs_* 做成桩。
English: this board wakes from deep sleep with reset=14 (ESP_RST_PWR_GLITCH) and a lost RTC
domain, so the gate no longer keys off the reset reason but off a one-shot NVS marker
(namespace "s31wake", key "deep"). The test therefore extracts the real marker functions from
sleep.c and stubs only the nvs_* API.
"""
from pathlib import Path
import subprocess, tempfile
from home_cover_cache_host_test import function
root=Path(__file__).resolve().parents[1]
source=root/'boards/metalio47/src/main/sleep.c'
unit=r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#define ESP_OK 0
#define ESP_ERR_NOT_FINISHED 1
#define ESP_RST_POWERON 1
#define ESP_RST_DEEPSLEEP 5
#define ESP_RST_SW 3
#define ESP_RST_PANIC 7
// 中文：本板深睡唤醒实测的复位原因 14 = ESP_RST_PWR_GLITCH：电源域跌落，RTC 域保不住。
// English: the measured deep-wake reason on this board, 14 = ESP_RST_PWR_GLITCH; the power
// domain fell and the RTC domain does not survive.
#define ESP_RST_PWR_GLITCH 14
#define ESP_SLEEP_WAKEUP_EXT1 2
#define ESP_SLEEP_WAKEUP_TIMER 4
#define READ_PICO_IOE_INT_GPIO 2
#define APP_LOCK_IDLE_TIMEOUT_MS 0
#define EPD_DRAW_SUCCESS 0
#define pdMS_TO_TICKS(x) (x)
// 中文：logs 与 TAG 属于 esp_log.h；放行的那一支会打一条 ESP_LOGI，桩里丢掉。
// English: logs and TAG come from esp_log.h; the pass-through branch logs one line, discarded here.
#define ESP_LOGI(...) ((void)0)
#define TAG "read_pico"
// 中文：深睡标记常量（与 sleep.c 一致）：RTC_NOINIT 里那一份已不再是证据，NVS 这一份才是。
// English: deep-wake marker constants matching sleep.c; the RTC_NOINIT copy is no longer the
// evidence, the NVS one is.
#define S31_DEEP_WAKE_MAGIC UINT32_C(0x53314450)
#define S31_WAKE_NS "s31wake"
#define S31_WAKE_KEY "deep"
typedef int esp_err_t;
typedef int app_wake_source_t;
typedef void *sc7a20h_handle_t;
typedef void *cst836u_handle_t;
typedef uint32_t nvs_handle_t;
typedef enum{NVS_READONLY,NVS_READWRITE} nvs_open_mode_t;
typedef struct{int marker;}EpdiyHighlevelState;
typedef struct{bool mounted;}read_pico_sd_info_t;
typedef struct{int marker;}app_lock_font_t;
enum EpdDrawError{EPD_OK};
enum{APP_WAKE_KEY,APP_WAKE_PICKUP,APP_WAKE_TIMEOUT};
static uint32_t s_boot_reset_code,s_deep_wake_mark;
static bool s_boot_from_deep,s_boot_power_wake,s_boot_power_on,s_boot_lock_draw;
static bool s_boot_key_proven,s_boot_nvs_mark,s_wake_mark_resolved,deep_key;
static int reset_reason,cause,wake_presses;
static uint64_t ext1;
static bool resume,pin,physical,prior_lock;
static unsigned waits,auth,holds,ready,frames,hints,manual_presses,clears,nvs_commits;
// 中文：NVS 里那一个字节的深睡标记；flash 由主电源供电，外设轨掉了它还在。
// English: the one-byte deep-wake marker in NVS; flash is fed by the main rail and survives.
static uint8_t nvs_deep;
static esp_err_t nvs_open(const char *ns,nvs_open_mode_t mode,nvs_handle_t *out){
 assert(!strcmp(ns,S31_WAKE_NS));(void)mode;*out=1;return ESP_OK;
}
static esp_err_t nvs_get_u8(nvs_handle_t h,const char *key,uint8_t *v){
 assert(h==1&&!strcmp(key,S31_WAKE_KEY));*v=nvs_deep;return ESP_OK;
}
static esp_err_t nvs_set_u8(nvs_handle_t h,const char *key,uint8_t v){
 assert(h==1&&!strcmp(key,S31_WAKE_KEY));nvs_deep=v;return ESP_OK;
}
static esp_err_t nvs_commit(nvs_handle_t h){assert(h==1);++nvs_commits;return ESP_OK;}
static void nvs_close(nvs_handle_t h){assert(h==1);}
static int esp_reset_reason(void){return reset_reason;}
static int esp_sleep_get_wakeup_cause(void){return cause;}
static uint64_t esp_sleep_get_ext1_wakeup_status(void){return ext1;}
// 中文：新源码在捕获/门禁里打轨迹（写 SD 卡）；这里只保留签名，不产生副作用。
// English: the new source traces the gate to the SD card; the stub keeps the signature only.
static void power_trace(const char *fmt,...){(void)fmt;}
static bool metalio_boot_power_key(void){return physical;}
static bool lock_pin_enabled(void){return pin;}
static int app_settings_wake_presses(void){return wake_presses;}
static bool pico_boot_has_resume(void){return resume;}
static bool pico_boot_lock_pending(void){return prior_lock;}
static void pico_boot_clear_resume(void){assert(prior_lock&&!resume);prior_lock=false;++clears;}
static void pico_boot_ready(void){++ready;}
static void pico_boot_hold_resume(void){++holds;}
static void lock_hint_begin(EpdiyHighlevelState *h){assert(h);++hints;}
static void lock_hint_end(void){assert(hints);--hints;}
static void lock_resume_service(void){}
static bool app_lock_resume_frame(EpdiyHighlevelState *h){assert(!h);++frames;return false;}
static int app_light_sleep_wait_timed(void *acc,unsigned ms){
 // 中文：门禁保留时，必须等到一次真实按键；桩把每次等待算成用户按了一下。
 // English: a retained gate waits for one real press; the mock counts each wait as that press.
 assert(!acc&&!ms);++waits;++manual_presses;return APP_WAKE_KEY;
}
static int read_pico_sd_get_info(read_pico_sd_info_t *i){i->mounted=true;return ESP_OK;}
static void vTaskDelay(unsigned ms){(void)ms;}
static void app_font_begin_lock(app_lock_font_t *f){f->marker=1;}
static void app_font_end_lock(app_lock_font_t *f){assert(f->marker);}
static uint8_t fb[32];
static uint8_t *epd_hl_get_framebuffer(EpdiyHighlevelState *h){assert(h);return fb;}
static bool lock_screen_restore(uint8_t *f){assert(f==fb);return true;}
static int app_settings_lock_style(void){return 0;}
static int epd_width(void){return 8;}static int epd_height(void){return 8;}
static bool book_ticket_draw(uint8_t *f,bool b){assert(f==fb&&!b);return true;}
static bool book_lock_collage_prepare(uint8_t *f){assert(f==fb);return true;}
static const char *app_settings_bookmark_path(void){return "";}
static bool book_lock_bookmark_draw(uint8_t *f,const char *p){(void)p;assert(f==fb);return true;}
static bool draw_wallpaper(uint8_t *f){assert(f==fb);return true;}
static void ui_lock_fallback_draw(uint8_t *f){assert(f==fb);}
static enum EpdDrawError lock_present_art(EpdiyHighlevelState *h,int s){(void)s;assert(h);return EPD_OK;}
static void guard_draw_result(EpdiyHighlevelState *h,enum EpdDrawError e){assert(h&&!e);}
static void epd_poweroff(void){}
static bool lock_screen_save(uint8_t *f){assert(f==fb);return true;}
static void lock_screen_authenticate(EpdiyHighlevelState *h,void *t,void *a,bool b){assert(h&&t&&a&&b);++auth;}
'''
# 中文：标记那一套必须抽真实源码，否则测的是桩自己：进深睡落标记、开机读走并清掉。
# English: extract the real marker code, or the test would only exercise its own stub.
for name in ('lock_wake_mark_read','lock_wake_mark_write','lock_mark_deep_wake','lock_clear_deep_wake',
             'app_lock_resolve_wake_mark','app_lock_deep_wake_key','app_lock_capture_boot_wakeup',
             'lock_wait_for_unlock','app_lock_boot_gate'):
    unit+=function(name,source)+'\n'
unit+=r'''
static void reset(void){
 s_boot_from_deep=s_boot_power_wake=s_boot_power_on=s_boot_lock_draw=false;
 s_boot_key_proven=s_boot_nvs_mark=s_wake_mark_resolved=false;
 s_boot_reset_code=0;s_deep_wake_mark=0;
 reset_reason=ESP_RST_POWERON;cause=0;ext1=0;wake_presses=1;
 resume=true;pin=physical=prior_lock=false;
 waits=auth=holds=ready=frames=hints=manual_presses=clears=nvs_commits=0;
 nvs_deep=0;
}
// 中文：完整开机序列，顺序与 app_main() 一致——捕获 → NVS 就绪后解析标记 → 门禁。
// English: the full boot sequence in app_main()'s order: capture, resolve the marker once NVS is up, then the gate.
static void boot(void){
 EpdiyHighlevelState hl={0};
 app_lock_capture_boot_wakeup();
 app_lock_resolve_wake_mark();
 deep_key=app_lock_deep_wake_key();
 app_lock_boot_gate(&hl,(void*)1,(void*)2);
 assert(!hints&&frames==1&&s_boot_lock_draw);
}
// 中文：门禁放行 = 等待循环一次都没跑（真实按键那一按直接放行，且不再补记）。
// English: an open gate means the wait loop never ran: the real press passes through and is not latched.
static void opened(void){assert(!waits&&!manual_presses&&ready==1&&holds==1);}
// 中文：门禁保留 = 必须再等一次真实按键。
// English: a retained gate means one more real press is required.
static void locked(void){assert(waits==1&&manual_presses==1);}
int main(void){
 // 中文：① 单击硬件开机：bootloader 早期采样到 P17，直接进系统；普通开机不写 flash。
 // English: ① a single hardware press: the bootloader sampled P17, so the gate opens; a normal boot never touches flash.
 reset();physical=true;boot();opened();assert(!deep_key&&!nvs_deep&&!nvs_commits);
 // 中文：② 复位原因仍报 DEEPSLEEP 且 RTC 标记还在（旧路径）：标记被读走并清掉，只写两次 flash。
 // English: ② the reset reason still reports DEEPSLEEP with the RTC mark intact (the old path): the
 // mark is consumed and cleared, touching flash exactly twice (set, then clear).
 reset();lock_mark_deep_wake();
 assert(s_deep_wake_mark==S31_DEEP_WAKE_MAGIC&&nvs_deep==1&&nvs_commits==1);
 reset_reason=ESP_RST_DEEPSLEEP;cause=ESP_SLEEP_WAKEUP_EXT1;ext1=4;boot();
 opened();assert(deep_key&&s_boot_from_deep);
 assert(!s_deep_wake_mark&&!nvs_deep&&nvs_commits==2);
 // 中文：③ 证据放宽：深睡只留 P17 一条 EXT1 线，所以 DEEPSLEEP 复位本身就是证据——
 // 物理采样读到高（手指早离开）、ext1 位图读回 0 或读到别的线、唤醒原因是定时，都不再挡住。
 // English: ③ evidence relaxed: deep sleep arms only the P17 EXT1 line, so a DEEPSLEEP reset is itself
 // proof — a high physical sample (finger long gone), an empty or foreign ext1 bitmap, or a timer
 // cause must no longer park the boot on the wallpaper.
 reset();reset_reason=ESP_RST_DEEPSLEEP;cause=ESP_SLEEP_WAKEUP_EXT1;ext1=0;boot();
 opened();assert(deep_key);
 reset();reset_reason=ESP_RST_DEEPSLEEP;cause=ESP_SLEEP_WAKEUP_EXT1;ext1=4;boot();
 opened();assert(deep_key);
 // 中文：定时/其它线时只有 s_boot_from_deep 这条保底生效，deep-key 接口仍为假（app_main 据此保留锁图）。
 // English: for a timer or a foreign line only the s_boot_from_deep fallback applies, so the deep-key
 // API stays false and app_main keeps the lock art.
 reset();reset_reason=ESP_RST_DEEPSLEEP;cause=ESP_SLEEP_WAKEUP_TIMER;ext1=8;boot();
 opened();assert(!deep_key&&s_boot_from_deep);
 // 中文：④ 本板实际路径：reset=14（PWR_GLITCH，RTC 域丢失）+ NVS 标记命中 → 按「按键唤醒」放行，
 // 命中后立刻清标记，且只解析一次。
 // English: ④ the real path on this board: reset=14 (PWR_GLITCH, RTC lost) plus an NVS hit counts as a
 // key wake; the marker is cleared immediately and resolved only once.
 reset();lock_mark_deep_wake();s_deep_wake_mark=0;  // 中文：电源域跌落，RTC_NOINIT 保不住。/ English: the power domain fell; RTC_NOINIT is gone.
 reset_reason=ESP_RST_PWR_GLITCH;cause=ESP_SLEEP_WAKEUP_EXT1;ext1=4;boot();
 opened();assert(deep_key&&s_boot_nvs_mark&&!s_boot_from_deep&&!s_boot_power_on);
 assert(!nvs_deep&&nvs_commits==2);
 unsigned commits=nvs_commits;app_lock_resolve_wake_mark();assert(nvs_commits==commits);
 // 中文：⑤ 没有任何证据的复位一律保留门禁：深睡「样子」的 EXT1 原因但没有标记、软件复位、
 // 上电却没采到按键、看门狗/异常重启，都不能自行解锁。
 // English: ⑤ every unproven reset keeps the gate: an EXT1 cause that looks like a deep wake but has no
 // marker, a software reset, a power-on without a sampled press, and watchdog/abnormal reboots.
 for(unsigned i=0;i<4;i++){
  reset();
  if(i==0){reset_reason=ESP_RST_PWR_GLITCH;cause=ESP_SLEEP_WAKEUP_EXT1;ext1=4;}
  else if(i==1)reset_reason=ESP_RST_SW;
  else if(i==2){reset_reason=ESP_RST_POWERON;cause=ESP_SLEEP_WAKEUP_EXT1;ext1=4;}
  else reset_reason=ESP_RST_PANIC;
  boot();locked();assert(!deep_key&&!nvs_commits&&ready==1&&holds==1);
 }
 // 中文：⑥ 唤醒次数设置只进轨迹，不改变门禁结论；rc108 起长按开机的硬件上电
 // （采样到 P17）在单/双击设置下都直接放行，不再等短按。
 // English: ⑥ the wake-press count only reaches the trace; it never changes the gate's decision.
 // Since rc108 a long-press hardware boot (P17 sampled at boot) opens the gate for either setting.
 reset();wake_presses=2;reset_reason=ESP_RST_PWR_GLITCH;cause=ESP_SLEEP_WAKEUP_EXT1;ext1=4;nvs_deep=1;
 boot();opened();assert(deep_key);
 reset();wake_presses=2;boot();locked();
 reset();wake_presses=2;physical=true;boot();opened();
 // 中文：⑦ PIN 无论复位原因还是单击/双击都要走认证，且不进入等待循环。
 // English: ⑦ the PIN always authenticates regardless of the reset reason or the single/double setting, and never waits.
 for(unsigned i=0;i<3;i++){
  reset();pin=true;
  if(i==0)reset_reason=ESP_RST_POWERON;
  else if(i==1)reset_reason=ESP_RST_DEEPSLEEP;
  else {reset_reason=ESP_RST_PWR_GLITCH;nvs_deep=1;}
  boot();assert(auth==1&&!waits&&!manual_presses);
 }
 // 中文：⑧ 无恢复记录：门禁不动作。/ English: ⑧ with no resume record the gate does nothing.
 reset();resume=false;boot();assert(!waits&&!holds&&!ready);
 // 中文：⑨ 看门狗恢复：禁止续读但仍保留此前的锁，解锁/认证后清掉保留标记。
 // English: ⑨ watchdog recovery: book/font resume is forbidden, the prior lock is retained, and the
 // marker is cleared after unlock or authentication.
 reset();resume=false;prior_lock=true;reset_reason=ESP_RST_PANIC;boot();
 locked();assert(clears==1&&!prior_lock&&!deep_key);
 reset();resume=false;prior_lock=true;reset_reason=ESP_RST_PANIC;pin=true;boot();
 assert(auth==1&&!waits&&clears==1&&!prior_lock);
 reset();prior_lock=true;boot();locked();assert(!clears&&prior_lock);
 puts("PASS: S31 boot gate opens on the hardware press, the DEEPSLEEP fallback and the NVS s31wake/deep marker with the RTC domain lost; unproven glitch/software/power-on/watchdog resets stay locked, PIN always authenticates, no second press");
}
'''
with tempfile.TemporaryDirectory(prefix='s31-boot-wake-') as d:
    p=Path(d);(p/'test.c').write_text(unit)
    subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-fsanitize=address,undefined',str(p/'test.c'),'-o',str(p/'test')],check=True)
    subprocess.run([str(p/'test')],check=True)
