# The following lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.22)

include("${CMAKE_CURRENT_LIST_DIR}/cmake/board.cmake")
set(KIIKO_BOARD "${KIIKO_BOARD}" CACHE STRING "Firmware device: pico or metalio47")
set_property(CACHE KIIKO_BOARD PROPERTY STRINGS pico metalio47)
if(DEFINED IDF_TARGET AND NOT IDF_TARGET STREQUAL KIIKO_IDF_TARGET)
    message(FATAL_ERROR "${KIIKO_BOARD} requires ${KIIKO_IDF_TARGET}; use a separate build/config directory")
endif()
if(DEFINED ENV{IDF_TARGET} AND NOT "$ENV{IDF_TARGET}" STREQUAL "${KIIKO_IDF_TARGET}")
    message(FATAL_ERROR "IDF_TARGET environment conflicts with ${KIIKO_BOARD}")
endif()
set(IDF_TARGET "${KIIKO_IDF_TARGET}" CACHE STRING "Selected chip")
set(ENV{KIIKO_BOARD} "${KIIKO_BOARD}")
# 编译档：release（正式版，日志只留 ERROR）或 debug（默认 DEBUG，可开到逐模块）。
# 档位决定最后叠加的 sdkconfig 覆盖文件；它在板级 defaults 之后，所以能覆盖日志项。
# 命令行 -DMETALIO_PROFILE=debug 切换，build_metalio.py --profile 就是传这个。
# Build profile: release (ERROR-only logs) or debug (DEBUG by default). The profile overlay is
# appended after the board defaults, so it wins on log settings. Switch with
# -DMETALIO_PROFILE=debug, which is what build_metalio.py --profile forwards.
set(METALIO_PROFILE "release" CACHE STRING "Build profile: release or debug")
set_property(CACHE METALIO_PROFILE PROPERTY STRINGS release debug)
if(NOT METALIO_PROFILE STREQUAL "release" AND NOT METALIO_PROFILE STREQUAL "debug")
    message(FATAL_ERROR "Unknown METALIO_PROFILE=${METALIO_PROFILE}; choose release or debug")
endif()
if(NOT DEFINED SDKCONFIG_DEFAULTS)
    set(SDKCONFIG_DEFAULTS "${KIIKO_BOARD_ROOT}/sdkconfig.defaults")
endif()
list(APPEND SDKCONFIG_DEFAULTS "${KIIKO_BOARD_ROOT}/sdkconfig.${METALIO_PROFILE}")
message(STATUS "METALIO_PROFILE=${METALIO_PROFILE} (defaults overlay: sdkconfig.${METALIO_PROFILE})")
# SD 卡诊断日志（power_trace.log / usb-download.log / selftest.log 等）：仅 debug 档编入。
# release 档不定义 METALIO_DIAG_SD_LOGS，所有写卡调试分支编译成空操作，交付固件不写卡。
# SD-card diagnostic logs (power_trace.log / usb-download.log / selftest.log ...): compiled in
# only for the debug profile. release leaves METALIO_DIAG_SD_LOGS undefined, so every SD-write
# diagnostic branch becomes a no-op and the shipping firmware never touches the card.
if(METALIO_PROFILE STREQUAL "debug")
    add_compile_definitions(METALIO_DIAG_SD_LOGS=1)
endif()
# S31专用本地波形实验；板级与正式构建开关隔离。/ S31-only local waveform experiment, isolated from other boards and release defaults.
option(METALIO_DU14_EXPERIMENT "Local S31 vendor DU14 black/white experiment" OFF)
if(METALIO_DU14_EXPERIMENT AND NOT KIIKO_BOARD_METALIO47)
    message(FATAL_ERROR "DU14 experiment requires Metalio S31")
endif()
add_compile_definitions(KIIKO_BOARD_METALIO47=${KIIKO_BOARD_METALIO47})
# IDF v6.1 的 Bluedroid 内部代码（bta_dm_act.c 等）用 strncpy 截断 BD_NAME_LEN，
# gcc 12+ 报 -Werror=stringop-truncation；这是 IDF 已知问题，官方提示用编译选项抑制。
# 只降级这一个警告为非致命，不关闭整个 -Werror。
# IDF v6.1's own Bluedroid sources (bta_dm_act.c etc.) strncpy-truncate BD_NAME_LEN and
# gcc 12+ flags -Werror=stringop-truncation; known IDF issue, suppressed via compiler option.
# Downgrade only this warning; keep -Werror for everything else.
add_compile_options(-Wno-error=stringop-truncation)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
# "Trim" the build. Include the minimal set of components, main, and anything it depends on.
idf_build_set_property(MINIMAL_BUILD ON)

# 本地测试沿用当前正式版号；仅正式发布或明确的 OTA 验证指定新版本。
# Local tests keep the current release version; override only for publication or explicit OTA validation.
if(DEFINED ENV{KIIKO_PROJECT_VER} AND NOT "$ENV{KIIKO_PROJECT_VER}" STREQUAL "")
    set(PROJECT_VER "$ENV{KIIKO_PROJECT_VER}")
elseif(DEFINED ENV{PICO_PROJECT_VER} AND NOT "$ENV{PICO_PROJECT_VER}" STREQUAL "")
    set(PROJECT_VER "$ENV{PICO_PROJECT_VER}")
else()
    set(PROJECT_VER "${KIIKO_PROJECT_VER}")
endif()
project(${KIIKO_PROJECT_NAME})

# 固件描述时间统一为 UTC，与概览页标识一致；保留已有编译缓存启动器。
# Compile app-description timestamps in UTC to match Overview; preserve any compiler-cache launcher.
idf_component_get_property(app_desc_lib esp_app_format COMPONENT_LIB)
get_target_property(app_desc_launcher ${app_desc_lib} C_COMPILER_LAUNCHER)
if(NOT app_desc_launcher)
    set(app_desc_launcher "")
endif()
set(app_desc_utc_launcher "${CMAKE_COMMAND}" -E env TZ=UTC)
if(app_desc_launcher)
    list(APPEND app_desc_utc_launcher ${app_desc_launcher})
endif()
set_property(TARGET ${app_desc_lib} PROPERTY C_COMPILER_LAUNCHER "${app_desc_utc_launcher}")

# panic 包装器的链接选项在 main/CMakeLists.txt 里挂到 main 组件上：写在 project() 之后
# 用 idf_build_set_property 设全局链接选项已经太晚，链接器看不到 --wrap。
# The panic wrapper's link option hangs off the main component in main/CMakeLists.txt: setting a
# global link option after project() is too late for the linker to see --wrap.
