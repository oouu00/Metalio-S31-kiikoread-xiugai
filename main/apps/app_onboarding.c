/* SPDX-License-Identifier: LicenseRef-Kiikoread-Personal-Study-1.0
 * 中文：首次引导共用上层流程，复用本板配网、传书和字体下载；不触碰电源、波形或卡挂载参数。
 * English: Shared setup flow reusing each board's WiFi, transfer and font workers; never change power, waveforms or mount parameters.
 */
#ifdef ESP_PLATFORM
#include "ui_locale_format.h"
#endif
#include <stdio.h>
#include <string.h>
#include "app.h"
#include "app_registry.h"
#include "app_transfer_mode.h"
#include "setup_state.h"
#include "settings.h"
#include "ui_kit.h"
#include "ui_gesture.h"
#include "ui_font.h"
#include "read_pico_transfer.h"
#include "read_pico_sd.h"
#include "ttf_font.h"
#include "app_font_context.h"
#include "display.h"
#include "time_service.h"

extern const app_desc_t app_transfer,app_onboarding;
enum {SETUP_WIFI,SETUP_UPLOAD,SETUP_USB};
static unsigned s_step;
static bool s_initialized,s_local_picker,s_setup_time_requested;
static unsigned s_local_page;
static app_desc_t s_transfer_page;
static int s_font_index;
static char s_font_path[TTF_FONT_PATH_MAX],s_font_name[TTF_FONT_NAME_MAX],s_notice[96];
static unsigned s_font_count;
static int64_t s_poll_ms;
static read_pico_transfer_status_t s_network;
static const char *label(const char *zh,const char *en){return app_setup_english()||(!ui_font_has_text(zh)&&!ttf_font_has_text(zh))?en:zh;}
static EpdRect next_rect(void){return (EpdRect){52,976,580,76};}
static EpdRect skip_rect(void){return (EpdRect){52,1120,580,64};}
static EpdRect language_rect(int i){return (EpdRect){52,560+i*136,580,118};}
static EpdRect local_rect(void){return (EpdRect){52,358,580,174};}
static EpdRect source_rect(int i){return (EpdRect){52,564+i*116,580,94};}
static void button(uint8_t *fb,EpdRect r,const char *text,bool active){
    ui_fill_round_rect(fb,r,20,active?UI_GRAY_BLACK:UI_GRAY_WHITE);
    ui_draw_control_frame(fb,r,20,UI_GRAY_BLACK);
    ui_text_box(fb,ui_inset_rect(r,12),28,text,EPD_DRAW_ALIGN_CENTER,active);
}
static bool no(app_ctx_t *ctx);
static void transfer_draw(app_ctx_t *ctx,uint8_t *fb){
    app_transfer.render(ctx,fb);
    ui_fill_rect_fast((EpdRect){0,1088,684,128},UI_GRAY_WHITE,fb);
    ui_text_box(fb,skip_rect(),24,label("返回","Back"),EPD_DRAW_ALIGN_CENTER,false);
}
static void transfer_enter(app_ctx_t *ctx){app_transfer_set_setup_chrome(true);ctx->request_back_ripple=false;if(app_transfer.on_enter)app_transfer.on_enter(ctx);}
static void transfer_exit(app_ctx_t *ctx){if(app_transfer.on_exit)app_transfer.on_exit(ctx);app_transfer_set_setup_chrome(false);}
static bool transfer_present(app_ctx_t *ctx,app_redraw_t redraw){
    if(redraw==APP_REDRAW_AREA)return app_transfer.present&&app_transfer.present(ctx,redraw);
    if(redraw!=APP_REDRAW_PAGE&&redraw!=APP_REDRAW_FULL)return true;
    display_main_transition_cancel();transfer_draw(ctx,ctx->fb);
    guard_draw_result(ctx->hl,update_display_full(ctx->hl));return true;
}
static void setup_child_return(app_ctx_t *ctx){
    if(ctx->request_return){ctx->request_return=false;ctx->request_app=&app_onboarding;}
}
static void setup_network_time(void){
    if(s_setup_time_requested)return;
    read_pico_transfer_status_t status;read_pico_transfer_get_status(&status);
    if(status.state==READ_PICO_TRANSFER_READY&&status.mode==READ_PICO_TRANSFER_MODE_STA&&status.network_ready){
        app_time_service_request_online_sync();s_setup_time_requested=true;
    }
}
static app_redraw_t transfer_tick(app_ctx_t *ctx){
    app_redraw_t redraw=app_transfer.on_tick?app_transfer.on_tick(ctx):APP_REDRAW_NONE;
    setup_network_time();return redraw;
}
static app_redraw_t transfer_gesture(app_ctx_t *ctx,const ui_gesture_event_t *ev){
    app_redraw_t redraw=APP_REDRAW_NONE;
    if(ev->y0>=1088){
        if(ev->type==UI_GESTURE_TAP&&ui_rect_hit(skip_rect(),ev->x0,ev->y0))
            redraw=app_transfer.on_key?app_transfer.on_key(ctx,0):APP_REDRAW_NONE;
    }else redraw=app_transfer.on_gesture?app_transfer.on_gesture(ctx,ev):APP_REDRAW_NONE;
    setup_child_return(ctx);return redraw;
}
static app_redraw_t transfer_key(app_ctx_t *ctx,int key){
    app_redraw_t redraw=key==1?APP_REDRAW_FULL:app_transfer.on_key?app_transfer.on_key(ctx,0):APP_REDRAW_NONE;
    setup_child_return(ctx);return redraw;
}
static void open_transfer(app_ctx_t *ctx,int mode){
    s_transfer_page=app_transfer;s_transfer_page.binary_ui=false;s_transfer_page.persistent_navigation=false;s_transfer_page.setup_flow=true;
    s_transfer_page.navigation_visible=no;s_transfer_page.navigation_enabled=no;s_transfer_page.edge_back_enabled=no;
    s_transfer_page.enter_full=true;s_transfer_page.on_enter=transfer_enter;s_transfer_page.on_exit=transfer_exit;
    s_transfer_page.render=transfer_draw;s_transfer_page.present=transfer_present;
    s_transfer_page.on_gesture=transfer_gesture;s_transfer_page.on_key=transfer_key;s_transfer_page.on_tick=transfer_tick;
    if(mode==SETUP_USB)app_transfer_request_usb_start();
    else if(mode==SETUP_UPLOAD)app_transfer_request_wifi_upload();
    else app_transfer_request_wifi_setup();
    ctx->request_app=&s_transfer_page;
}
static EpdRect local_row(unsigned i){return (EpdRect){52,294+(int)i*100,580,94};}
static void local_draw(uint8_t *fb){
    ui_text_box(fb,(EpdRect){52,156,580,80},40,label("选择本地字体","Choose a local font"),EPD_DRAW_ALIGN_CENTER,false);
    for(unsigned row=0;row<6;++row){unsigned i=s_local_page*6+row;if(i>=s_font_count)break;
        const ttf_font_item_t *font=ttf_font_item((int)i);if(!font)continue;EpdRect r=local_row(row);
        char fallback[40];snprintf(fallback,sizeof(fallback),"Local font %u",i+1);
        const char *name=ui_font_has_text(font->name)||ttf_font_has_text(font->name)?font->name:fallback;
        ui_draw_control_frame(fb,r,16,0x60);ui_text_box(fb,(EpdRect){r.x+18,r.y+10,r.width-76,r.height-20},27,name,EPD_DRAW_ALIGN_LEFT,false);
        epd_draw_circle(594,r.y+47,14,0x60,fb);if((unsigned)s_font_index==i)epd_fill_circle(594,r.y+47,5,UI_GRAY_BLACK,fb);
    }
    char pages[40];snprintf(pages,sizeof(pages),"%u / %u",s_local_page+1,(s_font_count+5)/6);
    ui_text_box(fb,(EpdRect){240,926,204,56},23,pages,EPD_DRAW_ALIGN_CENTER,false);
    button(fb,(EpdRect){52,920,170,68},label("上一页","Previous"),false);
    button(fb,(EpdRect){462,920,170,68},label("下一页","Next"),false);
    button(fb,(EpdRect){52,1008,580,68},label("重新扫描","Scan again"),false);
    ui_text_box(fb,skip_rect(),24,label("返回引导","Back to setup"),EPD_DRAW_ALIGN_CENTER,false);
}
static void select_font(int direction){
    int count=ttf_font_count();s_font_count=count>0?(unsigned)count:0;
    s_font_path[0]=s_font_name[0]=0;
    if(!count){s_font_index=0;return;}
    s_font_index=(s_font_index+direction+count)%count;
    const ttf_font_item_t *item=ttf_font_item(s_font_index);
    if(item){snprintf(s_font_path,sizeof(s_font_path),"%s",item->path);snprintf(s_font_name,sizeof(s_font_name),"%s",item->name);}
}
static void scan_fonts(void){
    read_pico_sd_info_t sd;read_pico_sd_get_info(&sd);
    if(!sd.mounted){s_font_count=0;s_font_path[0]=s_font_name[0]=0;return;}
    (void)ttf_font_scan();select_font(0);
}
static void enter(app_ctx_t *ctx){
    if(!s_initialized){s_step=0;s_initialized=true;s_font_index=0;s_notice[0]=0;s_setup_time_requested=false;}
    read_pico_transfer_get_status(&s_network);s_poll_ms=ctx->now_ms;setup_network_time();
    if(s_step==2)scan_fonts();
}
static app_redraw_t finish(app_ctx_t *ctx,bool skip){
    if(!skip&&s_font_path[0]){
        read_pico_sd_info_t sd;read_pico_sd_get_info(&sd);
        if(!sd.mounted){snprintf(s_notice,sizeof(s_notice),"%s",label("请先恢复存储卡，或跳过引导","Restore the card, or skip setup"));return APP_REDRAW_PAGE;}
        app_settings_set_system_font_path(s_font_path);
        if(strcmp(app_settings_system_font_path(),s_font_path)){
            snprintf(s_notice,sizeof(s_notice),"%s",label("字体设置保存失败，请重试","Could not save the font. Try again"));return APP_REDRAW_PAGE;
        }
        app_settings_set_font_path(s_font_path);
        if(strcmp(app_settings_font_path(),s_font_path)){
            snprintf(s_notice,sizeof(s_notice),"%s",label("阅读字体保存失败，请重试","Could not save the reading font"));return APP_REDRAW_PAGE;
        }
    }
    if(app_setup_finish()!=ESP_OK){snprintf(s_notice,sizeof(s_notice),"%s",label("配置保存失败，请重试","Could not save setup. Try again"));return APP_REDRAW_PAGE;}
    ctx->request_app=app_home_page();s_initialized=false;
    return APP_REDRAW_NONE;
}
static void draw(app_ctx_t *ctx,uint8_t *fb){
    (void)ctx;
    ui_text_set_system_scale(false);ui_clear_page(fb);
    ui_text(fb,36,28,28,"kiikoread",EPD_DRAW_ALIGN_LEFT,false);
    char text[96];snprintf(text,sizeof(text),"%02u / 03",s_step+1);
    ui_text(fb,648,36,18,text,EPD_DRAW_ALIGN_RIGHT,false);
    for(unsigned i=0;i<3;++i)ui_fill_round_rect(fb,(EpdRect){36+(int)i*210,102,192,4},2,i<=s_step?0x30:0xd8);
    if(s_local_picker){local_draw(fb);return;}
    if(s_step<2){
        ui_draw_icon(fb,342,228,44,s_step?UI_ICON_WIFI:UI_ICON_BOOK_OPEN,0x30);
        ui_text_box(fb,(EpdRect){52,306,580,88},52,s_step?label("连接 Wi-Fi","Connect to Wi-Fi"):label("你好。","Hello."),EPD_DRAW_ALIGN_CENTER,false);
        ui_text_box(fb,(EpdRect){52,408,580,70},27,s_step?label("连接网络，传入书籍和完整字体","Connect for books and complete fonts"):label("欢迎使用 kiikoread","Welcome to kiikoread"),EPD_DRAW_ALIGN_CENTER,false);
    }
    if(s_step==0){
        for(int i=0;i<2;++i){EpdRect r=language_rect(i);bool selected=app_setup_english()==(i==1);
            ui_draw_choice_round_rect(fb,r,22,false);
            ui_text_vc(fb,r.x+26,r.y+43,33,i?"English":label("简体中文","Chinese"),EPD_DRAW_ALIGN_LEFT,false);
            ui_text_vc(fb,r.x+26,r.y+86,20,i?label("英语","English"):"Chinese",EPD_DRAW_ALIGN_LEFT,false);
            epd_draw_circle(r.x+r.width-38,r.y+59,15,0x60,fb);if(selected)epd_fill_circle(r.x+r.width-38,r.y+59,5,0x30,fb);
        }
    }else if(s_step==1){
        EpdRect r={52,506,580,146};ui_draw_choice_round_rect(fb,r,22,false);
        ui_text_box(fb,(EpdRect){78,526,528,48},29,s_network.network_ready?s_network.wifi_ssid:label("尚未连接网络","Not connected"),EPD_DRAW_ALIGN_LEFT,false);
        ui_text_box(fb,(EpdRect){78,588,528,38},21,s_network.network_ready?label("已连接，可以继续","Connected. You can continue"):label("也可以跳过，稍后在设置中连接","You can connect later in Settings"),EPD_DRAW_ALIGN_LEFT,false);
        button(fb,(EpdRect){52,700,580,76},label("选择 Wi-Fi 网络","Choose a Wi-Fi network"),false);
    }else{
        ui_draw_icon(fb,342,196,42,UI_ICON_TYPE,0x30);
        ui_text_box(fb,(EpdRect){52,246,580,76},44,label("准备好字体","Make it yours"),EPD_DRAW_ALIGN_CENTER,false);
        ui_text_box(fb,(EpdRect){52,315,580,35},22,label("通过 U 盘或 Wi-Fi 导入 TF 卡","Import fonts to the TF card via USB or Wi-Fi"),EPD_DRAW_ALIGN_CENTER,false);
        EpdRect r=local_rect();ui_draw_choice_round_rect(fb,r,22,false);
        snprintf(text,sizeof(text),app_setup_english()?"Local fonts: %u":"本地字体 · 已发现 %u 款",s_font_count);
        ui_text_box(fb,(EpdRect){76,375,530,44},25,text,EPD_DRAW_ALIGN_LEFT,false);
        char local_name[40];snprintf(local_name,sizeof(local_name),"Local font %d",s_font_index+1);
        const char *caption=s_font_path[0]?(ui_font_has_text(s_font_name)||ttf_font_has_text(s_font_name)?s_font_name:local_name):label("未发现字体","No local fonts found");
        ui_text_box(fb,(EpdRect){76,430,400,44},25,caption,EPD_DRAW_ALIGN_LEFT,false);
        ui_text_box(fb,(EpdRect){76,486,382,30},19,label("点此选择本地字体","Tap to choose a local font"),EPD_DRAW_ALIGN_LEFT,false);
        if(s_font_count)ui_draw_icon(fb,594,459,25,UI_ICON_CHEVRON_RIGHT,0x30);
        for(int i=0;i<2;++i){r=source_rect(i);ui_draw_choice_round_rect(fb,r,22,false);ui_draw_icon(fb,91,r.y+47,32,i?UI_ICON_USB:UI_ICON_WIFI,0x30);
            ui_text_box(fb,(EpdRect){130,r.y+14,450,64},27,i?label("U 盘模式导入字体","Import fonts via USB"):label("扫码 Wi-Fi 传入字体","Transfer fonts over Wi-Fi"),EPD_DRAW_ALIGN_LEFT,false);}
        ui_text_box(fb,(EpdRect){52,796,580,76},22,label("未导入字体，阅读界面可能出现方格字","Without imported fonts, reading may show square boxes"),EPD_DRAW_ALIGN_CENTER,false);
        ui_text_box(fb,(EpdRect){52,874,580,40},19,label("TTF / OTF 放入 TF 卡 fonts 文件夹后选择","Copy TTF / OTF to fonts/, then select above"),EPD_DRAW_ALIGN_CENTER,false);
    }
    button(fb,next_rect(),s_step==2?label("完成引导","Finish setup"):label("继续","Continue"),true);
    ui_text_box(fb,skip_rect(),24,label("跳过引导","Skip setup"),EPD_DRAW_ALIGN_CENTER,false);
    if(s_step)ui_text_box(fb,(EpdRect){52,1060,580,48},21,label("上一步","Back"),EPD_DRAW_ALIGN_CENTER,false);
    if(s_notice[0])ui_text_box(fb,(EpdRect){52,898,580,64},21,s_notice,EPD_DRAW_ALIGN_CENTER,false);
}
static app_redraw_t gesture(app_ctx_t *ctx,const ui_gesture_event_t *ev){
    if(ev->type!=UI_GESTURE_TAP)return APP_REDRAW_NONE;
    int x=ev->x0,y=ev->y0;
    if(s_local_picker){
        if(ui_rect_hit(skip_rect(),x,y)){s_local_picker=false;return APP_REDRAW_PAGE;}
        if(ui_rect_hit((EpdRect){52,1008,580,68},x,y)){scan_fonts();s_local_page=0;if(!s_font_count)s_local_picker=false;return APP_REDRAW_PAGE;}
        if(y>=920&&y<988){if(x<222&&s_local_page)--s_local_page;else if(x>=462&&(s_local_page+1)*6<s_font_count)++s_local_page;return APP_REDRAW_PAGE;}
        for(unsigned row=0;row<6;++row){unsigned i=s_local_page*6+row;if(i<s_font_count&&ui_rect_hit(local_row(row),x,y)){
            s_font_index=(int)i;select_font(0);s_local_picker=false;return APP_REDRAW_PAGE;
        }}return APP_REDRAW_NONE;
    }
    if(ui_rect_hit(skip_rect(),x,y))return finish(ctx,true);
    if(s_step&&ui_rect_hit((EpdRect){52,1060,580,48},x,y)){--s_step;s_notice[0]=0;return APP_REDRAW_PAGE;}
    if(ui_rect_hit(next_rect(),x,y)){
        s_notice[0]=0;if(s_step==2)return finish(ctx,false);
        if(++s_step==2)scan_fonts();
        return APP_REDRAW_PAGE;
    }
    if(s_step==0){for(int i=0;i<2;++i)if(ui_rect_hit(language_rect(i),x,y)){
        if(app_setup_set_english(i==1)!=ESP_OK)snprintf(s_notice,sizeof(s_notice),"%s",label("配置保存失败，请重试","Could not save setup. Try again"));
        return APP_REDRAW_PAGE;}}
    if(s_step==1&&ui_rect_hit((EpdRect){52,700,580,76},x,y)){open_transfer(ctx,SETUP_WIFI);return APP_REDRAW_NONE;}
    if(s_step==2){
        if(ui_rect_hit(local_rect(),x,y)){scan_fonts();if(s_font_count){s_local_picker=true;s_local_page=(unsigned)s_font_index/6;}return APP_REDRAW_PAGE;}
        if(ui_rect_hit(source_rect(0),x,y)){open_transfer(ctx,SETUP_UPLOAD);return APP_REDRAW_NONE;}
        if(ui_rect_hit(source_rect(1),x,y)){open_transfer(ctx,SETUP_USB);return APP_REDRAW_NONE;}
    }
    return APP_REDRAW_NONE;
}
static app_redraw_t tick(app_ctx_t *ctx){
    setup_network_time();
    if(ctx->now_ms-s_poll_ms<500)return APP_REDRAW_NONE;
    s_poll_ms=ctx->now_ms;
    read_pico_transfer_status_t next;read_pico_transfer_get_status(&next);
    bool changed=next.network_ready!=s_network.network_ready||strcmp(next.wifi_ssid,s_network.wifi_ssid);
    s_network=next;return changed&&s_step==1?APP_REDRAW_PAGE:APP_REDRAW_NONE;
}
static bool present(app_ctx_t *ctx,app_redraw_t redraw){
    if(redraw==APP_REDRAW_NONE||redraw==APP_REDRAW_DONE)return true;
    display_main_transition_cancel();draw(ctx,ctx->fb);
    guard_draw_result(ctx->hl,update_display_full(ctx->hl));
    return true;
}
static void stop(app_ctx_t *ctx){(void)ctx;}
static void media_lost(app_ctx_t *ctx){stop(ctx);s_local_picker=false;s_font_count=0;s_font_path[0]=s_font_name[0]=0;}
static void media_ready(app_ctx_t *ctx){(void)ctx;if(s_step==2)scan_fonts();}
static bool no(app_ctx_t *ctx){(void)ctx;return false;}
static app_redraw_t key(app_ctx_t *ctx,int key){
    (void)ctx;
    if(s_local_picker){s_local_picker=false;return APP_REDRAW_PAGE;}
    if(key==0&&s_step){--s_step;return APP_REDRAW_PAGE;}
    if(key==1)return APP_REDRAW_FULL;
    return APP_REDRAW_NONE;
}
const app_desc_t app_onboarding={
    .title="首次引导 Setup",.detail="语言、网络与字体",.enter_full=true,.owns_keys=true,.setup_flow=true,
    .menu_handle_enabled=no,.navigation_enabled=no,.on_enter=enter,.on_exit=stop,
    .on_before_lock=stop,.on_media_lost=media_lost,.on_media_ready=media_ready,
    .render=draw,.present=present,.on_gesture=gesture,.on_tick=tick,.on_key=key,
};
