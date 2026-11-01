#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
upload_github.py —— 一键将当前项目强制覆盖推送到 GitHub 仓库（清空原内容）。

用法:
    python upload_github.py                 # 默认推送到 oouu00/Metalio-S31-kiikoread-xiugai 的 main 分支
    python upload_github.py --dry           # 只暂存+提交，不推送（用于本地自检）
    python upload_github.py --url <giturl> --branch <branch> --message "<msg>"

行为说明:
    1. 若当前目录不是 git 仓库，则 git init。
    2. 配置 core.autocrlf=false（与上游 LF 一致），并设置提交身份。
    3. 配置 origin 远程地址（已存在则更新）。
    4. git add -A 暂存全部（遵守 .gitignore，因此 build/、managed_components/、*.a、*.bin 等不会上传）。
    5. 若有变更则提交。
    6. 切换到 --branch（默认 main）。
    7. git push --force origin <branch>：用本地内容完整覆盖远程，等价于“清空原仓库再上传”。

注意:
    - 推送需要 GitHub 身份。本机用 Git Credential Manager(HTTPS)；若未登录会在 push 时失败，
      请先在本机任意目录执行一次 `git push` 完成浏览器/令牌登录，再重跑本脚本。
    - 本脚本强制直连 GitHub，绕过本机不稳定的本地代理（曾返回 502）。
"""

import os
import sys
import argparse
import subprocess

# 默认值：目标仓库与分支
REPO_URL = "https://github.com/oouu00/Metalio-S31-kiikoread-xiugai.git"
DEFAULT_BRANCH = "main"
PROJECT_DIR = os.path.dirname(os.path.abspath(__file__))


def run(cmd, check=True, capture=True):
    """执行命令；强制直连 GitHub（移除本机代理环境变量）。"""
    env = dict(os.environ)
    for k in ("HTTP_PROXY", "HTTPS_PROXY", "http_proxy", "https_proxy", "ALL_PROXY", "all_proxy"):
        env.pop(k, None)
    env["NO_PROXY"] = "*"
    print(f"\n$ {' '.join(cmd)}")
    r = subprocess.run(cmd, cwd=PROJECT_DIR, env=env, capture_output=capture, text=True)
    if capture:
        if r.stdout and r.stdout.strip():
            print(r.stdout.strip())
        if r.stderr and r.stderr.strip():
            print(r.stderr.strip())
    if check and r.returncode != 0:
        raise RuntimeError(f"命令失败 (exit {r.returncode}): {' '.join(cmd)}")
    return r


def git(*args, **kw):
    return run(["git"] + list(args), **kw)


def ensure_identity():
    name = git("config", "user.name", check=False, capture=True).stdout.strip()
    email = git("config", "user.email", check=False, capture=True).stdout.strip()
    if not name:
        git("config", "user.name", os.environ.get("GIT_USER", "zhuyue"))
    if not email:
        git("config", "user.email", os.environ.get("GIT_EMAIL", "zhuyue@local"))


def main():
    ap = argparse.ArgumentParser(description="一键强制覆盖推送当前项目到 GitHub")
    ap.add_argument("--url", default=REPO_URL, help="远程仓库 HTTPS 地址")
    ap.add_argument("--branch", default=DEFAULT_BRANCH, help="目标分支（默认 main）")
    ap.add_argument("--message", default="chore: overwrite repo with rc107.1 full source",
                    help="提交说明")
    ap.add_argument("--dry", action="store_true", help="只提交不推送")
    args = ap.parse_args()

    # 1. init
    if not os.path.isdir(os.path.join(PROJECT_DIR, ".git")):
        git("init", "-q")
        print("已 git init")

    # 2. config
    git("config", "core.autocrlf", "false")
    git("config", "core.safecrlf", "false")
    ensure_identity()

    # 3. remote
    res = git("remote", "get-url", "origin", check=False, capture=True)
    if res.returncode == 0 and res.stdout.strip():
        git("remote", "set-url", "origin", args.url)
        print(f"更新 origin -> {args.url}")
    else:
        git("remote", "add", "origin", args.url)
        print(f"添加 origin -> {args.url}")

    # 4. stage
    git("add", "-A")
    out = git("diff", "--cached", "--name-only", capture=True).stdout
    files = [l for l in out.splitlines() if l.strip()]
    print(f"已暂存 {len(files)} 个文件")

    # 5. commit (仅当有变更)
    status = git("status", "--porcelain", capture=True).stdout.strip()
    if status:
        git("commit", "-q", "-m", args.message)
        print("已提交")
    else:
        print("无新变更，跳过提交")

    # 6. 切换到目标分支
    git("checkout", "-B", args.branch)

    # 7. push
    if args.dry:
        print("[dry] 已跳过推送")
        return

    print(f"\n⚠️ 即将强制推送并覆盖远程 {args.branch}（清空远程原内容）...")
    git("push", "--force", "origin", args.branch)
    print("✅ 完成：已上传并覆盖远程仓库。")


if __name__ == "__main__":
    try:
        main()
    except RuntimeError as e:
        print(f"\n❌ 失败: {e}", file=sys.stderr)
        sys.exit(1)
