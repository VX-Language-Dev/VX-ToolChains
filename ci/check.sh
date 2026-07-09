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
step "检查 zig 是否可用"
if command -v zig &>/dev/null; then
  ok "zig $(zig version)"
else
  fail "zig 未找到 — 请安装 Zig 0.13+"
  exit 1
fi

# ── 1. Zig 格式检查 ────────────────────────────────
step "zig fmt — 格式检查"
if zig fmt --check src-zig/src/ 2>/dev/null; then
  ok "Zig 格式正确"
else
  fail "Zig 格式问题 — 运行 'zig fmt src-zig/src/' 修复"
fi

# ── 2. Zig 测试 ────────────────────────────────────
step "zig build test — 单元测试"
if (cd src-zig && zig build test 2>/dev/null); then
  ok "Zig 测试通过"
else
  fail "Zig 测试失败"
fi

# ── 3. Zig 构建 ────────────────────────────────────
step "zig build — Debug 构建"
if (cd src-zig && zig build 2>/dev/null); then
  ok "Zig Debug 构建成功"
else
  fail "Zig Debug 构建失败"
fi

step "zig build -Doptimize=ReleaseSafe — Release 构建"
if (cd src-zig && zig build -Doptimize=ReleaseSafe 2>/dev/null); then
  ok "Zig Release 构建成功"
else
  fail "Zig Release 构建失败"
fi

# ── 4. Rust 库检查 (保留) ──────────────────────────
step "cargo check — Rust 库编译检查"
if cargo check 2>/dev/null; then
  ok "Rust 库编译通过"
else
  fail "Rust 库编译失败"
fi

step "cargo test — Rust 库测试"
if cargo test 2>/dev/null; then
  ok "Rust 测试通过"
else
  fail "Rust 测试失败"
fi

# ── 5. 产物验证 ────────────────────────────────────
step "验证 Zig 二进制产物"
for bin in vxc vlnk vpm; do
  path="src-zig/zig-out/bin/${bin}"
  if [ -f "$path" ]; then
    size=$(du -h "$path" | cut -f1)
    ok "${bin}  (${size})"
  else
    fail "${bin}  缺失!"
  fi
done

# ── 结果 ────────────────────────────────────────────
echo ""
if [ "$FAIL" -eq 0 ]; then
  echo "🎉 全部检查通过！可以推送了。"
else
  echo "💥 ${FAIL} 项检查失败，修复后重试。"
  exit 1
fi
