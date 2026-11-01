/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * 中文：NimBLE 形状的 BLE central 适配层，底层是 Bluedroid 的 GATT client。
 * `ble_page_turner.c` 的 2000 行状态机与 HID 解析保持原样，只把传输层换成本文件。
 *
 * 关键约束：Bluedroid 明确禁止在协议栈回调里阻塞、等信号量或读写 flash，而翻页器的
 * 回调会写 NVS。因此回调只把事件压进队列，真正的派发在 `nimble_port_run()` 里跑，
 * 也就是翻页器自己的 `nimble_host` 任务。
 *
 * English: a NimBLE-shaped BLE central adapter implemented on top of Bluedroid's GATT
 * client. `ble_page_turner.c`'s 2000-line state machine and HID parsing stay untouched; only
 * the transport changes. Bluedroid forbids blocking, long semaphore waits and flash access
 * inside stack callbacks, while the page turner's callbacks write NVS, so callbacks only
 * enqueue events and the dispatch runs in `nimble_port_run()` -- the page turner's own
 * `nimble_host` task.
 */

#include "ble_hid_host.h"

#if KIIKO_BOARD_METALIO47

#include <stdlib.h>
#include <string.h>

#include "bt_host.h"
#include "bt_host_internal.h"
#include "esp_bt_main.h"
#include "esp_gap_ble_api.h"
#include "esp_gatt_defs.h"
#include "esp_gattc_api.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

static const char *TAG = "ble_hid";

#define ADAPTER_QUEUE_DEPTH 32
#define ADAPTER_MAX_DSC 24
#define ADAPTER_MAX_CHR 16

/* ---- 事件 / Events ---- */

enum {
    EV_DISC = 1,
    EV_DISC_DONE,
    EV_CONNECT,
    EV_DISCONNECT,
    EV_ENC,
    EV_PASSKEY_DISP,
    EV_PASSKEY_NUMCMP,
    EV_NOTIFY,
    EV_SVC_RES,
    EV_SVC_DONE,
    EV_READ_DONE,
    EV_WRITE_DONE,
};

typedef struct {
    uint8_t kind;
    int status;
    uint16_t conn;
    uint16_t handle;
    uint16_t len;
    uint16_t start;
    uint16_t end;
    uint16_t uuid16;
    uint8_t properties;
    uint8_t addr_type;
    uint8_t event_type;
    uint8_t addr[6];
    int8_t rssi;
    uint32_t passkey;
    uint8_t data[];
} adapter_ev_t;

/* ---- 状态 / State ---- */

struct ble_hs_cfg ble_hs_cfg;

static QueueHandle_t s_queue;
static bool s_started;
static volatile bool s_in_run;
static volatile bool s_stop_requested;
static esp_gatt_if_t s_gattc_if = (esp_gatt_if_t)0xFF;
static uint16_t s_conn_id = 0xFFFF;
static uint8_t s_conn_bda[6];
static uint8_t s_conn_addr_type = BLE_ADDR_PUBLIC;
static volatile bool s_conn_open;
static volatile bool s_encrypted;
static volatile bool s_bonded;
static uint32_t s_static_passkey;

static ble_gap_event_fn *s_gap_cb;
static void *s_gap_arg;
static ble_gatt_svc_fn *s_svc_cb;
static void *s_svc_arg;
static ble_gatt_attr_fn *s_read_cb;
static void *s_read_arg;
static ble_gatt_attr_fn *s_write_cb;
static void *s_write_arg;

static uint16_t s_dsc_handles[ADAPTER_MAX_DSC];
static uint8_t s_dsc_count;

static bool s_scan_pending;
static uint32_t s_scan_seconds;

static esp_ble_scan_params_t s_scan_params = {
    .scan_type = BLE_SCAN_TYPE_ACTIVE,
    .own_addr_type = BLE_ADDR_TYPE_PUBLIC,
    .scan_filter_policy = BLE_SCAN_FILTER_ALLOW_ALL,
    .scan_interval = 0x50,
    .scan_window = 0x30,
    .scan_duplicate = BLE_SCAN_DUPLICATE_DISABLE,
};

/* ---- 小工具 / Helpers ---- */

static adapter_ev_t *ev_new(uint8_t kind, uint16_t len) {
    adapter_ev_t *ev = heap_caps_malloc(sizeof(*ev) + len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!ev) ev = heap_caps_malloc(sizeof(*ev) + len, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!ev) return NULL;
    memset(ev, 0, sizeof(*ev));
    ev->kind = kind;
    ev->len = len;
    return ev;
}

static void ev_post(adapter_ev_t *ev) {
    if (!ev) return;
    if (!s_queue || xQueueSend(s_queue, &ev, 0) != pdTRUE) {
        heap_caps_free(ev);
    }
}

static void dsc_record(uint16_t handle) {
    if (handle == 0) return;
    for (uint8_t i = 0; i < s_dsc_count; ++i) {
        if (s_dsc_handles[i] == handle) return;
    }
    if (s_dsc_count < ADAPTER_MAX_DSC) s_dsc_handles[s_dsc_count++] = handle;
}

static bool dsc_is_descriptor(uint16_t handle) {
    for (uint8_t i = 0; i < s_dsc_count; ++i) {
        if (s_dsc_handles[i] == handle) return true;
    }
    return false;
}

static void copy_uuid(ble_uuid16_t *out, const esp_bt_uuid_t *in) {
    memset(out, 0, sizeof(*out));
    if (in->len == ESP_UUID_LEN_16) {
        out->u.type = 0;
        out->u.value = in->uuid.uuid16;
        out->value = in->uuid.uuid16;
    }
}

static int status_to_err(esp_gatt_status_t status) {
    return status == ESP_GATT_OK ? 0 : (int)status;
}

/* ---- GATT client 回调（BTC 上下文，只入队）/ GATTC callback (BTC context, enqueue only) ---- */

static void gattc_cb(esp_gattc_cb_event_t event, esp_gatt_if_t gattc_if,
                     esp_ble_gattc_cb_param_t *p) {
    switch (event) {
        case ESP_GATTC_REG_EVT:
            if (p->reg.status == ESP_GATT_OK) s_gattc_if = gattc_if;
            break;

        case ESP_GATTC_OPEN_EVT: {
            adapter_ev_t *ev = ev_new(EV_CONNECT, 0);
            if (ev) {
                ev->status = status_to_err(p->open.status);
                ev->conn = p->open.conn_id;
                memcpy(ev->addr, p->open.remote_bda, 6);
                ev_post(ev);
            }
            if (p->open.status == ESP_GATT_OK) {
                s_conn_id = p->open.conn_id;
                memcpy(s_conn_bda, p->open.remote_bda, 6);
                s_conn_open = true;
                s_encrypted = false;
                s_bonded = false;
                s_dsc_count = 0;
            }
            break;
        }

        case ESP_GATTC_DISCONNECT_EVT:
        case ESP_GATTC_CLOSE_EVT: {
            const bool open = s_conn_open;
            const int reason = event == ESP_GATTC_DISCONNECT_EVT ? (int)p->disconnect.reason
                                                                 : (int)p->close.reason;
            if (open) {
                s_conn_open = false;
                s_conn_id = 0xFFFF;
                s_encrypted = false;
                adapter_ev_t *ev = ev_new(EV_DISCONNECT, 0);
                if (ev) {
                    ev->status = reason;
                    ev_post(ev);
                }
            }
            break;
        }

        case ESP_GATTC_SEARCH_RES_EVT: {
            adapter_ev_t *ev = ev_new(EV_SVC_RES, 0);
            if (ev) {
                ev->conn = p->search_res.conn_id;
                ev->start = p->search_res.start_handle;
                ev->end = p->search_res.end_handle;
                ev->uuid16 = p->search_res.srvc_id.uuid.len == ESP_UUID_LEN_16
                                 ? p->search_res.srvc_id.uuid.uuid.uuid16
                                 : 0;
                ev_post(ev);
            }
            break;
        }

        case ESP_GATTC_SEARCH_CMPL_EVT: {
            adapter_ev_t *ev = ev_new(EV_SVC_DONE, 0);
            if (ev) {
                ev->conn = p->search_cmpl.conn_id;
                ev_post(ev);
            }
            break;
        }

        case ESP_GATTC_READ_CHAR_EVT:
        case ESP_GATTC_READ_DESCR_EVT: {
            const uint16_t len = p->read.value_len;
            adapter_ev_t *ev = ev_new(EV_READ_DONE, len);
            if (ev) {
                ev->status = status_to_err(p->read.status);
                ev->conn = p->read.conn_id;
                ev->handle = p->read.handle;
                if (len && p->read.value) memcpy(ev->data, p->read.value, len);
                ev_post(ev);
            }
            break;
        }

        case ESP_GATTC_WRITE_CHAR_EVT:
        case ESP_GATTC_WRITE_DESCR_EVT: {
            adapter_ev_t *ev = ev_new(EV_WRITE_DONE, 0);
            if (ev) {
                ev->status = status_to_err(p->write.status);
                ev->conn = p->write.conn_id;
                ev->handle = p->write.handle;
                ev_post(ev);
            }
            break;
        }

        case ESP_GATTC_NOTIFY_EVT: {
            const uint16_t len = p->notify.value_len;
            adapter_ev_t *ev = ev_new(EV_NOTIFY, len);
            if (ev) {
                ev->conn = p->notify.conn_id;
                ev->handle = p->notify.handle;
                if (len && p->notify.value) memcpy(ev->data, p->notify.value, len);
                ev_post(ev);
            }
            break;
        }

        default:
            break;
    }
}

/* ---- BLE GAP 回调（BTC 上下文，只入队）/ BLE GAP callback (BTC context, enqueue only) ---- */

static void gap_ble_cb(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t *p) {
    switch (event) {
        case ESP_GAP_BLE_SCAN_PARAM_SET_COMPLETE_EVT:
            if (s_scan_pending) {
                s_scan_pending = false;
                if (p->scan_param_cmpl.status == ESP_BT_STATUS_SUCCESS) {
                    esp_ble_gap_start_scanning(s_scan_seconds);
                } else {
                    ESP_LOGW(TAG, "scan params refused: %d", (int)p->scan_param_cmpl.status);
                    adapter_ev_t *ev = ev_new(EV_DISC_DONE, 0);
                    ev_post(ev);
                }
            }
            break;

        case ESP_GAP_BLE_SCAN_RESULT_EVT:
            if (p->scan_rst.search_evt == ESP_GAP_SEARCH_INQ_RES_EVT) {
                uint16_t len = p->scan_rst.adv_data_len + p->scan_rst.scan_rsp_len;
                if (len > sizeof(p->scan_rst.ble_adv)) len = sizeof(p->scan_rst.ble_adv);
                adapter_ev_t *ev = ev_new(EV_DISC, len);
                if (ev) {
                    memcpy(ev->addr, p->scan_rst.bda, 6);
                    ev->addr_type =
                        p->scan_rst.ble_addr_type == BLE_ADDR_TYPE_RANDOM ? BLE_ADDR_RANDOM
                                                                          : BLE_ADDR_PUBLIC;
                    ev->event_type = (uint8_t)p->scan_rst.ble_evt_type;
                    ev->rssi = (int8_t)p->scan_rst.rssi;
                    if (len) memcpy(ev->data, p->scan_rst.ble_adv, len);
                    ev_post(ev);
                }
            } else if (p->scan_rst.search_evt == ESP_GAP_SEARCH_INQ_CMPL_EVT) {
                adapter_ev_t *ev = ev_new(EV_DISC_DONE, 0);
                ev_post(ev);
            }
            break;

        case ESP_GAP_BLE_SCAN_STOP_COMPLETE_EVT: {
            adapter_ev_t *ev = ev_new(EV_DISC_DONE, 0);
            ev_post(ev);
            break;
        }

        case ESP_GAP_BLE_SEC_REQ_EVT:
            esp_ble_gap_security_rsp(p->ble_security.ble_req.bd_addr, true);
            break;

        case ESP_GAP_BLE_PASSKEY_NOTIF_EVT:
            s_static_passkey = p->ble_security.key_notif.passkey;
            {
                adapter_ev_t *ev = ev_new(EV_PASSKEY_DISP, 0);
                if (ev) {
                    ev->conn = s_conn_id;
                    ev->passkey = s_static_passkey;
                    ev_post(ev);
                }
            }
            break;

        case ESP_GAP_BLE_PASSKEY_REQ_EVT:
            esp_ble_passkey_reply(p->ble_security.ble_req.bd_addr, true, s_static_passkey);
            break;

        case ESP_GAP_BLE_NC_REQ_EVT: {
            adapter_ev_t *ev = ev_new(EV_PASSKEY_NUMCMP, 0);
            if (ev) {
                ev->conn = s_conn_id;
                ev_post(ev);
            }
            break;
        }

        case ESP_GAP_BLE_AUTH_CMPL_EVT: {
            const bool ok = p->ble_security.auth_cmpl.success;
            if (ok) {
                s_bonded = p->ble_security.auth_cmpl.key_present;
                s_encrypted = true;
            }
            adapter_ev_t *ev = ev_new(EV_ENC, 0);
            if (ev) {
                ev->status = ok ? 0 : -1;
                ev->conn = s_conn_id;
                ev_post(ev);
            }
            if (!ok) ESP_LOGW(TAG, "auth failed: %d", (int)p->ble_security.auth_cmpl.fail_reason);
            break;
        }

        default:
            break;
    }
}

/* ---- 派发 / Dispatch ---- */

static void dispatch(adapter_ev_t *ev) {
    switch (ev->kind) {
        case EV_DISC: {
            struct ble_gap_event e;
            memset(&e, 0, sizeof(e));
            e.type = BLE_GAP_EVENT_DISC;
            e.disc.event_type = ev->event_type;
            e.disc.addr.type = ev->addr_type;
            memcpy(e.disc.addr.val, ev->addr, 6);
            e.disc.rssi = ev->rssi;
            e.disc.length_data = (uint8_t)ev->len;
            e.disc.data = ev->data;
            if (s_gap_cb) s_gap_cb(&e, s_gap_arg);
            break;
        }
        case EV_DISC_DONE: {
            struct ble_gap_event e;
            memset(&e, 0, sizeof(e));
            e.type = BLE_GAP_EVENT_DISC_COMPLETE;
            if (s_gap_cb) s_gap_cb(&e, s_gap_arg);
            break;
        }
        case EV_CONNECT: {
            struct ble_gap_event e;
            memset(&e, 0, sizeof(e));
            e.type = BLE_GAP_EVENT_CONNECT;
            e.connect.status = ev->status;
            e.connect.conn_handle = ev->conn;
            if (s_gap_cb) s_gap_cb(&e, s_gap_arg);
            break;
        }
        case EV_DISCONNECT: {
            struct ble_gap_event e;
            memset(&e, 0, sizeof(e));
            e.type = BLE_GAP_EVENT_DISCONNECT;
            e.disconnect.reason = ev->status;
            e.disconnect.conn_handle = ev->conn;
            if (s_gap_cb) s_gap_cb(&e, s_gap_arg);
            break;
        }
        case EV_ENC: {
            struct ble_gap_event e;
            memset(&e, 0, sizeof(e));
            e.type = BLE_GAP_EVENT_ENC_CHANGE;
            e.enc_change.status = ev->status;
            e.enc_change.conn_handle = ev->conn;
            if (s_gap_cb) s_gap_cb(&e, s_gap_arg);
            break;
        }
        case EV_PASSKEY_DISP:
        case EV_PASSKEY_NUMCMP: {
            struct ble_gap_event e;
            memset(&e, 0, sizeof(e));
            e.type = BLE_GAP_EVENT_PASSKEY_ACTION;
            e.passkey.conn_handle = ev->conn;
            e.passkey.params.action =
                ev->kind == EV_PASSKEY_DISP ? BLE_SM_IOACT_DISP : BLE_SM_IOACT_NUMCMP;
            if (s_gap_cb) s_gap_cb(&e, s_gap_arg);
            break;
        }
        case EV_NOTIFY: {
            struct ble_gap_event e;
            struct os_mbuf om = {.data = ev->data, .len = ev->len};
            memset(&e, 0, sizeof(e));
            e.type = BLE_GAP_EVENT_NOTIFY_RX;
            e.notify_rx.conn_handle = ev->conn;
            e.notify_rx.attr_handle = ev->handle;
            e.notify_rx.om = &om;
            e.notify_rx.indication = false;
            if (s_gap_cb) s_gap_cb(&e, s_gap_arg);
            break;
        }
        case EV_SVC_RES: {
            struct ble_gatt_error err = {.att_handle = 0, .status = 0};
            struct ble_gatt_svc svc;
            memset(&svc, 0, sizeof(svc));
            svc.uuid.u.type = 0;
            svc.uuid.u.value = ev->uuid16;
            svc.uuid.value = ev->uuid16;
            svc.start_handle = ev->start;
            svc.end_handle = ev->end;
            if (s_svc_cb) s_svc_cb(ev->conn, &err, &svc, s_svc_arg);
            break;
        }
        case EV_SVC_DONE: {
            const struct ble_gatt_error err = {.att_handle = 0, .status = BLE_HS_EDONE};
            if (s_svc_cb) s_svc_cb(ev->conn, &err, NULL, s_svc_arg);
            s_svc_cb = NULL;
            s_svc_arg = NULL;
            break;
        }
        case EV_READ_DONE: {
            struct ble_gatt_error err = {.att_handle = ev->handle, .status = ev->status};
            struct os_mbuf om = {.data = ev->data, .len = ev->len};
            struct ble_gatt_attr attr = {
                .handle = ev->handle, .offset = 0, .om = ev->status == 0 ? &om : NULL};
            ble_gatt_attr_fn *cb = s_read_cb;
            void *arg = s_read_arg;
            s_read_cb = NULL;
            s_read_arg = NULL;
            if (cb) cb(ev->conn, &err, &attr, arg);
            break;
        }
        case EV_WRITE_DONE: {
            struct ble_gatt_error err = {.att_handle = ev->handle, .status = ev->status};
            struct ble_gatt_attr attr = {.handle = ev->handle, .offset = 0, .om = NULL};
            ble_gatt_attr_fn *cb = s_write_cb;
            void *arg = s_write_arg;
            s_write_cb = NULL;
            s_write_arg = NULL;
            if (cb) cb(ev->conn, &err, &attr, arg);
            break;
        }
        default:
            break;
    }
}

/* ---- 宿主 / Host ---- */

static void apply_security_config(void) {
    uint8_t auth = 0;
    if (ble_hs_cfg.sm_bonding) auth |= 0x01;
    if (ble_hs_cfg.sm_mitm) auth |= 0x04;
    if (ble_hs_cfg.sm_sc) auth |= 0x08;
    esp_ble_gap_set_security_param(ESP_BLE_SM_AUTHEN_REQ_MODE, &auth, sizeof(auth));

    uint8_t iocap = ESP_IO_CAP_NONE;
    if (ble_hs_cfg.sm_io_cap == BLE_SM_IO_CAP_DISP_YES_NO) iocap = ESP_IO_CAP_IO;
    esp_ble_gap_set_security_param(ESP_BLE_SM_IOCAP_MODE, &iocap, sizeof(iocap));

    uint8_t key_size = 16;
    esp_ble_gap_set_security_param(ESP_BLE_SM_MAX_KEY_SIZE, &key_size, sizeof(key_size));
}

esp_err_t bt_ble_host_start(void) {
    if (s_started) return ESP_OK;
    if (!s_queue) {
        s_queue = xQueueCreate(ADAPTER_QUEUE_DEPTH, sizeof(adapter_ev_t *));
        if (!s_queue) return ESP_ERR_NO_MEM;
    }
    s_stop_requested = false;
    esp_err_t err = esp_ble_gap_register_callback(gap_ble_cb);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "gap callback: %s", esp_err_to_name(err));
        return err;
    }
    err = esp_ble_gattc_register_callback(gattc_cb);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "gattc callback: %s", esp_err_to_name(err));
        return err;
    }
    err = esp_ble_gattc_app_register(0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "gattc app register: %s", esp_err_to_name(err));
        return err;
    }
    esp_ble_gap_set_device_name("Metalio S31");
    s_started = true;
    return ESP_OK;
}

void bt_ble_host_stop(void) {
    if (!s_started) return;
    if (s_conn_open) esp_ble_gattc_close(s_gattc_if, s_conn_id);
    s_conn_open = false;
    s_conn_id = 0xFFFF;
    if (s_gattc_if != (esp_gatt_if_t)0xFF &&
        esp_bluedroid_get_status() == ESP_BLUEDROID_STATUS_ENABLED) {
        esp_ble_gattc_app_unregister(s_gattc_if);
    }
    s_gattc_if = (esp_gatt_if_t)0xFF;
    if (s_queue) {
        adapter_ev_t *ev = NULL;
        while (xQueueReceive(s_queue, &ev, 0) == pdTRUE) heap_caps_free(ev);
        vQueueDelete(s_queue);
        s_queue = NULL;
    }
    s_gap_cb = NULL;
    s_svc_cb = NULL;
    s_read_cb = NULL;
    s_write_cb = NULL;
    s_started = false;
}

bool bt_ble_host_running(void) { return s_started; }

esp_err_t nimble_port_init(void) {
    const esp_err_t err = bt_host_start();
    if (err != ESP_OK) return err;
    // 音乐可能先起了协议栈：确保本适配层的 GATTC 应用已注册。
    // Music may have brought the stack up first; make sure our GATTC app is registered.
    return bt_ble_host_start();
}

void nimble_port_run(void) {
    s_in_run = true;
    s_stop_requested = false;
    apply_security_config();
    if (ble_hs_cfg.sync_cb) ble_hs_cfg.sync_cb();
    while (!s_stop_requested) {
        adapter_ev_t *ev = NULL;
        if (xQueueReceive(s_queue, &ev, pdMS_TO_TICKS(50)) != pdTRUE) continue;
        dispatch(ev);
        heap_caps_free(ev);
    }
    s_in_run = false;
}

int nimble_port_stop(void) {
    if (!s_in_run) return 0;
    s_stop_requested = true;
    for (int i = 0; i < 200 && s_in_run; ++i) vTaskDelay(pdMS_TO_TICKS(10));
    return s_in_run ? -1 : 0;
}

esp_err_t nimble_port_deinit(void) {
    return bt_host_stop();
}

bool ble_hs_synced(void) { return s_started && bt_host_running(); }

int ble_hs_util_ensure_addr(int prefer_public) {
    (void)prefer_public;
    return s_started ? 0 : BLE_HS_EINVAL;
}

int ble_hs_id_infer_auto(int prefer_public, uint8_t *out_addr_type) {
    (void)prefer_public;
    if (!out_addr_type) return BLE_HS_EINVAL;
    uint8_t type = BLE_ADDR_TYPE_PUBLIC;
    if (esp_ble_gap_get_local_used_addr(NULL, &type) == ESP_OK &&
        type == BLE_ADDR_TYPE_RANDOM) {
        *out_addr_type = BLE_ADDR_RANDOM;
    } else {
        *out_addr_type = BLE_ADDR_PUBLIC;
    }
    return 0;
}

/* ---- GAP ---- */

int ble_gap_disc(uint8_t own_addr_type, int32_t duration_ms,
                 const struct ble_gap_disc_params *params, ble_gap_event_fn *cb, void *arg) {
    if (!s_started) return BLE_HS_EINVAL;
    s_gap_cb = cb;
    s_gap_arg = arg;
    s_scan_params.own_addr_type =
        own_addr_type == BLE_ADDR_RANDOM ? BLE_ADDR_TYPE_RANDOM : BLE_ADDR_TYPE_PUBLIC;
    if (params && params->passive) s_scan_params.scan_type = BLE_SCAN_TYPE_PASSIVE;
    else s_scan_params.scan_type = BLE_SCAN_TYPE_ACTIVE;
    s_scan_seconds = duration_ms > 0 ? (uint32_t)((duration_ms + 999) / 1000) : 0;
    s_scan_pending = true;
    const esp_err_t err = esp_ble_gap_set_scan_params(&s_scan_params);
    if (err != ESP_OK) {
        s_scan_pending = false;
        ESP_LOGW(TAG, "set scan params: %s", esp_err_to_name(err));
        return BLE_HS_EBUSY;
    }
    return 0;
}

int ble_gap_disc_cancel(void) {
    if (!s_started) return BLE_HS_EINVAL;
    esp_ble_gap_stop_scanning();
    return 0;
}

int ble_gap_connect(uint8_t own_addr_type, const ble_addr_t *peer, int32_t timeout_ms,
                    const struct ble_gap_upd_params *params, ble_gap_event_fn *cb, void *arg) {
    (void)own_addr_type;
    (void)timeout_ms;
    (void)params;
    if (!s_started || !peer || s_gattc_if == (esp_gatt_if_t)0xFF) return BLE_HS_EBUSY;
    s_gap_cb = cb;
    s_gap_arg = arg;
    const esp_ble_addr_type_t type =
        peer->type == BLE_ADDR_RANDOM ? BLE_ADDR_TYPE_RANDOM : BLE_ADDR_TYPE_PUBLIC;
    const esp_err_t err = esp_ble_gattc_open(s_gattc_if, (uint8_t *)peer->val, type, true);
    return err == ESP_OK ? 0 : BLE_HS_EINVAL;
}

int ble_gap_terminate(uint16_t conn_handle, uint8_t reason) {
    (void)reason;
    if (!s_conn_open || conn_handle != s_conn_id) return BLE_HS_ENOTCONN;
    return esp_ble_gattc_close(s_gattc_if, conn_handle) == ESP_OK ? 0 : BLE_HS_EINVAL;
}

int ble_gap_security_initiate(uint16_t conn_handle) {
    (void)conn_handle;
    if (!s_conn_open) return BLE_HS_ENOTCONN;
    return esp_ble_set_encryption(s_conn_bda, ESP_BLE_SEC_ENCRYPT) == ESP_OK ? 0 : BLE_HS_EINVAL;
}

int ble_gap_conn_find(uint16_t conn_handle, struct ble_gap_conn_desc *out) {
    if (!s_conn_open || conn_handle != s_conn_id || !out) return BLE_HS_ENOTCONN;
    memset(out, 0, sizeof(*out));
    out->conn_handle = conn_handle;
    memcpy(out->peer_id_addr.val, s_conn_bda, 6);
    out->peer_id_addr.type = s_conn_addr_type;
    out->peer_ota_addr = out->peer_id_addr;
    out->sec_state.encrypted = s_encrypted;
    out->sec_state.bonded = s_bonded;
    return 0;
}

int ble_sm_inject_io(uint16_t conn_handle, const struct ble_sm_io *io) {
    (void)conn_handle;
    if (!s_conn_open || !io) return BLE_HS_EINVAL;
    if (io->action == BLE_SM_IOACT_DISP) {
        return esp_ble_passkey_reply(s_conn_bda, true, io->passkey) == ESP_OK ? 0 : BLE_HS_EINVAL;
    }
    if (io->action == BLE_SM_IOACT_NUMCMP) {
        return esp_ble_confirm_reply(s_conn_bda, io->numcmp_accept != 0) == ESP_OK
                   ? 0
                   : BLE_HS_EINVAL;
    }
    return BLE_HS_EINVAL;
}

int ble_store_util_delete_peer(const ble_addr_t *peer) {
    if (!peer) return BLE_HS_EINVAL;
    return esp_ble_remove_bond_device((uint8_t *)peer->val) == ESP_OK ? 0 : BLE_HS_EINVAL;
}

/* ---- 广播 / UUID 工具 / Adv and UUID helpers ---- */

int ble_hs_adv_parse_fields(struct ble_hs_adv_fields *fields, const uint8_t *data, uint8_t len) {
    if (!fields) return BLE_HS_EINVAL;
    memset(fields, 0, sizeof(*fields));
    if (!data) return 0;
    int i = 0;
    while (i + 1 < len) {
        const uint8_t block_len = data[i];
        if (block_len == 0 || i + 1 + block_len > len) break;
        const uint8_t type = data[i + 1];
        const uint8_t *value = &data[i + 2];
        const uint8_t value_len = (uint8_t)(block_len - 1);
        if ((type == 0x08 || type == 0x09) && !fields->name) {
            fields->name = value;
            fields->name_len = value_len;
            fields->name_is_complete = (type == 0x09);
        } else if ((type == 0x02 || type == 0x03) && fields->num_uuids16 < BLE_HS_ADV_MAX_UUIDS) {
            for (uint8_t k = 0; k + 1 < value_len && fields->num_uuids16 < BLE_HS_ADV_MAX_UUIDS;
                 k += 2) {
                const uint16_t uuid = (uint16_t)(value[k] | (value[k + 1] << 8));
                ble_uuid16_t *slot = &fields->uuids16[fields->num_uuids16++];
                slot->u.type = 0;
                slot->u.value = uuid;
                slot->value = uuid;
            }
            fields->uuids16_is_complete = (type == 0x03);
        }
        i += block_len + 1;
    }
    return 0;
}

int ble_uuid_cmp(const ble_uuid_t *a, const ble_uuid_t *b) {
    if (!a || !b) return -1;
    if (a->type != b->type) return -1;
    return a->value == b->value ? 0 : -1;
}

uint16_t ble_uuid_u16(const ble_uuid_t *uuid) { return uuid ? uuid->value : 0; }

/* ---- GATT client ---- */

int ble_gattc_disc_all_svcs(uint16_t conn_handle, ble_gatt_svc_fn *cb, void *arg) {
    if (!s_started || s_gattc_if == (esp_gatt_if_t)0xFF) return BLE_HS_EBUSY;
    s_svc_cb = cb;
    s_svc_arg = arg;
    const esp_err_t err = esp_ble_gattc_search_service(s_gattc_if, conn_handle, NULL);
    if (err != ESP_OK) {
        s_svc_cb = NULL;
        s_svc_arg = NULL;
        return BLE_HS_EINVAL;
    }
    return 0;
}

int ble_gattc_disc_all_chrs(uint16_t conn_handle, uint16_t start_handle, uint16_t end_handle,
                            ble_gatt_chr_fn *cb, void *arg) {
    if (!s_started || s_gattc_if == (esp_gatt_if_t)0xFF || !cb) return BLE_HS_EINVAL;
    esp_gattc_char_elem_t elems[ADAPTER_MAX_CHR];
    uint16_t count = ADAPTER_MAX_CHR;
    const esp_gatt_status_t st = esp_ble_gattc_get_all_char(
        s_gattc_if, conn_handle, start_handle, end_handle, elems, &count, 0);
    if (st == ESP_GATT_OK) {
        const struct ble_gatt_error err = {.att_handle = 0, .status = 0};
        for (uint16_t i = 0; i < count; ++i) {
            struct ble_gatt_chr chr;
            memset(&chr, 0, sizeof(chr));
            copy_uuid(&chr.uuid, &elems[i].uuid);
            chr.def_handle = elems[i].char_handle;
            chr.val_handle = elems[i].char_handle;
            chr.properties = (uint8_t)elems[i].properties;
            if (cb(conn_handle, &err, &chr, arg) != 0) break;
        }
    }
    const struct ble_gatt_error done = {.att_handle = 0, .status = BLE_HS_EDONE};
    cb(conn_handle, &done, NULL, arg);
    return 0;
}

int ble_gattc_disc_all_dscs(uint16_t conn_handle, uint16_t chr_val_handle, uint16_t end_handle,
                            ble_gatt_dsc_fn *cb, void *arg) {
    (void)end_handle;
    if (!s_started || s_gattc_if == (esp_gatt_if_t)0xFF || !cb) return BLE_HS_EINVAL;
    esp_gattc_descr_elem_t elems[ADAPTER_MAX_CHR];
    uint16_t count = ADAPTER_MAX_CHR;
    const esp_gatt_status_t st = esp_ble_gattc_get_all_descr(
        s_gattc_if, conn_handle, chr_val_handle, elems, &count, 0);
    if (st == ESP_GATT_OK) {
        const struct ble_gatt_error err = {.att_handle = 0, .status = 0};
        for (uint16_t i = 0; i < count; ++i) {
            dsc_record(elems[i].handle);
            struct ble_gatt_dsc dsc;
            memset(&dsc, 0, sizeof(dsc));
            copy_uuid(&dsc.uuid, &elems[i].uuid);
            dsc.handle = elems[i].handle;
            if (cb(conn_handle, &err, chr_val_handle, &dsc, arg) != 0) break;
        }
    }
    const struct ble_gatt_error done = {.att_handle = 0, .status = BLE_HS_EDONE};
    cb(conn_handle, &done, chr_val_handle, NULL, arg);
    return 0;
}

int ble_gattc_read(uint16_t conn_handle, uint16_t chr_val_handle, ble_gatt_attr_fn *cb, void *arg) {
    if (!s_started || s_gattc_if == (esp_gatt_if_t)0xFF || !cb) return BLE_HS_EINVAL;
    s_read_cb = cb;
    s_read_arg = arg;
    const esp_err_t err = esp_ble_gattc_read_char(s_gattc_if, conn_handle, chr_val_handle,
                                                  ESP_GATT_AUTH_REQ_NONE);
    if (err != ESP_OK) {
        s_read_cb = NULL;
        s_read_arg = NULL;
        return BLE_HS_EINVAL;
    }
    return 0;
}

int ble_gattc_write_flat(uint16_t conn_handle, uint16_t chr_val_handle, const void *data,
                         uint16_t data_len, ble_gatt_attr_fn *cb, void *arg) {
    if (!s_started || s_gattc_if == (esp_gatt_if_t)0xFF || !data) return BLE_HS_EINVAL;
    s_write_cb = cb;
    s_write_arg = arg;
    esp_err_t err;
    if (dsc_is_descriptor(chr_val_handle)) {
        err = esp_ble_gattc_write_char_descr(s_gattc_if, conn_handle, chr_val_handle, data_len,
                                             (uint8_t *)data, ESP_GATT_WRITE_TYPE_RSP,
                                             ESP_GATT_AUTH_REQ_NONE);
    } else {
        err = esp_ble_gattc_write_char(s_gattc_if, conn_handle, chr_val_handle, data_len,
                                       (uint8_t *)data, ESP_GATT_WRITE_TYPE_RSP,
                                       ESP_GATT_AUTH_REQ_NONE);
    }
    if (err != ESP_OK) {
        s_write_cb = NULL;
        s_write_arg = NULL;
        return BLE_HS_EINVAL;
    }
    return 0;
}

int os_mbuf_copydata(const struct os_mbuf *om, int off, int len, void *dst) {
    if (!om || !dst || off < 0 || len < 0) return -1;
    if ((uint32_t)off + (uint32_t)len > om->len) return -1;
    memcpy(dst, om->data + off, (size_t)len);
    return 0;
}

#endif  // KIIKO_BOARD_METALIO47
