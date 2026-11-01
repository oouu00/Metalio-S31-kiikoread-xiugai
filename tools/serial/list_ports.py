#!/usr/bin/env python3
# -*- coding: utf-8 -*-

# SPDX-FileCopyrightText: 2026 zhuyue
# SPDX-License-Identifier: LicenseRef-Kiikoread-Personal-Study-1.0

"""枚举串口"""
import serial.tools.list_ports as lp

for p in lp.comports():
    print(p.device, "|", p.description)
