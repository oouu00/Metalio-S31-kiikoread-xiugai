# SPDX-FileCopyrightText: 2026 zhuyue
# SPDX-License-Identifier: LicenseRef-Kiikoread-Personal-Study-1.0

import serial, time, sys

# COM4 全程跟踪：SD 挂载触发 USJ 恢复时设备会短暂掉线重枚举，
# 旧版 10 秒重开窗口接不回来；这里在整段采集期内持续重连，日志拼接不丢。
# Track COM4 for the whole window: the SD mount USJ recovery drops the device
# briefly; the old 10s reopen budget missed it, so reconnect until the window ends.
def op(budget):
    end = time.time() + budget
    while time.time() < end:
        try:
            return serial.Serial("COM4", 115200, timeout=0.3)
        except Exception:
            time.sleep(0.3)
    return None

ser = op(5)
if not ser:
    sys.stderr.write("cannot open COM4\n")
    sys.exit(1)
try:
    ser.setDTR(False); ser.setRTS(True); time.sleep(0.1)
    ser.setRTS(False); ser.setDTR(True)
except Exception as e:
    sys.stderr.write("pulse err %s\n" % e)

buf = b""
start = time.time()
deadline = start + (float(sys.argv[1]) if len(sys.argv) > 1 else 40)
while time.time() < deadline:
    try:
        chunk = ser.read(8192)
        if chunk:
            buf += chunk
            sys.stdout.write(chunk.decode("utf-8", "replace"))
            sys.stdout.flush()
    except Exception as e:
        sys.stderr.write("read err %s; reopening\n" % e)
        try:
            ser.close()
        except Exception:
            pass
        ser = op(max(1.0, min(20.0, deadline - time.time())))
        if not ser:
            break
if ser:
    try:
        ser.close()
    except Exception:
        pass
open(r"C:\esp\metalio\serial.log", "wb").write(buf)
sys.stderr.write("captured %d bytes\n" % len(buf))
