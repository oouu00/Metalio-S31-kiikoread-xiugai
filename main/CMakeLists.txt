include("${CMAKE_CURRENT_LIST_DIR}/../cmake/board.cmake")
if(KIIKO_BOARD_METALIO47)
set(main_requires
    kiiko_locale
    kiiko_board
    read_pico
    metalio_hw
    read_pico_transfer
    read_pico_search
    espressif__qrcode
    espressif__esp_jpeg
    esp_driver_jpeg
    read_pico_webp
    espressif__libjpeg-turbo
    # 渐进式 JPEG；基线仍走 esp_jpeg。/ Progressive JPEG; baseline stays on esp_jpeg.
    jpegdec
    espressif__libpng
    espressif__zlib
    espressif__esp_tinyusb
    # BLE HID 主机（蓝牙翻页器）。本仓库开了 MINIMAL_BUILD，组件不进依赖图的话连它的
    # Kconfig 都不会加载，sdkconfig.defaults 里的 CONFIG_BT_* 会被判为未知符号而丢弃。
    # BLE HID host (Bluetooth page-turner). This tree sets MINIMAL_BUILD, so a component
    # outside the dependency graph does not even load its Kconfig and the CONFIG_BT_*
    # entries in sdkconfig.defaults are dropped as unknown symbols.
    bt
    # 蓝牙翻页器本体。/ The page-turner itself.
    ble_page_turner
    # 双模蓝牙宿主：经典音频（A2DP source / AVRCP CT）与 BLE HID 共用一个协议栈。
    # Dual-mode Bluetooth host: classic audio and BLE HID share one stack.
    bt_host
    # 语音翻页：MFCC + DTW 固定词识别，麦克风走 bt_host 的 I2S 收通道；不用 esp-sr 模型。
    # Voice page-turn: MFCC + DTW fixed words over bt_host's I2S capture; no esp-sr model.
    voice_turn
    continuous_du
    e0470_epaper_waveform
    e0470_page_turn
    read_pico_pmu
    fatfs
    wear_levelling
    bootloader_support
    spi_flash
    heap
    nvs_flash
    esp_hw_support
    esp_rom
    esp_driver_gpio
    esp_driver_usb_serial_jtag
    esp_app_format
    app_update
    esp_http_client
    mbedtls
    espressif__cjson
    # 音乐解码（Simple Decoder：MP3/AAC/FLAC/WAV）。进依赖图才能吃到它的 Kconfig。
    # Music decoding (Simple Decoder: MP3/AAC/FLAC/WAV); in the graph so its Kconfig loads.
    espressif__esp_audio_codec
)
kiiko_board_sources(kiiko_selected_sources "${CMAKE_CURRENT_LIST_DIR}" "app_main.c"
        "boot_state.c"
        "apps/app_boot_recovery.c"
        "ota_update.c"
        "ota_online.c"
        "ota_release.c"
        "display.c"
        "anti_fade.c"
        "display_main_waveform.c"
        "display_reader_waveform.c"
        "metalio_text_ripple.c"
        "sleep.c"
        "time_service.c"
        "lock_pin.c"
        "lock_pin_flow.c"
        "lock_screen.c"
        "settings.c"
        "setup_state.c"
        "apps/app_onboarding.c"
        "factory_reset.c"
        "app/app_loop.c"
        "app/sd_recovery.c"
        "app/crash_note.c"
        "app/turn_trace.c"
        # 蓝牙/睡眠诊断：关键事件落到 TF 卡 /sdcard/.metalio_s31/bt_trace.log。
        # Bluetooth/sleep diagnostics: key events parked in /sdcard/.metalio_s31/bt_trace.log.
        "app/bt_trace.c"
        "app/app_registry.c"
        # 无人值守自测（KIIKO_AUTO_TEST 空实现时也编进 metalio47，交付不启用）。
        # Unattended self-test (compiled even when KIIKO_AUTO_TEST is off; delivery keeps it inert).
        "app/app_auto_test.c"
        "ui/ui_kit.c"
        "ui/ui_text_edit.c"
        "ui/ui_text_input.c"
        "ui/ui_ime.c"
        "ui/ui_keyboard.c"
        "ui/ui_gesture.c"
        "ui/ui_glass.c"
        "ui/ui_pinpad.c"
        "ui/ui_power_dialog.c"
        "ui/ui_quick_menu.c"
        "ui/ui_ai_menu.c"
        "ui/ui_lock_feedback.c"
        "ui/ui_quick_menu_input.c"
        "ui/ui_click_feedback.c"
        "ui/ui_wifi_qr.c"
        "ui/datamatrix.c"
        "ui/ui_menu.c"
        "ui/ui_nav.c"
        "ui/ui_home_widget.c"
        "ui/ui_home_clock.c"
        "ui/ui_image_dither.c"
        "ui/ui_wallpaper.c"
        "ui/ui_avatar.c"
        "ui/ui_reader_background.c"
        "apps/app_book.c"
        "apps/book_toc.c"
        "apps/app_dashboard.c"
        "apps/app_files.c"
        "apps/file_tree.c"
        "apps/file_search.c"
        "apps/app_device_settings.c"
        "apps/app_image.c"
        "apps/app_transfer.c"
        "apps/usb_storage.c"
        # 音乐播放器、标签解析、完整播放页。/ The music player, tag parsing and the player page.
        "music/music_player.c"
        "music/music_tags.c"
        "music/music_lyrics.c"
        "apps/app_music.c"
        "book/book_store.c"
        "book/book_progress.c"
        "book/book_history_backup.c"
        "book/book_title.c"
        "book/book_added.c"
        "book/book_ticket.c"
        "book/book_excerpt.c"
        "book/book_lock_collage.c"
        "book/book_lock_bookmark.c"
        "book/book_source.c"
        "book/book_index_cache.c"
        "book/book_shelf_cache.c"
        "book/book_txt.c"
        "book/book_epub.c"
        "book/book_cover.c"
        "book/metalio_jpeg.c"
        "book/book_cover_jobs.c"
        "book/metalio_cover_cache.c"
        "book/reader_page_cache.c"
        "book/book_image_header.c"
        "book/book_cover_auto.c"
        "book/zip_reader.c"
        "book/html_text.c"
        "book/book_layout.c"
        "book/gbk.c"
        "font/ttf_font.c"
        "font/app_font_context.c"
        "font/online_font.c"
        "font/online_font_store.c"
        "ui/ui_online_fonts.c"
        "font/ui_font.c"
        "font/ui_hanzi.c"
        "factory/pmu_selftest.c"
        "factory/vcom_setup.c"
        "factory/debug_panel.c")
idf_component_register(
    LDFRAGMENTS "metalio_crash.lf"
    SRCS ${kiiko_selected_sources}
    REQUIRES ${main_requires}
    INCLUDE_DIRS "." "app" "apps" "font" "ui" "factory" "book" "music"
    EMBED_FILES
        "assets/lock_4bpp.bin"
        "assets/builtin.ttf"
        "assets/ui-hanzi.bin"
        "assets/display-test.png"
)

# 包住 IDF 的 panic 处理器：崩溃现场在重启前落进 RTC，没有串口时那是唯一的现场。
# 链接选项挂在 main 上，INTERFACE 会带到最终链接；写在 project() 之后的全局属性则太晚。
# Wrap the IDF panic handler so the crash scene reaches RTC before a restart; without a console
# that is the only place it survives. The option hangs off main, whose INTERFACE carries it into
# the final link; a global property set after project() is too late for the linker to see it.
target_link_options(${COMPONENT_LIB} INTERFACE "-Wl,--wrap=esp_panic_handler")

# 分步翻页计时：结果落到 TF 卡 /sdcard/turn_trace.csv。默认关，只有 debug 档或显式
# -DMETALIO_TURN_TRACE=ON 才编进去；正式版不该把写 CSV 的调试代码带上。
# Per-stage page-turn tracing writes /sdcard/turn_trace.csv on the TF card. Off by default:
# only the debug profile or an explicit -DMETALIO_TURN_TRACE=ON compiles it in.
option(METALIO_TURN_TRACE "Log per-stage page-turn timing to /sdcard/turn_trace.csv" OFF)
if(METALIO_TURN_TRACE)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE METALIO_TURN_TRACE=1)
endif()

# 蓝牙/睡眠诊断：bt_host 的 AT 收发、模式切换、链路、喂帧统计与播放器/睡眠节点，
# 全部排进 /sdcard/.metalio_s31/bt_trace.log（超 192KB 轮转一份 .1）。没有串口时靠它定位
# 「显示已连接却没声音」。默认关：正式版不带写卡调试代码，debug 档或显式
# -DMETALIO_BT_TRACE=ON 才编进去。
# Bluetooth/sleep diagnostics: AT traffic, mode switches, link state, feed stats and the
# player/sleep milestones land in /sdcard/.metalio_s31/bt_trace.log (one .1 rotation above
# 192KB). It is how "connected but silent" gets diagnosed without a console. Off by default;
# only the debug profile or an explicit -DMETALIO_BT_TRACE=ON compiles the writer in.
option(METALIO_BT_TRACE "Log Bluetooth/sleep events to /sdcard/.metalio_s31/bt_trace.log" OFF)
if(METALIO_BT_TRACE)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE METALIO_BT_TRACE=1)
endif()

# 本地显式测时构建才接收 USB 测试命令；交付构建默认关闭。
# Only explicit local benchmark builds accept USB probe commands; delivery defaults off.
option(METALIO_LOCAL_LATENCY "Local USB latency probe; never enable for delivery" OFF)
if(METALIO_LOCAL_LATENCY)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE KIIKO_LOCAL_LATENCY=1)
endif()
option(METALIO_LOCAL_TRANSFER_START "Unattended local WiFi transfer test; never deliver" OFF)
if(METALIO_LOCAL_TRANSFER_START)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE KIIKO_LOCAL_TRANSFER_START=1)
endif()
# 无人值守自测：两次重启间在「起热点」与「连蓝牙耳机播一首」之间轮换，进度存 NVS；
# 只接串口就能远程看结果。仅本地测试，交付固件不要开。
# Unattended self-test: alternate hotspot and Bluetooth-earphone phases across reboots with
# progress in NVS, so a console log alone shows the result. Local tests only; never deliver.
option(METALIO_AUTO_TEST "Unattended hotspot/Bluetooth self-test; never deliver" OFF)
if(METALIO_AUTO_TEST)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE KIIKO_AUTO_TEST=1)
endif()

# 仅显式本地实验包启用；普通构建不继承缓存中的实验值。/ Explicit local packages only; normal builds must not inherit cached experiment flags.
option(METALIO_MAIN_CLEAN_EXPERIMENT "Local S31 stronger root-switch white erasure" OFF)
if(METALIO_MAIN_CLEAN_EXPERIMENT)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE KIIKO_S31_MAIN_CLEAN=1)
endif()
if(METALIO_DU14_EXPERIMENT)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE KIIKO_S31_DU14=1)
endif()
option(METALIO_FAST_READER_PANELS "Local four-gray font/reading panels" OFF)
if(METALIO_FAST_READER_PANELS)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE KIIKO_S31_FAST_READER_PANELS=1)
endif()
# 用户2026-10-29确认：普通翻页固定走 GL37 面板表（37+6+3相），不再回退GL48。
# 显式 -DMETALIO_READER_PANEL37_EXPERIMENT=OFF 才恢复完整GL48。
# User decision 2026-10-29: ordinary turns use the GL37 panel table (37+6+3); full GL48 only with an explicit OFF.
option(METALIO_READER_PANEL37_EXPERIMENT "S31 vendor-derived GL37 reader panels (default)" ON)
if(METALIO_READER_PANEL37_EXPERIMENT)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE KIIKO_S31_READER_PANEL37=1)
endif()
option(METALIO_SHELF37_EXPERIMENT "Local vendor-derived S31 GL37 shelf art/chrome" OFF)
if(METALIO_SHELF37_EXPERIMENT)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE KIIKO_S31_SHELF37=1)
endif()
# 用户 2026-10-28 决定：普通文字翻页固定走 22 相四级灰，不再使用 GL48。
# 只有显式 -DMETALIO_TEXT_RIPPLE_EXPERIMENT=OFF 才回到 48 相；速读黑白仍是原厂 DU20。
# User decision 2026-10-28: ordinary text turns use the 22-phase four-level table, not GL48.
# Pass -DMETALIO_TEXT_RIPPLE_EXPERIMENT=OFF to fall back; fast BW keeps native DU20.
option(METALIO_TEXT_RIPPLE_EXPERIMENT "S31 four-level 22-phase reader text turn (default)" ON)
if(METALIO_TEXT_RIPPLE_EXPERIMENT)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE KIIKO_S31_TEXT_RIPPLE=1)
endif()

else()
# ---------------------------------------------------------------------------
# Read_Pico 分支：板型已停维护（用户 2026-10-28 决定）。此分支的编译/链接错误一律忽略，
# 不排查、不修、不为它做适配；板型保留不删，共享代码只需保证 metalio47 通过。
# Read_Pico branch: retired per user decision 2026-10-28. Ignore every build error
# here — do not debug, fix, or adapt for it; the board stays and shared code only
# has to keep metalio47 green.
# ---------------------------------------------------------------------------
set(main_requires
    kiiko_locale
    kiiko_board
    read_pico
    read_pico_transfer
    read_pico_search
    espressif__qrcode
    espressif__esp_jpeg
    # 渐进式 JPEG；基线仍走 esp_jpeg。/ Progressive JPEG; baseline stays on esp_jpeg.
    jpegdec
    defensiblelogic__lowzag
    # WebP 插图与封面的解码器（libwebp 解码子集）。/ WebP decoder subset for illustrations and covers.
    read_pico_webp
    espressif__libpng
    espressif__zlib
    espressif__esp_tinyusb
    # BLE HID 主机（蓝牙翻页器）。本仓库开了 MINIMAL_BUILD，组件不进依赖图的话连它的
    # Kconfig 都不会加载，sdkconfig.defaults 里的 CONFIG_BT_* 会被判为未知符号而丢弃。
    # BLE HID host (Bluetooth page-turner). This tree sets MINIMAL_BUILD, so a component
    # outside the dependency graph does not even load its Kconfig and the CONFIG_BT_*
    # entries in sdkconfig.defaults are dropped as unknown symbols.
    bt
    # 蓝牙翻页器本体。/ The page-turner itself.
    ble_page_turner
    # 双模蓝牙宿主：经典音频（A2DP source / AVRCP CT）与 BLE HID 共用一个协议栈。
    # Dual-mode Bluetooth host: classic audio and BLE HID share one stack.
    bt_host
    # 语音翻页：MFCC + DTW 固定词识别，麦克风走 bt_host 的 I2S 收通道；不用 esp-sr 模型。
    # Voice page-turn: MFCC + DTW fixed words over bt_host's I2S capture; no esp-sr model.
    voice_turn
    continuous_du
    e0470_epaper_waveform
    e0470_page_turn
    read_pico_pmu
    fatfs
    wear_levelling
    bootloader_support
    spi_flash
    heap
    nvs_flash
    esp_hw_support
    esp_rom
    esp_driver_gpio
    esp_driver_usb_serial_jtag
    esp_app_format
    app_update
    esp_http_client
    mbedtls
    espressif__cjson
)
kiiko_board_sources(kiiko_selected_sources "${CMAKE_CURRENT_LIST_DIR}" "app_main.c"
        "boot_state.c"
        "apps/app_boot_recovery.c"
        "ota_update.c"
        "ota_online.c"
        "ota_release.c"
        "display.c"
        "display_main_waveform.c"
        "display_reader_waveform.c"
        "sleep.c"
        "time_service.c"
        "lock_pin.c"
        "lock_pin_flow.c"
        "lock_screen.c"
        "settings.c"
        "setup_state.c"
        "apps/app_onboarding.c"
        "factory_reset.c"
        "app/app_loop.c"
        "app/crash_note.c"
        "app/turn_trace.c"
        "app/app_registry.c"
        "ui/ui_kit.c"
        "ui/ui_text_edit.c"
        "ui/ui_text_input.c"
        "ui/ui_ime.c"
        "ui/ui_keyboard.c"
        "ui/ui_gesture.c"
        "ui/ui_glass.c"
        "ui/ui_pinpad.c"
        "ui/ui_power_dialog.c"
        "ui/ui_quick_menu.c"
        "ui/ui_lock_feedback.c"
        "ui/ui_quick_menu_input.c"
        "ui/ui_click_feedback.c"
        "ui/ui_wifi_qr.c"
        "ui/datamatrix.c"
        "ui/ui_menu.c"
        "ui/ui_nav.c"
        "ui/ui_home_widget.c"
        "ui/ui_home_clock.c"
        "ui/ui_image_dither.c"
        "ui/ui_wallpaper.c"
        "ui/ui_avatar.c"
        "ui/ui_reader_background.c"
        "apps/app_book.c"
        "apps/book_toc.c"
        "apps/app_dashboard.c"
        "apps/app_files.c"
        "apps/file_tree.c"
        "apps/file_search.c"
        "apps/app_device_settings.c"
        "apps/app_image.c"
        "apps/app_transfer.c"
        "apps/usb_storage.c"
        "book/book_store.c"
        "book/book_progress.c"
        "book/book_history_backup.c"
        "book/book_title.c"
        "book/book_added.c"
        "book/book_ticket.c"
        "book/book_excerpt.c"
        "book/book_lock_collage.c"
        "book/book_lock_bookmark.c"
        "book/book_source.c"
        "book/book_index_cache.c"
        "book/book_shelf_cache.c"
        "book/book_txt.c"
        "book/book_epub.c"
        "book/book_cover.c"
        "book/book_cover_jobs.c"
        "book/book_image_header.c"
        "book/book_cover_auto.c"
        "book/zip_reader.c"
        "book/html_text.c"
        "book/book_layout.c"
        "book/gbk.c"
        "font/ttf_font.c"
        "font/online_font.c"
        "font/online_font_store.c"
        "ui/ui_online_fonts.c"
        "font/app_font_context.c"
        "font/ui_font.c"
        "font/ui_hanzi.c"
        "factory/pmu_selftest.c"
        "factory/vcom_setup.c")
idf_component_register(
    SRCS ${kiiko_selected_sources}
    REQUIRES ${main_requires}
    INCLUDE_DIRS "." "app" "apps" "font" "ui" "factory" "book"
    EMBED_FILES
        "assets/loading_4bpp.bin"
        "assets/builtin.ttf"
        "assets/ui-hanzi.bin"
        "assets/display-test.png"
)

# 包住 IDF 的 panic 处理器：崩溃现场在重启前落进 RTC，没有串口时那是唯一的现场。
# 链接选项挂在 main 上，INTERFACE 会带到最终链接；写在 project() 之后的全局属性则太晚。
# Wrap the IDF panic handler so the crash scene reaches RTC before a restart; without a console
# that is the only place it survives. The option hangs off main, whose INTERFACE carries it into
# the final link; a global property set after project() is too late for the linker to see it.
target_link_options(${COMPONENT_LIB} INTERFACE "-Wl,--wrap=esp_panic_handler")

# 本地 USB 测时入口，普通构建不包含。/ Local USB timing probe, omitted from ordinary product builds.
if("$ENV{KIIKO_LOCAL_LATENCY}" STREQUAL "1")
    target_compile_definitions(${COMPONENT_LIB} PRIVATE KIIKO_LOCAL_LATENCY=1)
endif()

# 分步翻页计时：默认关，只有 debug 档或显式 -DMETALIO_TURN_TRACE=ON 才编进去。
# Per-stage page-turn tracing: off by default; the debug profile or an explicit
# -DMETALIO_TURN_TRACE=ON enables it.
option(METALIO_TURN_TRACE "Log per-stage page-turn timing to /sdcard/turn_trace.csv" OFF)
if(METALIO_TURN_TRACE)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE METALIO_TURN_TRACE=1)
endif()

endif()

if(NOT KIIKO_BOARD_METALIO47)
    option(PICO_MAIN_CLEAN_EXPERIMENT "Local Pico stronger root-switch white erasure" OFF)
    if(PICO_MAIN_CLEAN_EXPERIMENT)
        target_compile_definitions(${COMPONENT_LIB} PRIVATE KIIKO_PICO_MAIN_CLEAN=1)
    endif()
endif()

target_compile_definitions(${COMPONENT_LIB} PRIVATE KIIKO_UI_LOCALIZATION=1)
