# SPDX-FileCopyrightText: 2026 zhuyue
# SPDX-License-Identifier: LicenseRef-Kiikoread-Personal-Study-1.0

import os
import subprocess
import shutil
from pathlib import Path

def main():
    # 脚本所在目录，等价 %~dp0
    PROJ = Path(__file__).resolve().parent
    print("=" * 60)
    print("  Metalio S31 一键编译  （版本 rc108）")
    print("=" * 60)
    print()
    print("  请选择要编译的固件版本：")
    print()
    print("    1 = release   正式版（日志最少、体积小，推荐日常使用）")
    print("    2 = debug     调试版（含全部日志与诊断输出，便于排查问题）")
    print()
    print("  直接回车默认编译 release。")
    print()

    PROFILE = "release"
    sel = input("请输入 1 或 2 ：").strip()
    if sel == "2" or sel.lower() == "debug":
        PROFILE = "debug"

    # 构建目录
    if PROFILE == "debug":
        BDIR = "build-debug"
    else:
        BDIR = "build"

    # ---- 产物格式：默认仅单文件线刷镜像（烧 0x0 即可），其余可选 ----
    print()
    print("  请选择产物格式（线刷 / 发布）：")
    print()
    print("    1 = 仅单文件线刷镜像（默认）")
    print("        -> Metalio-S31-combined.bin（烧到 0x0 即可，最省事）")
    print("    2 = 单文件 + 分开的多文件发布包")
    print("        -> 含 bootloader / 分区表 / app / otadata 单独镜像")
    print("    3 = 单文件 + 多文件 + TF卡升级包")
    print("        -> 额外含 Metalio-S31-update.bin（放 TF 卡根目录 OTA 升级）")
    print()
    print("  直接回车默认 1（仅单文件）。")
    print()

    psel = input("请输入 1 / 2 / 3 ：").strip()
    if psel == "2":
        OUT_MODE = "multi"
    elif psel == "3":
        OUT_MODE = "sd"
    else:
        OUT_MODE = "single"   # 默认

    print()
    print(f"[i] 已选择：{PROFILE}  （构建目录：{BDIR}）")
    print(f"[i] 产物格式：{'仅单文件' if OUT_MODE=='single' else ('单文件+多文件' if OUT_MODE=='multi' else '单文件+多文件+SD升级包')}")
    wrapper_ps1 = PROJ / "build_wrapper.ps1"
    print(f"[i] 编译入口：{wrapper_ps1}")
    print(f"[i] 单文件线刷镜像：{PROJ / BDIR / 'Metalio-S31-combined.bin'}")
    print()

    # 调用 powershell 执行 ps1；仅 SD 升级包模式传 -Sd
    cmd = [
        "powershell",
        "-NoProfile",
        "-ExecutionPolicy", "Bypass",
        "-File", str(wrapper_ps1),
        "-Profile", PROFILE,
    ]
    if OUT_MODE == "sd":
        cmd.append("-Sd")
    ret = subprocess.run(cmd)
    RC = ret.returncode

    print()
    if RC == 0:
        print(f"[OK] 编译成功，exit={RC}")
        OUT = PROJ / "Metalio-Release"
        OUT.mkdir(exist_ok=True)

        combined = PROJ / BDIR / "Metalio-S31-combined.bin"
        if OUT_MODE == "single":
            files_to_copy = [combined]
        elif OUT_MODE == "multi":
            files_to_copy = [
                combined,
                PROJ / BDIR / "Kiikoread_Metalio_S31.bin",
                PROJ / BDIR / "bootloader" / "bootloader.bin",
                PROJ / BDIR / "partition_table" / "partition-table.bin",
                PROJ / BDIR / "ota_data_initial.bin",
            ]
        else:   # sd
            files_to_copy = [
                combined,
                PROJ / BDIR / "Kiikoread_Metalio_S31.bin",
                PROJ / BDIR / "bootloader" / "bootloader.bin",
                PROJ / BDIR / "partition_table" / "partition-table.bin",
                PROJ / BDIR / "ota_data_initial.bin",
                PROJ / "Metalio-S31-update.bin",
            ]

        # 先确认产物齐全再动发布目录：产物缺失说明这次编译没真正产出（构建目录不对或编译中途失败），
        # 此时若先清空发布目录，会把上一次可用的固件一起删掉。
        # Verify every artifact before touching the release folder; wiping first would destroy the
        # last known-good package when this build produced nothing.
        missing = [src for src in files_to_copy if not src.exists()]
        if missing:
            print(f"[!!] 产物缺失 {len(missing)} 个，发布目录保持不变（未删除任何旧文件）：")
            for src in missing:
                print(f"    - {src}")
            print(f"[i] 构建目录：{PROJ / BDIR}")
            print("[i] 常见原因：编译中途失败（看 build.log 里的 METALIO_EXITCODE），或构建目录选错。")
            return 1

        # 只清掉本次将要写入的同名文件，其他内容保留。
        # Only clear the exact files this run will overwrite.
        for src in files_to_copy:
            target = OUT / src.name
            if target.exists():
                try:
                    target.unlink()
                except OSError:
                    pass
        print(f"[i] 复制到发布目录 {OUT} ...")
        for src in files_to_copy:
            shutil.copy2(src, OUT)
            print(f"    + {src.name}")
        print(f"[OK] 已复制到 {OUT}")
    else:
        print(f"[!!] 编译失败，exit={RC}")
        print(f"[i] 未复制任何文件，发布目录保持不变。构建目录：{PROJ / BDIR}")

    print(f"[i] 详见 {PROJ / 'build.log'}")
    input("\n按回车键关闭窗口...")
    return RC


if __name__ == "__main__":
    raise SystemExit(main())
