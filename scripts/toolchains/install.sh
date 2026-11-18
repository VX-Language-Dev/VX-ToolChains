#!/usr/bin/env bash
# ─────────────────────────────────────────────────────────────────────
# install.sh — VX-ToolKit 安装脚本（随 Zip 分发）
# ─────────────────────────────────────────────────────────────────────
# 用法：
#   chmod +x install.sh
#   ./install.sh              # 安装到 ~/.local（默认）
#   PREFIX=/opt ./install.sh  # 安装到 /opt
#
# 安装内容：
#   $PREFIX/bin/vxc, vlnk, vmake, vmake-lsp, vfmt, vx
#   $PREFIX/lib/libvx_*.a
#   $PREFIX/lib/libvxrt.a        预编译运行时（vlnk 直接链接，缺失时回退内嵌源码现编）
#   $PREFIX/bin/headers/   @imp / @cppimp 导入库（与 vxc 同目录，默认查找根）
#   $PREFIX/std/           VX 标准库源码
#   $PREFIX/share/         shell 补全（bash / zsh）
#
# 头文件查找规则：vxc / vx 默认在各自可执行文件同目录的 headers/ 中查找
# 导入库（-I 可追加搜索目录，VX_HEADERS_DIR 可覆盖库根，VX_HEADERS 为兼容别名），
# 因此 headers 必须与二进制装在同一目录（$PREFIX/bin）。
# ─────────────────────────────────────────────────────────────────────
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PREFIX="${PREFIX:-$HOME/.local}"

BIN_DIR="$PREFIX/bin"
LIB_DIR="$PREFIX/lib"
HEADER_DIR="$BIN_DIR/headers"
STD_DIR="$PREFIX/std"

step() { printf "  → %s\n" "$*"; }
info() { printf "  • %s\n" "$*"; }

echo "VX-ToolKit 安装脚本"
echo "  安装目录: $PREFIX"
echo ""

# ── 前置检查 ────────────────────────────────────────────────────────
if ! command -v clang >/dev/null 2>&1; then
  echo "  ⚠️  未找到 clang — vlnk 链接需要宿主 clang（Xcode CLT / LLVM）"
fi

# ── 创建目录 ────────────────────────────────────────────────────────
mkdir -p "$BIN_DIR" "$LIB_DIR" "$HEADER_DIR" "$STD_DIR"

# ── 安装可执行文件 ──────────────────────────────────────────────────
for bin in vxc vlnk vmake vmake-lsp vfmt vx; do
  if [ -f "$SCRIPT_DIR/$bin" ]; then
    cp "$SCRIPT_DIR/$bin" "$BIN_DIR/"
    chmod +x "$BIN_DIR/$bin"
    step "已安装 $BIN_DIR/$bin"
  fi
done

# ── 安装静态库 ──────────────────────────────────────────────────────
for lib in libvx_util.a libvx_compiler.a libvx_backend.a libvxrt.a; do
  if [ -f "$SCRIPT_DIR/$lib" ]; then
    cp "$SCRIPT_DIR/$lib" "$LIB_DIR/"
    step "已安装 $LIB_DIR/$lib"
  fi
done

# ── 安装资源目录 ────────────────────────────────────────────────────
if [ -d "$SCRIPT_DIR/headers" ]; then
  rm -rf "$HEADER_DIR"
  cp -r "$SCRIPT_DIR/headers" "$BIN_DIR/"
  step "已安装 ${HEADER_DIR}（vxc 默认查找根）"
fi
if [ -d "$SCRIPT_DIR/std" ]; then
  cp -r "$SCRIPT_DIR/std" "$STD_DIR/"
  step "已安装 $STD_DIR/std"
fi

# ── 安装 shell 补全 ─────────────────────────────────────────────────
if [ -d "$SCRIPT_DIR/completions" ]; then
  mkdir -p "$PREFIX/share/bash-completion/completions" "$PREFIX/share/zsh/site-functions"
  cp "$SCRIPT_DIR/completions/"*.bash "$PREFIX/share/bash-completion/completions/" 2>/dev/null || true
  cp "$SCRIPT_DIR/completions/"_*     "$PREFIX/share/zsh/site-functions/" 2>/dev/null || true
  step "已安装补全到 $PREFIX/share（zsh 需将 $PREFIX/share/zsh/site-functions 加入 fpath）"
fi

# ── PATH 提示 ───────────────────────────────────────────────────────
echo ""
echo "✅ 安装完成"
echo ""
echo "  使用方法："
echo "    $BIN_DIR/vxc hello.vx              # 编译为 VXOBJ（-I <dir> 可追加头文件搜索目录）"
echo "    $BIN_DIR/vlnk hello.vxobj -o hello # 链接为原生可执行文件"
echo "    $BIN_DIR/vmake build -B build-out  # VMakeSet.vmake 驱动构建（-I 透传给 vxc）"
echo "    $BIN_DIR/vfmt hello.vx -w          # 格式化 VX 源码（-w 就地改写）"
echo "    $BIN_DIR/vx compile hello.vx       # 元命令（compile/link/build/lsp）"
echo ""
echo "  导入库查找：-I 最优先 → 项目 ./headers/ → $BIN_DIR/headers（VX_HEADERS_DIR 覆盖库根；VX_HEADERS 为兼容别名）"
echo ""
echo "  确保 PATH 包含 ${BIN_DIR}："
echo "    export PATH=\"\$PATH:$BIN_DIR\""
echo ""
echo "  vlnk 需要宿主 clang："
echo "    macOS:  Xcode CLT (xcode-select --install) 或 brew install llvm"
echo "    Linux:  apt install clang / dnf install clang"
