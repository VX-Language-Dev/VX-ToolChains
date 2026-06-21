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

# ── 1. 格式 ────────────────────────────────────────
step "cargo fmt — 格式检查"
if cargo fmt -- --check 2>/dev/null; then
  ok "格式正确"
else
  fail "格式问题 — 运行 'cargo fmt' 修复"
fi

# ── 2. 静态分析 ────────────────────────────────────
step "cargo clippy — 静态分析"
if cargo clippy -- -D warnings 2>/dev/null; then
  ok "clippy 通过"
else
  fail "clippy 报错 — 修复警告后重试"
fi

# ── 3. 测试 ────────────────────────────────────────
step "cargo test — 基础"
if cargo test --lib 2>/dev/null; then
  ok "基础测试通过"
else
  fail "基础测试失败"
fi

step "cargo test — AOT"
if cargo test --features aot 2>/dev/null; then
  ok "AOT 测试通过"
else
  fail "AOT 测试失败"
fi

# ── 4. 构建 ────────────────────────────────────────
step "cargo build — Release"
if cargo build --release 2>/dev/null; then
  ok "Release 构建成功"
else
  fail "Release 构建失败"
fi

step "cargo build — Release + AOT"
if cargo build --release --features aot 2>/dev/null; then
  ok "AOT Release 构建成功"
else
  fail "AOT Release 构建失败"
fi

# ── 5. 产物验证 ────────────────────────────────────
step "验证二进制产物"
for bin in vxcompiler vxlinker vx_runtime vpm vx-lsp vxdbg; do
  path="target/release/${bin}"
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
