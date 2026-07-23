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
step "检查 go 是否可用"
if command -v go &>/dev/null; then
  ok "go $(go version | grep -oP 'go\S+')"
else
  fail "go 未找到 — 请安装 Go 1.21+"
  exit 1
fi

cd "$(dirname "$0")/../go"

# ── 1. 构建 ──────────────────────────────────────────
step "go build — Debug 构建"
if go build -gcflags "-N -l" -trimpath -o bin/vxc ./cmd/vxc/main.go 2>/dev/null; then
  ok "vxc 构建成功"
else
  fail "vxc 构建失败"
fi

step "go build — Release 构建"
BUILD_SUCCESS=0
for tool in vxc vlnk vdlnk vxde vxlsp vpm vfmt; do
  if go build -ldflags "-s -w" -trimpath -o "bin/${tool}" "./cmd/${tool}/main.go" 2>/dev/null; then
    ok "${tool} 构建成功"
    BUILD_SUCCESS=$((BUILD_SUCCESS + 1))
  else
    fail "${tool} 构建失败"
  fi
done

# ── 2. 测试 ──────────────────────────────────────────
step "go test — 单元测试"
if go test ./internal/... 2>/dev/null; then
  ok "所有测试通过"
else
  fail "测试失败"
fi

# ── 3. 产物验证 ────────────────────────────────────
step "验证二进制产物"
for bin in vxc vlnk vdlnk vxde vxlsp vpm vfmt; do
  path="bin/${bin}"
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
