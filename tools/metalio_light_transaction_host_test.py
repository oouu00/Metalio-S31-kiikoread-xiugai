"""Exercise production S31 light sleep: long timer idle, USB, keys and rollback."""
from pathlib import Path
import subprocess
import tempfile
from home_cover_cache_host_test import function

root = Path(__file__).resolve().parents[1]
source = root / 'boards/metalio47/src/main/sleep.c'
if not source.exists():
    source = root / 'boards/metalio47/src/main/sleep.c'
unit = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <setjmp.h>
typedef int esp_err_t;
typedef void *sc7a20h_handle_t;
typedef void *cst836u_handle_t;
typedef struct {bool mounted;} read_pico_sd_info_t;
enum {ESP_OK=0,ESP_ERR_NOT_FINISHED=1,ESP_ERR_INVALID_STATE=-1,ESP_ERR_TIMEOUT=-3,ESP_SLEEP_WAKEUP_EXT1=2,ESP_SLEEP_WAKEUP_TIMER=4};
#define CST836U_MODE_DEEPSLEEP 1
#define pdMS_TO_TICKS(x) (x)
#define ESP_LOGI(tag,...) do {if(0)printf(__VA_ARGS__);} while(0)
#define ESP_LOGW ESP_LOGI
static const char *esp_err_to_name(int e){(void)e;return "error";}
static bool s_lock_io_pending,s_lock_io_no_media,usb,busy,online,queued,allow_key,keys,rails,pins,lcd,touch,media,usb_at_timer,slow_media,confirmed;
static bool off_success;static unsigned parked_deep;
static jmp_buf deep_jump;
static unsigned sleeps,stop_after,usb_after,timers,cut,restore,closes,mounts,resume,keys_restored,latches;
static unsigned fail_at,operation;
static int64_t clock_us;
static uint64_t timer_us;
static int64_t esp_timer_get_time(void){return clock_us;}
static unsigned fail(void){return ++operation==fail_at;}
static bool metalio_usb_blocks_sleep(void){return usb||(usb_after&&sleeps>=usb_after);}
static bool usb_storage_active(void){return busy;}
static bool pico_online_busy(void){return online;}
static int read_pico_sd_get_info(read_pico_sd_info_t *i){i->mounted=media;return fail()?-2:ESP_OK;}
static int book_suspend_io(void){return fail()?-2:ESP_OK;}
static int ttf_font_suspend_io(void){return fail()?-2:ESP_OK;}
// 新桩：SD 卡轨迹日志。卡不可用时静默丢弃，桩不记数、不影响流程与时钟。
// English: new stub for the SD trace log; silently dropped without a card and inert here.
static void power_trace(const char *fmt,...){ (void)fmt; }
// 新桩：唤醒后的按键证据。源码已把「没认出来<｜hy_place▁holder▁no▁813｜>二级 evidence：
//   seen = 休眠期间 P17 真的低过（按下过），held = 事后 12 ms 仍按着。
// 旧的 metalio_confirm_power_key() 只等价于 held，所以现在额外给出独立的 seen。
// English: new stub for the post-wake key evidence, which splits one read into two levels:
//   seen = P17 was really low during sleep (a press happened), held = still low 12 ms later.
// The old metalio_confirm_power_key() only matched `held`, hence the separate `seen` flag.
typedef struct {bool seen;bool held;} metalio_power_key_evidence_t;
static bool evidence_seen;
static metalio_power_key_evidence_t metalio_power_key_evidence(void){
    metalio_power_key_evidence_t e={evidence_seen,confirmed};
    return e;
}
// 新桩：解锁判定现在还会看 app_settings_wake_presses()（1=单击直接解锁，2=仍要采样双击）。
// 默认取双击，保留「GPIO 只唤醒 CPU、双击才解锁」的原语义。
// English: the unlock decision now reads app_settings_wake_presses() (1: EXT1 alone unlocks,
// 2: still needs the sampled double click). Double is the default so the original semantics hold.
static unsigned wake_presses=2;
static unsigned app_settings_wake_presses(void){return wake_presses;}
// CST836U 模式：源码新增「连试两次都唤不醒时强制拉回 normal」兜底（值取自 cst836u.h）。
// English: the new source forces NORMAL mode when the touch chip will not wake twice; value from cst836u.h.
#define CST836U_MODE_NORMAL 0
static int cst836u_set_mode(void *t,int m){assert(t&&(m==CST836U_MODE_NORMAL||m==CST836U_MODE_DEEPSLEEP));if(fail())return -2;touch=m==CST836U_MODE_DEEPSLEEP;return ESP_OK;}
static int read_pico_sd_sync(void){if(fail())return -2;media=false;++closes;return ESP_OK;}
static void metalio_haptic_stop(void){}
static void metalio_haptic_resume(void){}
static void metalio_frontlight(unsigned b,unsigned w){assert(!b&&w==50);}
static void epd_poweroff(void){}
static void epd_lcd_deinit(void){lcd=true;}
static int metalio_prepare_light_sleep_pins(void){pins=true;return fail()?-2:ESP_OK;}
static int sc7a20h_shared_irq_idle(void *a){assert(a);return fail()?-2:ESP_OK;}
static bool lock_take_key_wakeup(void){bool v=allow_key&&queued;queued=false;return v;}
static bool lock_prepare_key_wake(void){if(fail())return false;keys=true;return true;}
static int esp_sleep_enable_timer_wakeup(uint64_t us){assert(us>0&&us<=60000000);timer_us=us;if(usb_at_timer)usb=true;return fail()?-2:ESP_OK;}
static int metalio_sleep_cut_rails(uint16_t *s){*s=123;rails=true;++cut;return fail()?-2:ESP_OK;}
static int metalio_hold_for_light_sleep(void){return fail()?-2:ESP_OK;}
static int metalio_light_sleep_start(void){
 assert(pins&&rails&&keys&&touch&&lcd&&!media);assert(++sleeps<=181);
 clock_us+=(int64_t)timer_us;return fail()?-2:ESP_OK;
}
static uint32_t esp_sleep_get_wakeup_causes(void){return 1u<<(sleeps==stop_after?ESP_SLEEP_WAKEUP_EXT1:ESP_SLEEP_WAKEUP_TIMER);}
static int read_pico_ioe_int_level(void){return 1;}
static bool metalio_confirm_power_key(void){assert(keys&&rails&&pins);return confirmed;}
static void vTaskDelay(unsigned ms){clock_us+=(int64_t)ms*1000;}
static void metalio_release_light_sleep_pins(void){pins=false;}
static int esp_sleep_disable_wakeup_source(int s){assert(s==ESP_SLEEP_WAKEUP_TIMER);++timers;return ESP_OK;}
static int metalio_sleep_restore_rails(uint16_t s){assert(s==123);rails=false;++restore;return fail()?-2:ESP_OK;}
static void lock_restore_key_wake(void){keys=false;++keys_restored;}
static void read_pico_pmu_latch_power_wakeup(void){assert(!keys);queued=true;++latches;}
static int read_pico_sd_remount(void){assert(!rails&&!pins);++mounts;media=true;return ESP_OK;}
static int lock_resume_media_io(unsigned wait){assert(wait==500&&(!closes||media)&&!lcd&&!touch);++resume;s_lock_io_pending=slow_media;return slow_media?ESP_ERR_NOT_FINISHED:ESP_OK;}
static void read_pico_epd_restore_lcd(void){assert(!rails&&!pins&&(!closes||media));lcd=false;}
// 新行为：源码在 cst836u_wake() 失败后再试一次（ recovery 重试），所以单次 I2C 毛刺不再算失败。
// 桩同时提供 switch（touch_wake_fail）模拟「真的坏了」：两次都失败。
// English: the source now retries cst836u_wake() once, so a single I2C glitch is absorbed; the
// persistent switch models a chip that really stays broken across both attempts.
static unsigned touch_wakes;
static bool touch_wake_fail;
static int cst836u_wake(void *t){assert(t);touch=false;++touch_wakes;return (fail()||touch_wake_fail)?-2:ESP_OK;}
'''
unit += r'''
// 旧桩 lock_power_off_parked() 已随源码删除：现在走 lock_deep_parked()（保电深睡，不再硬切 IO11 电源）。
// 行为与原来一致：停在已切断的电源轨上，不恢复 LCD/TF/前光；成功即长跳转，
// 失败返回 ESP_ERR_TIMEOUT（深睡没睡成）。
// English: the retired lock_power_off_parked() stub is replaced by lock_deep_parked() (power is kept
// and the chip parks in deep sleep instead of hard-cutting IO11). Behaviour matches otherwise: LCD/SD/
// frontlight stay parked, success longjmps, a refused drop returns ESP_ERR_TIMEOUT.
static int lock_deep_parked(void){assert(rails&&pins&&keys&&touch&&lcd&&!media&&closes==1&&!mounts&&!resume);++parked_deep;if(off_success)longjmp(deep_jump,1);return ESP_ERR_TIMEOUT;}
'''
unit += function('lock_light_sleep_slice_us', source)
unit += function('lock_light_sleep_once', source)
unit += r'''
static void reset(void){
 s_lock_io_no_media=s_lock_io_pending=usb=busy=online=queued=keys=rails=pins=lcd=touch=usb_at_timer=slow_media=false;
 off_success=false;parked_deep=0;media=allow_key=confirmed=evidence_seen=true;touch_wake_fail=false;
 sleeps=usb_after=timers=cut=restore=closes=mounts=resume=keys_restored=latches=touch_wakes=0;
 stop_after=1;fail_at=operation=0;clock_us=0;wake_presses=2;
}
static void restored(void){assert(!keys&&!rails&&!pins&&!lcd&&!touch);assert(cut==restore);assert(closes==mounts);}
int main(void){
 bool key;
 reset();assert(lock_light_sleep_once((void*)1,(void*)2,&key,0,false)==ESP_OK&&key&&latches==1);restored();
 reset();slow_media=true;assert(lock_light_sleep_once((void*)1,(void*)2,&key,0,false)==ESP_OK&&key&&s_lock_io_pending);restored();
 // Three hours of timer slices only prepare/close/remount once; none of the timer wakes grants unlock.
 reset();stop_after=180;assert(lock_light_sleep_once((void*)1,(void*)2,&key,0,false)==ESP_OK&&key);
 assert(sleeps==180&&closes==1&&mounts==1&&cut==1&&restore==1&&latches==1&&clock_us>=10800000000LL);restored();
 // Inserting USB exits maintenance and restores the normal serial route's peripherals without unlocking.
 reset();stop_after=180;usb_after=3;assert(lock_light_sleep_once((void*)1,(void*)2,&key,0,false)==ESP_OK&&!key&&!latches);
 assert(sleeps==3&&mounts==1&&closes==1);restored();
 // Double-click remains enforced after a single EXT1: the caller stays locked and retries later.
 reset();allow_key=false;assert(lock_light_sleep_once((void*)1,(void*)2,&key,0,false)==ESP_ERR_INVALID_STATE&&!key&&latches==1);restored();
 // A shared EXT1 without verified physical P17 is not an unlock in single or double mode.
 // 新源码把证据拆成两级，所以「没有验证到 P17」要同时清掉 seen 与 held。
 // English: the evidence is split in two now, so an unverified shared IRQ clears seen as well as held.
 for(unsigned mode=0;mode<2;mode++){
  reset();allow_key=mode;confirmed=evidence_seen=false;
  assert(lock_light_sleep_once(NULL,(void*)2,&key,0,false)==ESP_ERR_INVALID_STATE&&!key&&!latches);restored();
 }
 // Every failing operation restores all resources already changed; no timer or error unlocks.
 // 例外：源码新增的 cst836u_wake() 重试能吃掉一次注入的 I2C 毛刺——只有这一次
 // （touch_wakes==2，说明第二次尝试救回来了）允许返回 ESP_OK，且仍然不解锁。
 // English: one exception — the brand-new cst836u_wake() retry absorbs a single injected I2C glitch,
 // so that operation (touch_wakes==2, i.e. the second attempt rescued it) may return ESP_OK while
 // still leaving the caller locked.
 reset();assert(lock_light_sleep_once((void*)1,(void*)2,&key,0,false)==ESP_OK);unsigned count=operation;
 for(unsigned i=1;i<=count;i++){
  reset();fail_at=i;int err=lock_light_sleep_once((void*)1,(void*)2,&key,0,false);
  assert(err!=ESP_OK||touch_wakes==2);restored();assert(!key||latches==1);assert(timers<=1);
 }
 for(unsigned i=0;i<3;i++){
  reset();if(i==0)usb=true;else if(i==1)busy=true;else online=true;
  assert(lock_light_sleep_once(NULL,(void*)2,&key,0,false)==ESP_ERR_INVALID_STATE&&!key&&!sleeps&&!closes);
 }
 reset();media=false;assert(lock_light_sleep_once(NULL,(void*)2,&key,0,false)==ESP_OK&&key&&sleeps==1&&closes==1);restored();
 reset();queued=true;assert(lock_light_sleep_once((void*)1,(void*)2,&key,0,false)!=ESP_OK&&key&&!sleeps);restored();
 reset();usb_at_timer=true;assert(lock_light_sleep_once((void*)1,(void*)2,&key,0,false)!=ESP_OK&&!key&&!sleeps);restored();
 // A timer session stops at the deep deadline rather than endlessly repeating60s slices.
 reset();stop_after=180;assert(lock_light_sleep_once((void*)1,(void*)2,&key,600000000,false)==ESP_OK&&!key);
 assert(clock_us>=600000000&&clock_us<600100000&&sleeps==10&&mounts==1&&!latches);restored();
 // Short final slices and already-expired deadlines cannot oversleep by another minute.
 // 新源码在电源轨恢复后先给触摸芯片 20 ms 上电稳定再发复位脉冲，所以总时长多 20 ms。
 // English: the source now settles the touch chip for 20 ms after the rails come back, so add it here.
 reset();stop_after=180;assert(lock_light_sleep_once((void*)1,(void*)2,&key,1500000,false)==ESP_OK&&!key);
 assert(clock_us==1520000&&sleeps==1);restored();
 reset();stop_after=180;assert(lock_light_sleep_once((void*)1,(void*)2,&key,1,false)==ESP_OK&&!key&&!sleeps);restored();
 // The delayed deep handoff parks from parked rails, with no TF/LCD/frontlight restoration.
 // 旧实现在这里硬切 IO11 断电，新源码改为保电深睡（lock_deep_parked），其余不变：
 // 不恢复 TF/LCD/前光，交接期间没有真实按键就不解锁。
 // English: this used to hard-cut IO11; the new source parks the chip in deep sleep instead. Everything
 // else is unchanged: no TF/LCD/frontlight restore and no unlock without a real press.
 reset();stop_after=180;confirmed=false;off_success=true;
 if(!setjmp(deep_jump)){lock_light_sleep_once(NULL,(void*)2,&key,600000000,true);assert(0);}
 assert(parked_deep==1&&rails&&pins&&lcd&&touch&&!mounts&&!resume&&!latches);
 // A refused deep entry rolls back all changed peripherals and remains locked.
 reset();stop_after=180;confirmed=false;
 assert(lock_light_sleep_once(NULL,(void*)2,&key,600000000,true)==ESP_ERR_TIMEOUT&&!key&&parked_deep==1);restored();
 // A physical press at the deadline takes the normal verified wake path instead of deep sleeping.
 reset();stop_after=180;
 assert(lock_light_sleep_once(NULL,(void*)2,&key,600000000,true)==ESP_OK&&key&&!parked_deep);restored();
 // New: a tap released while peripherals restore still counts — the latch keys off `seen`, not `held`.
 // 新行为：醒后已经松开的一按（seen=1, held=0）照样补记并解锁，不再要求「仍按着」。
 reset();confirmed=false;assert(lock_light_sleep_once((void*)1,(void*)2,&key,0,false)==ESP_OK&&key&&latches==1);restored();
 // New: single-press mode treats the sole EXT1 line itself as proof of a power press and unlocks at once.
 // 新行为：单击模式下不再要求 PMU FIFO 二次采样，EXT1 命中即解锁。
 reset();wake_presses=1;confirmed=false;allow_key=false;
 assert(lock_light_sleep_once((void*)1,(void*)2,&key,0,false)==ESP_OK&&key&&latches==1);restored();
 // New: a touch chip that never wakes is forced back to NORMAL after two tries — it must never be left
 // parked in deep sleep, yet the failure still surfaces as an error and does not unlock by itself.
 // 新行为：连试两次唤不醒就强制回 normal（I2C 恢复后可自愈），失败照旧上报、仍不解锁。
 reset();touch_wake_fail=true;
 assert(lock_light_sleep_once((void*)1,(void*)2,&key,0,false)==-2&&touch_wakes==2&&!touch&&key&&latches==1);restored();
 puts("PASS: actual S31 light transaction, 3h timer retention and exact 10min/short/expired deadlines with one TF cycle, USB insertion/no unlock, single IRQ/double gate, all operation faults/rollback, key-evidence tap rescue, single-press unlock and deep-parked handoff");
}
'''
with tempfile.TemporaryDirectory() as folder:
    path = Path(folder)
    (path / 'test.c').write_text(unit)
    subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-g',
                    '-fsanitize=address,undefined', str(path / 'test.c'), '-o', str(path / 'test')], check=True)
    subprocess.run([str(path / 'test')], check=True)
