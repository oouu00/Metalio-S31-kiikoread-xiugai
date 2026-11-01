#!/usr/bin/env python3
"""上游 rc 版本升级：把新版本的改动三方合并进本地树。

中文：以“上一版上游”为 base、新上游为 theirs、本地树为 ours，逐文件做三方合并，只落上游增量，
本地改动按行级保留；合并前自动备份，合并后自动刷新 mtime，避免 ninja 复用旧目标文件。
English: Merge a new upstream release into the local tree with a per-file three-way merge:
the previous upstream release is the base, the new one is theirs, and the local tree is ours,
so only upstream deltas land while local edits survive line by line; every touched file is
backed up first and its mtime refreshed afterwards so ninja cannot reuse stale objects.

用法 / Usage:
  # 1) 先预览：只报告分类结果，不写盘
  python tools/rc_upgrade_merge.py --base C:\\esp\\老版本固件\\...rc105\\... --theirs C:\\esp\\老版本固件\\...rc106\\...

  # 2) 确认无误后落盘
  python tools/rc_upgrade_merge.py --base <rc105> --theirs <rc106> --apply

  # 3) 合并后复查：列出本地树与新版本仍有差异的文件
  python tools/rc_upgrade_merge.py --base <rc105> --theirs <rc106> --check

约定 / Conventions:
  * --base 必须是「上一次已合并干净的上游版本」，不是本地树。
  * 退出码 0 = 无冲突；1 = 有冲突需人工处理（冲突文件保持原样，草稿写到 build/rc_upgrade_merge/）。
"""

from __future__ import annotations

import argparse
import os
import shutil
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

# 中文：参与同步的源码区域，与 build_firmware.py / board_source_view.py 的物化范围一致。
# English: Source areas that participate in the sync, matching the materialisation scope of the builder.
AREAS = ("main", "components", "boards", "tools", "docs", "assets", "cmake", "licenses", ".github", ".devcontainer")


def collect(tree: Path) -> list[str]:
    """列出上游树里需要比对的相对路径。"""
    found: list[str] = []
    for area in AREAS:
        top = tree / area
        if not top.is_dir():
            continue
        for path in top.rglob("*"):
            if path.is_file():
                found.append(path.relative_to(tree).as_posix())
    for path in tree.iterdir():
        if path.is_file():
            found.append(path.name)
    return found


def same(a: Path, b: Path) -> bool:
    if not a.is_file() or not b.is_file():
        return False
    if a.stat().st_size != b.stat().st_size:
        return False
    return a.read_bytes() == b.read_bytes()


def backup(path: Path, stamp: Path) -> None:
    """把要改写的本地文件备份到 build/rc_upgrade_backup/<时间戳>/ 下。"""
    dest = stamp / path.relative_to(ROOT)
    dest.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(path, dest)


def install(src: Path, dst: Path, stamp_dir: Path | None) -> None:
    """写入上游内容并刷新 mtime，防止增量构建漏编。"""
    if stamp_dir is not None and dst.is_file():
        backup(dst, stamp_dir)
    dst.parent.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(src, dst)
    now = time.time()
    os.utime(dst, (now, now))


def merge(base: Path, theirs: Path, ours: Path, draft: Path) -> bool:
    """三方合并 single file；返回 True 表示无冲突。"""
    work = draft.parent
    work.mkdir(parents=True, exist_ok=True)
    cand = work / "ours"
    shutil.copyfile(ours, cand)
    result = subprocess.run(
        ["git", "merge-file", "-L", "ours(local)", "-L", f"base({base.parent.name})", "-L", f"theirs({theirs.parent.name})",
         str(cand), str(base), str(theirs)],
        capture_output=True,
    )
    if result.returncode < 0:
        raise RuntimeError(result.stderr.decode("utf-8", "replace"))
    shutil.copyfile(cand, draft)
    return result.returncode == 0


def main() -> int:
    parser = argparse.ArgumentParser(description="上游 rc 版本三方合并工具")
    parser.add_argument("--base", required=True, type=Path, help="上一版上游树（已合并干净的版本）")
    parser.add_argument("--theirs", required=True, type=Path, help="新上游树")
    parser.add_argument("--ours", type=Path, default=ROOT, help="本地树，默认仓库根")
    parser.add_argument("--apply", action="store_true", help="真正写盘；缺省只预览")
    parser.add_argument("--check", action="store_true", help="只列出本地树与新版本的剩余差异")
    args = parser.parse_args()

    base, theirs, ours = args.base.resolve(), args.theirs.resolve(), args.ours.resolve()
    for tree in (base, theirs):
        if not tree.is_dir():
            print(f"目录不存在 / missing: {tree}")
            return 2

    paths = sorted(set(collect(base)) | set(collect(theirs)))
    if args.check:
        remain = []
        for rel in paths:
            # 中文：两边都不存在（上游已删除且本地也删了）不算差异，避免误报。
            # English: Paths absent on both sides are not a difference, so skip them to avoid false reports.
            if not (theirs / rel).is_file() and not (ours / rel).is_file():
                continue
            if not same(theirs / rel, ours / rel):
                remain.append(rel)
        print(f"本地树与新版本仍有差异 / still different from theirs: {len(remain)}")
        for rel in remain:
            print(f"  local-diff {rel}")
        return 0

    stamp = time.strftime("%Y%m%d-%H%M%S")
    stamp_dir = ROOT / "build" / "rc_upgrade_backup" / stamp if args.apply else None
    work = ROOT / "build" / "rc_upgrade_merge"

    stats = {"add": 0, "take": 0, "merged": 0, "conflict": 0, "newconflict": 0, "same": 0, "delete": 0}
    conflicts: list[str] = []
    deleting: list[str] = []

    for rel in paths:
        b, t, o = base / rel, theirs / rel, ours / rel
        if o.is_file() and same(o, t):
            stats["same"] += 1
            continue
        if t.is_file() and not b.is_file():
            if o.is_file():
                # 中文：上游新增的文件本地已存在同名版本（双方各自新建），没有 base 可三方合并，按冲突处理，绝不覆盖本地文件。
                # English: Upstream adds a file we already created under the same name; with no base to merge against,
                # treat it as a conflict and never overwrite the local file.
                stats["newconflict"] += 1
                conflicts.append(rel)
                draft_dir = work / rel.replace("/", "__")
                draft_dir.mkdir(parents=True, exist_ok=True)
                shutil.copyfile(t, draft_dir / "theirs-new")
                print(f"CONFLICT-NEW {rel}  -> {draft_dir / 'theirs-new'}")
                continue
            stats["add"] += 1
            print(f"ADD     {rel}")
            if args.apply:
                install(t, o, stamp_dir)
            continue
        if b.is_file() and not t.is_file():
            stats["delete"] += 1
            deleting.append(rel)
            if not o.is_file():
                print(f"DEL     {rel}  (本地已无此文件)")
            elif same(o, b):
                print(f"DEL     {rel}")
                if args.apply:
                    if stamp_dir is not None:
                        backup(o, stamp_dir)
                    o.unlink()
                    print(f"        已删除 {rel}")
            else:
                # 中文：上游删了这个文件但本地改过，保留本地版本交人工判断。
                # English: Upstream deleted it while we edited it; keep the local copy for a manual decision.
                print(f"DEL     {rel}  (本地已改，保留本地版本)")
            continue
        if same(o, b):
            stats["take"] += 1
            print(f"TAKE    {rel}")
            if args.apply:
                install(t, o, stamp_dir)
            continue
        # 中文：双方都改过，走三方合并；冲突时保持本地文件不变，草稿留在 build/ 下待人工处理。
        # English: Both sides changed; merge in three ways, and on conflict keep the local file untouched.
        draft_dir = work / rel.replace("/", "__")
        clean = merge(b, t, o, draft_dir / "merged")
        if clean:
            stats["merged"] += 1
            print(f"MERGE   {rel}")
            if args.apply:
                install(draft_dir / "merged", o, stamp_dir)
        else:
            stats["conflict"] += 1
            conflicts.append(rel)
            print(f"CONFLICT {rel}  -> {draft_dir / 'merged'}")

    print("\n汇总 / summary:")
    print("  " + ", ".join(f"{k}={v}" for k, v in stats.items()))
    if deleting and not args.apply:
        print("  待删除 / pending deletes: " + ", ".join(deleting))
    if conflicts:
        print("  需人工处理 / resolve manually:")
        for rel in conflicts:
            print(f"    {rel}")
        print("  规则 / rule: 若本地版本对 base 零新增行，说明只是上游旧版残留，直接取 theirs。")
    if not args.apply:
        print("\n预览模式，未写盘。确认后加 --apply 落盘。 / Preview only; rerun with --apply.")
    else:
        print(f"\n已备份到 / backup: {stamp_dir}")
    return 1 if conflicts else 0


if __name__ == "__main__":
    sys.exit(main())
