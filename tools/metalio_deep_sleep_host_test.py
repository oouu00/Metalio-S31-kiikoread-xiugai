"""Run production S31 sleep admission/masks and lock timeout continuation with fault injection.

中文：本脚本抽取 boards/metalio47/src/main/sleep.c 里的真实函数：
  * app_enter_host_sleep / lock_prepare_key_wake / lock_restore_key_wake / lock_wait_for_unlock
  * lock_deep_parked（原 lock_power_off 的语义继承者）
English: this harness extracts the real functions from boards/metalio47/src/main/sleep.c:
  * app_enter_host_sleep / lock_prepare_key_wake / lock_restore_key_wake / lock_wait_for_unlock
  * lock_deep_parked (the semantic heir of the deleted lock_power_off)

中文：改动说明（源码演进，只改本脚本的桩与断言，未改任何源码）：
  1. 旧的 lock_power_off()（IO11 硬切电源）已被整体删除，"关机"现在折成深睡：
     app_enter_host_sleep(APP_SLEEP_OFF) → 内部 mode 折成 APP_SLEEP_DEEP → 同一套停放流程。
     抽取目标随之改为 lock_deep_parked()（"媒体/电源轨已停好时直接转入深睡"的交接函数），
     并用 app_enter_host_sleep(APP_SLEEP_OFF) 断言"折进来的关机"仍走到深睡、且不再硬切电源。
  2. 深睡标记改走 NVS：新增 lock_mark_deep_wake()/lock_clear_deep_wake()/lock_wake_mark_write()
     的抽取与 nvs_* 桩，断言标记写在 metalio_hold_for_deep_sleep() 锁引脚之前。
  3. 新增打点函数 power_trace()、音乐挂起 music_set_suspended_for_sleep()/music_set_suspended()、
     按键确认 metalio_confirm_power_key()，以及 ESP_SLEEP_WAKEUP_TIMER（深睡前关掉定时切片）。
English: change log (the sources moved on; only the stubs/assertions here were adapted):
  1. The old lock_power_off() (IO11 hard cut) is gone; "off" now folds into deep sleep via
     app_enter_host_sleep(APP_SLEEP_OFF) -> APP_SLEEP_DEEP. The extraction target became
     lock_deep_parked(), and the folded off is asserted to reach deep sleep without any hard cut.
  2. The deep-wake marker now lives in NVS: lock_mark_deep_wake()/lock_clear_deep_wake()/
     lock_wake_mark_write() are extracted with nvs_* stubs, asserting the mark is written before
     metalio_hold_for_deep_sleep() pins the IOs.
  3. New stubs: power_trace(), music_set_suspended_for_sleep()/music_set_suspended(),
     metalio_confirm_power_key(), and ESP_SLEEP_WAKEUP_TIMER (deep sleep drops the timer slice).
"""
from pathlib import Path
import subprocess, tempfile
from home_cover_cache_host_test import function
root=Path(__file__).resolve().parents[1]
unit=r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <setjmp.h>
#define ESP_OK 0
static bool book_cover_jobs_quiesce(unsigned ms){assert(ms==2000);return true;}
static bool ui_avatar_quiesce(unsigned ms){assert(ms==2000);return true;}
#define ESP_ERR_INVALID_ARG -3
#define ESP_ERR_NOT_FINISHED -4
#define ESP_ERR_INVALID_STATE -1
#define ESP_ERR_TIMEOUT -2
#define ESP_SLEEP_WAKEUP_ALL 0
// 深睡前要关掉浅睡的定时切片，否则深睡会被每 <=60 s 的定时器反复叫醒。
// Deep sleep disables the light-sleep timer slice, or it would wake every <=60 s.
#define ESP_SLEEP_WAKEUP_TIMER 2
#define ESP_EXT1_WAKEUP_ANY_LOW 1
#define READ_PICO_IOE_INT_GPIO 2
#define APP_LOCK_IDLE_TIMEOUT_MS 0
#define pdMS_TO_TICKS(x) (x)
#define pdTRUE 1
#define ESP_LOGW(...) ((void)0)
#define ESP_LOGE(...) ((void)0)
#define ESP_LOGI(...) ((void)0)
// 新增的 SD 卡打点函数；host 测试里静默丢弃。/ New SD-card trace hook; silently dropped on host.
static void power_trace(const char *fmt, ...){(void)fmt;}
typedef int esp_err_t;typedef int app_sleep_mode_t;typedef int app_wake_source_t;typedef void *sc7a20h_handle_t;
enum {APP_SLEEP_DEEP,APP_SLEEP_OFF};
enum {APP_WAKE_NONE,APP_WAKE_KEY,APP_WAKE_PICKUP,APP_WAKE_TIMEOUT};
typedef struct {char path[288];bool reading;} app_lock_font_t;
// 深睡标记现在写在 NVS（flash）里，必须在 metalio_hold_for_deep_sleep() 锁引脚之前落盘；
// 桩记录写入/提交次数，顺序由 metalio_hold_for_deep_sleep() 的断言把守。
// The deep-wake marker lives in NVS now and must land before hold_for_deep_sleep() pins the IOs;
// these stubs count writes/commits and the hold stub enforces the order.
typedef unsigned int nvs_handle_t;
#define NVS_READONLY 0
#define NVS_READWRITE 1
#define S31_WAKE_NS "s31wake"
#define S31_WAKE_KEY "deep"
#define S31_DEEP_WAKE_MAGIC 0x53314450u
static uint32_t s_deep_wake_mark;
static uint8_t nvs_value;static bool nvs_have;static int nvs_writes,nvs_commits;
static int nvs_open(const char *ns,int mode,nvs_handle_t *out){assert(!strcmp(ns,S31_WAKE_NS));(void)mode;*out=7;return ESP_OK;}
static int nvs_get_u8(nvs_handle_t h,const char *key,uint8_t *out){assert(h==7&&!strcmp(key,S31_WAKE_KEY));if(!nvs_have)return -11;*out=nvs_value;return ESP_OK;}
static int nvs_set_u8(nvs_handle_t h,const char *key,uint8_t v){assert(h==7&&!strcmp(key,S31_WAKE_KEY));++nvs_writes;nvs_value=v;nvs_have=true;return ESP_OK;}
static int nvs_commit(nvs_handle_t h){assert(h==7);++nvs_commits;return ESP_OK;}
static void nvs_close(nvs_handle_t h){assert(h==7);}
// 进睡挂起蓝牙宿主（音乐收栈，翻页器已拆）；失败回滚必须成对恢复。
// Suspend the Bluetooth host for sleep (music tears the stack down); a rollback restores pairwise.
static int music_suspends,music_restores;
static void music_set_suspended_for_sleep(void){++music_suspends;}
static void music_set_suspended(bool s){assert(!s);++music_restores;}
static int ioe=1,mutex,io_calls,io_fail_at,io_taken,delay_ms,irq_low,key_low;
static uint8_t outputs[2]={0x20,0x14},latches[2],config[2];
static bool paused,usb,transfer_busy,ble_failed,deep_success;
static void *s_lock_sleep_touch;
static bool s_lock_io_pending,deep_pins,deep_rails,deep_touch;
static int hardware_error;static bool late_usb;
#define CST836U_MODE_DEEPSLEEP 3
#define CST836U_MODE_NORMAL 0
static unsigned network_resumes;
static int font_closed,sd_closed,sd_error,restored,hold_calls,lcd_deinit,wake_source,sleep_calls;
static unsigned waits,wait_timeouts;
static jmp_buf deep;
static int xSemaphoreTake(int m,int timeout){(void)m;assert(timeout==100);assert(!io_taken);io_taken=1;return pdTRUE;}
static void xSemaphoreGive(int m){(void)m;assert(io_taken);io_taken=0;}
static int fail_io(void){assert(io_taken);return ++io_calls==io_fail_at?ESP_ERR_TIMEOUT:ESP_OK;}
static int fca9555_set_output(int dev,unsigned port,uint8_t value){assert(dev==ioe&&port<2);int err=fail_io();if(!err)latches[port]=value;return err;}
static int fca9555_set_config(int dev,unsigned port,uint8_t value){assert(dev==ioe&&port<2);int err=fail_io();if(!err)config[port]=value;return err;}
static int fca9555_read_input(int dev,unsigned port,uint8_t *out){assert(dev==ioe&&port<2);int err=fail_io();if(!err)*out=port==1&&key_low?0x7f:0xff;return err;}
static int read_pico_pmu_keys_pause(void){if(paused)return -1;paused=true;return ESP_OK;}
static void read_pico_pmu_keys_resume(void){assert(paused);paused=false;}
static void vTaskDelay(int ms){delay_ms+=ms;assert(delay_ms<5000);}
static int read_pico_ioe_int_level(void){return !irq_low;}
// 按键确认：深睡交接用它判断用户是否正按着电源键（按住则不睡）。
// Power-key confirmation: the parked handoff defers while the user is still holding the key.
static bool held_key;
static bool metalio_confirm_power_key(void){return held_key;}
static int timer_disabled;
static int esp_sleep_disable_wakeup_source(int source){
 assert(source==ESP_SLEEP_WAKEUP_ALL||source==ESP_SLEEP_WAKEUP_TIMER);
 if(source==ESP_SLEEP_WAKEUP_ALL)wake_source=0;else ++timer_disabled;return ESP_OK;}
static int esp_sleep_enable_ext1_wakeup_io(uint64_t mask,int mode){assert(mask==4&&mode==ESP_EXT1_WAKEUP_ANY_LOW);wake_source=1;return ESP_OK;}
static bool metalio_usb_blocks_sleep(void){return usb||(late_usb&&sd_closed);}
static bool read_pico_transfer_pause_for_sleep(void){return !transfer_busy;}
static void read_pico_transfer_resume_after_sleep(void){++network_resumes;}
static int ble_pt_suspend_for_sleep(void){return ble_failed?-1:ESP_OK;}
static int app_sync_time_to_pmu(void){return ESP_OK;}
static void app_font_begin_lock(app_lock_font_t *font){assert(!font_closed);font->reading=true;}
static void app_font_end_lock(const app_lock_font_t *font){assert(font->reading);font_closed=0;restored++;}
static int ttf_font_suspend_io(void){return 0;}
// 中文：book_suspend_io()/metalio_haptic_stop() 不在这些抽取函数的路径上（新的深睡停放只挂起字体、
// 不单独停书 I/O），桩已删除；缺卡判定搬到 lock_light_sleep_once()，本脚本不覆盖。
// English: book_suspend_io()/metalio_haptic_stop() are not on the path of the extracted functions
// (the deep-sleep park suspends the font only), so those stubs were dropped; the absent-card rule
// moved to lock_light_sleep_once(), which this harness does not cover.
static int lock_resume_media_io(unsigned wait){assert(wait==15000||wait==500);s_lock_io_pending=false;return 0;}
static int cst836u_set_mode(void *t,int mode){assert(t&&mode==3);if(hardware_error==1)return -3;deep_touch=true;return 0;}
static int cst836u_wake(void *t){assert(t);deep_touch=false;return 0;}
static int metalio_prepare_light_sleep_pins(void){deep_pins=true;return hardware_error==2?-3:0;}
static void metalio_release_light_sleep_pins(void){deep_pins=false;}
static int metalio_sleep_cut_rails(uint16_t *saved){*saved=123;deep_rails=true;return hardware_error==3?-3:0;}
static int metalio_sleep_restore_rails(uint16_t saved){assert(saved==123);deep_rails=false;return 0;}
static void read_pico_epd_restore_lcd(void){lcd_deinit=0;}
static int ttf_font_open_builtin(void){font_closed++;return ESP_OK;}
static int read_pico_sd_sync(void){if(!sd_error)sd_closed++;return sd_error;}
static void metalio_frontlight(unsigned b,unsigned w){assert(!b&&w==50);}
static void epd_poweroff(void){}
static unsigned resumes, ble_resumes;
static bool online;
static bool pico_online_busy(void){return online;}
static void ble_pt_resume_after_sleep(void){++ble_resumes;}
static int read_pico_sd_remount(void){sd_closed=0;return ESP_OK;}
static void metalio_haptic_resume(void){++resumes;}
// 旧的 metalio_poweroff()（IO11 硬切电源）已随 lock_power_off() 一起删除：这里不再有桩，
// 断言改为"关机折成深睡"，即 esp_deep_sleep_start() 被调用一次。
// The old metalio_poweroff() (IO11 hard cut) went away with lock_power_off(): there is no stub
// for it any more; the assertion becomes "off folds into deep sleep", i.e. one deep-sleep start.
static void epd_lcd_deinit(void){++lcd_deinit;}
// 锁引脚前必须已经写好 NVS 深睡标记（flash 由主电源供电，锁引脚后写就来不及了）。
// The NVS deep-wake marker must already be written when the pins are locked.
static void metalio_hold_for_deep_sleep(void){++hold_calls;assert(nvs_have&&nvs_value==1&&s_deep_wake_mark==S31_DEEP_WAKE_MAGIC);}
static bool deep_returns;
static void esp_deep_sleep_start(void){++sleep_calls;assert(wake_source&&paused&&font_closed&&sd_closed&&hold_calls&&lcd_deinit&&deep_pins&&deep_rails);assert(nvs_have&&nvs_value==1);if(deep_returns)return;deep_success=true;longjmp(deep,1);}
static int app_light_sleep_wait_timed(void *acc,unsigned ms){assert(!acc&&ms==0);return ++waits<=wait_timeouts?APP_WAKE_TIMEOUT:APP_WAKE_KEY;}
'''
for name in ('metalio_io_read','metalio_prepare_key_wake','metalio_restore_key_io'):
    unit+=function(name,root/'components/metalio_hw/metalio_hw.c')+'\n'
# 中文：lock_power_off() 已删除（IO11 硬切电源），它的语义继承者是「已停放即转深睡」的
# lock_deep_parked()；深睡标记改走 NVS，所以连它的三个标记函数一起抽出来。
# English: lock_power_off() is gone; its semantic heir is lock_deep_parked() ("already parked ->
# drop straight into deep sleep"). The wake marker moved to NVS, so pull its helpers too.
for name in ('lock_restore_key_wake','lock_prepare_key_wake','lock_wake_mark_write','lock_mark_deep_wake','lock_clear_deep_wake','lock_deep_parked','app_enter_host_sleep','lock_wait_for_unlock'):
    unit+=function(name,root/'boards/metalio47/src/main/sleep.c')+'\n'
unit+=r'''
static void reset(void){io_calls=io_fail_at=io_taken=delay_ms=irq_low=key_low=font_closed=sd_closed=sd_error=restored=hold_calls=lcd_deinit=wake_source=sleep_calls=0;paused=usb=transfer_busy=ble_failed=deep_success=false;config[0]=0xc0;config[1]=0xe3;latches[0]=outputs[0];latches[1]=outputs[1];waits=wait_timeouts=0;s_lock_sleep_touch=NULL;s_lock_io_pending=deep_pins=deep_rails=deep_touch=late_usb=false;hardware_error=0;nvs_have=false;nvs_value=0;nvs_writes=nvs_commits=0;s_deep_wake_mark=0;timer_disabled=0;held_key=false;deep_returns=false;music_suspends=music_restores=0;}
static void intact(void){assert(!font_closed&&!sd_closed&&!hold_calls&&!lcd_deinit&&!sleep_calls&&!paused&&!deep_pins&&!deep_rails&&!deep_touch&&!s_lock_io_pending);assert(config[0]==0xc0&&config[1]==0xe3&&latches[0]==0x20&&latches[1]==0x14);}
// lock_deep_parked() 的前置条件：调用方已经停好屏/字体/SD/电源轨并配好 EXT1。
// Precondition of lock_deep_parked(): the caller already parked panel/font/SD/rails and armed EXT1.
static void park_ready(void){paused=true;font_closed=1;sd_closed=1;lcd_deinit=1;deep_pins=deep_rails=true;wake_source=1;}
int main(void){
 reset();assert(app_enter_host_sleep(99)==ESP_ERR_INVALID_ARG);intact();
 reset();online=true;assert(app_enter_host_sleep(APP_SLEEP_DEEP)==ESP_ERR_NOT_FINISHED);intact();online=false;
 // 中文："关机"现在折成深睡，不再硬切电源：同一套停放流程后进入 esp_deep_sleep_start()，
 // 并且插着 USB 也照样执行（用户明确按了关机）；普通的深睡请求仍会被 USB 推迟。
 // English: "off" folds into deep sleep instead of hard-cutting power: the same parking runs and
 // deep sleep starts, even with USB attached (the user asked for it); a plain deep request defers.
 reset();if(!setjmp(deep)){app_enter_host_sleep(APP_SLEEP_OFF);assert(0);}assert(deep_success&&sleep_calls==1&&!resumes);
 reset();usb=true;if(!setjmp(deep)){app_enter_host_sleep(APP_SLEEP_OFF);assert(0);}assert(sleep_calls==1);
 reset();usb=true;assert(app_enter_host_sleep(APP_SLEEP_DEEP)==ESP_ERR_INVALID_STATE);assert(!io_calls&&!sleep_calls);intact();
 reset();assert(lock_prepare_key_wake());assert(paused&&wake_source&&config[0]==0&&config[1]==0x80&&latches[0]==0x20&&latches[1]==0x97);
 lock_restore_key_wake();intact();
 // Every failed transfer in the reference mask restores normal directions before TF/font closure.
 for(int i=1;i<=9;i++){reset();io_fail_at=i;app_enter_host_sleep(APP_SLEEP_DEEP);intact();}
 reset();irq_low=1;app_enter_host_sleep(APP_SLEEP_DEEP);assert(delay_ms==600);intact();
 reset();key_low=1;app_enter_host_sleep(APP_SLEEP_DEEP);assert(delay_ms==600);intact();
 reset();usb=true;app_enter_host_sleep(APP_SLEEP_DEEP);assert(!io_calls);intact();
 reset();transfer_busy=true;app_enter_host_sleep(APP_SLEEP_DEEP);assert(!io_calls);intact();
 reset();ble_failed=true;app_enter_host_sleep(APP_SLEEP_DEEP);assert(!io_calls);intact();
 reset();sd_error=-1;app_enter_host_sleep(APP_SLEEP_DEEP);assert(restored==1);intact();
 reset();s_lock_sleep_touch=(void*)1;sd_error=-1;
 unsigned keep_ble=ble_resumes,keep_net=network_resumes;
 assert(app_enter_host_sleep(APP_SLEEP_DEEP)==-1);assert(ble_resumes==keep_ble&&network_resumes==keep_net);intact();s_lock_sleep_touch=NULL;
 reset();if(!setjmp(deep)){app_enter_host_sleep(APP_SLEEP_DEEP);assert(0);}assert(deep_success&&sleep_calls==1);
 // Touch, bus and partial-rail failures roll back every changed resource without unlocking.
 for(int e=1;e<=3;e++){
  reset();s_lock_sleep_touch=(void*)1;hardware_error=e;
  assert(app_enter_host_sleep(APP_SLEEP_DEEP)==-3);intact();
 }
 reset();late_usb=true;assert(app_enter_host_sleep(APP_SLEEP_DEEP)==ESP_ERR_INVALID_STATE);intact();
 // 中文：停放好的交接（lock_deep_parked，被删除的 lock_power_off 的语义继承者）：
 // 同步 RTC -> 关掉浅睡定时切片 -> 写 NVS 深睡标记 -> 锁引脚 -> 深睡。
 // USB 在位或用户还按着键则推迟；深睡若返回（不该发生）要清掉标记并报超时。
 // English: the parked handoff (lock_deep_parked, heir of the deleted lock_power_off):
 // sync RTC -> drop the light-sleep timer slice -> write the NVS mark -> lock pins -> deep sleep.
 // USB or a still-held key defers; a returning deep sleep (impossible) clears the mark and times out.
 reset();park_ready();
 if(!setjmp(deep)){lock_deep_parked();assert(0);}
 assert(deep_success&&sleep_calls==1&&hold_calls==1&&timer_disabled==1&&nvs_value==1);
 reset();park_ready();usb=true;assert(lock_deep_parked()==ESP_ERR_INVALID_STATE&&!sleep_calls&&!hold_calls);
 reset();park_ready();held_key=true;assert(lock_deep_parked()==ESP_ERR_INVALID_STATE&&!sleep_calls&&!hold_calls);
 reset();park_ready();deep_returns=true;assert(lock_deep_parked()==ESP_ERR_TIMEOUT);
 assert(sleep_calls==1&&hold_calls==1&&nvs_value==0);
 // A timeout handoff that returns never unlocks; only the third, real key wake exits this loop.
 reset();usb=true;wait_timeouts=2;lock_wait_for_unlock((void*)1);assert(waits==3&&!sleep_calls);intact();
 puts("PASS: production reference TCA mask/P17-only wake, all 9 I2C faults, stuck IRQ/held key/USB/radio deferral before TF closure, SD-failure font restore, deep rail/touch/bus transaction and rollback, late USB deferral, off folded into parked deep sleep with NVS mark before the pins are held, and timeout cannot unlock");
}
'''
with tempfile.TemporaryDirectory() as d:
    p=Path(d);(p/'test.c').write_text(unit)
    subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-fsanitize=address,undefined',str(p/'test.c'),'-o',str(p/'test')],check=True)
    subprocess.run([str(p/'test')],check=True)
