#!/usr/bin/env python3
"""串口日志抓取：读设备串口，原样写入日志文件并按行回显（带相对时间戳）。

中文：固件调试用。只读不写串口，不动 DTR/RTS，避免打开句柄时把板子复位。
English: Read-only serial capture for firmware debugging. DTR/RTS are left alone so
opening the port never resets the board.
"""
from __future__ import annotations

import argparse
import sys
import time

import serial


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", default="COM4")
    ap.add_argument("--baud", type=int, default=115200)
    ap.add_argument("--out", default="serial.log")
    ap.add_argument("--seconds", type=float, default=0.0, help="0 = 直到 Ctrl+C")
    args = ap.parse_args()

    ser = serial.Serial()
    ser.port = args.port
    ser.baudrate = args.baud
    ser.timeout = 0.2
    ser.dtr = False
    ser.rts = False
    ser.open()
    # 打开后再确认一次，某些驱动会在 open 时拉起控制线。
    ser.dtr = False
    ser.rts = False

    started = time.monotonic()
    last = started
    pending = b""
    print(f"[capture] {args.port} @ {args.baud} -> {args.out}", file=sys.stderr, flush=True)
    with open(args.out, "wb", buffering=0) as out:
        out.write(f"# capture start {time.strftime('%Y-%m-%d %H:%M:%S')}\n".encode())
        while True:
            if args.seconds and time.monotonic() - started >= args.seconds:
                break
            chunk = ser.read(4096)
            if not chunk:
                continue
            out.write(chunk)
            pending += chunk
            while b"\n" in pending:
                line, pending = pending.split(b"\n", 1)
                now = time.monotonic()
                stamp = now - last
                last = now
                text = line.decode("utf-8", "replace").rstrip("\r")
                print(f"[{now - started:8.2f} +{stamp:6.2f}] {text}", flush=True)
    ser.close()
    print("[capture] done", file=sys.stderr, flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
