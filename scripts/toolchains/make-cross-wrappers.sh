#!/usr/bin/env bash
# ─────────────────────────────────────────────────────────────────────
# make-cross-wrappers.sh
# ─────────────────────────────────────────────────────────────────────
# 在 $OUT_DIR 下生成各目标平台的交叉编译器 wrapper 脚本。
#
# 每个 wrapper 是一个 shell 脚本，被 CMake 当作编译器调用，内部把参数
# 转交给 zig c++（或宿主 clang），并固定好 target 三元组与 libc++ 头文件路径。
#
# 用法：
#   OUT_DIR=/tmp/cross-wrappers ./make-cross-wrappers.sh
# ─────────────────────────────────────────────────────────────────────
set -euo pipefail

OUT_DIR="${OUT_DIR:-$PWD}"
ZIG_LIBCXX="/opt/homebrew/Cellar/zig/0.17.0/lib/zig/libcxx/include"

mkdir -p "$OUT_DIR"

# ── Linux 目标（zig c++ + libc++，静态 musl）────────────────────────
# zig 自带 musl libc 与 lld，无需外部 sysroot。
# 目标三元组格式：zig 可识别 <arch>-linux-musl
linux_targets=(
  "zig-cxx-linux-x86_64:x86_64-linux-musl"
  "zig-cxx-linux-aarch64:aarch64-linux-musl"
  "zig-cxx-linux-riscv64:riscv64-linux-musl"
  "zig-cxx-linux-armv7:arm-linux.2.1.0-musleabihf"
)

for entry in "${linux_targets[@]}"; do
  name="${entry%%:*}"
  target="${entry##*:}"
  cat > "$OUT_DIR/$name" <<EOF
#!/usr/bin/env bash
exec zig c++ -target "$target" -I"$ZIG_LIBCXX" "\$@"
EOF
  chmod +x "$OUT_DIR/$name"
  echo "  -> $name ($target)"
done

# ── FreeBSD 目标（zig c++ + musl libc）──────────────────────────────
# zig 提供 musl 实现的 FreeBSD libc（版本较旧，可接受）。
freebsd_targets=(
  "zig-cxx-freebsd-x86_64:x86_64-freebsd.5.1.0-none"
  "zig-cxx-freebsd-aarch64:aarch64-freebsd.11.0.0-none"
)

for entry in "${freebsd_targets[@]}"; do
  name="${entry%%:*}"
  target="${entry##*:}"
  cat > "$OUT_DIR/$name" <<EOF
#!/usr/bin/env bash
exec zig c++ -target "$target" -I"$ZIG_LIBCXX" "\$@"
EOF
  chmod +x "$OUT_DIR/$name"
  echo "  -> $name ($target)"
done

# ── macOS 目标（宿主 Apple clang，Mach-O）────────────────────────────
# macOS 二进制依赖系统 SDK（macOS SDK），由宿主 clang 自动定位；
# 无需 zig。macOS 不做静态链接（系统库动态链接）。
macos_targets=(
  "clang-cxx-macos-x86_64:x86_64-apple-macosx"
  "clang-cxx-macos-aarch64:aarch64-apple-macosx"
)

for entry in "${macos_targets[@]}"; do
  name="${entry%%:*}"
  target="${entry##*:}"
  cat > "$OUT_DIR/$name" <<EOF
#!/usr/bin/env bash
exec clang++ -target "$target" "\$@"
EOF
  chmod +x "$OUT_DIR/$name"
  echo "  -> $name ($target)"
done

echo ""
echo "交叉编译器 wrapper 已生成到: $OUT_DIR"
ls -1 "$OUT_DIR"