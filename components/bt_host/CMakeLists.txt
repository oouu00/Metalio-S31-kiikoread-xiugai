include("${CMAKE_CURRENT_LIST_DIR}/../../cmake/board.cmake")
# 蓝牙宿主：BLE HID 主机（翻页器）走片内 Bluedroid BLE-only；音乐/听书音频走板载
# 蓝牙音频模组（UART AT + I2S 从机）。仅 metalio47 有实现，其它板型编译为空单元。
# Bluetooth host: the BLE HID host (page turner) runs on the on-chip Bluedroid in BLE-only
# mode, while music/TTS audio goes through the board's Bluetooth audio module (UART AT +
# I2S slave). Only metalio47 has an implementation; other boards build empty units.
kiiko_board_sources(kiiko_selected_sources "${CMAKE_CURRENT_LIST_DIR}"
    "src/bt_host.c" "src/ble_hid_host.c")
set(bt_host_requires bt nvs_flash esp_timer)
if(KIIKO_BOARD_METALIO47)
    # 模组后端用 UART1 发 AT、用 I2S0 从机送 PCM，模组电源/功放走 FCA9555。
    # Read_Pico 已停维护（用户 2026-10-28）：pico 侧一切编译错误直接忽略。
    # The module backend drives AT over UART1 and PCM over I2S0 slave; the module's rails and
    # PA live on the FCA9555. Read_Pico is retired per user decision 2026-10-28: ignore all
    # pico-side compile errors.
    list(APPEND bt_host_requires esp_driver_uart esp_driver_i2s metalio_hw)
endif()
idf_component_register(
    SRCS ${kiiko_selected_sources}
    INCLUDE_DIRS "include"
    REQUIRES ${bt_host_requires}
)

# 诊断埋点跟 main 的 METALIO_BT_TRACE 共用同一个开关（命令行 -D 已进 cache 时这里的默认值
# 不生效）。关闭时 bt_log 编成空宏，格式串与 at_text 中转都不会留在交付固件里。
# The trace sink shares main's METALIO_BT_TRACE switch (a command-line -D already in the cache
# wins over this default). When off, bt_log becomes a no-op macro so neither the format strings
# nor the at_text relay reach the shipping image.
option(METALIO_BT_TRACE "Log Bluetooth/sleep events to the TF card trace file" OFF)
if(METALIO_BT_TRACE)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE METALIO_BT_TRACE=1)
endif()
