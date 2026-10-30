#!/usr/bin/env bash
# ─────────────────────────────────────────────────────────────────────
# VX-ToolChains 本地 CI 检查脚本
# 在推送之前运行，避免 Gitee CI 排队半天才发现问题
# ─────────────────────────────────────────────────────────────────────
set -euo pipefail
FAIL=0

step()   { printf "\n━━━ [%s] %s ━━━\n" "$(date +%H:%M:%S)" "$*"; }
ok()     { printf "  ✅ %s\n" "$1"; }
fail()   { printf "  ❌ %s\n" "$1"; FAIL=1; }

# ── 0. 前置检查 ────────────────────────────────────
step "检查构建工具"
command -v cmake  >/dev/null || { fail "cmake 未找到 — 请安装 CMake 3.23+"; exit 1; }
command -v ninja >/dev/null || { fail "ninja 未找到 — 请安装 Ninja"; exit 1; }
# e2e 测试依赖 clang（vlnk 链接期调用宿主 clang）；缺失则无法验证后端，视为硬失败
command -v clang >/dev/null || { fail "clang 未找到 — 请安装 clang（vlnk/e2e 测试必需）"; exit 1; }
cmake --version | head -1 | sed 's/^/  /'
ninja --version | sed 's/^/  /'

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT_DIR"

# CMake 列表要求用分号 ';' 分隔；CONFIGS 以空格分隔传入，内部转为分号列表。
# 兼容旧单配置习惯：CONFIGS="MinSizeRel" ./ci/check.sh
CONFIGS_DEFAULT=(MinSizeRel Release Debug RelWithDebInfo)
if [ -n "${CONFIGS:-}" ]; then
  read -ra CONFIGS <<< "$CONFIGS"
else
  CONFIGS=("${CONFIGS_DEFAULT[@]}")
fi
cmake_configs=$(IFS=';'; echo "${CONFIGS[*]}")
GENERATOR="Ninja Multi-Config"

# 若已有构建目录是普通 Ninja 生成器，需先清理（CMake 不允许同一目录切换生成器）
if [ -f build/CMakeCache.txt ]; then
  cur_gen=$(grep '^CMAKE_GENERATOR:INTERNAL=' build/CMakeCache.txt | cut -d= -f2-)
  if [ -n "$cur_gen" ] && [ "$cur_gen" != "$GENERATOR" ]; then
    step "清理旧构建目录: build (生成器 $cur_gen → $GENERATOR)"
    rm -rf build
  fi
fi

# ── 1. 构建 ──────────────────────────────────────────
step "cmake configure + build ($GENERATOR, configs=$cmake_configs)"
if cmake -S . -B build -G "$GENERATOR" \
    -DCMAKE_CONFIGURATION_TYPES="$cmake_configs" >/dev/null 2>&1; then
  ok "CMake 配置成功"
else
  fail "CMake 配置失败"
  exit 1
fi

for config in "${CONFIGS[@]}"; do
  if cmake --build build --config "$config" \
      --target vxc002 vlnk vx_util_tests vx_diag_tests vx_lexer_tests \
      vx_parser_tests vx_codegen_tests vx_vxobj_tests vx_e2e_tests \
      vx_backend_tests vx_import_tests vx_import_e2e_tests vx_vlnk_e2e_tests >/dev/null 2>&1; then
    ok "构建成功 [$config]"
  else
    fail "构建失败 [$config]"
  fi
done

# ── 1.5. 代码格式检查 (clang-format，可选) ─────────
if command -v clang-format >/dev/null 2>&1; then
  step "clang-format 检查"
  if cmake --build build --target check-format >/dev/null 2>&1; then
    ok "clang-format 通过"
  else
    printf "  ⚠️  clang-format 存在差异（当前为建议性检查，不阻断）\n"
  fi
else
  printf "  ⏭  clang-format 未安装，跳过格式检查\n"
fi

# ── 2. 测试 ──────────────────────────────────────────
# 注意：Ninja Multi-Config 下 ctest 对 tools/ 子项目 CTestTestfile 的
# 可执行文件路径解析存在缺陷（会出现 MinSizeRel/MinSizeRel/ 等重复路径），
# 因此主项目测试改为直接运行二进制，tools/ 子项目测试可单独构建验证。
config_dir="build/${CONFIGS[0]}"
main_tests=(
  "$config_dir/vx_util_tests"
  "$config_dir/vx_diag_tests"
  "$config_dir/vx_lexer_tests"
  "$config_dir/vx_parser_tests"
  "$config_dir/vx_codegen_tests"
  "$config_dir/vx_vxobj_tests"
  "$config_dir/vx_e2e_tests"
  "$config_dir/vx_backend_tests"
  "$config_dir/vx_import_tests"
  "$config_dir/vx_import_e2e_tests"
  "$config_dir/vx_vlnk_e2e_tests"
)
step "运行主项目单元/集成测试 (config=${CONFIGS[0]})"
for t in "${main_tests[@]}"; do
  name=$(basename "$t")
  if [ ! -f "$t" ]; then
    fail "测试二进制缺失: $name"; continue
  fi
  if "$t" >/dev/null 2>&1; then
    ok "通过: $name"
  else
    rc=$?
    # vlnk_e2e_tests / import_e2e_tests 未找到 clang 时返回 77 → 记为跳过
    if [ "$rc" -eq 77 ]; then
      ok "跳过: $name (缺少 clang/依赖，退出码 77)"
    else
      fail "失败: $name (退出码 $rc)"
    fi
  fi
done

# ── 3. 产物验证 ────────────────────────────────────
# Ninja Multi-Config 产物放在 per-config 子目录 build/<配置>/ 下
step "验证测试可执行文件 (config=${CONFIGS[0]})"
test_bin="build/${CONFIGS[0]}/vx_util_tests"
if [ -f "$test_bin" ]; then
  size=$(du -h "$test_bin" | cut -f1)
  ok "vx_util_tests (${size})"
else
  fail "vx_util_tests 缺失: $test_bin"
fi

# ── 结果 ────────────────────────────────────────────
echo ""
if [ "$FAIL" -eq 0 ]; then
  echo "🎉 全部检查通过！可以推送了。"
else
  echo "💥 $FAIL 项检查失败，修复后重试。"
  exit 1
fi
