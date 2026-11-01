idf_component_register(
    SRCS simple_ftp_server.c
    INCLUDE_DIRS .
    REQUIRES esp_netif lwip freertos
)

# 第三方源码在 GCC 15 的 format-truncation 下有 4 处告警，均非溢出风险（截断即失败路径）；
# 仅对该文件关闭此项 -Werror，不动上游代码。/ Upstream code trips GCC 15 format-truncation in 4
# places, all truncation-to-failure paths rather than overflows; disable only this -Werror for it.
target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-error=format-truncation)
