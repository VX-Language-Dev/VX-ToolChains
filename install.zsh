ls -la
echo "正在安装 VX 工具包"
cmake -G Ninja -S . -B build
cd build
ninja
mkdir -p ~/.local/bin
cp ./tools/vxls/vxls ~/.local/bin/vxls
cp ./tools/vx-tidy/vx-tidy ~/.local/bin/vx-tidy
cp ./tools/vmake/vmake ~/.local/bin/vmake
echo "VX 工具包安装完成"
