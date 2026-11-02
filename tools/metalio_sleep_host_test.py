"""Run the actual retained S31 lock wait; model scheduler progress and long idle."""
from pathlib import Path
import subprocess,tempfile
from home_cover_cache_host_test import function
root=Path(__file__).resolve().parents[1]
unit=r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <setjmp.h>
// 锁屏转深睡的延时已不再是 APP_LOCK_OFF_AFTER_US 常量 + app_settings_staged_shutdown() 开关，
// 而是设置项 app_settings_sleep_off_us()（int64_t 微秒，0=从不深睡）。测试继续用 staged
// 表达原来的两档语义：staged=10 分钟档，非 staged=立即档（1 µs），原有断言全部保留。
// English: the lock-to-deep-sleep delay is now the app_settings_sleep_off_us() setting
// (int64 microseconds, 0 = never) instead of the APP_LOCK_OFF_AFTER_US constant plus the
// app_settings_staged_shutdown() switch. `staged` keeps the meaning it always had: ten minutes
// when set, immediate (1 us) when clear.
#define STAGED_OFF_DELAY_US INT64_C(600000000)
typedef int app_wake_source_t;
enum {APP_WAKE_NONE,APP_WAKE_KEY,APP_WAKE_PICKUP,APP_WAKE_TIMEOUT};
typedef void *sc7a20h_handle_t;
typedef struct {int x_mg,y_mg,z_mg;unsigned status;} sc7a20h_sample_t;
#define ESP_OK 0
#define ESP_ERR_TIMEOUT -2
#define ESP_LOGW(tag,...) do {if(0)printf(__VA_ARGS__);} while(0)
static const char *esp_err_to_name(int e){(void)e;return "error";}
#define APP_SLEEP_OFF 1
static void *s_lock_sleep_touch;
static bool s_lock_deep_ready,s_lock_frame_cached,s_lock_io_pending;
static struct {int64_t pending_at,until;} s_lock_hint;
static bool hint_window;
static int64_t esp_timer_get_time(void);
static void lock_hint_service(void){if(hint_window)s_lock_hint.until=esp_timer_get_time()<500000?500:0;}
static void lock_hint_finish(bool restore){(void)restore;}
static bool pin;
static unsigned cached_marks;
static int cached_error;
static bool lock_pin_enabled(void){return pin;}
static int pico_boot_mark_frame_cached(void){++cached_marks;return cached_error;}
static unsigned light_calls,deep_calls;
static int64_t clock_us,light_step;
static bool deep_success;static jmp_buf deep_jump;
static bool staged=true;
static bool light_key,light_fail;
static int64_t recover_at;
static int64_t esp_timer_get_time(void);
static int lock_light_sleep_once(void*a,void*t,bool*k,int64_t until,bool off_at_deadline){
 (void)off_at_deadline;
 (void)a;(void)t;light_calls++;
 int64_t step=light_step;if(until&&clock_us+step>until)step=until>clock_us?until-clock_us:0;
 clock_us+=step;*k=light_key||(recover_at>=0&&clock_us>=recover_at);return light_fail&&!*k?-1:0;
}
static int lock_resume_media_io(unsigned ms){assert(ms==0);return 0;}
static int app_enter_host_sleep(int m){assert(m==APP_SLEEP_OFF);assert(clock_us>=(staged?STAGED_OFF_DELAY_US:1));deep_calls++;if(deep_success)longjmp(deep_jump,1);return -1;}
typedef int esp_err_t;
static bool book_cover_jobs_quiesce(unsigned ms){assert(ms==2000);return true;}
static bool ui_avatar_quiesce(unsigned ms){assert(ms==2000);return true;}
#define pdMS_TO_TICKS(ms) (ms)
#define ESP_LOGI(...) ((void)0)
static int64_t key_at,plug_at=-1,unplug_at=-1;
static int frontlight_calls,motion_reads,sensor_on,sensor_off,reconnects,scheduler_yields;
static unsigned last_wake;
// 新接口：app_settings_sleep_off_us() 取代旧的 staged_shutdown 开关，staged 映射到两档延时。
// English: app_settings_sleep_off_us() replaces the retired staged_shutdown switch; staged selects the two legs.
static int64_t app_settings_sleep_off_us(void){return staged?STAGED_OFF_DELAY_US:INT64_C(1);}
// 新接口：浅睡播放音乐。默认关闭，保证原有入睡前/十分钟后交接的断言不受影响。
// English: music-through-light-sleep setting; off here so every existing handoff assertion still holds.
static bool music_in_sleep,music_playing;
static bool app_settings_music_in_sleep(void){return music_in_sleep;}
typedef enum {MUSIC_IDLE=0,MUSIC_LOADING,MUSIC_PLAYING,MUSIC_PAUSED,MUSIC_ERROR} music_state_t;
static music_state_t music_state(void){return music_playing?MUSIC_PLAYING:MUSIC_IDLE;}
// 新接口：入睡/关机前的最后一次按键确认（源码去抖窗口）。桩恒返回假，保留原有
// 「只有物理按键才会返回」的断言语义。
// English: last key check before the multi-second suspend; false keeps the existing debounce assertions.
static bool lock_settle_key(void){return false;}
// 新接口：SD 卡轨迹日志。卡不可用时静默丢弃，桩只占位、不影响流程与时钟。
// English: SD trace log; silently dropped without a card, so the stub is inert.
static void power_trace(const char *fmt,...){ (void)fmt; }
static bool pickup,usb,powered,stale_motion;
static int64_t esp_timer_get_time(void){return clock_us;}
static bool app_settings_pickup_wake(void){return pickup;}
static int sc7a20h_power_up(void *a){assert(a);sensor_on++;return ESP_OK;}
static int sc7a20h_shared_irq_idle(void *a){assert(a);sensor_off++;return ESP_OK;}
static int sc7a20h_read(void *a,sc7a20h_sample_t *s){assert(a);s->x_mg=motion_reads++?1000:0;s->y_mg=s->z_mg=0;s->status=stale_motion?0:8;return ESP_OK;}
static void metalio_frontlight(unsigned b,unsigned w){assert(!b&&w==50);frontlight_calls++;}
static void metalio_frontlight_restore(void){frontlight_calls++;}
static void lock_resume_io_service(void){}
static bool lock_take_key_wakeup(void){return key_at>=0&&clock_us>=key_at;}
// 新接口：锁屏等待顶部要求硬件按键证据（P17 仍为低）。桩恒报「有按下」，
// 让既有的 key_at 断言语义不变；drain 桩为惰性，不影响时钟与调度计数。
// New gate: the lock wait now needs hardware key evidence; the stub always reports a press
// so the existing key_at assertions keep their meaning. The drain stub is inert.
typedef struct {bool seen;bool held;bool stable;bool readable;} metalio_power_key_evidence_t;
static metalio_power_key_evidence_t metalio_power_key_evidence(void){metalio_power_key_evidence_t e={true,true,true,true};return e;}
static void read_pico_pmu_drain_events(void){}
static bool metalio_usb_blocks_sleep(void){return (unplug_at<0||clock_us<unplug_at)&&(usb||powered||(plug_at>=0&&clock_us>=plug_at));}
static void metalio_usb_serial_reconnect(void){++reconnects;}
static void read_pico_clear_ioe_int(void){}
static void vTaskDelay(int ms){assert(ms==30);scheduler_yields++;clock_us+=(int64_t)ms*1000;}
static void app_settings_set_last_wake(unsigned source){last_wake=source;}
'''
unit+='static void lock_resume_service(void){}\n'
unit+=function('app_light_sleep_wait_timed',root/'boards/metalio47/src/main/sleep.c')
unit+=r'''
static void reset(void){staged=true;hint_window=false;music_in_sleep=music_playing=false;s_lock_hint.pending_at=s_lock_hint.until=0;clock_us=0;recover_at=key_at=plug_at=unplug_at=-1;frontlight_calls=motion_reads=sensor_on=sensor_off=reconnects=scheduler_yields=0;pickup=usb=powered=stale_motion=false;light_calls=deep_calls=cached_marks=0;light_step=0;deep_success=false;cached_error=0;pin=s_lock_frame_cached=s_lock_io_pending=false;light_key=light_fail=s_lock_deep_ready=false;s_lock_sleep_touch=NULL;}
static void settled(void){assert(frontlight_calls==(last_wake==APP_WAKE_NONE?1:2));}
int main(void){
 reset();assert(app_light_sleep_wait_timed(NULL,1000)==APP_WAKE_TIMEOUT&&last_wake==APP_WAKE_NONE&&scheduler_yields==34);settled();
 reset();key_at=500000;assert(app_light_sleep_wait_timed(NULL,1000)==APP_WAKE_KEY&&clock_us<540000&&!reconnects);settled();
 reset();pickup=true;assert(app_light_sleep_wait_timed((void*)1,1000)==APP_WAKE_PICKUP&&sensor_on==1&&sensor_off==1&&motion_reads==2&&clock_us>=250000&&clock_us<300000);settled();
 reset();pickup=stale_motion=true;assert(app_light_sleep_wait_timed((void*)1,1000)==APP_WAKE_TIMEOUT&&sensor_on==1&&sensor_off==1&&motion_reads==4);settled();
 reset();assert(app_light_sleep_wait_timed((void*)1,1000)==APP_WAKE_TIMEOUT&&!sensor_on&&sensor_off==1);settled();
 reset();usb=true;key_at=500000;assert(app_light_sleep_wait_timed(NULL,100)==APP_WAKE_KEY&&clock_us>=500000);settled();
 reset();powered=true;key_at=500000;assert(app_light_sleep_wait_timed(NULL,100)==APP_WAKE_KEY&&!reconnects);settled();
 reset();plug_at=500000;key_at=1000000;assert(app_light_sleep_wait_timed(NULL,600)==APP_WAKE_KEY&&reconnects==1);settled();
 // Forty-five minutes and three hours: no deadline, only physical input returns, scheduler always progresses.
 for(unsigned mins=45;mins<=180;mins+=135){reset();key_at=(int64_t)mins*60*1000000;assert(app_light_sleep_wait_timed(NULL,0)==APP_WAKE_KEY&&clock_us>=key_at&&scheduler_yields==(int)(key_at/30000)&&!reconnects);settled();}
 reset();s_lock_sleep_touch=(void*)1;light_key=true;assert(app_light_sleep_wait_timed(NULL,0)==APP_WAKE_KEY&&light_calls==1&&!deep_calls);settled();
 reset();s_lock_sleep_touch=(void*)1;light_fail=true;key_at=500000;assert(app_light_sleep_wait_timed(NULL,0)==APP_WAKE_KEY&&light_calls==1);settled();
 // No checkpoint must not reboot or deep-sleep before the pending save has succeeded.
 reset();s_lock_sleep_touch=(void*)1;light_step=60000000;recover_at=700000000;key_at=800000000;
 assert(app_light_sleep_wait_timed(NULL,0)==APP_WAKE_KEY&&!deep_calls&&clock_us>=700000000);settled();
 // Ten-minute handoff is independent of a removed selector, including PIN and cached-art combinations.
 for(unsigned c=0;c<2;c++)for(unsigned p=0;p<2;p++)for(int e=0;e<2;e++){
 reset();s_lock_sleep_touch=(void*)1;s_lock_deep_ready=true;s_lock_frame_cached=c;pin=p;cached_error=e?-1:0;
 light_step=60000000;deep_success=true;
 if(!setjmp(deep_jump)){app_light_sleep_wait_timed(NULL,0);assert(0);}
 assert(deep_calls==1&&light_calls==10&&clock_us>=600000000&&clock_us<600100000&&cached_marks==(c&&!p));
 assert(frontlight_calls==1); // A timer-to-off handoff never restores the lamp or reports an unlock.
 }
 // A real wake immediately before the deadline wins over the handoff.
 reset();s_lock_sleep_touch=(void*)1;s_lock_deep_ready=true;light_step=60000000;recover_at=599000000;
 assert(app_light_sleep_wait_timed(NULL,0)==APP_WAKE_KEY&&!deep_calls);settled();
 // A refused power-off handoff returns to bounded light slices and retries after30s, remaining locked.
 reset();s_lock_sleep_touch=(void*)1;s_lock_deep_ready=true;light_step=60000000;recover_at=649000000;
 assert(app_light_sleep_wait_timed(NULL,0)==APP_WAKE_KEY&&deep_calls==2&&clock_us>=649000000);settled();
 reset();s_lock_sleep_touch=(void*)1;s_lock_deep_ready=true;usb=true;key_at=700000000;
 assert(app_light_sleep_wait_timed(NULL,0)==APP_WAKE_KEY&&!light_calls&&!deep_calls);settled();
 reset();s_lock_sleep_touch=(void*)1;usb=true;key_at=500000;assert(app_light_sleep_wait_timed(NULL,0)==APP_WAKE_KEY&&!light_calls);settled();
 reset();hint_window=true;s_lock_sleep_touch=(void*)1;light_key=true;assert(app_light_sleep_wait_timed(NULL,0)==APP_WAKE_KEY&&light_calls==1&&clock_us>=500000);settled();
 // A transient IRQ/IO admission failure recovers in the same lock session, without a reboot.
 reset();s_lock_sleep_touch=(void*)1;light_fail=true;recover_at=3500000;key_at=12000000;
 assert(app_light_sleep_wait_timed(NULL,0)==APP_WAKE_KEY&&light_calls==3&&clock_us>=4000000&&clock_us<4200000);settled();
 // Permanent faults are bounded at 1/3/10/30s, rather than retried every 30ms or disabled forever.
 reset();s_lock_sleep_touch=(void*)1;light_fail=true;key_at=75000000;
 assert(app_light_sleep_wait_timed(NULL,0)==APP_WAKE_KEY&&light_calls==6);settled();
 reset();s_lock_sleep_touch=(void*)1;s_lock_io_pending=true;key_at=500000;
 assert(app_light_sleep_wait_timed(NULL,0)==APP_WAKE_KEY&&!light_calls);settled();
 // A cable held for more than ten minutes must not cause immediate shutdown on unplug.
 reset();s_lock_sleep_touch=(void*)1;s_lock_deep_ready=true;usb=true;unplug_at=720000000;light_step=60000000;deep_success=true;
 if(!setjmp(deep_jump)){app_light_sleep_wait_timed(NULL,0);assert(0);}
 assert(deep_calls==1&&clock_us>=1320000000&&clock_us<1320100000);
 // Charging is not an unlock; a real key works while VBUS remains present.
 reset();s_lock_sleep_touch=(void*)1;s_lock_deep_ready=true;usb=true;key_at=900000000;
 assert(app_light_sleep_wait_timed(NULL,0)==APP_WAKE_KEY&&!deep_calls&&!light_calls);
 // The immediate-off lock option also takes the owned power-off path, never an unlock.
 reset();s_lock_sleep_touch=(void*)1;s_lock_deep_ready=true;staged=false;light_step=60000000;deep_success=true;
 if(!setjmp(deep_jump)){app_light_sleep_wait_timed(NULL,0);assert(0);}
 assert(deep_calls==1&&clock_us<100000&&frontlight_calls==1);
 // Music playing through light sleep keeps the CPU up: neither a light nor a deep entry may happen.
 // 新行为：勾选「浅睡播放音乐」且此刻在放时，既不进浅睡也不进深睡——浅睡会停 CPU 与电源轨，I2S 立刻断流。
 reset();s_lock_sleep_touch=(void*)1;s_lock_deep_ready=true;music_in_sleep=music_playing=true;key_at=1200000000;
 assert(app_light_sleep_wait_timed(NULL,0)==APP_WAKE_KEY&&!light_calls&&!deep_calls);settled();
 // 只有勾选了才算数；未勾选时在播的音乐不拦住正常的十分钟交接。
 // Only the setting counts: with it off, audio must not block the normal ten-minute handoff.
 reset();s_lock_sleep_touch=(void*)1;s_lock_deep_ready=true;music_playing=true;light_step=60000000;deep_success=true;
 if(!setjmp(deep_jump)){app_light_sleep_wait_timed(NULL,0);assert(0);}
 assert(deep_calls==1&&light_calls==10);
 puts("PASS: production S31 10min actual wait handoff model, no early power-off, PIN/art checkpoint boundaries, key priority, USB exclusion, power-off refusal then bounded retry, transient light recovery, music-in-sleep keeps the CPU up");
}
'''
with tempfile.TemporaryDirectory() as d:
 p=Path(d);(p/'test.c').write_text(unit)
 subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-g','-fsanitize=address,undefined',str(p/'test.c'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True)
