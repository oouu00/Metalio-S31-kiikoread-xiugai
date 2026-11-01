#!/usr/bin/env python3
# -*- coding: utf-8 -*-

# SPDX-FileCopyrightText: 2026 zhuyue
# SPDX-License-Identifier: LicenseRef-Kiikoread-Personal-Study-1.0

"""检查应用镜像头：magic / chip_id / 项目名 / 版本（OTA 会校验这几项）。"""
import struct
import sys

path = sys.argv[1] if len(sys.argv) > 1 else "build/Kiikoread_Metalio_S31.bin"
with open(path, "rb") as f:
    head = f.read(16)
    f.seek(0)
    blob = f.read(4096)

magic, segs, flash_mode, flash_freq = struct.unpack("<BBBB", head[0:4])
entry = struct.unpack("<I", head[4:8])[0]
chip_id = struct.unpack("<H", head[12:14])[0]
print("magic      = 0x%02x (expect 0xe9)" % magic)
print("segments   = %d" % segs)
print("entry      = 0x%08x" % entry)
print("chip_id    = %d (OTA expects 32 for Metalio S31)" % chip_id)

# esp_app_desc 位于第一个段开头：magic_word(4) secure_version(4) reserv1(8) version(32) project_name(32)
idx = blob.find(b"\xab\x21\xaa\x32")  # ESP_APP_DESC_MAGIC_WORD
if idx >= 0:
    desc = blob[idx:idx + 96]
    version = desc[16:48].split(b"\x00")[0].decode("utf-8", "replace")
    project = desc[48:80].split(b"\x00")[0].decode("utf-8", "replace")
    print("version    = %s" % version)
    print("project    = %s" % project)
else:
    print("app desc not found in first 4KB")
