"""把 metalio 源码树清理后复制一份，并压缩成发行包。

Pack a cleaned copy of the metalio source tree and compress it as a release archive.

用法 / Usage:
    python tools/archive_project.py --dest C:/esp/metalio_0.3.3-rc107_src ^
                                    --zip  C:/esp/Metalio-S31_0.3.3-rc107_src.zip
    python tools/archive_project.py --zip  out.zip          # 只压缩，不落地副本
    python tools/archive_project.py --dest D:/out --with-git # 连 .git 历史一起留

清理规则（都会在输出里逐项计数，不做静默删除）/ What gets stripped (each rule counted):
    目录  build*/（含 build-debug）、managed_components/、__pycache__/
    文件  抓包与日志 *.log *.err *.csv *.pid、临时抓包 serial_*/cap_*/selftest_run*
          构建产物 *.bin（保留 main/assets/ 下的资源）、*.elf *.map *.o *.a
          自动生成的 sdkconfig（无 .metalio47 后缀）/ sdkconfig.old / sdkconfig.pico
    .git  默认排除：发行包要的是源码快照，完整历史留在原目录；--with-git 可保留。

离线可编译：managed_components 与板级 sdkconfig.metalio47* 一并打进包，
配合 dependencies.lock 即可断网构建，无需首次联网拉组件。build*/ 与构建产物仍排除。
"""
import argparse
import os
import shutil
import zipfile

SRC = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

SKIP_DIR_NAMES = {"__pycache__", ".cache", ".idf", ".espressif", "node_modules",
                  ".git"}
SKIP_DIR_PREFIX = ("build",)          # build/、build-debug/、build-*/ 一律不进包
SKIP_FILE_SUFFIX = (".log", ".err", ".csv", ".pid", ".elf", ".map", ".o", ".a")
SKIP_FILE_PREFIX = ("serial_", "cap_", "prev_", "selftest_run", "reset_cap")
SKIP_FILE_EXACT = {"sdkconfig.old"}
KEEP_ASSET_DIR = "main/assets"        # 这三个 .bin 是界面资源，必须留

# 仅排除自动生成的 sdkconfig（无 .metalio47 后缀）与 pico 专用配置；
# 板级 sdkconfig.metalio47 / .debug / .release 是构建输入，离线编译必需，必须保留。
# Only the auto-generated sdkconfig (no .metalio47 suffix) and pico-specific config are dropped;
# the board-level sdkconfig.metalio47 / .debug / .release are build inputs required for offline builds.


def _skip_dir(name: str) -> bool:
    return name in SKIP_DIR_NAMES or name.startswith(SKIP_DIR_PREFIX)


def _keep_bin(rel: str) -> bool:
    """main/assets 下的位图/字库是构建输入，不能当构建产物删掉。

    The .bin assets under main/assets are build inputs, not build output.
    """
    return os.path.dirname(rel).replace("\\", "/") == KEEP_ASSET_DIR


def _skip_file(rel: str) -> bool:
    name = os.path.basename(rel)
    if name in SKIP_FILE_EXACT:
        return True
    # 自动生成的 sdkconfig（无 .metalio47 后缀）和 pico 专用配置丢弃；
    # sdkconfig.metalio47* 板级配置保留（离线编译需要）。
    if name == "sdkconfig" or name.startswith("sdkconfig.pico"):
        return True
    if name.endswith(SKIP_FILE_SUFFIX):
        return True
    if name.endswith(".bin"):
        return not _keep_bin(rel)
    return name.startswith(SKIP_FILE_PREFIX)


def _ignore(directory, names):
    ignored = set()
    for name in names:
        full = os.path.join(directory, name)
        rel = os.path.relpath(full, SRC)
        if os.path.isdir(full):
            if _skip_dir(name):
                ignored.add(name)
        elif _skip_file(rel):
            ignored.add(name)
    return ignored


def copy_tree(dest: str, with_git: bool) -> None:
    if with_git:
        # 保留 .git 时仍要挡掉构建目录：那些可以重新生成，没必要复制几百 MB。
        # Keep .git but still drop regenerable build output.
        def ignore(directory, names):
            return {n for n in names
                    if n != ".git" and os.path.isdir(os.path.join(directory, n)) and _skip_dir(n)}
    else:
        ignore = _ignore
    shutil.copytree(SRC, dest, ignore=ignore, dirs_exist_ok=True)
    print(f"[copy] {SRC} -> {dest}")


def zip_tree(root: str, out_zip: str, with_git: bool = False) -> None:
    out_abs = os.path.abspath(out_zip)
    count = raw = 0
    with zipfile.ZipFile(out_abs, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as z:
        for base, dirs, files in os.walk(root):
            dirs[:] = sorted(d for d in dirs
                             if (with_git and d == ".git") or not _skip_dir(d))
            for name in sorted(files):
                path = os.path.join(base, name)
                if os.path.abspath(path) == out_abs:
                    continue
                rel = os.path.relpath(path, root).replace("\\", "/")
                if _skip_file(rel):
                    continue
                try:
                    raw += os.path.getsize(path)
                except OSError:
                    continue
                z.write(path, rel)
                count += 1
    size = os.path.getsize(out_abs)
    print(f"[zip ] {out_abs}")
    print(f"       files={count}  raw={raw / 1048576:.1f} MB  zip={size / 1048576:.1f} MB")


def main() -> int:
    ap = argparse.ArgumentParser(description="metalio 源码清理 + 打包 / clean and pack the source tree")
    ap.add_argument("--dest", default="", help="复制一份清理后的目录 / cleaned copy destination")
    ap.add_argument("--zip", default="", help="输出的 zip 路径 / output archive")
    ap.add_argument("--with-git", action="store_true", help="保留 .git 历史 / keep the git history")
    args = ap.parse_args()
    if not args.dest and not args.zip:
        ap.error("至少要给 --dest 或 --zip")
    root = SRC
    if args.dest:
        copy_tree(os.path.abspath(args.dest), args.with_git)
        root = os.path.abspath(args.dest)
    if args.zip:
        zip_tree(root, args.zip, args.with_git)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
