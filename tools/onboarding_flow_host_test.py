#!/usr/bin/env python3
# SPDX-License-Identifier: LicenseRef-Kiikoread-Personal-Study-1.0
"""运行真实引导事件与保存流程，模拟页面返回、失卡及写失败。 / Actual setup actions, child returns, media loss and save failures."""
from pathlib import Path
import subprocess,tempfile
from home_cover_cache_host_test import function
root=Path(__file__).resolve().parents[1];source=root/'main/apps/app_onboarding.c'
u=r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#define ESP_OK 0
#define TTF_FONT_PATH_MAX 288
#define TTF_FONT_NAME_MAX 96
typedef struct {int x,y,width,height;}EpdRect;
typedef enum{APP_REDRAW_NONE,APP_REDRAW_PAGE,APP_REDRAW_FULL}app_redraw_t;
typedef struct{int id;}app_desc_t;
typedef struct{int64_t now_ms;const app_desc_t *request_app;}app_ctx_t;
typedef struct{int type,x0,y0,x,y;}ui_gesture_event_t;
#define UI_GESTURE_TAP 1
typedef struct{bool mounted;}read_pico_sd_info_t;
typedef struct{int state,mode;bool network_ready;char wifi_ssid[33];}read_pico_transfer_status_t;
typedef struct{bool busy;}online_font_status_t;
typedef struct{bool confirm_cancel,closed,onboarding,english,installed[5];online_font_status_t status;}ui_online_fonts_t;
typedef struct{const char *name,*filename;}online_font_item_t;
#define ONLINE_FONT_SETUP_INDEX 5
#define ONLINE_FONT_DIRECTORY "/sdcard/assets/fonts"
typedef struct{char path[288],name[96];}ttf_font_item_t;
static const app_desc_t app_transfer={1},home={0};
enum{SETUP_WIFI,SETUP_UPLOAD,SETUP_USB,READ_PICO_TRANSFER_READY,READ_PICO_TRANSFER_MODE_STA};
static unsigned s_step;static bool s_initialized,s_local_picker,s_setup_time_requested;static unsigned s_local_page;static int s_font_index;
static char s_font_path[288],s_font_name[96],s_notice[96];static unsigned s_font_count;static int64_t s_poll_ms;
static read_pico_transfer_status_t s_network;static bool connected;

static bool mounted=true,english,save_fail,finish_fail;static int scans,wifi,uploads,usb,time_requests,finished,saved;
static void app_time_service_request_online_sync(void){++time_requests;}
static char system_path[288],reading_path[288];
static const ttf_font_item_t catalog[]={{"/sdcard/fonts/one.ttf","One"},{"/sdcard/fonts/two.otf","Two"}};
static bool ui_rect_hit(EpdRect r,int x,int y){return x>=r.x&&y>=r.y&&x<r.x+r.width&&y<r.y+r.height;}
static const char *label(const char*z,const char*e){return english?e:z;}
static const app_desc_t *app_home_page(void){return &home;}
static int app_setup_set_english(bool on){if(save_fail)return 9;english=on;return 0;}
static int app_setup_finish(void){if(finish_fail)return 9;++finished;return 0;}
static void app_settings_set_system_font_path(const char*p){if(!save_fail){strcpy(system_path,p);++saved;}}
static void app_settings_set_font_path(const char*p){if(!save_fail){strcpy(reading_path,p);++saved;}}
static const char *app_settings_system_font_path(void){return system_path;}
static const char *app_settings_font_path(void){return reading_path;}
static void read_pico_sd_get_info(read_pico_sd_info_t*s){s->mounted=mounted;}
static void read_pico_transfer_get_status(read_pico_transfer_status_t*s){s->state=READ_PICO_TRANSFER_READY;s->mode=READ_PICO_TRANSFER_MODE_STA;s->network_ready=connected;strcpy(s->wifi_ssid,"Saved WiFi");}
static int ttf_font_scan(void){assert(mounted);++scans;return 0;}
static int ttf_font_count(void){return 2;}
static const ttf_font_item_t *ttf_font_item(int i){return &catalog[i];}
static void open_transfer(app_ctx_t*c,int mode){c->request_app=&app_transfer;if(mode==SETUP_USB)++usb;else if(mode==SETUP_UPLOAD)++uploads;else ++wifi;}

'''
for n in ['next_rect','skip_rect','language_rect','local_rect','source_rect','local_row','select_font','scan_fonts','setup_network_time','enter','finish','gesture','stop','media_lost','media_ready','key']:
 u+=function(n,source)+'\n'
u+=r'''
static app_redraw_t tap(app_ctx_t*c,EpdRect r){ui_gesture_event_t e={UI_GESTURE_TAP,r.x+3,r.y+3,r.x+3,r.y+3};return gesture(c,&e);}
static void reset(app_ctx_t*c){*c=(app_ctx_t){.now_ms=100};s_initialized=s_local_picker=false;s_step=0;s_notice[0]=0;scans=wifi=uploads=usb=time_requests=finished=saved=0;connected=false;save_fail=finish_fail=english=false;mounted=true;system_path[0]=reading_path[0]=0;enter(c);}
int main(void){app_ctx_t c;
 reset(&c);assert(s_step==0&&!scans);assert(tap(&c,language_rect(1))==APP_REDRAW_PAGE&&english);
 assert(tap(&c,next_rect())==APP_REDRAW_PAGE&&s_step==1&&!scans);
 assert(tap(&c,(EpdRect){52,700,580,76})==APP_REDRAW_NONE&&c.request_app==&app_transfer&&wifi==1);
 c.request_app=NULL;connected=true;enter(&c);assert(s_step==1&&s_network.network_ready&&time_requests==1);enter(&c);assert(time_requests==1);
 assert(tap(&c,next_rect())==APP_REDRAW_PAGE&&s_step==2&&scans==1&&s_font_count==2&&!saved);
 tap(&c,source_rect(0));assert(c.request_app==&app_transfer&&uploads==1);c.request_app=NULL;enter(&c);assert(s_step==2&&scans==2);
 assert(tap(&c,local_rect())==APP_REDRAW_PAGE&&s_local_picker);assert(tap(&c,local_row(1))==APP_REDRAW_PAGE&&!s_local_picker&&!strcmp(s_font_name,"Two"));
 assert(tap(&c,next_rect())==APP_REDRAW_NONE&&c.request_app==&home&&finished==1&&saved==2&&!strcmp(system_path,catalog[1].path)&&!strcmp(reading_path,system_path));
 reset(&c);s_step=2;scan_fonts();save_fail=true;assert(finish(&c,false)==APP_REDRAW_PAGE&&!c.request_app&&!finished&&s_notice[0]);
 save_fail=false;finish_fail=true;assert(finish(&c,false)==APP_REDRAW_PAGE&&!finished&&!c.request_app);
 finish_fail=false;assert(finish(&c,false)==APP_REDRAW_NONE&&finished==1);
 reset(&c);s_step=2;scan_fonts();mounted=false;assert(finish(&c,false)==APP_REDRAW_PAGE&&!finished&&!saved);
 assert(finish(&c,true)==APP_REDRAW_NONE&&finished==1&&c.request_app==&home&&!saved);
 reset(&c);s_step=2;scan_fonts();assert(tap(&c,source_rect(1))==APP_REDRAW_NONE&&c.request_app==&app_transfer&&usb==1&&!finished);c.request_app=NULL;enter(&c);assert(s_step==2&&scans==2);media_lost(&c);assert(!s_font_count&&!s_font_path[0]);mounted=true;media_ready(&c);assert(s_font_count==2&&scans==3);
 reset(&c);assert(key(&c,1)==APP_REDRAW_FULL&&!finished);assert(key(&c,2)==APP_REDRAW_NONE&&!finished&&!c.request_app);assert(key(&c,0)==APP_REDRAW_NONE&&!finished&&!c.request_app);
 puts("PASS: actual guide language/WiFi/font/transfer/USB/skip actions, return-step retention, SD loss and failed settings/completion preserve the wizard");
}
'''
with tempfile.TemporaryDirectory() as folder:
 p=Path(folder);(p/'test.c').write_text(u);exe=p/'test'
 subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-fsanitize=address,undefined',str(p/'test.c'),'-o',str(exe)],check=True);subprocess.run([str(exe)],check=True)
