# SPDX-FileCopyrightText: 2026 zhuyue
# SPDX-License-Identifier: LicenseRef-Kiikoread-Personal-Study-1.0

import serial, time, sys, re

# 被动监听：只打开串口读取，不脉冲 DTR/RTS。
# 上一版 read_serial.py 会 setRTS(True)/setDTR(True) 做硬复位，串口异常重连时
# 又会再复位一次，导致抓到的「开机」其实是脚本自己按的复位，无法判断真机开机行为。
# Passive listen: open the port and read only, never pulse DTR/RTS, so a capture
# reflects the device's own boot instead of a reset triggered by this script.
PORT = sys.argv[2] if len(sys.argv) > 2 else "COM4"
budget = float(sys.argv[1]) if len(sys.argv) > 1 else 25.0
out = sys.argv[3] if len(sys.argv) > 3 else r"C:\esp\Metalio-S31_0.3.3-rc107.1_src\serial_passive.log"

ser = serial.Serial(PORT, 115200, timeout=0.2)
try:
    ser.setDTR(False)
    ser.setRTS(False)
except Exception as e:
    sys.stderr.write("line state err %s\n" % e)

buf = bytearray()
end = time.time() + budget
while time.time() < end:
    try:
        chunk = ser.read(4096)
    except Exception as e:
        sys.stderr.write("read err %s\n" % e)
        break
    if chunk:
        buf += chunk
        sys.stdout.write(chunk.decode("utf-8", "replace"))
        sys.stdout.flush()
try:
    ser.close()
except Exception:
    pass
open(out, "wb").write(bytes(buf))
sys.stderr.write("captured %d bytes -> %s\n" % (len(buf), out))
