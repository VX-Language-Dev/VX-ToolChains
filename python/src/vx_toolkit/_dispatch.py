"""vx_toolkit._dispatch —— uv 安装入口：定位真实工具链二进制并 exec。

`uv tool install` 只能装 Python 代码，C++ 工具链本体需要由
`python/scripts/stage.py` 预置到包内 `_bin/`（随 wheel 分发），
或通过环境变量 / PATH 指向已有安装。

二进制查找顺序：
  1. VX_TOOLKIT_BIN —— 显式指定的工具目录（优先，便于指向自定义构建）
  2. 包内 _bin/       —— stage.py 复制的工具链（wheel 自带）
  3. PATH            —— 系统里已安装的同一套工具

vxc / vxls 默认从「可执行文件同目录的 headers/」解析 @imp / @cppimp 库，
因此 stage.py 会把 headers/ 复制到 _bin/headers；`-I` 追加搜索目录，
`VX_HEADERS` 覆盖库根，二者仍可用。
"""

from __future__ import annotations

import os
import sys
from pathlib import Path

# uv 注册的 console script 名称，同时也是二进制文件名
COMMANDS = ("vxc", "vlnk", "vmake", "vxls", "vx-tidy", "vmake-lsp", "vx")

# 包内自带的工具链目录（stage.py 产出）
PACKAGE_BIN = Path(__file__).resolve().parent / "_bin"


def _is_executable(path: Path) -> bool:
    # Windows 上 os.access(X_OK) 对普通文件恒为真，需额外判断后缀
    return path.is_file() and os.access(str(path), os.X_OK)


def _search_dirs():
    """按优先级产出候选二进制目录。"""
    env_dir = os.environ.get("VX_TOOLKIT_BIN", "").strip()
    if env_dir:
        yield Path(env_dir)
    yield PACKAGE_BIN
    for part in os.environ.get("PATH", "").split(os.pathsep):
        if part:
            yield Path(part)


def find_tool(name: str):
    """在候选目录中查找工具二进制，返回路径或 None。"""
    suffix = ".exe" if os.name == "nt" else ""
    for directory in _search_dirs():
        candidate = directory / (name + suffix)
        if _is_executable(candidate):
            return candidate
    return None


def _invoked_name() -> str:
    """从 argv[0] 取命令名（uv 生成的脚本以命令名命名）。"""
    name = Path(sys.argv[0]).name
    if name.lower().endswith(".exe"):
        name = name[: -len(".exe")]
    return name


def _prepend_lib_path(exe: Path) -> None:
    """把二进制同目录加入动态库搜索路径。

    libvxtidy 等动态库随 _bin/ 分发，构建期 rpath 在安装后失效；
    补 DYLD_LIBRARY_PATH（macOS）/ LD_LIBRARY_PATH（Linux）保证加载。
    """
    exe_dir = exe.parent
    has_shared = any(exe_dir.glob("*.dylib")) or any(exe_dir.glob("*.so"))
    if not has_shared:
        return
    for var in ("DYLD_LIBRARY_PATH", "LD_LIBRARY_PATH"):
        current = os.environ.get(var, "")
        parts = [str(exe_dir)] + [p for p in current.split(os.pathsep) if p]
        os.environ[var] = os.pathsep.join(parts)


def _missing_help(name: str) -> str:
    return (
        f"error: 未找到 VX 工具链中的 '{name}'。\n"
        f"  已搜索: VX_TOOLKIT_BIN、{PACKAGE_BIN}、PATH\n"
        f"修复方式（任选其一）：\n"
        f"    1) 从源码构建并装入本包: python3 python/scripts/stage.py -B build\n"
        f"    2) 指向已有安装目录:      export VX_TOOLKIT_BIN=/path/to/bin\n"
        f"    3) 把工具目录加入 PATH\n"
    )


def main() -> int:
    args = sys.argv[1:]
    name = _invoked_name()
    if name not in COMMANDS:
        # 以 `python -m` 或其他方式调用时，允许首个参数指定命令
        if args and args[0] in COMMANDS:
            name, args = args[0], args[1:]
        else:
            sys.stderr.write(
                "error: 无法确定要调用的命令\n"
                "  可用命令: " + ", ".join(COMMANDS) + "\n"
            )
            return 2

    exe = find_tool(name)
    if exe is None:
        sys.stderr.write(_missing_help(name))
        return 127

    _prepend_lib_path(exe)

    if os.name == "nt":
        # Windows 没有 POSIX execv 语义，用子进程转发退出码
        import subprocess

        return subprocess.call([str(exe)] + args)

    os.execv(str(exe), [str(exe)] + args)
    # execv 成功时不会返回
    sys.stderr.write(f"error: 无法执行 {exe}\n")
    return 127
