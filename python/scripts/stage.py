#!/usr/bin/env python3
"""把 CMake 构建产物复制进 Python 包的 _bin/，供 uv 安装的 wheel 携带工具链。

用法：
    python3 python/scripts/stage.py                      # 使用 ./build
    python3 python/scripts/stage.py -B build-linux-x86_64

约定：
  - 核心产物 vxc / vlnk 缺失即失败（没有它们工具链不可用）
  - 其余工具、libvxrt.a、libvxtidy 动态库存在则复制，缺失只警告
  - headers/ 复制为 _bin/headers —— vxc / vxls 的默认导入库根
    （可用 -I 追加搜索目录，VX_HEADERS 覆盖库根）
"""

from __future__ import annotations

import argparse
import os
import shutil
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
PACKAGE_BIN = REPO / "python" / "src" / "vx_toolkit" / "_bin"

# (包内文件名, 构建目录相对路径, 是否核心)
TOOLS = (
    ("vxc", "vxc", True),
    ("vlnk", "vlnk", True),
    ("vmake", "tools/vmake/vmake", False),
    ("vxls", "tools/vxls/vxls", False),
    ("vmake-lsp", "tools/vmake-lsp/vmake-lsp", False),
    ("vx", "tools/vx/vx", False),
)

# (包内文件名, 构建目录相对路径) —— 运行时资源，缺失可接受
LIBS = (
    ("libvxrt.a", "libvxrt.a"),
    ("libvxtidy.dylib", "tools/vx-tidy/libvxtidy.dylib"),
    ("libvxtidy.so", "tools/vx-tidy/libvxtidy.so"),
)


def _resolve(build_dir: Path, relative: str):
    """找构建产物；同时兼容 Windows 的 .exe 后缀。"""
    plain = build_dir / relative
    if plain.is_file():
        return plain
    with_exe = build_dir / (relative + ".exe")
    if with_exe.is_file():
        return with_exe
    return None


def main() -> int:
    parser = argparse.ArgumentParser(description="把工具链产物装进 Python 包的 _bin/")
    parser.add_argument("-B", "--build-dir", default="build",
                        help="CMake 构建目录（默认: build）")
    args = parser.parse_args()

    build_dir = Path(args.build_dir)
    if not build_dir.is_absolute():
        build_dir = REPO / build_dir
    build_dir = build_dir.resolve()
    if not build_dir.is_dir():
        sys.stderr.write(
            f"error: 构建目录不存在: {build_dir}\n"
            f"  先构建工具链: cmake -G Ninja -B build -S . && cmake --build build\n"
        )
        return 1

    # 重建 _bin，避免残留上一次构建的旧二进制
    if PACKAGE_BIN.exists():
        shutil.rmtree(PACKAGE_BIN)
    PACKAGE_BIN.mkdir(parents=True)

    staged = []
    warnings = []

    for name, relative, required in TOOLS:
        source = _resolve(build_dir, relative)
        if source is None:
            message = f"缺少构建产物: {build_dir / relative}"
            if required:
                sys.stderr.write(f"error: {message}（核心工具，无法打包）\n")
                return 1
            warnings.append(message)
            continue
        target = PACKAGE_BIN / name
        shutil.copy2(source, target)
        staged.append(target)

    for name, relative in LIBS:
        source = _resolve(build_dir, relative)
        if source is None:
            continue
        target = PACKAGE_BIN / name
        shutil.copy2(source, target)
        staged.append(target)

    headers = REPO / "headers"
    if headers.is_dir():
        shutil.copytree(headers, PACKAGE_BIN / "headers")
        staged.append(PACKAGE_BIN / "headers")
    else:
        warnings.append(f"缺少导入库目录: {headers}")

    if os.name != "nt":
        # 仅工具二进制需要执行位；静态库/动态库保持原权限
        for path in staged:
            if path.is_file() and path.name in {name for name, _, _ in TOOLS}:
                path.chmod(path.stat().st_mode | 0o755)

    print(f"已 stage 到 {PACKAGE_BIN}")
    for path in staged:
        label = path.name + ("/" if path.is_dir() else "")
        print(f"  + {label}")
    for message in warnings:
        print(f"  ! {message}")
    missing_optional = [n for n, r, req in TOOLS
                        if not req and _resolve(build_dir, r) is None]
    if missing_optional:
        print(f"  ! 未 stage 的可选工具: {', '.join(missing_optional)}")
    print()
    print("下一步： uv tool install ./python")
    return 0


if __name__ == "__main__":
    sys.exit(main())
