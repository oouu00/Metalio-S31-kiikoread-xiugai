/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 * 配网解析与连接时限的真实策略回归。/ Regression of actual provisioning parsing and connection deadlines.
 */
#include <assert.h>
#include <stdio.h>
#include "../components/read_pico_transfer/transfer_policy.h"

static int64_t body_time;
static int body_reads, body_cancel_at, body_step = 4000, body_failure;
static bool body_stopped(void *ctx) { (void)ctx; return body_cancel_at && body_reads >= body_cancel_at; }
static int64_t body_now(void *ctx) { (void)ctx; return body_time; }
static int body_receive(void *ctx, char *out, size_t cap) {
    (void)ctx; assert(cap); *out = 'a'; ++body_reads; body_time += body_step;
    return body_failure ? body_failure : 1;
}
static void test_body_deadline(void) {
    char body[257] = {0};
    body_time = body_reads = 0; body_cancel_at = 1;
    assert(!receive_credentials_body(body, 1, body_receive, body_stopped, body_now, NULL));
    assert(body_reads == 1);
    body_time = body_reads = body_cancel_at = 0;
    assert(!receive_credentials_body(body, 256, body_receive, body_stopped, body_now, NULL));
    assert(body_reads == 4);
    body_time = body_reads = 0;
    assert(receive_credentials_body(body, 2, body_receive, body_stopped, body_now, NULL));
    assert(body_reads == 2);
    body_time = body_reads = 0; body_cancel_at = -1;
    assert(!receive_credentials_body(body, 2, body_receive, body_stopped, body_now, NULL));
    assert(body_reads == 0);
    body_cancel_at = 0; body_step = 15000;
    assert(!receive_credentials_body(body, 1, body_receive, body_stopped, body_now, NULL));
    body_time = body_reads = 0; body_step = 1; body_failure = -1;
    assert(!receive_credentials_body(body, 2, body_receive, body_stopped, body_now, NULL));
    body_time = body_reads = 0; body_failure = 3;
    assert(!receive_credentials_body(body, 2, body_receive, body_stopped, body_now, NULL));
    body_failure = 0;
}

// 故障注入只替换NVS边界；生产读写函数原样执行。/ Inject faults only at NVS boundaries; run production storage helpers unchanged.
#define READ_PICO_TRANSFER_NVS_TEST
typedef int esp_err_t;
typedef unsigned nvs_handle_t;
enum { ESP_OK, ESP_ERR_NVS_NOT_FOUND, ESP_ERR_INVALID_STATE, ESP_ERR_INVALID_ARG, ESP_FAIL, ESP_ERR_NO_MEM };
enum { NVS_READONLY, NVS_READWRITE };
typedef struct { unsigned char bytes[1024];size_t size;bool present; } mock_blob_t;
static mock_blob_t committed[2], staged[2];
static int fail_open,fail_write,fail_commit,fail_read,handles,writes;
static unsigned key_index(const char *key){
    if(!strcmp(key,"credentials"))return 0;
    assert(!strcmp(key,"networks_v1"));return 1;
}
static esp_err_t nvs_open(const char *name,int mode,nvs_handle_t *handle){
#if KIIKO_BOARD_METALIO47
    assert(!strcmp(name,"metalio_wifi"));
#else
    assert(!strcmp(name,"rp_wifi"));
#endif
    (void)mode;if(fail_open)return ESP_FAIL;
    assert(!handles);++handles;*handle=1;memcpy(staged,committed,sizeof(staged));return ESP_OK;
}
static esp_err_t nvs_get_blob(nvs_handle_t handle,const char *key,void *out,size_t *size){
    assert(handle==1);if(fail_read)return ESP_FAIL;
    mock_blob_t *blob=&committed[key_index(key)];
    if(!blob->present)return ESP_ERR_NVS_NOT_FOUND;
    if(*size<blob->size)return ESP_ERR_INVALID_STATE;
    *size=blob->size;memcpy(out,blob->bytes,*size);return ESP_OK;
}
static esp_err_t nvs_set_blob(nvs_handle_t handle,const char *key,const void *in,size_t size){
    assert(handle==1&&size<=sizeof(staged[0].bytes));if(fail_write)return ESP_FAIL;
    mock_blob_t *blob=&staged[key_index(key)];memcpy(blob->bytes,in,size);blob->size=size;blob->present=true;++writes;return ESP_OK;
}
static esp_err_t nvs_erase_key(nvs_handle_t handle,const char *key){
    assert(handle==1);if(fail_write)return ESP_FAIL;
    mock_blob_t *blob=&staged[key_index(key)];bool was=blob->present;memset(blob,0,sizeof(*blob));return was?ESP_OK:ESP_ERR_NVS_NOT_FOUND;
}
static esp_err_t nvs_commit(nvs_handle_t handle){
    assert(handle==1);if(fail_commit)return ESP_FAIL;
    memcpy(committed,staged,sizeof(committed));return ESP_OK;
}
static void nvs_close(nvs_handle_t handle){assert(handle==1&&handles==1);--handles;clear_secret(staged,sizeof(staged));}
#include "../components/read_pico_transfer/transfer_credentials_store.h"

static bool parse(const char *json, transfer_credentials_t *c) { return parse_credentials(json, strlen(json), c); }
int main(void) {
    test_body_deadline();
    bool stopping = false, uploading = false;
    assert(admission_begin(stopping, &uploading));
    assert(!admission_stop(&stopping, uploading) && !stopping);
    uploading = false;
    assert(admission_stop(&stopping, uploading) && stopping);
    assert(!admission_begin(stopping, &uploading) && !uploading);
    stopping = false;
    assert(admission_stop(&stopping, uploading));
    assert(!admission_begin(stopping, &uploading));
    transfer_credentials_t c;
    assert(credentials_from_text("device", "12345678", &c));
    assert(!credentials_from_text("device", "short", &c) && c.version == 0);
    assert(!credentials_from_text("", "12345678", &c));
    assert(!credentials_from_text("123456789012345678901234567890123", "12345678", &c));
    read_pico_transfer_network_t networks[READ_PICO_TRANSFER_SCAN_MAX] = {0};
    size_t count = 0;
    read_pico_transfer_network_t ap = {.ssid = "same", .rssi = -70, .requires_password = true, .supported = true};
    scan_offer(networks, &count, &ap); ap.rssi = -40; scan_offer(networks, &count, &ap);
    assert(count == 1 && networks[0].rssi == -40);
    ap.ssid[0] = 0; scan_offer(networks, &count, &ap); assert(count == 1);
    for (int i = 0; i < 25; ++i) {
        snprintf(ap.ssid, sizeof(ap.ssid), "network-%02d", i); ap.rssi = (int8_t)(-100 + i);
        scan_offer(networks, &count, &ap);
    }
    assert(count == READ_PICO_TRANSFER_SCAN_MAX && networks[0].rssi == -40);
    for (size_t i = 1; i < count; ++i) assert(networks[i - 1].rssi >= networks[i].rssi);
    strcpy(ap.ssid, "network-24"); ap.rssi = -20; ap.requires_password = false;
    scan_offer(networks, &count, &ap);
    assert(count == READ_PICO_TRANSFER_SCAN_MAX && networks[0].rssi == -20 && !networks[0].requires_password);
    assert(parse("{\"ssid\":\"家庭 WiFi\",\"password\":\"12345678\"}", &c));
    assert(!strcmp(c.ssid, "家庭 WiFi"));
    assert(parse("{\"ssid\":\"open\",\"password\":\"\"}", &c));
    transfer_credentials_t old;
    assert(store_credentials(&c)==ESP_OK&&!handles);
    assert(load_credentials(&old)==ESP_OK&&!strcmp(old.ssid,"open"));
    strcpy(c.ssid,"replacement");fail_write=1;
    assert(store_credentials(&c)==ESP_FAIL&&!handles);
    assert(load_credentials(&old)==ESP_OK&&!strcmp(old.ssid,"open"));
    fail_write=0;fail_commit=1;
    assert(store_credentials(&c)==ESP_FAIL&&!handles);
    assert(load_credentials(&old)==ESP_OK&&!strcmp(old.ssid,"open"));
    assert(store_credentials(NULL)==ESP_FAIL&&!handles);
    fail_commit=0;fail_open=1;
    assert(store_credentials(&c)==ESP_FAIL&&!handles);
    assert(load_credentials(&old)==ESP_FAIL&&old.version==0);
    fail_open=0;
    assert(store_credentials(NULL)==ESP_OK&&!handles);
    assert(load_credentials(&old)==ESP_OK&&old.version==0);
    assert(store_credentials(NULL)==ESP_OK&&!handles);
    // 旧记录迁移与八网络MRU；仅连接成功的记录可直连，密码变化重新确认。
    // Migrate legacy data and keep eight MRU profiles; only successful connections qualify, and changed passwords need confirmation.
    memset(committed,0,sizeof(committed));
    assert(credentials_from_text("legacy","12345678",&c));
    committed[0].present=true;committed[0].size=sizeof(c);memcpy(committed[0].bytes,&c,sizeof(c));
    bool confirmed=false;
    assert(find_saved_credentials("legacy",&old,&confirmed)==ESP_OK&&confirmed&&!strcmp(old.password,"12345678"));
    for(int i=0;i<9;++i){
        char name[20];snprintf(name,sizeof(name),"saved-%d",i);
        assert(credentials_from_text(name,"12345678",&c)&&store_credentials(&c)==ESP_OK);
        assert(find_saved_credentials(name,&old,&confirmed)==ESP_OK&&!confirmed);
        assert(confirm_current_credentials()==ESP_OK);
        assert(find_saved_credentials(name,&old,&confirmed)==ESP_OK&&confirmed);
    }
    assert(find_saved_credentials("legacy",&old,NULL)==ESP_ERR_NVS_NOT_FOUND);
    assert(find_saved_credentials("saved-0",&old,NULL)==ESP_ERR_NVS_NOT_FOUND);
    assert(find_saved_credentials("saved-1",&old,NULL)==ESP_OK);
    int before=writes;assert(confirm_current_credentials()==ESP_OK&&writes==before);
    assert(find_saved_credentials("saved-1",&c,NULL)==ESP_OK&&store_credentials(&c)==ESP_OK);
    assert(load_credentials(&old)==ESP_OK&&!strcmp(old.ssid,"saved-1"));
    assert(find_saved_credentials("saved-1",&old,&confirmed)==ESP_OK&&confirmed);
    strcpy(c.password,"87654321");assert(store_credentials(&c)==ESP_OK);
    assert(find_saved_credentials("saved-1",&old,&confirmed)==ESP_OK&&!confirmed);
    read_pico_transfer_network_t saved_scan[3]={
        {.ssid="saved-1",.requires_password=true,.saved=true},
        {.ssid="saved-2",.requires_password=true},
        {.ssid="saved-2",.requires_password=false,.saved=true}};
    mark_saved_networks(saved_scan,3);assert(!saved_scan[0].saved&&saved_scan[1].saved&&!saved_scan[2].saved);
    assert(confirm_current_credentials()==ESP_OK);mark_saved_networks(saved_scan,3);assert(saved_scan[0].saved);
    assert(forget_current_credentials()==ESP_OK&&!committed[0].present);
    assert(find_saved_credentials("saved-1",&old,NULL)==ESP_ERR_NVS_NOT_FOUND);
    assert(find_saved_credentials("saved-2",&old,&confirmed)==ESP_OK&&confirmed);
    assert(load_credentials(&old)==ESP_OK&&!strcmp(old.ssid,"saved-8"));
    assert(credentials_from_text("public","",&c)&&store_credentials(&c)==ESP_OK&&confirm_current_credentials()==ESP_OK);
    saved_scan[0]=(read_pico_transfer_network_t){.ssid="public"};mark_saved_networks(saved_scan,1);assert(saved_scan[0].saved);
    mock_blob_t bank_snapshot=committed[1];fail_commit=1;
    assert(forget_current_credentials()==ESP_FAIL&&!memcmp(&bank_snapshot,&committed[1],sizeof(bank_snapshot)));
    fail_commit=0;fail_read=1;memset(&old,0xff,sizeof(old));
    assert(load_credentials(&old)==ESP_FAIL&&old.version==0&&!handles);
    saved_scan[0].saved=true;mark_saved_networks(saved_scan,1);assert(!saved_scan[0].saved);fail_read=0;
    committed[1].size--;assert(load_credentials(&old)==ESP_ERR_INVALID_STATE&&old.version==0);
    committed[1]=bank_snapshot;committed[1].bytes[0]^=1;
    assert(store_credentials(&c)==ESP_ERR_INVALID_STATE&&!handles);
    committed[1]=bank_snapshot;
    transfer_wifi_bank_t malformed;memcpy(&malformed,committed[1].bytes,sizeof(malformed));
    malformed.reserved[0]=1;assert(!credentials_bank_valid(&malformed));malformed.reserved[0]=0;
    malformed.count=9;assert(!credentials_bank_valid(&malformed));malformed.count=2;
    malformed.items[1]=malformed.items[0];assert(!credentials_bank_valid(&malformed));
    assert(store_credentials(NULL)==ESP_OK&&load_credentials(&old)==ESP_OK&&!old.version&&!handles);
    const char *bad[] = {"{}", "[]", "null", "{\"ssid\":\"\",\"password\":\"12345678\"}",
        "{\"ssid\":\"home\",\"password\":12345678}", "{\"ssid\":\"home\",\"password\":\"short\"}",
        "{\"ssid\":\"home\",\"password\":\"12345678\"}x", "{\"ssid\":\"home\\u0000x\",\"password\":\"12345678\"}",
        "{\"ssid\":\"home\",\"ssid\":\"evil\",\"password\":\"12345678\"}",
        "{\"ssid\":\"home\",\"password\":\"12345678\",\"unknown\":1}"};
    for (size_t i = 0; i < sizeof(bad)/sizeof(*bad); ++i) assert(!parse(bad[i], &c));
    assert(!parse("{\"ssid\":[],\"password\":\"12345678\"}", &c));
    assert(parse("{\"ssid\":\"home[{}]\",\"password\":\"12345678\"}", &c));
    const char embedded[] = "{\"ssid\":\"home\",\"password\":\"12345678\"}\0junk";
    assert(!parse_credentials(embedded, sizeof(embedded) - 1, &c));
    memset(&c, 0, sizeof(c)); c.version = 1; memset(c.ssid, 'a', 32); memset(c.password, 'a', 64);
    assert(credentials_valid(&c)); c.password[0] = 'g'; assert(!credentials_valid(&c));
    c.password[0] = 'a'; c.ssid[32] = 'b'; assert(!credentials_valid(&c));
    clear_secret(&c, sizeof(c)); for (size_t i = 0; i < sizeof(c); ++i) assert(((unsigned char *)&c)[i] == 0);
    transfer_connection_t net;
    connection_begin(&net, 100);
    assert(connection_poll(&net, 100) == 1 && net.attempts == 1);
    assert(connection_poll(&net, 29999) == 0);
    assert(connection_poll(&net, 30100) == -1 && net.failed);
    assert(connection_poll(&net, 40000) == 0);
    connection_begin(&net, 0);
    for (int i = 0; i < 5; ++i) {
        assert(connection_poll(&net, i * 1000) == 1);
        connection_lost(&net, i * 1000);
    }
    assert(connection_poll(&net, 5000) == -1);
    connection_begin(&net, 0); net.online = true;
    assert(connection_poll(&net, 100000) == 0);
    connection_lost(&net, 100000);
    assert(connection_poll(&net, 100999) == 0);
    assert(connection_poll(&net, 101000) == 1 && net.attempts == 1);
    assert(time_route(true, true, true, false) == TRANSFER_TIME_ONLINE);
    assert(time_route(true, true, false, false) == TRANSFER_TIME_UNAVAILABLE);
    assert(time_route(true, false, true, false) == TRANSFER_TIME_UNAVAILABLE);
    assert(time_route(false, true, false, true) == TRANSFER_TIME_TEMPORARY);
    assert(time_route(false, true, false, false) == TRANSFER_TIME_UNAVAILABLE);
    puts("transfer WiFi policy tests passed");
}
