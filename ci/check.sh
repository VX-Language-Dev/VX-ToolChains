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
command -v make >/dev/null || { fail "make 未找到 — 请安装 make（CI 构建必需）"; exit 1; }
# e2e 测试依赖 clang（vlnk 链接期调用宿主 clang）；缺失则无法验证后端，视为硬失败
command -v clang >/dev/null || { fail "clang 未找到 — 请安装 clang（vlnk/e2e 测试必需）"; exit 1; }
cmake --version | head -1 | sed 's/^/  /'
make --version | head -1 | sed 's/^/  /'

# 并行度等价于 make -j$(nproc)：nproc 属 coreutils（Linux，或装了 coreutils 的
# macOS）；缺失时回退 sysctl（macOS/BSD），再不行保守用 2。
if command -v nproc >/dev/null 2>&1; then
  NPROC="$(nproc)"
else
  NPROC="$(sysctl -n hw.ncpu 2>/dev/null || echo 2)"
fi

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
GENERATOR="Unix Makefiles"

# 若已有构建目录是其他生成器，需先清理（CMake 不允许同一目录切换生成器）
if [ -f build/CMakeCache.txt ]; then
  cur_gen=$(grep '^CMAKE_GENERATOR:INTERNAL=' build/CMakeCache.txt | cut -d= -f2-)
  if [ -n "$cur_gen" ] && [ "$cur_gen" != "$GENERATOR" ]; then
    step "清理旧构建目录: build (生成器 $cur_gen → $GENERATOR)"
    rm -rf build
  fi
fi

# ── 1. 构建 ──────────────────────────────────────────
step "cmake configure + build ($GENERATOR, make -j${NPROC}, configs=$cmake_configs)"
if cmake -S . -B build -G "$GENERATOR" -DCMAKE_BUILD_TYPE="${CONFIGS[0]}" >/dev/null 2>&1; then
  ok "CMake 配置成功"
else
  fail "CMake 配置失败"
  exit 1
fi

# Unix Makefiles 是单配置生成器：各 config 共用 build/ 产物目录，切换
# CMAKE_BUILD_TYPE 会全量重编。因此倒序构建，收尾自然落在首个 config，
# 下方测试与产物验证直接取用 build/ 中首个 config 的产物。
i=$((${#CONFIGS[@]} - 1))
while [ "$i" -ge 0 ]; do
  config="${CONFIGS[$i]}"
  if cmake -S . -B build -G "$GENERATOR" -DCMAKE_BUILD_TYPE="$config" >/dev/null 2>&1 &&
    make -C build -j"${NPROC}" vxc002 vlnk vx_util_tests vx_diag_tests vx_lexer_tests \
      vx_parser_tests vx_codegen_tests vx_vxobj_tests vx_e2e_tests \
      vx_backend_tests vx_import_tests vx_import_e2e_tests vx_vlnk_e2e_tests \
      vfmt vfmt_tests >/dev/null 2>&1; then
    ok "构建成功 [$config]"
  else
    fail "构建失败 [$config]"
  fi
  i=$((i - 1))
done

# ── 1.5. 代码格式检查 (clang-format，阻断式) ─────────
if command -v clang-format >/dev/null 2>&1; then
  step "clang-format 检查"
  if cmake --build build --target check-format >/dev/null 2>&1; then
    ok "clang-format 通过"
  else
    fail "clang-format 存在差异 — 请执行 'cmake --build build --target format' 后重试"
  fi
else
  printf "  ⏭  clang-format 未安装，跳过格式检查\n"
fi

# ── 2. 测试 ──────────────────────────────────────────
# Unix Makefiles 单配置：产物直接落在 build/ 下。主项目测试直接运行二进制
# （也绕开 ctest 对 tools/ 子项目可执行路径解析的已知问题），
# tools/ 子项目测试可单独构建验证。
config_dir="build"
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
  "$config_dir/tools/vfmt/vfmt_tests"
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
# Unix Makefiles 单配置：产物直接落在 build/ 下
step "验证测试可执行文件 (config=${CONFIGS[0]})"
test_bin="build/vx_util_tests"
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
