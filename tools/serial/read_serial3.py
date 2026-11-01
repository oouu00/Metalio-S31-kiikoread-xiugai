# SPDX-FileCopyrightText: 2026 zhuyue
# SPDX-License-Identifier: LicenseRef-Kiikoread-Personal-Study-1.0

import serial, time, sys, gc

# 被动监听 + 强重连：打开串口时就把 DTR/RTS 置低（构造参数里给，避免 open 瞬间的默认
# 高电平把设备按进复位），读异常时丢句柄重开。SD 挂载 / 轻睡唤醒会让原生 USB CDC 重新
# 枚举，旧句柄 thereafter 报 PermissionError(13)，不重连日志就永久断流。
# Passive listen with strong reconnect: DTR/RTS are cleared in the constructor so the default
# high levels at open time cannot reset the board; on a read error the handle is dropped and
# reopened. SD mount / light-sleep wake re-enumerates the native USB CDC and a stale handle
# then raises PermissionError(13) forever.
PORT = sys.argv[2] if len(sys.argv) > 2 else "COM4"
BUDGET = float(sys.argv[1]) if len(sys.argv) > 1 else 900.0
OUT = sys.argv[3] if len(sys.argv) > 3 else r"C:\esp\metalio\passive_out.log"


def reopen(deadline):
    while time.time() < deadline:
        try:
            s = serial.Serial(PORT, 115200, timeout=0.2)
            # 这个 pyserial 版本不接受 dtr/rts 构造参数，只能开完再清：保持两项都为低，
            # 不复制 read_serial.py 里那套 RTS→DTR 的脉冲（那才是把板子按进复位的动作）。
            # This pyserial build rejects the dtr/rts kwargs, so clear them right after open.
            # Both stay low: no RTS→DTR pulse like read_serial.py, which is what resets the board.
            try:
                s.setDTR(False)
                s.setRTS(False)
            except Exception:
                pass
            return s
        except Exception:
            time.sleep(0.5)
    return None


def drop(s):
    try:
        s.close()
    except Exception:
        pass
    del s
    gc.collect()
    time.sleep(0.5)


ser = reopen(time.time() + 20)
if not ser:
    sys.stderr.write("cannot open %s\n" % PORT)
    sys.exit(1)

buf = bytearray()
errs = 0
deadline = time.time() + BUDGET
while time.time() < deadline:
    try:
        chunk = ser.read(8192)
    except Exception as e:
        errs += 1
        sys.stderr.write("read err %r; reopening (%d)\n" % (e, errs))
        drop(ser)
        ser = reopen(deadline)
        if not ser:
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
open(OUT, "wb").write(bytes(buf))
sys.stderr.write("captured %d bytes, %d read errors\n" % (len(buf), errs))
