/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 * 中文：同一私有NVS内记忆最多八个网络；读迁移旧单网络，失败不更新公开快照。
 * English: Remember eight networks in the private NVS, migrate the legacy single record, and never publish on failure.
 */
#pragma once
#ifndef READ_PICO_TRANSFER_NVS_TEST
#include "nvs.h"
#endif
#include <stdlib.h>
#include "transfer_policy.h"
#define TRANSFER_SAVED_WIFI_MAX 8
#define TRANSFER_WIFI_BANK_MAGIC UINT32_C(0x57494632)
typedef struct {
    uint32_t magic;
    uint8_t count, confirmed, reserved[2];
    transfer_credentials_t items[TRANSFER_SAVED_WIFI_MAX];
} transfer_wifi_bank_t;
static esp_err_t credentials_open(int mode, nvs_handle_t *handle) {
#if KIIKO_BOARD_METALIO47
    return nvs_open("metalio_wifi",mode,handle);
#else
    return nvs_open("rp_wifi",mode,handle);
#endif
}
static bool credentials_bank_valid(const transfer_wifi_bank_t *bank) {
    if(bank->magic!=TRANSFER_WIFI_BANK_MAGIC||bank->count>TRANSFER_SAVED_WIFI_MAX||bank->reserved[0]||bank->reserved[1])return false;
    if(bank->confirmed & (uint8_t)~((1u<<bank->count)-1u))return false;
    for(unsigned i=0;i<bank->count;++i){
        if(!credentials_valid(&bank->items[i]))return false;
        for(unsigned j=0;j<i;++j)if(!strcmp(bank->items[i].ssid,bank->items[j].ssid))return false;
    }
    return true;
}
static esp_err_t load_credentials_bank(transfer_wifi_bank_t *out) {
    memset(out,0,sizeof(*out));out->magic=TRANSFER_WIFI_BANK_MAGIC;
    nvs_handle_t handle;esp_err_t err=credentials_open(NVS_READONLY,&handle);
    if(err==ESP_ERR_NVS_NOT_FOUND)return ESP_OK;
    if(err!=ESP_OK)return err;
    size_t size=sizeof(*out);err=nvs_get_blob(handle,"networks_v1",out,&size);
    if(err==ESP_ERR_NVS_NOT_FOUND){
        memset(out,0,sizeof(*out));out->magic=TRANSFER_WIFI_BANK_MAGIC;
        size=sizeof(out->items[0]);err=nvs_get_blob(handle,"credentials",&out->items[0],&size);
        if(err==ESP_ERR_NVS_NOT_FOUND)err=ESP_OK;
        else if(err==ESP_OK&&size==sizeof(out->items[0])&&credentials_valid(&out->items[0])){out->count=1;out->confirmed=1;}
        else if(err==ESP_OK)err=ESP_ERR_INVALID_STATE;
    }else if(err==ESP_OK&&(size!=sizeof(*out)||!credentials_bank_valid(out)))err=ESP_ERR_INVALID_STATE;
    nvs_close(handle);
    if(err!=ESP_OK)clear_secret(out,sizeof(*out));
    return err;
}
static esp_err_t write_credentials_bank(const transfer_wifi_bank_t *bank, bool clear_legacy) {
    if(!credentials_bank_valid(bank))return ESP_ERR_INVALID_ARG;
    nvs_handle_t handle;esp_err_t err=credentials_open(NVS_READWRITE,&handle);
    if(err!=ESP_OK)return err;
    if(clear_legacy){err=nvs_erase_key(handle,"credentials");if(err==ESP_ERR_NVS_NOT_FOUND)err=ESP_OK;}
    if(err==ESP_OK)err=nvs_set_blob(handle,"networks_v1",bank,sizeof(*bank));
    if(err==ESP_OK)err=nvs_commit(handle);
    nvs_close(handle);return err;
}
static esp_err_t load_credentials(transfer_credentials_t *out) {
    memset(out,0,sizeof(*out));
    transfer_wifi_bank_t *bank=calloc(1,sizeof(*bank));if(!bank)return ESP_ERR_NO_MEM;
    esp_err_t err=load_credentials_bank(bank);
    if(err==ESP_OK&&bank->count)*out=bank->items[bank->count-1];
    clear_secret(bank,sizeof(*bank));free(bank);return err;
}
static esp_err_t store_credentials(const transfer_credentials_t *next) {
    if(next&&!credentials_valid(next))return ESP_ERR_INVALID_ARG;
    transfer_wifi_bank_t *bank=calloc(1,sizeof(*bank));if(!bank)return ESP_ERR_NO_MEM;
    esp_err_t err=ESP_OK;
    if(next)err=load_credentials_bank(bank);else bank->magic=TRANSFER_WIFI_BANK_MAGIC;
    if(err==ESP_OK&&next){
        unsigned found=bank->count;bool confirmed=false;
        for(unsigned i=0;i<bank->count;++i)if(!strcmp(bank->items[i].ssid,next->ssid)){found=i;break;}
        if(found<bank->count){
            confirmed=(bank->confirmed&(1u<<found))&&!strcmp(bank->items[found].password,next->password);
            memmove(bank->items+found,bank->items+found+1,(bank->count-found-1)*sizeof(*next));
            unsigned low=bank->confirmed&((1u<<found)-1u),high=(bank->confirmed>>(found+1))<<found;
            bank->confirmed=(uint8_t)(low|high);--bank->count;
        }
        if(bank->count==TRANSFER_SAVED_WIFI_MAX){
            memmove(bank->items,bank->items+1,(TRANSFER_SAVED_WIFI_MAX-1)*sizeof(*next));
            bank->confirmed>>=1;--bank->count;
        }
        bank->items[bank->count]=*next;
        if(confirmed)bank->confirmed|=(uint8_t)(1u<<bank->count);
        ++bank->count;
    }
    if(err==ESP_OK)err=write_credentials_bank(bank,!next);
    clear_secret(bank,sizeof(*bank));free(bank);return err;
}
static esp_err_t confirm_current_credentials(void) {
    transfer_wifi_bank_t *bank=calloc(1,sizeof(*bank));if(!bank)return ESP_ERR_NO_MEM;
    esp_err_t err=load_credentials_bank(bank);
    if(err==ESP_OK&&bank->count){
        uint8_t bit=(uint8_t)(1u<<(bank->count-1));
        if(!(bank->confirmed&bit)){bank->confirmed|=bit;err=write_credentials_bank(bank,false);}
    }
    clear_secret(bank,sizeof(*bank));free(bank);return err;
}
static esp_err_t find_saved_credentials(const char *ssid, transfer_credentials_t *out, bool *confirmed) {
    memset(out,0,sizeof(*out));if(confirmed)*confirmed=false;
    if(!ssid||!ssid[0]||strnlen(ssid,33)>32)return ESP_ERR_INVALID_ARG;
    transfer_wifi_bank_t *bank=calloc(1,sizeof(*bank));if(!bank)return ESP_ERR_NO_MEM;
    esp_err_t err=load_credentials_bank(bank);
    if(err==ESP_OK){
        err=ESP_ERR_NVS_NOT_FOUND;
        for(unsigned i=0;i<bank->count;++i)if(!strcmp(bank->items[i].ssid,ssid)){
            *out=bank->items[i];if(confirmed)*confirmed=(bank->confirmed&(1u<<i))!=0;err=ESP_OK;break;
        }
    }
    clear_secret(bank,sizeof(*bank));free(bank);return err;
}
static void mark_saved_networks(read_pico_transfer_network_t *networks, size_t count) {
    for(size_t i=0;i<count;++i)networks[i].saved=false;
    transfer_wifi_bank_t *bank=calloc(1,sizeof(*bank));if(!bank)return;
    if(load_credentials_bank(bank)==ESP_OK)for(size_t i=0;i<count;++i){
        for(unsigned j=0;j<bank->count;++j)if((bank->confirmed&(1u<<j))&&
            !strcmp(networks[i].ssid,bank->items[j].ssid)&&
            networks[i].requires_password==(bank->items[j].password[0]!=0)){
            networks[i].saved=true;break;
        }
    }
    clear_secret(bank,sizeof(*bank));free(bank);
}
static esp_err_t forget_current_credentials(void) {
    transfer_wifi_bank_t *bank=calloc(1,sizeof(*bank));if(!bank)return ESP_ERR_NO_MEM;
    esp_err_t err=load_credentials_bank(bank);
    if(err==ESP_OK){
        if(bank->count){--bank->count;clear_secret(&bank->items[bank->count],sizeof(bank->items[0]));bank->confirmed&=(uint8_t)((1u<<bank->count)-1u);}
        err=write_credentials_bank(bank,true);
    }
    clear_secret(bank,sizeof(*bank));free(bank);return err;
}
