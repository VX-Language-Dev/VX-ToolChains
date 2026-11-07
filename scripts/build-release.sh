#!/usr/bin/env bash
# ─────────────────────────────────────────────────────────────────────
# build-release.sh
# ─────────────────────────────────────────────────────────────────────
# 用 CMake + Ninja Multi-Config 交叉构建 VX-ToolKit 的多平台、多配置二进制 Zip 压缩包。
#
# 产出（默认输出到 ./dist/，每个 (目标, 构建类型) 一个 Zip）：
#   dist/vx-toolkit-linux-x86_64-MinSizeRel.zip
#   dist/vx-toolkit-linux-x86_64-Release.zip
#   dist/vx-toolkit-linux-x86_64-Debug.zip
#   dist/vx-toolkit-linux-aarch64-MinSizeRel.zip
#   ... (linux-aarch64 / linux-riscv64 / linux-armv7)
#   ... (freebsd-x86_64 / freebsd-aarch64)
#   ... (macos-x86_64 / macos-aarch64)
#
# 每个 Zip 包含：
#   - vxc, vlnk, vmake, vmake-lsp, vxls, vfmt, vx  （可执行文件）
#   - libvxtidy.dylib/.so （vx-tidy 静态分析引擎，纯动态库、无 CLI）
#   - libvx_util.a, libvx_compiler.a, libvx_backend.a, libvxrt.a
#   - headers/     （@imp / @cppimp 导入库；安装后与二进制同目录，默认查找根）
#   - std/         （VX 标准库源码）
#   - completions/ （bash / zsh 补全）
#   - install.sh   （安装脚本）
#
# 与旧版（单配置 Ninja）最大区别：
#   - 使用 -G "Ninja Multi-Config"，一个构建目录同时产出多种构建类型；
#   - 产物放在 build-<target>/<配置>/ 子目录（Ninja Multi-Config per-config 布局）；
#   - 无需为每个构建类型创建独立的构建目录。
#
# 用法：
#   ./scripts/build-release.sh
#   TARGETS="linux-x86_64 macos-aarch64" ./scripts/build-release.sh
#   CONFIGS="MinSizeRel Release" ./scripts/build-release.sh
#   OUT_DIR=/tmp/dist ./scripts/build-release.sh
# ─────────────────────────────────────────────────────────────────────
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
SCRIPTS_DIR="$ROOT_DIR/scripts/toolchains"
OUT_DIR="${OUT_DIR:-$ROOT_DIR/dist}"
TARGETS="${TARGETS:-linux-x86_64 linux-aarch64 linux-riscv64 linux-armv7 freebsd-x86_64 freebsd-aarch64 macos-aarch64 macos-x86_64}"
# CMake 列表要求用分号 ';' 分隔；此处先按空格拆成数组（兼容用户以空格分隔的 CONFIGS 环境变量），
# 传给 CMake 时再用 ';' 连接（CMAKE_CONFIGURATION_TYPES 须为分号列表）。
CONFIGS_DEFAULT=(MinSizeRel Release Debug RelWithDebInfo)
if [ -n "${CONFIGS:-}" ]; then
  read -ra CONFIGS <<< "$CONFIGS"
else
  CONFIGS=("${CONFIGS_DEFAULT[@]}")
fi
cmake_configs=$(IFS=';'; echo "${CONFIGS[*]}")
GENERATOR="Ninja Multi-Config"

# ── 前置检查 ────────────────────────────────────────────────────────
step() { printf "\n━━━ %s ━━━\n" "$*"; }
die()  { printf "❌ %s\n" "$*" >&2; exit 1; }

command -v cmake  >/dev/null || die "cmake 未找到"
command -v ninja >/dev/null || die "ninja 未找到"
command -v zip    >/dev/null || die "zip 未找到"

cmake --version | head -1
ninja --version

# ── 生成交叉编译器 wrapper（输出到 toolchain 同目录） ────────────────
step "生成交叉编译器 wrapper"
OUT_DIR="$SCRIPTS_DIR" "$SCRIPTS_DIR/make-cross-wrappers.sh"

# ── 逐目标构建 ─────────────────────────────────────────────────────
mkdir -p "$OUT_DIR"
cd "$ROOT_DIR"

for target in $TARGETS; do
  toolchain="$SCRIPTS_DIR/toolchain-${target}.cmake"
  [ -f "$toolchain" ] || die "toolchain 未找到: $toolchain"

  build_dir="$ROOT_DIR/build-$target"

  # CMake 不允许在同一构建目录切换生成器；若之前用普通 Ninja 配置过，先清理。
  if [ -f "$build_dir/CMakeCache.txt" ]; then
    cur_gen=$(grep '^CMAKE_GENERATOR:INTERNAL=' "$build_dir/CMakeCache.txt" | cut -d= -f2-)
    if [ -n "$cur_gen" ] && [ "$cur_gen" != "$GENERATOR" ]; then
      step "清理旧构建目录: $build_dir (生成器 $cur_gen → $GENERATOR)"
      rm -rf "$build_dir"
    fi
  fi

  step "交叉配置: $target ($GENERATOR, configs=$CONFIGS)"
  cmake -S . -B "$build_dir" -G "$GENERATOR" \
    -DCMAKE_TOOLCHAIN_FILE="$toolchain" \
    -DCMAKE_CONFIGURATION_TYPES="$cmake_configs" 2>&1 | tail -3

  for config in "${CONFIGS[@]}"; do
    step "交叉构建: $target [$config]"
    # Ninja Multi-Config 下，cmake --build --config 需显式指定 --target（否则会误加载不存在的 build-<config>.ninja）。
    # vx_runtime 产出预编译运行时 libvxrt.a（vlnk 链接期使用，非依赖关系，需显式构建）。
    # vxtidy 引擎动态库随 vxls 依赖自动构建。
    cmake --build "$build_dir" --config "$config" \
      --target vxc002 vlnk vx_runtime vmake vmake-lsp vxls vfmt vx 2>&1 | tail -5

    # ── 打包（按配置产出独立 Zip）─────────────────────────────────
    pkg_dir="$build_dir/_pkg-$config"
    rm -rf "$pkg_dir"
    mkdir -p "$pkg_dir"

    config_dir="$build_dir/$config"  # Ninja Multi-Config per-config 输出目录

    # 可执行文件（vxc002 的 OUTPUT_NAME=vxc，见根 CMakeLists.txt）
    for bin in vxc vlnk; do
      if [ -f "$config_dir/$bin" ]; then
        cp "$config_dir/$bin" "$pkg_dir/"
      fi
    done
    # 工具子项目：Ninja Multi-Config 下可执行文件位于 <build>/tools/<tool>/<config>/
    # （根工程 target 位于 <build>/<config>/，两者布局不同，故两处都探测）
    for tool in vmake vmake-lsp vxls vfmt vx; do
      for cand in "$config_dir/tools/$tool/$tool" "$build_dir/tools/$tool/$config/$tool"; do
        if [ -f "$cand" ]; then
          cp "$cand" "$pkg_dir/"
          break
        fi
      done
    done
    # 静态库（libvxrt.a 为预编译运行时，vlnk 优先直接链接）
    for lib in libvx_util.a libvx_compiler.a libvx_backend.a libvxrt.a; do
      if [ -f "$config_dir/$lib" ]; then
        cp "$config_dir/$lib" "$pkg_dir/"
      fi
    done
    # vx-tidy 动态库（.dylib macOS / .so Linux）
    for ext in dylib so; do
      for cand in "$config_dir/tools/vx-tidy/libvxtidy.$ext" "$build_dir/tools/vx-tidy/$config/libvxtidy.$ext"; do
        if [ -f "$cand" ]; then
          cp "$cand" "$pkg_dir/"
          break
        fi
      done
    done

    # 资源目录
    cp -r "$ROOT_DIR/headers"     "$pkg_dir/"
    cp -r "$ROOT_DIR/std"         "$pkg_dir/"
    cp -r "$ROOT_DIR/completions" "$pkg_dir/"

    # 安装脚本
    cp "$SCRIPTS_DIR/install.sh" "$pkg_dir/"

    zipname="vx-toolkit-${target}-${config}.zip"
    (cd "$pkg_dir" && zip -q -r "$OUT_DIR/$zipname" .)
    rm -rf "$pkg_dir"

    size=$(du -h "$OUT_DIR/$zipname" | cut -f1)
    printf "  ✅ %s (%s)\n" "$zipname" "$size"
  done
done

# ── 汇总 ────────────────────────────────────────────────────────────
echo ""
echo "🎉 构建完成，产物位于: $OUT_DIR"
ls -lh "$OUT_DIR"/*.zip 2>/dev/null || echo "  (无产物)"
