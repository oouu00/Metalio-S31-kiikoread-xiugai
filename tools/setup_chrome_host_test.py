#!/usr/bin/env python3
# SPDX-License-Identifier: LicenseRef-Kiikoread-Personal-Study-1.0
"""Exercise real setup buttons/child ownership and navigation scan admission."""
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
typedef struct{int x,y,width,height;}EpdRect;
typedef enum{APP_REDRAW_NONE,APP_REDRAW_AREA,APP_REDRAW_PAGE,APP_REDRAW_FULL,APP_REDRAW_DONE}app_redraw_t;
typedef struct app_desc app_desc_t;
typedef struct{uint8_t *fb;void *hl;bool request_return,request_back_ripple;const app_desc_t *request_app;}app_ctx_t;
typedef struct{int type,x0,y0;}ui_gesture_event_t;
#define UI_GESTURE_TAP 1
#define UI_GRAY_BLACK 0
#define UI_GRAY_WHITE 255
#define EPD_DRAW_ALIGN_CENTER 0
struct app_desc{bool binary_ui,persistent_navigation,enter_full,setup_flow;bool(*navigation_visible)(app_ctx_t*),(*navigation_enabled)(app_ctx_t*),(*edge_back_enabled)(app_ctx_t*);void(*render)(app_ctx_t*,uint8_t*),(*on_enter)(app_ctx_t*),(*on_exit)(app_ctx_t*);bool(*present)(app_ctx_t*,app_redraw_t);app_redraw_t(*on_gesture)(app_ctx_t*,const ui_gesture_event_t*),(*on_key)(app_ctx_t*,int),(*on_tick)(app_ctx_t*);};
static bool setup_chrome,can_leave=true,native_busy,inverted;static unsigned full,area,paints,cancels,enters,exits,started_wifi,started_upload,started_usb,time_requests;
enum{SETUP_WIFI,SETUP_UPLOAD,SETUP_USB,READ_PICO_TRANSFER_READY,READ_PICO_TRANSFER_MODE_STA};
static bool s_setup_time_requested;
static const app_desc_t app_onboarding={.setup_flow=true};
static void app_time_service_request_online_sync(void){++time_requests;}
typedef struct{int state,mode;bool network_ready;}read_pico_transfer_status_t;
static bool connected;
static void read_pico_transfer_get_status(read_pico_transfer_status_t*s){*s=(read_pico_transfer_status_t){READ_PICO_TRANSFER_READY,READ_PICO_TRANSFER_MODE_STA,connected};}
static uint8_t canvas[684*1216];static EpdRect ink_box;static app_desc_t s_transfer_page;
static const char *label(const char*z,const char*e){(void)e;return z;}
static bool ui_rect_hit(EpdRect r,int x,int y){return x>=r.x&&y>=r.y&&x<r.x+r.width&&y<r.y+r.height;}
static EpdRect ui_inset_rect(EpdRect r,int n){return(EpdRect){r.x+n,r.y+n,r.width-2*n,r.height-2*n};}
static void ui_fill_rect_fast(EpdRect r,int color,uint8_t *fb){assert(r.x>=0&&r.y>=0&&r.x+r.width<=684&&r.y+r.height<=1216);for(int y=r.y;y<r.y+r.height;++y)memset(fb+y*684+r.x,color,r.width);}
static void ui_fill_round_rect(uint8_t*f,EpdRect r,int radius,int c){(void)radius;ui_fill_rect_fast(r,c,f);}
static void ui_draw_control_frame(uint8_t*f,EpdRect r,int radius,int c){(void)f;(void)r;(void)radius;(void)c;}
static void ui_text_box(uint8_t*f,EpdRect r,int px,const char*t,int a,bool inv){(void)px;(void)t;(void)a;ink_box=r;inverted=inv;f[(r.y+r.height/2)*684+r.x+r.width/2]=inv?255:0;}
static void app_transfer_set_setup_chrome(bool on){setup_chrome=on;}
static void app_transfer_request_wifi_upload(void){++started_upload;}
static void app_transfer_request_usb_start(void){++started_usb;}
static void app_transfer_request_wifi_setup(void){++started_wifi;}
static void native_draw(app_ctx_t*c,uint8_t*f){(void)c;++paints;if(!setup_chrome)memset(f+1096*684,0,120*684);}
static void native_enter(app_ctx_t*c){(void)c;++enters;}
static void native_exit(app_ctx_t*c){(void)c;++exits;}
static bool native_present(app_ctx_t*c,app_redraw_t r){(void)c;assert(r==APP_REDRAW_AREA);++area;return true;}
static bool native_back(app_ctx_t*c){(void)c;return can_leave;}
static app_redraw_t native_gesture(app_ctx_t*c,const ui_gesture_event_t*e){if(e->y0<100&&!native_busy)c->request_return=true;return APP_REDRAW_AREA;}
static app_redraw_t native_key(app_ctx_t*c,int k){(void)c;assert(k==0);if(!native_busy)c->request_return=true;return APP_REDRAW_NONE;}
static const app_desc_t app_transfer={.binary_ui=true,.persistent_navigation=true,.render=native_draw,.on_enter=native_enter,.on_exit=native_exit,.present=native_present,.edge_back_enabled=native_back,.on_gesture=native_gesture,.on_key=native_key};
static void display_main_transition_cancel(void){++cancels;}
static int update_display_full(void*h){(void)h;++full;return 0;}
static void guard_draw_result(void*h,int e){(void)h;assert(!e);}
'''
for n in ['skip_rect','button','no','transfer_draw','transfer_enter','transfer_exit','transfer_present','setup_child_return','setup_network_time','transfer_tick','transfer_gesture','transfer_key','open_transfer']:u+=function(n,source)+'\n'
u+=r'''
static bool main_ready;static bool main_page_ready(app_ctx_t*c,const app_desc_t*a){(void)c;(void)a;return main_ready;}
'''
u+=function('navigation_page_ready',root/'main/app/app_loop.c')+'\n'
u+='static bool s_file_grid;static bool main_page_visible(app_ctx_t *ctx){(void)ctx;return false;}static bool s_file_selecting;static int s_view;enum{FILE_VIEW_LIST,FILE_VIEW_MOVE};\n'+function('files_navigation_visible',root/'main/apps/app_files.c')+'\n'+function('files_custom_footer_gray',root/'main/apps/app_files.c')+'\n'
u+=r'''
int main(void){app_ctx_t c={.fb=canvas};memset(canvas,255,sizeof(canvas));
 EpdRect r={52,976,580,76};button(canvas,r,"Continue",true);assert(inverted&&canvas[1000*684+100]==0&&canvas[1014*684+342]==255&&ink_box.x>=r.x&&ink_box.y+ink_box.height<=r.y+r.height);
 button(canvas,r,"Back",false);assert(!inverted&&canvas[1000*684+100]==255);
 for(int mode=0;mode<3;++mode){open_transfer(&c,mode);const app_desc_t*p=c.request_app;assert(p!=&app_transfer&&!p->binary_ui&&!p->persistent_navigation&&p->enter_full&&!p->navigation_enabled(&c)&&!p->navigation_visible(&c));p->on_enter(&c);assert(setup_chrome);memset(canvas+1088*684,0,128*684);assert(p->present(&c,APP_REDRAW_PAGE));assert(canvas[1100*684+342]==255);unsigned before=full;assert(p->present(&c,APP_REDRAW_AREA)&&full==before);ui_gesture_event_t e={UI_GESTURE_TAP,100,1140};c.request_return=false;can_leave=false;native_busy=true;p->on_gesture(&c,&e);assert(!c.request_return);native_busy=false;p->on_gesture(&c,&e);assert(!c.request_return&&c.request_app==&app_onboarding);c.request_app=NULL;e.y0=40;p->on_gesture(&c,&e);assert(!c.request_return&&c.request_app==&app_onboarding);c.request_app=NULL;p->on_key(&c,0);assert(!c.request_return&&c.request_app==&app_onboarding);assert(p->on_key(&c,1)==APP_REDRAW_FULL);p->on_exit(&c);assert(!setup_chrome);}
 assert(started_usb==1&&started_upload==1&&started_wifi==1&&enters==3&&exits==3&&full==3&&area==3&&paints==3&&cancels==3);
 connected=true;s_transfer_page.on_tick(&c);assert(time_requests==1);s_transfer_page.on_tick(&c);assert(time_requests==1);
 s_view=FILE_VIEW_LIST;s_file_selecting=false;assert(files_navigation_visible(&c));s_file_selecting=true;assert(!files_navigation_visible(&c)&&files_custom_footer_gray(&c));s_view=FILE_VIEW_MOVE;assert(files_navigation_visible(&c)&&!files_custom_footer_gray(&c));
 app_desc_t normal={.persistent_navigation=true};main_ready=true;assert(navigation_page_ready(&c,&normal));normal.navigation_visible=no;assert(!navigation_page_ready(&c,&normal));normal.navigation_visible=NULL;assert(navigation_page_ready(&c,&normal));
 puts("PASS: opaque contrast, setup child owns/cleans full bottom without tabs; input/progress remain partial; upload exit guard and custom-bar scan admission");
}
'''
with tempfile.TemporaryDirectory() as d:
 p=Path(d);(p/'test.c').write_text(u);exe=p/'test'
 subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-fsanitize=address,undefined',str(p/'test.c'),'-o',str(exe)],check=True);subprocess.run([str(exe)],check=True)
