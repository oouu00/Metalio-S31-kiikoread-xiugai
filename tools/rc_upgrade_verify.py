#!/usr/bin/env python3
"""rc 升级后的全树校验：确认上游改动没丢、本地改动还在。

中文：逐个文件比较三棵树（base = 上一次已合并干净的上游、theirs = 新上游、ours = 本地树），
按「归一化行尾后」的内容分类，输出四类结果：

  LOST     ours 仍等于 base，但 theirs 已经改过  ⇒ 上游改动被漏掉，必须处理（退出码 1）
  MISSING  theirs 有、ours 没有                ⇒ 本地删过或漏拷
  LOCAL    ours 与 theirs 不同且与 base 不同    ⇒ 本地改动，预期（逐条点检）
  EXTRA    ours 有、theirs 没有                ⇒ 本地新增文件，预期

English: compare three trees per file (base = last cleanly merged upstream, theirs = new upstream,
ours = local tree) on content with line endings normalised, and classify every difference as
LOST / MISSING / LOCAL / EXTRA. LOST means an upstream change never landed; exit code 1 then.

用法 / Usage:
  python tools/rc_upgrade_verify.py \
      --base   C:\\esp\\老版本固件\\Metalio-S31-kiikoread-mainrc107\\Metalio-S31-kiikoread-main \
      --theirs C:\\esp\\老版本固件\\Metalio-S31-kiikoread-main-rc108\\Metalio-S31-kiikoread-main

为什么要归一化行尾：上游发布包是 LF，本仓库部分文件被 git 检出成 CRLF。直接把字节交给
`git merge-file`/diff，会把「只差行尾」的文件当成「双方都改过」，整文件判冲突或整文件重写。
英文：the upstream zips are LF while some checked-out files are CRLF, so byte-level three-way
merges turn line-ending-only noise into whole-file conflicts. Normalise before comparing.
"""
from __future__ import annotations

import argparse
import sys
from pathlib import Path

AREAS = ("main", "components", "boards", "tools", "docs", "assets", "cmake", "licenses",
         ".github", ".devcontainer")


def collect(tree: Path) -> set[str]:
    """树里参与同步的相对路径（与 rc_upgrade_merge.py 一致）。"""
    found: set[str] = set()
    for area in AREAS:
        top = tree / area
        if not top.is_dir():
            continue
        for path in top.rglob("*"):
            if path.is_file():
                found.add(path.relative_to(tree).as_posix())
    for path in tree.iterdir():
        if path.is_file():
            found.add(path.name)
    return found


def norm(path: Path) -> bytes | None:
    """读取内容并把 CRLF 归一为 LF；文件不存在返回 None。"""
    if not path.is_file():
        return None
    return path.read_bytes().replace(b"\r\n", b"\n")


def main() -> int:
    parser = argparse.ArgumentParser(description="rc 升级后的全树校验")
    parser.add_argument("--base", required=True, type=Path, help="上一次已合并干净的上游树")
    parser.add_argument("--theirs", required=True, type=Path, help="新上游树")
    parser.add_argument("--ours", type=Path, default=Path(__file__).resolve().parents[1],
                        help="本地树，默认仓库根")
    parser.add_argument("--quiet", action="store_true", help="只打印汇总与异常项")
    args = parser.parse_args()

    base, theirs, ours = args.base.resolve(), args.theirs.resolve(), args.ours.resolve()
    for tree in (base, theirs, ours):
        if not tree.is_dir():
            print(f"目录不存在 / missing: {tree}")
            return 2

    lost: list[str] = []
    missing: list[str] = []
    local: list[str] = []
    same = 0

    for rel in sorted(collect(theirs)):
        t, b, o = norm(theirs / rel), norm(base / rel), norm(ours / rel)
        if o is None:
            missing.append(rel)
            continue
        if o == t:
            same += 1
            continue
        if b is not None and o == b:
            lost.append(rel)          # 上游改了，本地还是旧内容
        else:
            local.append(rel)         # 本地改动，预期
    extra = sorted(collect(ours) - collect(theirs))

    print(f"与上游一致 / identical to theirs: {same}")
    print(f"上游改动丢失 / LOST (must fix): {len(lost)}")
    for rel in lost:
        print(f"  LOST     {rel}")
    print(f"上游有本地无 / MISSING: {len(missing)}")
    for rel in missing:
        print(f"  MISSING  {rel}")
    print(f"本地改动 / LOCAL: {len(local)}")
    if not args.quiet:
        for rel in local:
            print(f"  local    {rel}")
    print(f"本地新增 / EXTRA: {len(extra)}")
    if not args.quiet:
        for rel in extra:
            print(f"  new      {rel}")
    return 1 if lost else 0


if __name__ == "__main__":
    sys.exit(main())
