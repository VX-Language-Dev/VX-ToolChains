#!/usr/bin/env bash
# 编译、链接并运行 examples/ 下的全部示例（需在仓库根目录可写 build/）。
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT_DIR"

VXC="${VXC:-./build/vxc}"
VLNK="${VLNK:-./build/vlnk}"
OUT_DIR="${OUT_DIR:-build/examples}"

for tool in "$VXC" "$VLNK"; do
  if [ ! -x "$tool" ]; then
    echo "缺少 $tool，请先构建：cmake -S . -B build -G Ninja && cmake --build build" >&2
    exit 1
  fi
done

mkdir -p "$OUT_DIR"
for src in examples/*.vx; do
  name="$(basename "$src" .vx)"
  printf '\n── %s ──\n' "$name"
  "$VXC" "$src" -o "$OUT_DIR/$name.vxobj" >/dev/null
  "$VLNK" "$OUT_DIR/$name.vxobj" -o "$OUT_DIR/$name" >/dev/null
  "$OUT_DIR/$name"
done
