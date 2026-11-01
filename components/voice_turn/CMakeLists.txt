# SPDX-FileCopyrightText: 2026 mindreset
# SPDX-License-Identifier: Apache-2.0
#
# 中文：语音翻页引擎（MFCC + DTW，无模型）。只依赖 bt_host 的麦克风接口。
# English: voice page-turn engine (MFCC + DTW, no model). Depends only on bt_host's mic API.
idf_component_register(
    SRCS "src/voice_turn.c"
    INCLUDE_DIRS "include"
    REQUIRES bt_host
    PRIV_REQUIRES nvs_flash esp_timer
)
