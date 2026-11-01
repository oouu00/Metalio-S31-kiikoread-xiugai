/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * 中文：NimBLE 形状的 BLE central 适配层，底层实现换成 Bluedroid GATT client。
 * 只提供翻页器实际用到的子集：扫描、连接、配对、服务/特征/描述符发现、读、写、
 * 通知。名字与结构体成员沿用 NimBLE，是为了让 ble_page_turner.c 的逻辑零改动。
 *
 * English: a NimBLE-shaped BLE central adapter implemented over the Bluedroid GATT client.
 * Only the subset the page turner uses: scan, connect, pairing, service/characteristic/
 * descriptor discovery, read, write and notifications. Names and struct members follow
 * NimBLE so ble_page_turner.c's logic stays untouched.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- 常量 / Constants ---- */

#define BLE_ADDR_PUBLIC 0
#define BLE_ADDR_RANDOM 1

#define BLE_HS_CONN_HANDLE_NONE 0xFFFF
// 只在本适配层内部取值；调用方仅做等值比较。
// Values are internal to this adapter; callers only compare for equality.
#define BLE_HS_EINVAL 1
#define BLE_HS_EBUSY 2
#define BLE_HS_EDONE 3
#define BLE_HS_ENOTCONN 4

#define BLE_GATT_CHR_PROP_READ 0x02
#define BLE_GATT_CHR_PROP_WRITE_NO_RSP 0x04
#define BLE_GATT_CHR_PROP_WRITE 0x08
#define BLE_GATT_CHR_PROP_NOTIFY 0x10

#define BLE_SM_IOACT_NONE 0
#define BLE_SM_IOACT_DISP 1
#define BLE_SM_IOACT_NUMCMP 2
#define BLE_SM_IO_CAP_DISP_YES_NO 3
#define BLE_SM_PAIR_KEY_DIST_ENC 0x01

#define BLE_GAP_EVENT_DISC 1
#define BLE_GAP_EVENT_DISC_COMPLETE 2
#define BLE_GAP_EVENT_CONNECT 3
#define BLE_GAP_EVENT_DISCONNECT 4
#define BLE_GAP_EVENT_ENC_CHANGE 5
#define BLE_GAP_EVENT_PASSKEY_ACTION 6
#define BLE_GAP_EVENT_NOTIFY_RX 7
#define BLE_GAP_EVENT_CONN_UPDATE_REQ 8

#define BLE_ATT_MTU_MAX 517

// 适配层任务参数：与原先 NimBLE 的两个宏同名同值，调用点不用改。
// Adapter task parameters; same names and values as the former NimBLE macros.
#define BT_HOST_TASK_STACK_SIZE 8192
#define BT_HOST_PINNED_TO_CORE 0

/* ---- 基础类型 / Basic types ---- */

typedef struct {
    uint8_t type;
    uint8_t val[6];
} ble_addr_t;

typedef struct {
    uint8_t type;  // 只用 16 位 UUID
    uint16_t value;
} ble_uuid_t;

typedef struct {
    ble_uuid_t u;
    uint16_t value;
} ble_uuid16_t;

#define BLE_UUID16_INIT(value_) \
    { .u = {.type = 0, .value = (uint16_t)(value_)}, .value = (uint16_t)(value_) }
#define BLE_UUID16_DECLARE(value_)                                   \
    ((ble_uuid16_t){.u = {.type = 0, .value = (uint16_t)(value_)}, \
                    .value = (uint16_t)(value_)})

// 通知负载的线性视图；只需读，不需要真正的 mbuf 链。
// A linear view of a notification payload; read only, no real mbuf chains.
struct os_mbuf {
    const uint8_t *data;
    uint16_t len;
};

#define OS_MBUF_DATA(om, type) ((type)((om) ? (om)->data : NULL))
#define OS_MBUF_PKTLEN(om) ((om) ? (uint16_t)(om)->len : 0)

/* ---- GAP ---- */

struct ble_gap_conn_desc {
    uint16_t conn_handle;
    ble_addr_t peer_id_addr;
    ble_addr_t peer_ota_addr;
    struct {
        unsigned bonded : 1;
        unsigned encrypted : 1;
    } sec_state;
    uint16_t conn_itvl;
    uint16_t supervision_timeout;
};

struct ble_gap_disc_desc {
    uint8_t event_type;
    ble_addr_t addr;
    int8_t rssi;
    uint8_t length_data;
    const uint8_t *data;
};

struct ble_gap_upd_params {
    uint16_t itvl_min;
    uint16_t itvl_max;
    uint16_t latency;
    uint16_t supervision_timeout;
};

struct ble_gap_disc_params {
    int32_t itvl;
    int32_t window;
    int filter_duplicates;
    int passive;
};

struct ble_sm_io {
    uint8_t action;
    uint32_t passkey;
    uint8_t numcmp_accept;
};

struct ble_gap_event {
    uint8_t type;
    union {
        struct {
            int status;
            uint16_t conn_handle;
        } connect;
        struct {
            uint16_t conn_handle;
            int reason;
        } disconnect;
        struct ble_gap_disc_desc disc;
        struct {
            uint16_t conn_handle;
            int status;
        } enc_change;
        struct {
            uint16_t conn_handle;
            struct ble_gap_upd_params *self_params;
            struct ble_gap_upd_params *peer_params;
        } conn_update_req;
        struct {
            uint16_t conn_handle;
            struct {
                uint8_t action;
            } params;
        } passkey;
        struct {
            uint16_t conn_handle;
            uint16_t attr_handle;
            struct os_mbuf *om;
            bool indication;
        } notify_rx;
    };
};

/* ---- 广播解析 ---- */

#define BLE_HS_ADV_MAX_UUIDS 8

struct ble_hs_adv_fields {
    const uint8_t *name;
    uint8_t name_len;
    bool name_is_complete;
    ble_uuid16_t uuids16[BLE_HS_ADV_MAX_UUIDS];
    uint8_t num_uuids16;
    bool uuids16_is_complete;
};

/* ---- GATT ---- */

struct ble_gatt_error {
    uint16_t att_handle;
    int status;
};

struct ble_gatt_svc {
    ble_uuid16_t uuid;
    uint16_t start_handle;
    uint16_t end_handle;
};

struct ble_gatt_chr {
    ble_uuid16_t uuid;
    uint16_t def_handle;
    uint16_t val_handle;
    uint8_t properties;
};

struct ble_gatt_dsc {
    ble_uuid16_t uuid;
    uint16_t handle;
};

struct ble_gatt_attr {
    uint16_t handle;
    uint16_t offset;
    struct os_mbuf *om;
};

typedef int ble_gap_event_fn(struct ble_gap_event *event, void *arg);
typedef int ble_gatt_svc_fn(uint16_t conn_handle, const struct ble_gatt_error *error,
                            const struct ble_gatt_svc *svc, void *arg);
typedef int ble_gatt_chr_fn(uint16_t conn_handle, const struct ble_gatt_error *error,
                            const struct ble_gatt_chr *chr, void *arg);
typedef int ble_gatt_dsc_fn(uint16_t conn_handle, const struct ble_gatt_error *error,
                            uint16_t chr_val_handle, const struct ble_gatt_dsc *dsc, void *arg);
typedef int ble_gatt_attr_fn(uint16_t conn_handle, const struct ble_gatt_error *error,
                             struct ble_gatt_attr *attr, void *arg);

/* ---- 配置（与 NimBLE 同名，供调用方赋值）---- */

struct ble_hs_cfg {
    void (*sync_cb)(void);
    uint8_t sm_io_cap;
    uint8_t sm_bonding;
    uint8_t sm_mitm;
    uint8_t sm_sc;
    uint8_t sm_our_key_dist;
    uint8_t sm_their_key_dist;
};

extern struct ble_hs_cfg ble_hs_cfg;

/* ---- 宿主（适配层内部委托 bt_host 的引用计数）---- */

esp_err_t nimble_port_init(void);
int nimble_port_stop(void);
esp_err_t nimble_port_deinit(void);
void nimble_port_run(void);

bool ble_hs_synced(void);
int ble_hs_util_ensure_addr(int prefer_public);
int ble_hs_id_infer_auto(int prefer_public, uint8_t *out_addr_type);

/* ---- GAP / SM ---- */

int ble_gap_disc(uint8_t own_addr_type, int32_t duration_ms,
                 const struct ble_gap_disc_params *params, ble_gap_event_fn *cb, void *arg);
int ble_gap_disc_cancel(void);
int ble_gap_connect(uint8_t own_addr_type, const ble_addr_t *peer, int32_t timeout_ms,
                    const struct ble_gap_upd_params *params, ble_gap_event_fn *cb, void *arg);
int ble_gap_terminate(uint16_t conn_handle, uint8_t reason);
int ble_gap_security_initiate(uint16_t conn_handle);
int ble_gap_conn_find(uint16_t conn_handle, struct ble_gap_conn_desc *out);
int ble_sm_inject_io(uint16_t conn_handle, const struct ble_sm_io *io);
int ble_store_util_delete_peer(const ble_addr_t *peer);

/* ---- 广播 / UUID 工具 ---- */

int ble_hs_adv_parse_fields(struct ble_hs_adv_fields *fields, const uint8_t *data, uint8_t len);
int ble_uuid_cmp(const ble_uuid_t *a, const ble_uuid_t *b);
uint16_t ble_uuid_u16(const ble_uuid_t *uuid);

/* ---- GATT client ---- */

int ble_gattc_disc_all_svcs(uint16_t conn_handle, ble_gatt_svc_fn *cb, void *arg);
int ble_gattc_disc_all_chrs(uint16_t conn_handle, uint16_t start_handle, uint16_t end_handle,
                            ble_gatt_chr_fn *cb, void *arg);
int ble_gattc_disc_all_dscs(uint16_t conn_handle, uint16_t chr_val_handle, uint16_t end_handle,
                            ble_gatt_dsc_fn *cb, void *arg);
int ble_gattc_read(uint16_t conn_handle, uint16_t chr_val_handle, ble_gatt_attr_fn *cb, void *arg);
int ble_gattc_write_flat(uint16_t conn_handle, uint16_t chr_val_handle, const void *data,
                         uint16_t data_len, ble_gatt_attr_fn *cb, void *arg);
int os_mbuf_copydata(const struct os_mbuf *om, int off, int len, void *dst);

#ifdef __cplusplus
}
#endif
