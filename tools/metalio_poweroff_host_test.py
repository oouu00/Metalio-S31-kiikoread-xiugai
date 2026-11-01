#!/usr/bin/env python3
"""中文：执行真实RTC/文件/按键/硬件停放顺序和失败恢复（"关机"现已折成深睡）。
English: Execute the real RTC/file/key/hardware parking order and fault recovery ("off" now folds into deep sleep).

中文：改动说明（源码演进，只改本脚本的桩与断言，未改任何源码）：
  1. 旧的 lock_power_off()（IO11 硬切电源）和 lock_power_off_parked() 已被整体删除。
     "关机"现在折成深睡：app_enter_host_sleep(APP_SLEEP_OFF) → 内部 mode 折成 APP_SLEEP_DEEP
     → 同一套屏/轨/SD 停放 → 写 NVS 深睡标记 → metalio_hold_for_deep_sleep() → esp_deep_sleep_start()。
     真断电后的开机门限（长按约 1 s）在硬件侧，固件改不了，这是用户明确要求的折法。
     因此抽取目标改为：
       * lock_power_off()          -> app_enter_host_sleep()（完整事务，用 APP_SLEEP_OFF 调用）
       * lock_power_off_parked()   -> lock_deep_parked()（已停放好的交接，只做 RTC+标记+锁引脚+深睡）
  2. 深睡标记改走 NVS（lock_mark_deep_wake/clear/写函数一并抽出），断言标记写在锁引脚之前。
  3. 新增桩：power_trace()（SD 打点）、music_set_suspended_for_sleep()/music_set_suspended()
     （蓝牙宿主挂起顺序：翻页器先、音乐后）、metalio_confirm_power_key()、ESP_SLEEP_WAKEUP_TIMER。
  4. 顺序断言按新源码更新：按键暂停与 RTC 都在文件落盘之前；book_suspend_io() 不再出现在
     这条路径上（深睡停放只挂起字体）；"缺卡"判定搬到 lock_light_sleep_once()，本脚本不再覆盖。
English: change log (the sources moved on; only the stubs/assertions here were adapted):
  1. The old lock_power_off() (IO11 hard cut) and lock_power_off_parked() were deleted. "Off" now
     folds into deep sleep: app_enter_host_sleep(APP_SLEEP_OFF) -> mode folded to APP_SLEEP_DEEP ->
     the same panel/rail/SD parking -> NVS deep-wake mark -> metalio_hold_for_deep_sleep() ->
     esp_deep_sleep_start(). The ~1 s hold needed after a real power cut lives in hardware, so this
     fold is what the user asked for. Extraction targets became:
       * lock_power_off()        -> app_enter_host_sleep() (full transaction, called with APP_SLEEP_OFF)
       * lock_power_off_parked() -> lock_deep_parked() (already-parked handoff: RTC + mark + hold + deep sleep)
  2. The deep-wake marker moved to NVS (lock_mark_deep_wake/clear/write are extracted too) and is
     asserted to be written before the pins are locked.
  3. New stubs: power_trace() (SD trace), music_set_suspended_for_sleep()/music_set_suspended()
     (Bluetooth host suspend order: page turner first, music second), metalio_confirm_power_key(),
     and ESP_SLEEP_WAKEUP_TIMER.
  4. Ordering assertions follow the new source: the key pause and the RTC sync both precede file
     flushing; book_suspend_io() is no longer on this path (the deep park suspends the font only);
     the absent-card rule moved to lock_light_sleep_once(), which this harness does not cover.
"""
from pathlib import Path
import subprocess,tempfile
from home_cover_cache_host_test import function
root=Path(__file__).resolve().parents[1]
unit=r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <setjmp.h>
typedef int esp_err_t;
typedef int app_sleep_mode_t;
typedef void *cst836u_handle_t;
typedef struct {int reading;} app_lock_font_t;
#define ESP_OK 0
#define ESP_ERR_INVALID_STATE -2
#define ESP_ERR_INVALID_ARG -3
#define ESP_ERR_TIMEOUT -4
#define ESP_ERR_NOT_FINISHED -5
#define ESP_ERR_NVS_NOT_FOUND -11
#define pdMS_TO_TICKS(x) (x)
#define pdTRUE 1
#define ESP_LOGW(...) ((void)0)
#define ESP_LOGE(...) ((void)0)
#define ESP_LOGI(...) ((void)0)
#define ESP_SLEEP_WAKEUP_ALL 0
// 深睡前关掉浅睡定时切片，否则会被每 <=60 s 的定时器反复叫醒。
// Deep sleep drops the light-sleep timer slice, or it would wake every <=60 s.
#define ESP_SLEEP_WAKEUP_TIMER 2
#define ESP_EXT1_WAKEUP_ANY_LOW 1
#define READ_PICO_IOE_INT_GPIO 2
#define CST836U_MODE_DEEPSLEEP 3
#define CST836U_MODE_NORMAL 0
enum {APP_SLEEP_DEEP,APP_SLEEP_OFF};
// 新增的 SD 卡打点函数；host 测试里静默丢弃。/ New SD-card trace hook; silently dropped on host.
static void power_trace(const char *fmt, ...){(void)fmt;}
// 深睡标记落在 NVS（flash）里，必须在 metalio_hold_for_deep_sleep() 锁引脚之前写完。
// The deep-wake marker lives in NVS and must be written before hold_for_deep_sleep() pins the IOs.
typedef unsigned int nvs_handle_t;
#define NVS_READONLY 0
#define NVS_READWRITE 1
#define S31_WAKE_NS "s31wake"
#define S31_WAKE_KEY "deep"
#define S31_DEEP_WAKE_MAGIC 0x53314450u
static uint32_t s_deep_wake_mark;
static uint8_t nvs_value;static bool nvs_have;static int nvs_writes,nvs_commits;
static int nvs_open(const char *ns,int mode,nvs_handle_t *out){assert(!strcmp(ns,S31_WAKE_NS));(void)mode;*out=7;return ESP_OK;}
static int nvs_get_u8(nvs_handle_t h,const char *key,uint8_t *out){assert(h==7&&!strcmp(key,S31_WAKE_KEY));if(!nvs_have)return ESP_ERR_NVS_NOT_FOUND;*out=nvs_value;return ESP_OK;}
static int nvs_set_u8(nvs_handle_t h,const char *key,uint8_t v){assert(h==7&&!strcmp(key,S31_WAKE_KEY));++nvs_writes;nvs_value=v;nvs_have=true;return ESP_OK;}
static int nvs_commit(nvs_handle_t h){assert(h==7);++nvs_commits;return ESP_OK;}
static void nvs_close(nvs_handle_t h){assert(h==7);}
static bool s_lock_io_pending;static cst836u_handle_t s_lock_sleep_touch;
// 扩展口 I2C：真实的 metalio_io_read/prepare/restore 在下面从源码抽取。
// Expander I2C: the real metalio_io_read/prepare/restore are extracted from source below.
static int ioe=1,mutex,io_taken;
static uint8_t outputs[2]={0x20,0x14},latches[2],config[2];
static int xSemaphoreTake(int m,int t){(void)m;assert(t==100);assert(!io_taken);io_taken=1;return pdTRUE;}
static void xSemaphoreGive(int m){(void)m;assert(io_taken);io_taken=0;}
static int fca9555_set_output(int dev,unsigned port,uint8_t v){assert(dev==ioe&&port<2);(void)dev;latches[port]=v;return ESP_OK;}
static int fca9555_set_config(int dev,unsigned port,uint8_t v){assert(dev==ioe&&port<2);(void)dev;config[port]=v;return ESP_OK;}
static int fca9555_read_input(int dev,unsigned port,uint8_t *out){assert(dev==ioe&&port<2);(void)dev;*out=0xff;return ESP_OK;}
static int read_pico_ioe_int_level(void){return 1;}
static int wake_source,timer_disabled,lcd_deinit;
static int esp_sleep_disable_wakeup_source(int source){
 assert(source==ESP_SLEEP_WAKEUP_ALL||source==ESP_SLEEP_WAKEUP_TIMER);
 if(source==ESP_SLEEP_WAKEUP_ALL)wake_source=0;else ++timer_disabled;return ESP_OK;}
static int esp_sleep_enable_ext1_wakeup_io(uint64_t mask,int mode){assert(mask==4&&mode==ESP_EXT1_WAKEUP_ANY_LOW);wake_source=1;return ESP_OK;}
// 状态计数：RTC/文件/按键/深睡/回滚。/ State counters: RTC/files/key/deep sleep/rollback.
static int rtc_error,failure,rtc,font,closes,key_pauses,deep_starts,holds,remounts,resumes,haptics,clock_ms;
static int builtin_opens,font_unlocks,music_suspends,music_restores,ble_resumes,net_resumes;
static bool paused,closed,usb,key_pending,ble_failed,transfer_busy,online,cut_success,deep_returns;
static bool font_locked,deep_pins,deep_rails;
static jmp_buf poweroff;
// RTC 必须早于任何文件落盘；按键已暂停说明交接已经开始。
// The RTC sync must precede every file flush; a paused key sampler means the handoff started.
static int app_sync_time_to_pmu(void){assert(paused&&!closes&&!deep_starts);++rtc;return rtc_error;}
static void vTaskDelay(unsigned ms){clock_ms+=(int)ms;}
static bool book_cover_jobs_quiesce(unsigned ms){assert(ms==2000);return true;}
static bool ui_avatar_quiesce(unsigned ms){assert(ms==2000);return true;}
static bool metalio_usb_blocks_sleep(void){return usb;}
static bool metalio_confirm_power_key(void){return key_pending;}
static bool pico_online_busy(void){return online;}
static bool read_pico_transfer_pause_for_sleep(void){return !transfer_busy;}
static void read_pico_transfer_resume_after_sleep(void){++net_resumes;}
static int ble_pt_suspend_for_sleep(void){return ble_failed?-1:ESP_OK;}
static void ble_pt_resume_after_sleep(void){++ble_resumes;}
// 中文：蓝牙宿主挂起顺序是「翻页器先、音乐后」；回滚必须成对恢复。
// English: the Bluetooth host suspends page turner first, music second; a rollback restores pairwise.
static void music_set_suspended_for_sleep(void){++music_suspends;}
static void music_set_suspended(bool s){assert(!s);++music_restores;}
static int read_pico_pmu_keys_pause(void){assert(!paused);if(key_pending)return ESP_ERR_INVALID_STATE;++key_pauses;paused=true;return ESP_OK;}
static void read_pico_pmu_keys_resume(void){assert(paused);paused=false;}
static void app_font_begin_lock(app_lock_font_t *f){assert(!font_locked);font_locked=true;f->reading=1;}
static void app_font_end_lock(const app_lock_font_t *f){assert(font_locked&&f->reading);font_locked=false;++font_unlocks;}
static int ttf_font_open_builtin(void){assert(font_locked);++builtin_opens;return ESP_OK;}
static int ttf_font_suspend_io(void){assert(rtc>=1&&!closes&&!deep_starts);++font;return failure==1?-4:ESP_OK;}
static int read_pico_sd_sync(void){assert(rtc>=1&&font>=1&&paused&&!deep_starts);++closes;if(failure==2)return ESP_ERR_NOT_FINISHED;closed=true;return ESP_OK;}
static void metalio_frontlight(unsigned b,unsigned w){assert(!b&&w==50);}
static void epd_poweroff(void){assert(closed&&paused);}
static void epd_lcd_deinit(void){++lcd_deinit;}
static void read_pico_epd_restore_lcd(void){lcd_deinit=0;}
static int cst836u_set_mode(void *t,int mode){assert(t&&(mode==CST836U_MODE_DEEPSLEEP||mode==CST836U_MODE_NORMAL));return ESP_OK;}
static int cst836u_wake(void *t){assert(t);return ESP_OK;}
static int metalio_prepare_light_sleep_pins(void){deep_pins=true;return ESP_OK;}
static void metalio_release_light_sleep_pins(void){deep_pins=false;}
static int metalio_sleep_cut_rails(uint16_t *saved){*saved=123;deep_rails=true;return ESP_OK;}
static int metalio_sleep_restore_rails(uint16_t saved){assert(saved==123);deep_rails=false;return ESP_OK;}
// 锁引脚前必须已写好 NVS 深睡标记。/ The NVS mark must already be written when the pins are locked.
static void metalio_hold_for_deep_sleep(void){++holds;assert(nvs_have&&nvs_value==1&&s_deep_wake_mark==S31_DEEP_WAKE_MAGIC);}
// 中文：两条路径共有的不变式——RTC 已同步、文件已落盘、按键已暂停、标记已写、引脚已锁；
// 屏/轨由调用方按路径停放（完整事务自己停，停放好的交接由调用方预先停好），故不在这里断言。
// English: invariants shared by both paths - RTC synced, files flushed, keys paused, mark written,
// pins held. Panel/rails are parked by the caller of each path, so they are asserted in main().
static void esp_deep_sleep_start(void){
 assert(rtc>=1&&font>=1&&closed&&paused&&holds>=1&&nvs_value==1);
 ++deep_starts;if(!deep_returns)longjmp(poweroff,1);}
static int read_pico_sd_remount(void){assert(closed);++remounts;closed=false;return ESP_OK;}
static int lock_resume_media_io(unsigned wait){assert(wait==15000);++resumes;s_lock_io_pending=false;return ESP_OK;}
static void metalio_haptic_resume(void){++haptics;}
'''
source=root/'boards/metalio47/src/main/sleep.c'
for name in ('metalio_io_read','metalio_prepare_key_wake','metalio_restore_key_io'):
    unit+=function(name,root/'components/metalio_hw/metalio_hw.c')+'\n'
# 中文：先抽标记函数，再抽使用它们的 lock_deep_parked/app_enter_host_sleep（C 要求先声明后使用）。
# English: pull the marker helpers first, then their users (C needs a declaration before use).
for name in ('lock_wake_mark_write','lock_mark_deep_wake','lock_clear_deep_wake','lock_restore_key_wake','lock_prepare_key_wake','lock_deep_parked','app_enter_host_sleep'):
    unit+=function(name,source)+'\n'
unit+=r'''
static void reset(void){
 usb=key_pending=ble_failed=transfer_busy=online=cut_success=deep_returns=false;
 paused=closed=font_locked=deep_pins=deep_rails=false;
 rtc_error=failure=rtc=font=closes=key_pauses=deep_starts=holds=remounts=resumes=haptics=clock_ms=0;
 builtin_opens=font_unlocks=music_suspends=music_restores=ble_resumes=net_resumes=0;
 lcd_deinit=wake_source=timer_disabled=0;io_taken=0;
 nvs_have=false;nvs_value=0;nvs_writes=nvs_commits=0;s_deep_wake_mark=0;
 s_lock_io_pending=false;s_lock_sleep_touch=NULL;
 config[0]=0xc0;config[1]=0xe3;latches[0]=outputs[0];latches[1]=outputs[1];
}
// 回滚完成后的状态：外设、方向、媒体都必须回到原样。
// After a rollback every peripheral, direction and media handle must be back to normal.
static void clean(void){
 assert(!paused&&!closed&&!deep_pins&&!deep_rails&&!s_lock_io_pending&&!lcd_deinit);
 assert(config[0]==0xc0&&config[1]==0xe3&&latches[0]==0x20&&latches[1]==0x14);
}
int main(void){
 // 中文：完整的「关机」事务：挂起无线/音乐 -> 按键暂停 -> RTC -> 字体挂起 -> SD 同步
 // -> 灭灯/断屏 -> 停屏/断轨 -> 写 NVS 标记 -> 锁引脚 -> 深睡（不再硬切电源）。
 // English: the full "off" transaction: radios/music -> key pause -> RTC -> font suspend -> SD sync
 // -> lamp/panel off -> panel/rails parked -> NVS mark -> pins held -> deep sleep (no hard cut).
 reset();cut_success=true;
 if(!setjmp(poweroff)){app_enter_host_sleep(APP_SLEEP_OFF);assert(!"deep sleep returned");}
 assert(rtc==1&&font==1&&closes==1&&closed&&deep_starts==1&&holds==1&&nvs_value==1&&music_suspends==1);
 assert(!remounts&&!resumes&&!haptics&&paused&&deep_pins&&deep_rails&&lcd_deinit==1&&timer_disabled==0);
 // 深睡返回（真机不会）：清掉 NVS 标记并完整回滚外设；源码仍以 ESP_OK 收尾。
 // A returning deep sleep (impossible on device) clears the NVS mark and restores everything;
 // the source still finishes with ESP_OK.
 reset();deep_returns=true;assert(app_enter_host_sleep(APP_SLEEP_OFF)==ESP_OK);
 assert(rtc==1&&font==1&&closes==1&&deep_starts==1&&holds==1&&nvs_value==0);
 assert(remounts==1&&resumes==1&&haptics==1&&font_unlocks==1&&music_restores==1&&ble_resumes==1&&net_resumes==1);
 clean();
 // 中文：字体挂起/SD 同步失败：不进深睡、不写标记，媒体按原路恢复，仍保持锁定。
 // English: font/SD failures never reach deep sleep or the mark; media is restored, lock retained.
 reset();failure=1;assert(app_enter_host_sleep(APP_SLEEP_OFF)==-4);
 assert(rtc==1&&font==1&&!closes&&!closed&&!deep_starts&&!holds&&!remounts&&resumes==1&&font_unlocks==1);clean();
 reset();failure=2;assert(app_enter_host_sleep(APP_SLEEP_OFF)==ESP_ERR_NOT_FINISHED);
 assert(rtc==1&&font==1&&closes==1&&!closed&&!deep_starts&&!holds&&!remounts&&resumes==1&&font_unlocks==1);clean();
 // 中文：无线/按键交接失败：RTC 与文件一步都没走，也不进深睡。
 // English: failed radio/key handoff stops before the RTC and any file work, without deep sleep.
 reset();ble_failed=true;assert(app_enter_host_sleep(APP_SLEEP_OFF)==-1);
 assert(!rtc&&!font&&!closes&&!deep_starts&&!paused&&!holds&&net_resumes==1&&!ble_resumes&&!music_restores);clean();
 reset();key_pending=true;assert(app_enter_host_sleep(APP_SLEEP_OFF)==ESP_ERR_INVALID_STATE);
 assert(key_pending&&!rtc&&!closes&&!deep_starts&&!holds&&!paused);
 assert(music_suspends==1&&music_restores==1&&ble_resumes==1&&net_resumes==1);clean();
 // RTC 同步失败不拦住折进来的关机（有界重试属于停放好的交接）。
 // A failed RTC sync does not block the folded off (the bounded retry belongs to the parked handoff).
 reset();cut_success=true;rtc_error=-4;
 if(!setjmp(poweroff)){app_enter_host_sleep(APP_SLEEP_OFF);assert(!"deep sleep returned");}
 assert(rtc==1&&deep_starts==1);
 // 中文：停放好的交接 lock_deep_parked()（被删除的 lock_power_off_parked 的继承者）：
 // RTC -> 关定时切片 -> 写标记 -> 锁引脚 -> 深睡；USB 在位或还按着键则推迟。
 // English: the parked handoff lock_deep_parked() (heir of the deleted lock_power_off_parked):
 // RTC -> drop the timer slice -> write the mark -> hold pins -> deep sleep; USB or a held key defers.
 reset();closed=paused=true;font=1;cut_success=true;
 if(!setjmp(poweroff)){lock_deep_parked();assert(!"deep sleep returned");}
 assert(rtc==1&&deep_starts==1&&holds==1&&timer_disabled==1&&nvs_value==1&&!closes&&!remounts&&!resumes&&closed&&paused);
 reset();closed=paused=true;font=1;usb=true;
 assert(lock_deep_parked()==ESP_ERR_INVALID_STATE&&!deep_starts&&!holds&&!remounts&&!nvs_writes);
 reset();closed=paused=true;font=1;key_pending=true;
 assert(lock_deep_parked()==ESP_ERR_INVALID_STATE&&!deep_starts&&!holds&&!remounts);
 // 有界的 RTC 重试：失败一次后隔 20 ms 再试，仍继续进深睡；INVALID_STATE 不重试。
 // Bounded RTC retry: one 20 ms retry, then deep sleep anyway; INVALID_STATE is not retried.
 reset();closed=paused=true;font=1;cut_success=true;rtc_error=-4;
 if(!setjmp(poweroff)){lock_deep_parked();assert(!"deep sleep returned");}
 assert(rtc==2&&clock_ms==20&&deep_starts==1);
 reset();closed=paused=true;font=1;cut_success=true;rtc_error=ESP_ERR_INVALID_STATE;
 if(!setjmp(poweroff)){lock_deep_parked();assert(!"deep sleep returned");}
 assert(rtc==1&&clock_ms==0&&deep_starts==1);
 // 深睡返回：清掉标记并报超时（真机不返回）。/ A returning deep sleep clears the mark and times out.
 reset();closed=paused=true;font=1;deep_returns=true;assert(lock_deep_parked()==ESP_ERR_TIMEOUT);
 assert(deep_starts==1&&holds==1&&nvs_value==0&&clock_ms==1000);
 puts("PASS: actual S31 off folded into parked deep sleep - key pause and RTC before file flush, NVS mark before the pins are held, font/SD/radio/key faults restored with the lock retained, bounded RTC retry and USB/held-key deferral");
}
'''
with tempfile.TemporaryDirectory(prefix='s31-poweroff-') as d:
 p=Path(d);(p/'test.c').write_text(unit)
 subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-fsanitize=address,undefined',str(p/'test.c'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True)
