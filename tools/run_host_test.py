"""Run one host test script with the -fsanitize flag dropped.

本机 MinGW 没有 asan/ubsan 运行时库，直接跑会 `ld: cannot find -lasan`；
这个 runner 把 `-fsanitize=address,undefined` 从 subprocess 参数里整体丢掉，其余原样执行。
CI（Linux/GitHub Actions）有 asan，仍然直接 `python3 tools/<name>.py`。

Usage: python tools/run_host_test.py tools/<name>.py
"""
import pathlib
import subprocess as sp
import sys

_real_run = sp.run


def _run(args, **kw):
    if isinstance(args, (list, tuple)):
        args = [a for a in args if a and a != '-fsanitize=address,undefined']
    return _real_run(args, **kw)


sp.run = _run
subprocess = sp

path = sys.argv[1]
sys.path.insert(0, str(pathlib.Path(path).resolve().parent))
src = pathlib.Path(path).read_text(encoding='utf-8')
g = {'__file__': path, '__name__': '__main__'}
try:
    exec(compile(src, path, 'exec'), g)
except SystemExit:
    pass
