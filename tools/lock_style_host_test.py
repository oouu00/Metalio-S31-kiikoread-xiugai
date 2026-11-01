#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# 中文：实际锁屏入口按当前样式绘图；自动/手动、深睡与密码缓存回退不改选择。
# English: Actual sleep entries honor the selected artwork for auto/manual locks, deep sleep and PIN cache fallback.
#
# 中文：本次为适配源码演进所做的桩/断言调整
#   1. 源码从 main/sleep.c 迁到 boards/metalio47/src/main/sleep.c（板型拆分）。
#   2. 新增「浅睡播放音乐」：app_settings_music_in_sleep()/music_state()/MUSIC_PLAYING/
#      music_set_suspended_for_sleep()，播放中整段不挂起 BLE、不挂起音乐。
#   3. 反馈标记 s_lock_feedback_visible 改名为 s_lock_feedback_started，语义变为
#      “事件已排空过”，置位早于推屏，所以推屏失败时它依然为真。
#   4. 锁屏统一走 lock_present_art()：票根/拼贴先 update_display_white() 擦白，
#      书签走 update_display_image_gray()，壁纸走 update_display_full()。
#      旧的 epd_hl_update_screen_from_white()/epd_clear() 通道已不存在。
#   5. 锁屏转深睡改为设置项（app_settings_sleep_off_us()），本用例不再覆盖；
#      lock_wait_for_unlock() 现在是稳定性版：超时既不解锁也不进深睡，
#      所以旧的“超时 → app_enter_host_sleep(APP_SLEEP_DEEP)”断言改为
#      “超时继续等待，直到真实按键才返回”。
# English: stub/assertion updates for the recent source changes
#   1. sleep.c moved to boards/metalio47/src/main/sleep.c (per-board split).
#   2. New "music through light sleep": app_settings_music_in_sleep()/music_state()/MUSIC_PLAYING/
#      music_set_suspended_for_sleep(); while playing, BLE and audio are never suspended.
#   3. The feedback flag was renamed s_lock_feedback_visible -> s_lock_feedback_started and now
#      means "events already drained"; it is set before the push, so a failed push leaves it true.
#   4. Locked presentation is unified in lock_present_art(): ticket/collage whiten first via
#      update_display_white(), bookmarks use update_display_image_gray(), wallpaper uses
#      update_display_full(). The old epd_hl_update_screen_from_white()/epd_clear() path is gone.
#   5. Lock-to-deep-sleep is now a setting (app_settings_sleep_off_us()) and is out of scope here;
#      lock_wait_for_unlock() is the stability variant where a timeout neither unlocks nor deep
#      sleeps, so the old "timeout -> app_enter_host_sleep(APP_SLEEP_DEEP)" assertion became
#      "keep waiting until a real key arrives".
from pathlib import Path
import subprocess
import tempfile
from home_cover_cache_host_test import function
ROOT = Path(__file__).resolve().parents[1]
unit = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#define UI_LOCK_WIDTH 684
#define UI_LOCK_HEIGHT 1216
#define FRAME_BYTES (684u*1216u/2u)
enum EpdDrawError {EPD_DRAW_SUCCESS=0,EPD_DRAW_FAULT=1};
#define APP_WAKE_NONE 0
#define APP_WAKE_KEY 1
#define APP_WAKE_PICKUP 2
#define APP_WAKE_TIMEOUT 3
#define APP_LOCK_IDLE_TIMEOUT_MS 0U
#define APP_LOCK_IGNORE_BOOT_MS 1000
#define ESP_OK 0
#define ESP_ERR_NOT_FINISHED 1
#define TAG "sleep"
// 中文：日志宏不能整体丢弃参数——源码里 lock_at/draw_at 等计时变量只出现在日志里，
// 丢弃会触发 -Werror=unused-variable；这里把参数喂给一个空实现的接收函数。
// English: log macros must not drop their arguments: production keeps timing locals such as
// lock_at/draw_at only inside log calls, and discarding them trips -Werror=unused-variable.
static int log_sink(const char *fmt, ...){ (void)fmt; return 0; }
#define ESP_LOGI(tag,...) ((void)(tag),(void)log_sink(__VA_ARGS__))
#define ESP_LOGW(tag,...) ((void)(tag),(void)log_sink(__VA_ARGS__))
#define power_trace(...) ((void)log_sink(__VA_ARGS__))
#define pdMS_TO_TICKS(x) (x)
typedef int esp_err_t;
typedef struct {int x,y,width,height;} EpdRect;
typedef struct {uint8_t *back_fb;} EpdiyHighlevelState;
typedef struct {bool mounted;} read_pico_sd_info_t;
typedef struct {bool active;} app_lock_font_t;
typedef void *sc7a20h_handle_t;typedef void *cst836u_handle_t;
typedef int app_wake_source_t;
typedef enum {MUSIC_IDLE=0,MUSIC_LOADING,MUSIC_PLAYING,MUSIC_PAUSED,MUSIC_ERROR} music_state_t;
// 中文：只写不读的状态位；-Werror 下单独引用一次，避免 unused-variable。
// English: write-only state bits; referenced once so -Werror does not flag them unused.
static bool s_boot_lock_draw, s_lock_frame_cached, s_lock_deep_ready;
static cst836u_handle_t s_lock_sleep_touch;
static bool s_boot_from_deep, s_boot_power_wake, s_boot_power_on, s_boot_nvs_mark;
static uint32_t s_boot_reset_code;
static void use_lock_state(void){(void)s_boot_lock_draw;(void)s_lock_frame_cached;(void)s_lock_deep_ready;(void)s_lock_sleep_touch;}
static unsigned app_settings_wake_presses(void){return 2;}
static uint8_t pixels[FRAME_BYTES],panel[FRAME_BYTES],style,current_background=0x32;
static bool frame_ready, s_lock_feedback_started, feedback_seen, panel_whitened, final_ok=true;
static unsigned white_presents, full_presents;
static unsigned draws[4],fallback,presents,auths,font_begin,font_end,pause,resume,clear_resume;
static unsigned frontlight, restores, drains, poweroffs, inverts, guards, suspends, ble_stops, ble_resumes;
static unsigned hint_begins, hint_ends, timed_waits, timeout_waits, pin_records;
static bool draw_ok=true,transfer_ok=true,reader,boot_cache,cache_ok,music_in_sleep,music_playing;
static bool pin;
static unsigned cached_style;
static uint8_t panel_expected(void){return (feedback_seen||panel_whitened)?0xff:current_background;}
static const char *app_settings_wallpaper_path(void){return "/sdcard/wallpaper.jpg";}
static uint8_t app_settings_lock_style(void){return style;}
static bool lock_pin_enabled(void){return pin;}
static bool app_settings_music_in_sleep(void){return music_in_sleep;}
static music_state_t music_state(void){return music_playing?MUSIC_PLAYING:MUSIC_IDLE;}
static void music_set_suspended_for_sleep(void){++suspends;}
static void music_set_suspended(bool suspended){assert(!suspended);++suspends;}
static int ble_pt_suspend_for_sleep(void){++ble_stops;return ESP_OK;}
static void ble_pt_resume_after_sleep(void){++ble_resumes;}
static void metalio_frontlight(unsigned brightness,unsigned warmth){assert(brightness==0&&warmth==50);++frontlight;}
static void metalio_frontlight_restore(void){++restores;}
static void read_pico_pmu_drain_events(void){++drains;}
static void epd_poweroff(void){++poweroffs;}
static void epd_set_inverted(bool inverted){assert(!inverted);++inverts;}
static void guard_draw_result(EpdiyHighlevelState *h,enum EpdDrawError e){(void)h;(void)e;++guards;}
static bool draw(unsigned chosen,uint8_t *fb){assert(chosen==style&&panel[0]==panel_expected());if(chosen==3)assert(fb[0]==(boot_cache?0xff:current_background));++draws[chosen];memset(fb,0x10+chosen,FRAME_BYTES);frame_ready=draw_ok;return draw_ok;}
static bool ui_wallpaper_prepare_lock(uint8_t *fb,const char *path){assert(!strcmp(path,app_settings_wallpaper_path()));return draw(1,fb);}
static void ui_wallpaper_cache_commit(const uint8_t *fb){assert(style==1&&draw_ok&&presents==1&&fb==pixels&&!memcmp(panel,pixels,FRAME_BYTES));}
static bool book_ticket_draw(uint8_t *fb,bool background){assert(background==reader);return draw(0,fb);}
static const char *app_settings_bookmark_path(void){return "/sdcard/overlay.png";}
static bool book_lock_bookmark_draw(uint8_t *fb,const char *p){assert(!strcmp(p,app_settings_bookmark_path()));return draw(3,fb);}
static bool lock_screen_save(const uint8_t *fb){assert(fb==pixels&&presents);return true;}
static bool book_lock_collage_prepare(uint8_t *fb){return draw(2,fb);}
static void book_lock_collage_cache_commit(const uint8_t *fb){assert(style==2&&draw_ok&&presents==1&&fb==pixels&&!memcmp(panel,pixels,FRAME_BYTES));}
// 中文：从真实 back 历史一次推满；失败只把目标帧写回面板占位，不算成功推送。
// English: one full push from real back history; a fault only scribbles the panel, never counts as presented.
static enum EpdDrawError present_once(void){
 ++full_presents;
 assert(frame_ready&&pixels[0]==(draw_ok?0x10+style:0xff)&&panel[0]==panel_expected());
 // 中文：失败时不提交——面板保持推送前的历史，用例用 !presents 判断没有宣布完成。
 // English: a fault never commits: the panel keeps its pre-push history and !presents proves nothing was announced.
 if(!final_ok)return EPD_DRAW_FAULT;
 memcpy(panel,pixels,FRAME_BYTES);++presents;return EPD_DRAW_SUCCESS;
}
static enum EpdDrawError update_display_full(EpdiyHighlevelState *h){(void)h;return present_once();}
static enum EpdDrawError update_display_image_gray(EpdiyHighlevelState *h){(void)h;return present_once();}
// 中文：票根/拼贴保留的“擦白”推送；壁纸和书签不再多推一次白屏。
// English: the whitening push retained by ticket/collage; wallpaper and bookmarks skip it.
static enum EpdDrawError update_display_white(EpdiyHighlevelState *h){(void)h;++white_presents;memset(panel,255,FRAME_BYTES);panel_whitened=true;return EPD_DRAW_SUCCESS;}
static bool display_system_binary_set(bool enabled){(void)enabled;return false;}
static void ui_lock_fallback_draw(uint8_t *fb){assert(panel[0]==panel_expected());++fallback;memset(fb,255,FRAME_BYTES);frame_ready=true;}
static uint8_t *epd_hl_get_framebuffer(EpdiyHighlevelState *h){(void)h;return pixels;}
static void epd_hl_set_all_white(void *h){(void)h;memset(pixels,255,FRAME_BYTES);}
static void app_font_begin_lock(app_lock_font_t *s){assert(panel[0]==panel_expected());s->active=true;++font_begin;}
static void app_font_end_lock(app_lock_font_t *s){assert(s->active);++font_end;}
static bool read_pico_transfer_pause_for_sleep(void){++pause;return transfer_ok;}
static void read_pico_transfer_resume_after_sleep(void){++resume;}
static void pico_boot_clear_resume(void){++clear_resume;}
static bool pico_boot_has_resume(void){return boot_cache;}
static bool pico_boot_lock_pending(void){return false;}
static void pico_boot_ready(void){}
static void pico_boot_hold_resume(void){}
static bool metalio_boot_power_key(void){return false;}
static bool app_lock_resume_frame(EpdiyHighlevelState *h){(void)h;return false;}
// 中文：冷启动门禁里的续读服务不在本用例范围，桩成空实现。
// English: the boot gate's resume service is out of scope here; stub it out.
static void lock_resume_service(void){}
// 中文：稳定性版超时只返回 APP_WAKE_TIMEOUT，等待循环继续；真实按键才退出。
// English: the stability build answers a timeout with APP_WAKE_TIMEOUT and loops; only a key exits.
static app_wake_source_t app_light_sleep_wait_timed(void *acc,unsigned ms){
 assert(!acc&&ms==APP_LOCK_IDLE_TIMEOUT_MS);++timed_waits;
 app_wake_source_t wake=timed_waits<=timeout_waits?APP_WAKE_TIMEOUT:APP_WAKE_KEY;
 // 中文：与生产一致——只有真实按键唤醒才恢复前光，超时继续灭灯。
 // English: as in production, only a real key wake restores the lamp; a timeout leaves it dark.
 if(wake!=APP_WAKE_TIMEOUT) metalio_frontlight_restore();
 return wake;
}
static void lock_hint_begin(EpdiyHighlevelState *h){(void)h;++hint_begins;}
static void lock_hint_end(void){++hint_ends;}
static esp_err_t lock_resume_media_io(uint32_t wait_ms){assert(!wait_ms);return ESP_OK;}
static void app_settings_record_pin_lock(uint32_t unix_seconds){(void)unix_seconds;++pin_records;}
static void lock_screen_authenticate(void *h,void *tp,void *acc,bool boot){(void)h;assert(tp==(void*)2&&acc==(void*)1);assert(pin);assert(boot==boot_cache);assert(pixels[0]==(draw_ok?0x10+style:0xff));++auths;}
static int read_pico_sd_get_info(read_pico_sd_info_t *i){i->mounted=true;return 0;}
static void vTaskDelay(int ms){assert(ms==20);}
static int epd_width(void){return 684;}
static int epd_height(void){return 1216;}
static int64_t esp_timer_get_time(void){return 2000000;}
static bool lock_screen_restore(uint8_t *fb){if(!cache_ok||cached_style!=style)return false;memset(fb,0x10+style,FRAME_BYTES);return true;}
'''
for name in ('draw_wallpaper', 'lock_present_art', 'lock_wait_for_unlock', 'enter_lock_and_sleep', 'app_lock_boot_gate'):
    unit += function(name, ROOT/'boards/metalio47/src/main/sleep.c') + '\n'
unit += r'''
static void reset(void){
 current_background=0x32;memset(draws,0,sizeof(draws));
 fallback=presents=auths=font_begin=font_end=pause=resume=clear_resume=0;
 white_presents=full_presents=frontlight=restores=drains=poweroffs=inverts=guards=0;
 suspends=ble_stops=ble_resumes=hint_begins=hint_ends=timed_waits=timeout_waits=pin_records=0;
 memset(pixels,0x32,FRAME_BYTES);memset(panel,0x32,FRAME_BYTES);
 frame_ready=s_lock_feedback_started=feedback_seen=panel_whitened=false;
 final_ok=draw_ok=transfer_ok=true;
 reader=boot_cache=cache_ok=music_in_sleep=music_playing=pin=false;
 s_lock_sleep_touch=NULL;s_lock_frame_cached=s_lock_deep_ready=false;
}
int main(void){
 EpdiyHighlevelState hl={panel};int64_t ignore=0;
 use_lock_state();
 // 两种来源的阅读背景只影响票根内容，不决定样式。/ Reader provenance affects ticket content, never the style.
 for(unsigned s=0;s<4;++s)for(unsigned password=0;password<2;++password)for(unsigned body=0;body<2;++body){
  reset();style=s;pin=password;reader=body;
  enter_lock_and_sleep(&hl,&ignore,(void*)1,(void*)2,reader);
  assert(draws[s]==1&&draws[(s+1)%3]==0&&draws[(s+2)%3]==0&&!fallback&&presents==1&&full_presents==1);
  assert(white_presents==(s==0||s==2)&&guards==(s==0||s==2));
  assert(auths==password&&pause==1&&resume==1&&clear_resume==1&&ignore==3000);
  assert(font_begin==(s==0||s==2||password)&&font_end==font_begin&&app_settings_lock_style()==s);
  // 中文：无密码时由等待循环按键唤醒恢复前光；有密码时走验证门禁，前光由解锁后流程接管。
  // English: without a PIN the wait loop restores the lamp on the key wake; with a PIN the auth gate hands the lamp to the post-unlock flow.
  assert(frontlight==1&&poweroffs==2&&inverts==1&&restores==!password);
  assert(ble_stops==1&&ble_resumes==1&&suspends==2&&hint_begins==1&&hint_ends==1&&pin_records==password);
  // 入口排空一次（反馈未起），返回前围绕清屏再排空两次，丢弃等待期间的按键。
  assert(drains==3);
 }
 // 首页/书架/设置/阅读都保留当前帧；没有全屏阅读重排或历史书籍依赖。
 // Home/shelf/settings/reader all preserve their actual frame without fullscreen reflow or book history.
 for(unsigned screen=0;screen<4;++screen){reset();style=3;pin=false;current_background=0x21+screen*17;memset(pixels,current_background,FRAME_BYTES);memset(panel,current_background,FRAME_BYTES);enter_lock_and_sleep(&hl,&ignore,(void*)1,(void*)2,false);assert(draws[3]==1&&presents==1&&!fallback&&!white_presents);}
 // 白底圆锁历史一次GC16完成后再缓存/PIN，失败恢复原清屏，自动/其他样式不走提前反馈。/ Present once from true badge history before cache/PIN; faults clear and idle/styles keep the original path.
 for(unsigned success=0;success<2;++success)for(unsigned password=0;password<2;++password){
  reset();style=2;pin=password;final_ok=success;s_lock_feedback_started=feedback_seen=true;memset(panel,255,FRAME_BYTES);panel[1]=0;
  enter_lock_and_sleep(&hl,NULL,(void*)1,(void*)2,false);
  // 推屏失败会重推一次；仍失败就不提交画面，但开了密码仍必须走到验证边界。
  // A failed push retries once; a second failure withholds the frame, yet a PIN still reaches auth.
  if(success)assert(full_presents==1&&presents==1&&presents==1);
  else assert(full_presents==2&&!presents);
  assert(auths==password&&!s_lock_feedback_started&&frontlight==1&&restores==!password);
  assert(resume==1&&clear_resume==1&&ble_stops==1&&ble_resumes==1);
  assert(drains==(success||password?2:0));
 }
 reset();style=2;pin=false;transfer_ok=false;s_lock_feedback_started=feedback_seen=true;enter_lock_and_sleep(&hl,NULL,(void*)1,(void*)2,false);assert(!full_presents&&!s_lock_feedback_started&&!presents&&clear_resume==1&&!resume&&restores==1&&!ble_stops);
 // 失败只用内建回退，绝不改成其他样式；传输忙时不绘锁屏。/ Draw failure falls back only to built-in art; active uploads defer lock.
 for(unsigned s=0;s<4;++s){reset();style=s;pin=false;draw_ok=false;enter_lock_and_sleep(&hl,NULL,(void*)1,(void*)2,false);assert(draws[s]==1&&fallback==1&&style==s&&presents==1);}
 reset();transfer_ok=false;enter_lock_and_sleep(&hl,NULL,(void*)1,(void*)2,false);assert(!presents&&!auths&&!font_begin&&clear_resume==1&&!resume);
 // 浅睡播放音乐且正在播放：整段不挂起 BLE/音乐，只关灯关屏；解锁也不需要恢复音频。
 // Music playing through light sleep: BLE and audio stay up; only panel and lamp go dark.
 reset();style=2;pin=false;music_in_sleep=music_playing=true;enter_lock_and_sleep(&hl,NULL,(void*)1,(void*)2,false);
 assert(presents==1&&draws[2]==1&&!ble_stops&&!ble_resumes&&!suspends&&frontlight==1&&restores==1);
 // 超时既不解锁也不进深睡：等待循环继续，真实按键才返回并恢复网络。
 // A timeout neither unlocks nor deep sleeps: the wait loop continues until a real key returns.
 for(unsigned s=0;s<4;++s){reset();style=s;pin=false;timeout_waits=1;enter_lock_and_sleep(&hl,NULL,(void*)1,(void*)2,false);assert(presents==1&&draws[s]==1&&style==s&&resume==1&&timed_waits==2&&!pin_records);}
 // 密码冷启动：同样式缓存才命中，跨样式/损坏时重画当前选择。/ Cold PIN gate redraws the current choice for stale/corrupt cache.
 for(unsigned s=0;s<4;++s)for(unsigned previous=0;previous<4;++previous){
  reset();style=s;pin=boot_cache=cache_ok=true;cached_style=previous;
  app_lock_boot_gate(&hl,(void*)2,(void*)1);
  assert(auths==1&&font_begin==1&&font_end==1&&!fallback&&style==s);
  assert(draws[s]==(previous!=s)&&draws[(s+1)%3]==0&&draws[(s+2)%3]==0&&presents==(previous!=s));
  // 中文：冷启动门禁不做擦白推送；重画时才走一次结果守卫。
  // English: the cold gate never runs the whitening push; the guard fires only when it redraws.
  assert(hint_begins==1&&hint_ends==1&&!white_presents&&guards==(previous!=s));
 }
 for(unsigned s=0;s<4;++s){reset();style=s;pin=boot_cache=true;draw_ok=false;app_lock_boot_gate(&hl,(void*)2,(void*)1);assert(auths==1&&draws[s]==1&&fallback==1&&style==s&&presents==1);}
 reset();pin=false;app_lock_boot_gate(&hl,(void*)2,(void*)1);assert(!auths&&!font_begin&&!presents);
 puts("PASS: real single-pass final collage retains white/badge history, avoids duplicate entry clear, recovers final faults and consumes feedback before cache/PIN; auto/manual lock entry chooses ticket/wallpaper/collage with/without PIN and reader background; upload deferral; music-in-light-sleep keeps BLE/audio up; timeout keeps waiting; cold gate cache/fault fallback keeps selected style");
}
'''
with tempfile.TemporaryDirectory() as folder:
    c, exe = Path(folder)/'test.c', Path(folder)/'test'
    c.write_text(unit)
    subprocess.run(['cc','-std=gnu11','-Wall','-Wextra','-Werror','-fsanitize=address,undefined',str(c),'-o',str(exe)],check=True)
    subprocess.run([str(exe)],check=True)
