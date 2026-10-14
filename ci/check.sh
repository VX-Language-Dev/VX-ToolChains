#!/usr/bin/env bash
# ─────────────────────────────────────────────────────
# VX-ToolChains 本地 CI 检查脚本
# 在推送之前运行，避免 Gitee CI 排队半天才发现问题
# ─────────────────────────────────────────────────────
set -euo pipefail
FAIL=0

step()   { printf "\n━━━ [%s] %s ━━━\n" "$(date +%H:%M:%S)" "$*"; }
ok()     { printf "  ✅ %s\n" "$1"; }
fail()   { printf "  ❌ %s\n" "$1"; FAIL=1; }

# ── 0. 前置检查 ────────────────────────────────────
step "检查 cmake 是否可用"
if command -v cmake &>/dev/null; then
  ok "cmake $(cmake --version | head -1)"
else
  fail "cmake 未找到 — 请安装 CMake 3.23+"
  exit 1
fi

if command -v ninja &>/dev/null; then
  ok "ninja $(ninja --version)"
else
  fail "ninja 未找到 — 请安装 Ninja"
  exit 1
fi

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT_DIR"

# ── 1. 构建 ──────────────────────────────────────────
step "cmake configure + build"
if cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=MinSizeRel 2>/dev/null \
   && cmake --build build --config MinSizeRel 2>/dev/null; then
  ok "C++ 构建成功"
else
  fail "C++ 构建失败"
fi

# ── 2. 测试 ──────────────────────────────────────────
step "ctest — C++ 单元测试"
if cmake --build build --target test 2>/dev/null; then
  ok "所有测试通过"
else
  fail "测试失败"
fi

# ── 3. 产物验证 ────────────────────────────────────
step "验证测试可执行文件"
if [ -f "build/vx_util_tests" ]; then
  size=$(du -h "build/vx_util_tests" | cut -f1)
  ok "vx_util_tests (${size})"
else
  fail "vx_util_tests 缺失!"
fi

# ── 结果 ────────────────────────────────────────────
echo ""
if [ "$FAIL" -eq 0 ]; then
  echo "🎉 全部检查通过！可以推送了。"
else
  echo "💥 $FAIL 项检查失败，修复后重试。"
  exit 1
fi