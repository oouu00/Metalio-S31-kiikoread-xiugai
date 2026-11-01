# SPDX-FileCopyrightText: 2026 zhuyue
# SPDX-License-Identifier: LicenseRef-Kiikoread-Personal-Study-1.0

import serial, time, sys, gc

PORT = sys.argv[2] if len(sys.argv) > 2 else "COM4"
BUDGET = float(sys.argv[1]) if len(sys.argv) > 1 else 60

# 比 read_serial.py 更强的重连：SD 挂载会让 USJ 重新枚举，此时旧句柄即使 close() 也可能
# 残留，直接 reopen 会被 Windows 以 PermissionError(13/22) 拒绝，日志会永久断流。
# Stronger reconnect than read_serial.py: the SD mount re-enumerates USJ; the stale handle may
# survive close() and Windows then refuses every reopen with PermissionError(13/22), which
# silently kills the capture. Drop the object, force GC, wait for the port to come back.
def reopen(deadline, verbose):
    attempt = 0
    while time.time() < deadline:
        try:
            s = serial.Serial(PORT, 115200, timeout=0.3, write_timeout=0.3)
            if verbose:
                sys.stderr.write("reopened after %d attempt(s)\n" % attempt)
            return s
        except Exception as e:
            attempt += 1
            if verbose and attempt <= 3:
                sys.stderr.write("open fail #%d: %r\n" % (attempt, e))
            time.sleep(0.5)
    return None


def drop(s):
    try:
        s.close()
    except Exception:
        pass
    del s
    gc.collect()
    time.sleep(1.0)


ser = reopen(time.time() + 5, True)
if not ser:
    sys.stderr.write("cannot open %s\n" % PORT)
    sys.exit(1)
try:
    ser.setDTR(False)
    ser.setRTS(True)
    time.sleep(0.1)
    ser.setRTS(False)
    ser.setDTR(True)
except Exception as e:
    sys.stderr.write("pulse err %s\n" % e)

buf = b""
deadline = time.time() + BUDGET
errs = 0
while time.time() < deadline:
    try:
        chunk = ser.read(8192)
        if chunk:
            buf += chunk
            sys.stdout.write(chunk.decode("utf-8", "replace"))
            sys.stdout.flush()
    except Exception as e:
        errs += 1
        sys.stderr.write("read err %r; reopening (%d)\n" % (e, errs))
        drop(ser)
        ser = reopen(deadline, True)
        if not ser:
            break
open(r"C:\esp\metalio\serial.log", "wb").write(buf)
sys.stderr.write("captured %d bytes, %d read errors\n" % (len(buf), errs))
