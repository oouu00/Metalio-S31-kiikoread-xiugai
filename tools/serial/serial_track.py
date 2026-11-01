# SPDX-FileCopyrightText: 2026 zhuyue
# SPDX-License-Identifier: LicenseRef-Kiikoread-Personal-Study-1.0

import serial, time, sys

# 被动跟踪：打开串口后立刻把 DTR/RTS 拉成非激活，触发异常掉线时只做「重开」，
# 不再脉冲复位。SD 挂载会触发 USJ 恢复导致设备重枚举，必须能重连才能拿到整段开机。
# Passive tracker: deassert DTR/RTS right after open and only reopen on disconnect,
# never pulse a reset. USJ recovery on SD mount re-enumerates the device, so the
# tracker must reconnect to keep the whole boot in one buffer.
PORT = sys.argv[2] if len(sys.argv) > 2 else "COM4"
budget = float(sys.argv[1]) if len(sys.argv) > 1 else 45.0
out = sys.argv[3] if len(sys.argv) > 3 else r"C:\esp\Metalio-S31_0.3.3-rc107.1_src\serial_track.log"


def open_port():
    for _ in range(40):
        try:
            s = serial.Serial(PORT, 115200, timeout=0.2)
            try:
                s.setDTR(False)
                s.setRTS(False)
            except Exception:
                pass
            return s
        except Exception:
            time.sleep(0.25)
    return None


ser = open_port()
if ser is None:
    sys.stderr.write("cannot open %s\n" % PORT)
    sys.exit(1)

buf = bytearray()
end = time.time() + budget
reopens = 0
while time.time() < end:
    try:
        chunk = ser.read(4096)
    except Exception:
        try:
            ser.close()
        except Exception:
            pass
        ser = open_port()
        reopens += 1
        if ser is None:
            break
        continue
    if chunk:
        buf += chunk
        sys.stdout.write(chunk.decode("utf-8", "replace"))
        sys.stdout.flush()

try:
    ser.close()
except Exception:
    pass
open(out, "wb").write(bytes(buf))
sys.stderr.write("captured %d bytes, reopens=%d -> %s\n" % (len(buf), reopens, out))
