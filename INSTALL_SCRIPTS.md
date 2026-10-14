# VX Toolkit 安装说明

本项目使用 Makefile 驱动构建系统，基于 CMake + Ninja。

## 构建工具链

```bash
# 构建 Util 层库与测试
make build

# 或直接使用 CMake
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=MinSizeRel
cmake --build build --config MinSizeRel
```

构建产物位于 `build/` 目录：
- `vx_util_tests` — Util 层单元测试可执行文件

> 注：C++ 版本目前仅实现 Util 层（内存分配器、字符串、文件系统），编译器、链接器、包管理器等工具正在开发中。

## 安装到系统

```bash
# 安装到 ~/.local/bin
make install

# 确保 ~/.local/bin 在 PATH 中
export PATH="$HOME/.local/bin:$PATH"
```

## 卸载

```bash
make uninstall
```

## 运行测试

```bash
make test          # 运行全部 C++ 单元测试
make lint          # 运行 cppcheck 代码检查
make check         # 完整检查（格式化 + lint + 测试）
```

## 清理

```bash
make clean         # 清理构建产物
```

## 开发环境要求

- CMake 3.23+
- Ninja 1.10+
- 支持 C++23 的编译器（Clang 16+、GCC 13+、MSVC 19.35+）