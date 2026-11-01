#!/usr/bin/env python3

# SPDX-FileCopyrightText: 2026 zhuyue
# SPDX-License-Identifier: LicenseRef-Kiikoread-Personal-Study-1.0

# -*- coding: utf-8 -*-
"""
Metalio-S31 (esp32s31) 一键线刷 —— 刷「全部分区」（USB/UART 直刷，非 OTA）。

刷什么（按 boards/metalio47/partitions.csv 实际偏移，不写死）：
    bootloader                      @ 0x2000   (bootloader/bootloader.bin)
    partition-table                 @ 0x8000   (partition_table/partition-table.bin)
    ota_0  (app 固件)               @ 0x10000  (Kiikoread_Metalio_S31.bin)
    otadata (OTA 状态初始化)         @ 0xfe0000 (ota_data_initial.bin)
    phy_init (RF 校准数据)           @ 0xfe2000 (phy_init.bin，构建产物有就刷)

默认行为：
    1) 整片擦除 flash（erase_flash）—— 这是真正的「全部分区」刷写，nvs/storage/
       coredump/ota_1 残留数据一并清空，固件首次启动会重新初始化它们。
    2) 写入上面 5 个有镜像的分区。
    nvs / storage(FAT) / coredump / ota_1 没有独立镜像，留给固件首启格式化，符合预期。

用法：
    双击本文件                 -> 自动探测串口、整片擦除、刷全部分区
    python flash_partitions.py  -> 同上，命令行下运行
    python flash_partitions.py --port COM4        -> 指定串口
    python flash_partitions.py --no-erase         -> 只覆盖写镜像，保留 nvs/已刷分区
    python flash_partitions.py --baud 921600      -> 指定波特率（默认 921600）
    python flash_partitions.py --combined         -> 刷编译生成的单文件合并镜像到 0x0
    python flash_partitions.py --combined 路径     -> 刷指定的单文件合并镜像到 0x0
    python flash_partitions.py --app              -> 只刷 app 主分区（日常改代码用这条，
                                                     设置与进度全部保留）
    （单文件镜像由编译自动生成：build/Metalio-S31-combined.bin，等价于整片擦除后
      同时写 bootloader/分区表/app/otadata，但只需烧一个文件，适合手机 ESP 刷机 App）

依赖：本机 ESP-IDF v6.1 的 venv（自动探测 ~/.espressif 下的 python_env），
      通过 `python -m esptool` 调用，无需单独安装 esptool。
"""
import os
import sys
import argparse
import glob
import subprocess

PROJ = os.path.dirname(os.path.abspath(__file__))
BUILD = os.path.join(PROJ, "build")
CHIP = "esp32s31"
FLASH_MODE = "dio"
FLASH_FREQ = "80m"
FLASH_SIZE = "16MB"
APP_BIN = "Kiikoread_Metalio_S31.bin"


def default_app_bin() -> str:
    """--app 不带路径时的默认镜像：build-debug 与 build 里取构建时间最新的一个。"""
    cands = [os.path.join(PROJ, "build-debug", APP_BIN), os.path.join(BUILD, APP_BIN)]
    have = [p for p in cands if os.path.isfile(p)]
    if not have:
        return cands[0]
    return max(have, key=os.path.getmtime)



# ---------------------------------------------------------------------------
# IDF / venv 探测（与 build_metalio.py 一致，不重复造轮子）
# ---------------------------------------------------------------------------
def _idf_path() -> str:
    env = os.environ.get("IDF_PATH")
    if env and os.path.isfile(os.path.join(env, "tools", "idf.py")):
        return env
    for c in (
        r"C:\esp\idf-v6.1",
        r"C:\Users\zhuyue\Desktop\gujian (2)\esp-idf-v6.1",
        os.path.join(os.path.expanduser("~"), "esp", "esp-idf-v6.1"),
        r"C:\Espressif\esp-idf-v6.1",
    ):
        if os.path.isfile(os.path.join(c, "tools", "idf.py")):
            return c
    return r"C:\esp\idf-v6.1"


def _venv_py() -> str:
    base = os.path.join(os.path.expanduser("~"), ".espressif", "python_env")
    for m in glob.glob(os.path.join(base, "idf6.1_*_env")):
        p = os.path.join(m, "Scripts", "python.exe")
        if os.path.isfile(p):
            return p
    return os.path.join(base, "idf6.1_py3.13_env", "Scripts", "python.exe")


IDF_PATH = _idf_path()
VENV_PY = _venv_py()


# ---------------------------------------------------------------------------
# 分区偏移：优先从 partitions.csv 解析，失败回退到这套已知值
# ---------------------------------------------------------------------------
DEFAULT_OFFSETS = {
    "bootloader": 0x2000,          # boards/metalio47/board.json: bootloader_offset = 8192
    "partition_table": 0x8000,     # CONFIG_PARTITION_TABLE_OFFSET
    "ota_0": 0x10000,
    "otadata": 0xFE0000,
    "phy_init": 0xFE2000,
}


def parse_partitions_csv():
    """读 boards/metalio47/partitions.csv，返回 {name: offset_int}。

    解析不到（文件缺失/格式异常）时返回 None，调用方回退到 DEFAULT_OFFSETS。
    """
    csv_path = os.path.join(PROJ, "boards", "metalio47", "partitions.csv")
    if not os.path.isfile(csv_path):
        print(f"[warn] 找不到分区表 {csv_path}，使用内置默认偏移")
        return None
    offs = {}
    try:
        with open(csv_path, encoding="utf-8") as fp:
            for line in fp:
                line = line.strip()
                if not line or line.startswith("#"):
                    continue
                parts = [p.strip() for p in line.split(",")]
                if len(parts) < 4:
                    continue
                name = parts[0]
                off_str = parts[3]
                try:
                    off = int(off_str, 16) if off_str.lower().startswith("0x") else int(off_str)
                except ValueError:
                    continue
                offs[name] = off
    except Exception as e:
        print(f"[warn] 解析分区表失败：{e}，使用内置默认偏移")
        return None
    return offs or None


def offsets():
    csv = parse_partitions_csv()
    if csv:
        merged = dict(DEFAULT_OFFSETS)
        # 用 csv 里实际读到的覆盖默认值
        for k in ("ota_0", "otadata", "phy_init"):
            if k in csv:
                merged[k] = csv[k]
        merged["bootloader"] = DEFAULT_OFFSETS["bootloader"]
        merged["partition_table"] = DEFAULT_OFFSETS["partition_table"]
        return merged, True
    return DEFAULT_OFFSETS, False


# ---------------------------------------------------------------------------
# 串口探测
# ---------------------------------------------------------------------------
def detect_ports():
    try:
        import serial.tools.list_ports
        ports = [p.device for p in serial.tools.list_ports.comports()]
        return ports
    except Exception:
        return []


def pick_port(arg_port):
    if arg_port:
        return arg_port
    ports = detect_ports()
    if not ports:
        return "COM4"  # 用户环境常用；探测不到时给个默认
    if len(ports) == 1:
        return ports[0]
    print("发现多个串口，请选择：")
    for i, p in enumerate(ports, 1):
        print(f"  {i}) {p}")
    try:
        sel = input(f"输入序号或串口名 [默认 {ports[0]}]：").strip()
    except (EOFError, KeyboardInterrupt):
        sel = ""
    if not sel:
        return ports[0]
    if sel.isdigit() and 1 <= int(sel) <= len(ports):
        return ports[int(sel) - 1]
    return sel


# ---------------------------------------------------------------------------
# 执行
# ---------------------------------------------------------------------------
def run(cmd):
    print(">>> " + " ".join(cmd))
    proc = subprocess.Popen(
        cmd, cwd=BUILD, env=os.environ,
        stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
        text=True, encoding="utf-8", errors="replace", bufsize=1,
    )
    assert proc.stdout is not None
    for line in proc.stdout:
        sys.stdout.write(line)
    return proc.wait()


def ensure_files(offs):
    """校验要刷的镜像都存在，缺哪个列出来。"""
    plan = [
        ("bootloader", os.path.join(BUILD, "bootloader", "bootloader.bin")),
        ("partition-table", os.path.join(BUILD, "partition_table", "partition-table.bin")),
        ("ota_0 (app)", os.path.join(BUILD, APP_BIN)),
        ("otadata", os.path.join(BUILD, "ota_data_initial.bin")),
        ("phy_init（可选）", os.path.join(BUILD, "phy_init.bin")),
    ]
    missing = []
    for label, path in plan:
        if not os.path.isfile(path):
            # phy_init 缺失不致命，固件会自己初始化
            if label.startswith("phy_init"):
                print(f"[i] 跳过 {label}：未生成 {path}（固件首启会写默认 RF 校准数据）")
                continue
            missing.append((label, path))
    if missing:
        print("[!!] 以下必需镜像缺失，请先编译（双击 双击编译.py 选 1）：")
        for label, path in missing:
            print(f"      - {label}: {path}")
        return False
    return True


def main():
    ap = argparse.ArgumentParser(description="Metalio-S31 一键线刷（全部分区）")
    ap.add_argument("--port", default=None, help="串口号，如 COM4（默认自动探测）")
    ap.add_argument("--baud", default="921600", help="波特率（默认 921600）")
    ap.add_argument("--no-erase", action="store_true",
                    help="跳过整片擦除，只覆盖写镜像（保留 nvs 等已刷数据）")
    ap.add_argument("--combined", nargs="?", const="__DEFAULT__", default=None,
                    help="刷单个合并镜像到 0x0（编译生成的 build/Metalio-S31-combined.bin，"
                         "或显式指定路径）。等价于整片擦除后写 bootloader/分区表/app/otadata，"
                         "但只用一个文件，适合手机 ESP 刷机 App。不带值用默认路径。")
    ap.add_argument("--app", nargs="?", const="__DEFAULT__", default=None,
                    help="只刷 app 主分区（约 0x10000），不碰 bootloader/分区表/otadata，"
                         "nvs/storage 里的设置与阅读进度全部保留。不带值自动取最新的 "
                         "build-debug 或 build 里的 Kiikoread_Metalio_S31.bin。")
    args = ap.parse_args()

    print("=" * 60)
    print("  Metalio-S31 一键线刷  ·  刷全部分区")
    print("=" * 60)
    print(f"  IDF : {IDF_PATH}")
    print(f"  venv: {VENV_PY}  ({'存在' if os.path.isfile(VENV_PY) else '!! 不存在'})")
    if not os.path.isfile(VENV_PY):
        print("[!!] 找不到 ESP-IDF v6.1 的 venv python，无法调用 esptool，退出。")
        input("\n按回车键关闭 ...") if len(sys.argv) <= 1 else None
        return 2

    # 单文件合并镜像模式：把一份合并好的镜像直接烧到 0x0
    if args.combined is not None:
        combined_path = (os.path.join(BUILD, "Metalio-S31-combined.bin")
                         if args.combined == "__DEFAULT__" else args.combined)
        combined_path = os.path.abspath(combined_path)
        if not os.path.isfile(combined_path):
            print(f"[!!] 合并镜像不存在：{combined_path}")
            print("     请先编译（双击 双击编译.py 选 1）生成 build/Metalio-S31-combined.bin，")
            print("     或用 --combined 显式指定镜像路径。")
            if len(sys.argv) <= 1:
                input("\n按回车键关闭 ...")
            return 2
        print("  模式：单文件合并镜像 -> 0x0")
        print("        （等价于整片擦除后写 bootloader/分区表/app/otadata，只需一个文件）")
        port = pick_port(args.port)
        print(f"\n  串口：{port}   波特率：{args.baud}")
        print(f"  镜像：{combined_path}  ({os.path.getsize(combined_path)} bytes)")
        print(f"  擦除：{'整片擦除(erase_flash)' if not args.no_erase else '跳过（--no-erase）'}")
        print()
        if len(sys.argv) <= 1:
            try:
                input("确认开始刷写？[回车继续，Ctrl+C 取消] ")
            except (EOFError, KeyboardInterrupt):
                print("\n已取消")
                return 0
        if not args.no_erase:
            print("[*] 整片擦除 flash ...")
            rc = run([VENV_PY, "-m", "esptool", "--chip", CHIP, "-p", port,
                      "-b", args.baud, "erase_flash"])
            if rc != 0:
                print(f"[!!] 擦除失败，exit={rc}")
                if len(sys.argv) <= 1:
                    input("\n按回车键关闭 ...")
                return rc
        print("[*] 写入合并镜像 (0x0) ...")
        rc = run([VENV_PY, "-m", "esptool", "--chip", CHIP, "-p", port, "-b", args.baud,
                  "write-flash", "--flash-mode", FLASH_MODE, "--flash-freq", FLASH_FREQ,
                  "--flash-size", FLASH_SIZE, "0x0", combined_path])
        print(f"\nFLASH_EXIT={rc}")
        if rc == 0:
            print("[OK] 刷写完成。设备将自动硬复位并启动新固件。")
            print("     若首次启动卡在格式化，属正常（nvs/storage 首次初始化）。")
        else:
            print("[!!] 刷写失败，请检查串口连线、驱动与设备是否进入下载模式。")
        if len(sys.argv) <= 1:
            input("\n按回车键关闭 ...")
        return rc

    offs, from_csv = offsets()
    print(f"  分区偏移来源：{'partitions.csv' if from_csv else '内置默认值'}")
    print(f"    bootloader      @ {offs['bootloader']:#x}")
    print(f"    partition-table @ {offs['partition_table']:#x}")
    print(f"    ota_0 (app)     @ {offs['ota_0']:#x}")
    print(f"    otadata         @ {offs['otadata']:#x}")
    print(f"    phy_init        @ {offs['phy_init']:#x}")

    # 把 IDF 工具链 bin 加进 PATH（esptool 依赖其 DLL/工具），并清掉 MSYSTEM
    os.environ.pop("MSYSTEM", None)
    os.environ.pop("MSYS", None)
    os.environ["IDF_PATH"] = IDF_PATH
    os.environ["PYTHONUTF8"] = "1"
    _inject_tools_path()

    # 只刷 app 主分区：日常改代码验证走这条路，bootloader/分区表/otadata/nvs 全不动，
    # 刷完设置、Wi-Fi、阅读进度都还在，也不用等 16MB 合并镜像写完。
    # App-partition-only flashing: daily iteration keeps bootloader/partitions/otadata/nvs intact,
    # so settings and reading progress survive and the wait is a third of the merged image.
    if args.app is not None:
        app_path = os.path.abspath(args.app) if args.app != "__DEFAULT__" else default_app_bin()
        if not os.path.isfile(app_path):
            print(f"[!!] app 镜像不存在：{app_path}")
            print("     请先编译（双击 双击编译.bat），或显式传入路径：--app <路径>")
            input("\n按回车键关闭 ...") if len(sys.argv) <= 1 else None
            return 2
        offset = offs["ota_0"]
        print("  模式：只刷 app 主分区（保留 nvs / storage / bootloader）")
        port = pick_port(args.port)
        print(f"\n  串口：{port}   波特率：{args.baud}")
        print(f"  镜像：{app_path}  ({os.path.getsize(app_path)} bytes)  @ {offset:#x}")
        print()
        if len(sys.argv) <= 1:
            try:
                input("确认开始刷写？[回车继续，Ctrl+C 取消] ")
            except (EOFError, KeyboardInterrupt):
                print("\n已取消")
                return 0
        rc = run([VENV_PY, "-m", "esptool", "--chip", CHIP, "-p", port, "-b", args.baud,
                  "write-flash", "--flash-mode", FLASH_MODE, "--flash-freq", FLASH_FREQ,
                  "--flash-size", FLASH_SIZE, hex(offset), app_path])
        print(f"\nFLASH_EXIT={rc}")
        if rc == 0:
            print("[OK] app 分区刷写完成，设备自动硬复位并运行新固件（设置与进度均已保留）。")
        else:
            print("[!!] 刷写失败，请检查串口连线、驱动与设备是否进入下载模式。")
        if len(sys.argv) <= 1:
            input("\n按回车键关闭 ...")
        return rc

    if not ensure_files(offs):
        input("\n按回车键关闭 ...") if len(sys.argv) <= 1 else None
        return 2

    port = pick_port(args.port)
    print(f"\n  串口：{port}   波特率：{args.baud}")
    print(f"  擦除：{'整片擦除(erase_flash)' if not args.no_erase else '跳过（--no-erase）'}")
    print()

    if len(sys.argv) <= 1:
        try:
            input("确认开始刷写？[回车继续，Ctrl+C 取消] ")
        except (EOFError, KeyboardInterrupt):
            print("\n已取消")
            return 0

    # 1) 整片擦除（默认）
    if not args.no_erase:
        print("[*] 整片擦除 flash ...")
        rc = run([VENV_PY, "-m", "esptool", "--chip", CHIP, "-p", port,
                  "-b", args.baud, "erase_flash"])
        if rc != 0:
            print(f"[!!] 擦除失败，exit={rc}")
            input("\n按回车键关闭 ...") if len(sys.argv) <= 1 else None
            return rc

    # 2) 写全部分区
    print("[*] 写入分区 ...")
    flash_args = [
        VENV_PY, "-m", "esptool", "--chip", CHIP, "-p", port, "-b", args.baud,
        "write-flash", "--flash-mode", FLASH_MODE, "--flash-freq", FLASH_FREQ,
        "--flash-size", FLASH_SIZE,
        f"{offs['bootloader']:#x}", "bootloader/bootloader.bin",
        f"{offs['partition_table']:#x}", "partition_table/partition-table.bin",
        f"{offs['ota_0']:#x}", APP_BIN,
        f"{offs['otadata']:#x}", "ota_data_initial.bin",
    ]
    phy = os.path.join(BUILD, "phy_init.bin")
    if os.path.isfile(phy):
        flash_args += [f"{offs['phy_init']:#x}", "phy_init.bin"]
    rc = run(flash_args)
    print(f"\nFLASH_EXIT={rc}")
    if rc == 0:
        print("[OK] 刷写完成。设备将自动硬复位并启动新固件。")
        print("     若首次启动卡在格式化，属正常（nvs/storage 首次初始化）。")
    else:
        print("[!!] 刷写失败，请检查串口连线、驱动与设备是否进入下载模式。")
    if len(sys.argv) <= 1:
        input("\n按回车键关闭 ...")
    return rc


def _inject_tools_path():
    """把 ~/.espressif/tools 下的 bin 目录塞进 PATH（esptool 运行时可能需要）。"""
    root = os.path.join(os.path.expanduser("~"), ".espressif", "tools")
    dirs = []
    for pat in (os.path.join(root, "*", "*", "bin"),
                os.path.join(root, "*", "*", "*", "bin")):
        dirs.extend(d for d in glob.glob(pat) if os.path.isdir(d))
    if dirs:
        os.environ["PATH"] = os.pathsep.join(dirs) + os.pathsep + os.environ.get("PATH", "")
        print(f"[env] injected {len(dirs)} toolchain dirs into PATH")


if __name__ == "__main__":
    try:
        sys.exit(main())
    except KeyboardInterrupt:
        print("\n已取消")
        sys.exit(130)
