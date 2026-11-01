#!/usr/bin/env python3

# SPDX-FileCopyrightText: 2026 zhuyue
# SPDX-License-Identifier: LicenseRef-Kiikoread-Personal-Study-1.0

# -*- coding: utf-8 -*-
"""
Metalio-S31-kiikoread-main 编译脚本（boards/metalio47，目标芯片 esp32s31）。

板型通过 KIIKO_BOARD=metalio47 选择，cmake/board.cmake 会自动把目标芯片
设为 esp32s31、并把 KIIKO_BOARD_METALIO47=1 注入编译，无需改任何源码。

依赖官方 ESP-IDF v6.1（install.bat 装好的 venv 在 ~/.espressif）。
IDF_PATH 默认指向 C:/Users/zhuyue/Desktop/gujian (2)/esp-idf-v6.1，
可用环境变量 IDF_PATH / IDF_TOOLS_PATH 覆盖。

用法：
    build_metalio.bat            # 双击编译
    python build_metalio.py -p   # 编译 + 烧写 COM5
"""
import os
import sys
import subprocess


PROJ = os.path.dirname(os.path.abspath(__file__))


def _idf_path() -> str:
    env = os.environ.get("IDF_PATH")
    if env and os.path.isfile(os.path.join(env, "tools", "idf.py")):
        return env
    candidates = [
        r"C:\esp\idf-v6.1",  # 无空格联接，绕过 ESP-IDF 不支持带空格路径的限制
        r"C:\Users\zhuyue\Desktop\gujian (2)\esp-idf-v6.1",
        os.path.join(os.path.expanduser("~"), "esp", "esp-idf-v6.1"),
        r"C:\Espressif\esp-idf-v6.1",
    ]
    for c in candidates:
        if os.path.isfile(os.path.join(c, "tools", "idf.py")):
            return c
    return r"C:\Users\zhuyue\Desktop\gujian (2)\esp-idf-v6.1"


def _idf_tools_path() -> str:
    env = os.environ.get("IDF_TOOLS_PATH")
    if env:
        return env
    return os.path.expanduser("~/.espressif")


IDF_PATH = _idf_path()
IDF_TOOLS_PATH = _idf_tools_path()
def _venv_py() -> str:
    base = os.path.join(IDF_TOOLS_PATH, "python_env")
    import glob
    for m in glob.glob(os.path.join(base, "idf6.1_*_env")):
        p = os.path.join(m, "Scripts", "python.exe")
        if os.path.isfile(p):
            return p
    return os.path.join(base, "idf6.1_py3.13_env", "Scripts", "python.exe")
VENV_PY = _venv_py()
VENV = os.path.dirname(os.path.dirname(VENV_PY))
IDF_PY = os.path.join(IDF_PATH, "tools", "idf.py")
PROJECT_VER = "0.3.3-rc108.1"
BUILD = os.path.join(PROJ, "build")
APP_BIN = "Kiikoread_Metalio_S31.bin"


def _board_version() -> str:
    """板级版本号以 boards/metalio47/board.json 为准，脚本只负责读出来显示。

    The board JSON owns the version; this helper only reads it for the banner.
    """
    try:
        import json
        with open(os.path.join(PROJ, "boards", "metalio47", "board.json"), encoding="utf-8") as fp:
            return str(json.load(fp).get("version") or PROJECT_VER)
    except Exception:
        return PROJECT_VER


def build_dir(profile: str) -> str:
    """每个编译档一个构建目录，两种档位来回切不用全量重编。

    One build directory per profile so switching profiles never rebuilds the other one.
    """
    return os.path.join(PROJ, "build") if profile == "release" \
        else os.path.join(PROJ, f"build-{profile}")


def sdkconfig_path(profile: str) -> str:
    """每个档位一份 sdkconfig：日志项不同，共用一份会来回覆写对方的配置。

    One sdkconfig per profile: the log settings differ, and sharing a file makes the two
    profiles overwrite each other on every switch.
    """
    stem = os.path.join(PROJ, "sdkconfig.metalio47")
    return stem if profile == "release" else f"{stem}.{profile}"


PROXY_HOST = "127.0.0.1"
PROXY_PORT = 26561

import argparse
import socket


def proxy_up() -> bool:
    try:
        with socket.create_connection((PROXY_HOST, PROXY_PORT), timeout=1):
            return True
    except OSError:
        return False


import glob


def _tool_bin_dirs() -> list:
    """探测 ~/.espressif/tools 下所有工具链的 bin 目录。

    不经 export.bat 直接运行 idf.py 时，PATH 里没有交叉编译器，
    cmake 会报 'riscv32-esp-elf-gcc is not a full path and was not
    found in the PATH'。这里手动把工具链目录补进 PATH。
    """
    root = os.path.join(IDF_TOOLS_PATH, "tools")
    dirs = []
    for pat in (
        os.path.join(root, "*", "*", "bin"),          # cmake/X.Y/bin
        os.path.join(root, "*", "*", "*", "bin"),     # riscv32-esp-elf/版本/前缀/bin
    ):
        dirs.extend(d for d in glob.glob(pat) if os.path.isdir(d))
    # ninja 等工具的 exe 直接放在版本目录下（无 bin 子目录）
    for d in glob.glob(os.path.join(root, "*", "*")):
        if os.path.isdir(d) and not os.path.isdir(os.path.join(d, "bin")) \
                and glob.glob(os.path.join(d, "*.exe")):
            dirs.append(d)
    return dirs


def setup_env() -> None:
    # Git Bash/MSYS 里 MSYSTEM 存在时，idf.py 的 __main__ 只打一条 "MSys/Mingw is no longer
    # supported" 警告就直接返回，压根不调用 main()（退出码 0、什么也不编）。构建前清掉。
    # Under Git Bash/MSYS, idf.py only warns about MSYSTEM and returns before main(), exiting 0
    # without building anything. Strip it so the build actually runs.
    for _msys in ("MSYSTEM", "MSYS"):
        os.environ.pop(_msys, None)
    os.environ["IDF_PATH"] = IDF_PATH
    os.environ["IDF_TOOLS_PATH"] = IDF_TOOLS_PATH
    os.environ["ESP_IDF_VERSION"] = "6.1.0"
    os.environ["IDF_PYTHON_ENV_PATH"] = VENV
    os.environ["PYTHONUTF8"] = "1"
    bins = _tool_bin_dirs()
    if bins:
        os.environ["PATH"] = os.pathsep.join(bins) + os.pathsep + os.environ.get("PATH", "")
        print(f"[env] injected {len(bins)} toolchain dirs into PATH")
    if proxy_up():
        os.environ["HTTP_PROXY"] = f"http://{PROXY_HOST}:{PROXY_PORT}"
        os.environ["HTTPS_PROXY"] = f"http://{PROXY_HOST}:{PROXY_PORT}"
    else:
        print("[env] proxy down, building offline (组件需联网时可能失败)")


def run(cmd, cwd) -> int:
    print(">>> " + " ".join(cmd))
    proc = subprocess.Popen(
        cmd, cwd=cwd, env=os.environ,
        stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
        text=True, encoding="utf-8", errors="replace", bufsize=1,
    )
    assert proc.stdout is not None
    for line in proc.stdout:
        sys.stdout.write(line)
    return proc.wait()


def build(ver: str = "", trace: bool = False, selftest: bool = False,
          bt_trace: bool = False, profile: str = "release") -> int:
    setup_env()
    if ver:
        # PROJECT_VER 在配置期读取环境变量；改了要强制 cmake 重新配置才会生效。
        os.environ["KIIKO_PROJECT_VER"] = ver
        os.utime(os.path.join(PROJ, "CMakeLists.txt"), None)
        print(f"[ver] PROJECT_VER={ver}")
    else:
        print(f"[ver] PROJECT_VER={_board_version()}（来自 board.json）")
    # 正式档案位：诊断代码一律不进固件。debug 档才按参数编入。
    # Release profile: no diagnostic code ships. The debug profile compiles it per flag.
    print(f"[profile] {profile}  日志级别={'ERROR(裁掉 INFO/WARN/DEBUG)' if profile == 'release' else 'DEBUG'}")
    print(f"[trace] METALIO_TURN_TRACE={'ON' if trace else 'OFF'}")
    # 蓝牙/睡眠诊断（写 TF 卡 bt_trace.log）单独一个开关：它和翻页计时是两件事，
    # 抓「锁屏唤醒没声音」时只要它，不必连带开翻页 CSV。交付包两个都不带。
    # Bluetooth/sleep diagnostics (bt_trace.log on the TF card) get their own switch: they are
    # unrelated to page-turn timing, so a silent-after-wake capture needs only this one and no
    # turn CSV. Delivery images carry neither.
    print(f"[bt-trace] METALIO_BT_TRACE={'ON' if bt_trace else 'OFF'}")
    # 无人值守自测：开机在热点与蓝牙耳机之间轮换，每相位重启，进度存 NVS。仅本地测试用。
    # Unattended self-test: rotate hotspot/Bluetooth phases across reboots with progress in NVS. Local tests only.
    print(f"[selftest] METALIO_AUTO_TEST={'ON' if selftest else 'OFF'}")
    print(f"[build] dir={BUILD}  sdkconfig={os.path.basename(sdkconfig_path(profile))}")
    rc = run(
        [VENV_PY, IDF_PY, "-B", BUILD, "-DKIIKO_BOARD=metalio47",
         f"-DMETALIO_PROFILE={profile}",
         f"-DSDKCONFIG={sdkconfig_path(profile)}",
         f"-DMETALIO_TURN_TRACE={'ON' if trace else 'OFF'}",
         f"-DMETALIO_BT_TRACE={'ON' if bt_trace else 'OFF'}",
         f"-DMETALIO_AUTO_TEST={'ON' if selftest else 'OFF'}", "--preview", "build"],
        PROJ,
    )
    print(f"METALIO_EXITCODE={rc}")
    return rc


def flash(port: str, chip: str) -> int:
    setup_env()
    cmd = [
        VENV_PY, "-m", "esptool",
        "--chip", chip, "-p", port, "-b", "921600",
        "write-flash", "--flash-mode", "dio", "--flash-freq", "80m",
        "--flash-size", "16MB",
        "0x2000", "bootloader/bootloader.bin",
        "0x8000", "partition_table/partition-table.bin",
        # otadata 实际在 0xfe0000（分区表 otadata offset），旧值 0xd000 从未生效，
        # 导致烧进 ota_0 的新固件一直不被启动、设备继续跑 ota_1 旧固件。
        # otadata really lives at 0xfe0000; the stale 0xd000 never took effect, so
        # freshly flashed ota_0 images were silently ignored in favour of the ota_1 copy.
        "0xfe0000", "ota_data_initial.bin",
        "0x10000", "Kiikoread_Metalio_S31.bin",
    ]
    rc = run(cmd, BUILD)
    print(f"FLASH_EXIT={rc}")
    return rc


def package_sd() -> int:
    """把应用镜像复制成 TF 卡升级包 /sdcard/Metalio-S31-update.bin。"""
    src = os.path.join(BUILD, APP_BIN)
    dst = os.path.join(PROJ, "Metalio-S31-update.bin")
    if not os.path.isfile(src):
        print("[!] app image missing:", src)
        return 1
    import shutil
    shutil.copyfile(src, dst)
    size = os.path.getsize(dst)
    print(f"[sd] package ready: {dst} ({size} bytes)")
    print("[sd] 把它放到 TF 卡根目录，文件名必须是 Metalio-S31-update.bin")
    return 0


def make_combined() -> int:
    """合并 bootloader/分区表/app/otadata 为单个可烧写镜像 build/Metalio-S31-combined.bin。

    该文件从 flash 0x0 开始，各镜像按真实偏移排布，空档填 0xFF（等同擦除态）。
    线刷时只需把这一份烧到 0x0：esptool write-flash 0x0 Metalio-S31-combined.bin
    —— 等价于整片擦除后分别写 bootloader/分区表/app/otadata，但只用一个文件。
    nvs/storage/ota_1 等数据分区没有独立镜像，文件中为 0xFF，烧写后被固件首启重新初始化。
    """
    out = os.path.join(BUILD, "Metalio-S31-combined.bin")
    images = [
        ("0x2000", os.path.join("bootloader", "bootloader.bin")),
        ("0x8000", os.path.join("partition_table", "partition-table.bin")),
        ("0x10000", APP_BIN),          # ota_0 应用固件
        ("0xfe0000", "ota_data_initial.bin"),   # otadata（OTA 状态初始化）
    ]
    missing = [rel for _, rel in images if not os.path.isfile(os.path.join(BUILD, rel))]
    if missing:
        print("[combined] 缺少镜像，无法合并：", ", ".join(missing))
        print("[combined] 请确认编译成功后再合并。")
        return 1
    cmd = [
        VENV_PY, "-m", "esptool", "--chip", "esp32s31", "merge-bin",
        "--flash-mode", "dio", "--flash-freq", "80m", "--flash-size", "16MB",
        "-o", out,
    ]
    for off, rel in images:
        cmd += [off, rel]
    rc = run(cmd, BUILD)
    if rc == 0:
        size = os.path.getsize(out)
        print(f"[combined] 单文件线刷镜像已生成: {out} ({size} bytes)")
        print(f"[combined] 烧写: esptool write-flash 0x0 {os.path.basename(out)}")
        print(f"[combined] 或直接用 双击线刷_单文件.bat / flash_partitions.py --combined")
    else:
        print("[combined] 合并失败（不影响已编译出的多文件固件）")
    print(f"COMBINED_EXIT={rc}")
    return rc


def main() -> int:
    parser = argparse.ArgumentParser(description="metalio build/flash")
    parser.add_argument("-p", "--flash", action="store_true", help="编译后烧写")
    parser.add_argument("--port", default="COM5")
    parser.add_argument("--chip", default="esp32s31")
    # 编译档：release 是交付用的正式版（日志只留 ERROR、诊断代码不编入）；debug 是排障用的
    # 调试版（DEBUG 日志、各种 trace 可开）。两档各有独立的 build 目录与 sdkconfig，互不覆盖。
    # release is the shipping image (ERROR-only logs, no diagnostic code); debug is the
    # troubleshooting image (DEBUG logs, tracing available). Each has its own build dir and
    # sdkconfig, so switching never clobbers the other one.
    parser.add_argument("--profile", choices=("release", "debug"), default="release",
                        help="编译档：release=正式版（默认）/ debug=调试版")
    # 本机已允许同版本卡刷；只有要把包刷进"还在拒绝同版本"的旧固件时才需要抬版本号。
    # / This build accepts same-version card packages; bump only when flashing into older
    # firmware that still rejects them.
    parser.add_argument("--ver", default="", help="覆盖 PROJECT_VER（默认取 board.json，当前 0.3.3-rc108.1）")
    parser.add_argument("--sd", action="store_true", help="编译后生成 TF 卡升级包")
    # debug 档默认全开；release 档强制关闭。--no-trace / --no-bt-trace 保留给想关掉的情况。
    # On by default in the debug profile, forced off in release; the --no-* forms turn them off.
    parser.add_argument("--trace", dest="trace", action="store_true", default=None,
                        help="打开分步翻页计时（写 TF 卡 turn_trace.csv）/ Enable page-turn tracing")
    parser.add_argument("--no-trace", dest="trace", action="store_false",
                        help="关闭分步翻页计时 / Disable page-turn tracing")
    parser.add_argument("--selftest", action="store_true",
                        help="编入无人值守自测（热点/蓝牙轮换重启，仅 debug 档）/ Build the unattended self-test")
    parser.add_argument("--bt-trace", dest="bt_trace", action="store_true", default=None,
                        help="打开蓝牙/睡眠诊断（写 TF 卡 .metalio_s31/bt_trace.log）/ Enable Bluetooth and sleep tracing")
    parser.add_argument("--no-bt-trace", dest="bt_trace", action="store_false",
                        help="关闭蓝牙/睡眠诊断 / Disable Bluetooth and sleep tracing")
    args = parser.parse_args()

    global BUILD
    BUILD = build_dir(args.profile)
    if args.profile == "release":
        if args.trace or args.bt_trace or args.selftest:
            print("[profile] release 档不带诊断代码：--trace / --bt-trace / --selftest 已忽略；"
                  "要抓日志请用 --profile debug")
        trace, bt_trace, selftest = False, False, False
    else:
        trace = True if args.trace is None else args.trace
        bt_trace = True if args.bt_trace is None else args.bt_trace
        selftest = args.selftest

    rc = build(args.ver, trace, selftest, bt_trace, args.profile)
    if rc != 0:
        print("[!] build failed")
        if len(sys.argv) <= 1:
            try:
                input("\n按回车键关闭窗口 ...")
            except (EOFError, KeyboardInterrupt):
                pass
        return rc
    # 编译成功后自动合并成单文件线刷镜像（build/Metalio-S31-combined.bin）
    make_combined()
    if args.sd:
        rc = package_sd()
        if rc != 0 and len(sys.argv) <= 1:
            try:
                input("\n按回车键关闭窗口 ...")
            except (EOFError, KeyboardInterrupt):
                pass
        if rc != 0:
            return rc
    if args.flash:
        rc = flash(args.port, args.chip)
    if len(sys.argv) <= 1:
        try:
            input("\n按回车键关闭窗口 ...")
        except (EOFError, KeyboardInterrupt):
            pass
    return rc


if __name__ == "__main__":
    sys.exit(main())
