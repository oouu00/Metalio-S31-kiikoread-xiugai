# SPDX-FileCopyrightText: 2026 zhuyue
# SPDX-License-Identifier: LicenseRef-Kiikoread-Personal-Study-1.0

import serial, time, sys, os

# 只开一次串口，立刻释放 DTR/RTS，避免重连时把芯片按在复位上；
# 全程不主动 pulse，纯粹旁观，用来判断「闪屏」是固件循环还是抓包工具造成的复位。
# Open once, release DTR/RTS immediately, never pulse: pure observation.
port = sys.argv[1] if len(sys.argv) > 1 else "COM4"
budget = float(sys.argv[2]) if len(sys.argv) > 2 else 60.0
out = sys.argv[3] if len(sys.argv) > 3 else r"C:\esp\metalio\serial_watch.txt"

ser = serial.Serial(port, 115200, timeout=0.2)
ser.dtr = False
ser.rts = False
time.sleep(0.05)

buf = b""
end = time.time() + budget
while time.time() < end:
    try:
        chunk = ser.read(4096)
        if chunk:
            buf += chunk
            sys.stdout.write(chunk.decode("utf-8", "replace"))
            sys.stdout.flush()
    except Exception as e:
        sys.stderr.write("read err %s\n" % e)
        break
try:
    ser.close()
except Exception:
    pass
open(out, "wb").write(buf)
git = "captured %d bytes\n" % len(buf)
sys.stderr.write(git)
