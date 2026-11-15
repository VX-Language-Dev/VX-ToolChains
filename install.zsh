#!/usr/bin/env zsh
# VX-ToolKit install script — builds and installs all tools to ~/.local/bin
set -euo pipefail

echo "Building VX-ToolKit..."
cmake -G Ninja -S . -B build -DCMAKE_BUILD_TYPE=MinSizeRel
cmake --build build --config MinSizeRel

echo "Installing to ~/.local/bin..."
mkdir -p ~/.local/bin
cp ./build/vxc ~/.local/bin/vxc
cp ./build/vlnk ~/.local/bin/vlnk
cp ./build/tools/vmake/vmake ~/.local/bin/vmake
cp ./build/tools/vmake-lsp/vmake-lsp ~/.local/bin/vmake-lsp
cp ./build/tools/vfmt/vfmt ~/.local/bin/vfmt
cp ./build/tools/vx/vx ~/.local/bin/vx
# Install import-library headers next to the binaries: vxc resolves
# @imp/@cppimp libraries from, in order: $VX_HEADERS_DIR/$VX_HEADERS,
# ./headers (project root, same priority as vmake), <executable_dir>/headers
# and its parent directories (extra search dirs via -I)
if [ -d headers ]; then
    rm -rf ~/.local/bin/headers
    cp -R headers ~/.local/bin/headers
fi
# Install prebuilt runtime lib (vlnk links it directly; fallback is
# recompiling the embedded vxrt.c which is slower)
if [ -f build/libvxrt.a ]; then
    mkdir -p ~/.local/lib
    cp build/libvxrt.a ~/.local/lib/libvxrt.a
fi
# Install shell completions (optional)
if [ -d completions ]; then
    mkdir -p ~/.local/share/bash-completion/completions
    cp completions/*.bash ~/.local/share/bash-completion/completions/ 2>/dev/null || true
    mkdir -p ~/.zsh/completions
    cp completions/_* ~/.zsh/completions/ 2>/dev/null || true
fi
echo "VX-ToolKit installed successfully."
echo "Ensure ~/.local/bin is in your PATH."
