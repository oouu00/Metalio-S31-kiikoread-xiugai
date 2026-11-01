#!/usr/bin/env python3
# SPDX-License-Identifier: LicenseRef-Kiikoread-Personal-Study-1.0
"""Run the production streamed license verifier with bounded HTTP/error fixtures."""
from pathlib import Path
import subprocess,tempfile
from home_cover_cache_host_test import function
root=Path(__file__).resolve().parents[1]
u=r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef int esp_err_t;typedef void *esp_http_client_handle_t;
typedef struct{const char *license_url,*license;}online_font_item_t;
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_INVALID_ARG -2
#define ESP_ERR_HTTP_EAGAIN 11
#define pdMS_TO_TICKS(x) (x)
static const char notice[]="Copyright test\nSIL OPEN FONT LICENSE Version 1.1\n";
static int mode,reads,cleaned;static size_t pos;static int64_t ticks;
static bool cancelled(void*p){(void)p;return mode==7;}
static void vTaskDelay(int n){(void)n;}
static int64_t esp_timer_get_time(void){ticks+=10000001;return ticks;}
static esp_http_client_handle_t open_source(const char*u){assert(!strcmp(u,"https://example.test/OFL.txt"));return (void*)1;}
static int64_t esp_http_client_get_content_length(void*c){assert(c==(void*)1);return mode==4?-1:(int64_t)strlen(notice)+(mode==3);}
static int esp_http_client_read(void*c,char*b,int capacity){assert(c==(void*)1&&capacity==8192);++reads;if(mode==5&&reads==1)return -ESP_ERR_HTTP_EAGAIN;if(mode==6)return -3;if(mode==8)return 0;size_t total=strlen(notice)-(mode==2);if(pos==total)return 0;size_t n=total-pos;if(n>7)n=7;memcpy(b,notice+pos,n);if(mode==1&&pos==7)b[0]^=1;pos+=n;return (int)n;}
static bool esp_http_client_is_complete_data_received(void*c){assert(c==(void*)1);return mode!=8;}
static void esp_http_client_cleanup(void*c){assert(c==(void*)1);++cleaned;}
'''
u+=function('verify_license',root/'main/font/online_font.c')+'\n'
u+=r'''
int main(void){uint8_t buffer[8192];online_font_item_t item={"https://example.test/OFL.txt",notice};
 for(mode=0;mode<=8;++mode){pos=reads=cleaned=ticks=0;int e=verify_license(&item,buffer);assert((e==ESP_OK)==(mode==0||mode==4||mode==5));assert(cleaned==1&&reads<20);}
 item.license_url=NULL;assert(verify_license(&item,buffer)==ESP_ERR_INVALID_ARG);
 puts("PASS: exact original notice, chunked transfer, changed/truncated bytes, incorrect length, EAGAIN/error/cancel/stalled EOF, bounded reads and cleanup");
}
'''
with tempfile.TemporaryDirectory() as d:
 p=Path(d);(p/'test.c').write_text(u);exe=p/'test'
 subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-fsanitize=address,undefined',str(p/'test.c'),'-o',str(exe)],check=True);subprocess.run([str(exe)],check=True)
