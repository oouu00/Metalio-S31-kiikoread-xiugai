/*
 * SPDX-FileCopyrightText: 2026 mindreset
 * SPDX-License-Identifier: Apache-2.0
 *
 * 中文：bt_host 与 BLE 适配层之间的内部接口。不对外暴露。
 * English: internal interface between bt_host and the BLE adapter. Not public.
 */
#pragma once

#include <stdbool.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// 协议栈起来后注册 BLE GAP/GATTC 回调并建立事件队列；可重复调用。
// Register the BLE GAP/GATTC callbacks and create the event queue once the stack is up.
esp_err_t bt_ble_host_start(void);
// 注销 BLE 应用、释放事件队列；之后再 start 会重新注册。
// Unregister the BLE app and free the queue; a later start registers again.
void bt_ble_host_stop(void);
bool bt_ble_host_running(void);

#ifdef __cplusplus
}
#endif
