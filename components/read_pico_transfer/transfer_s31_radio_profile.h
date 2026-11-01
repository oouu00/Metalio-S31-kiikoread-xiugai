/* SPDX-License-Identifier: LicenseRef-Kiikoread-Personal-Study-1.0
 * 中文：按Metalio公开网络文档统一S31运行期策略；不存取网络凭据。
 * English: Apply the documented Metalio S31 runtime profile without touching credentials.
 * Reference: CloudZao/Metalio-E-INK4-Plus@ad8edf4 docs/ap-file-transfer.md (MIT).
 */
#pragma once
#include <stdbool.h>
#include "esp_wifi.h"
#define TRANSFER_S31_MAX_TX_POWER 48
static inline esp_err_t transfer_s31_radio_profile(wifi_interface_t interface) {
    esp_err_t err = esp_wifi_set_ps(WIFI_PS_NONE);
    if (err == ESP_OK) err = esp_wifi_set_protocol(interface,
        WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G | WIFI_PROTOCOL_11N);
    if (err == ESP_OK) err = esp_wifi_set_max_tx_power(TRANSFER_S31_MAX_TX_POWER);
    return err;
}

// SDK：AMPDU接收池应覆盖BA窗口；只给传书补齐（热点与STA都用满16槽，因为传书页
// 启动前已停音频/封面/头像，与阅读音乐互斥），日常STA/BLE保持SDK默认不增占用。
// SDK: cover the BA window with static RX buffers; bulk transfer gets the full16 slots
// (audio and cover workers are already stopped before the radio starts, so transfer
// never coexists with reading/music), while normal STA/BLE keep the SDK defaults.
static inline void transfer_s31_bulk_receive_config(wifi_init_config_t *init, bool bulk, int cap) {
    if (!init || !bulk || !init->ampdu_rx_enable || init->rx_ba_win <= 0 ||
        init->rx_ba_win > 16 || cap <= 0 || init->static_rx_buf_num >= init->rx_ba_win ||
        (init->dynamic_rx_buf_num && init->dynamic_rx_buf_num < cap)) return;
#ifdef CONFIG_SPIRAM_TRY_ALLOCATE_WIFI_LWIP
    // The S31 SDK recommends at least16 BA slots when Wi-Fi uses PSRAM. The caller may ask for
    // fewer to trade upload throughput for scarce internal RAM; never exceed16.
    if (cap > 16) cap = 16;
    init->rx_ba_win = cap;
    init->static_rx_buf_num = cap;
#else
    (void)cap;
#endif
}
