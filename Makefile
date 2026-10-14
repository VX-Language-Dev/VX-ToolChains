.PHONY: all build test clean install uninstall help

# VX-ToolKit Makefile
# 使用 CMake + Ninja 驱动 C++ 构建系统

BUILD_DIR := build
CMAKE := cmake
NINJA := ninja

all: build test

build:
	$(CMAKE) -S . -B $(BUILD_DIR) -G Ninja -DCMAKE_BUILD_TYPE=MinSizeRel
	$(CMAKE) --build $(BUILD_DIR) --config MinSizeRel

test: build
	cd $(BUILD_DIR) && $(CMAKE) --build . --target test

lint:
	@if which cppcheck > /dev/null 2>&1; then \
		cppcheck --enable=all --inconclusive --std=c++23 src/ include/; \
	else \
		echo "cppcheck 未安装，跳过 lint"; \
	fi

fmt:
	@which clang-format > /dev/null || (echo "Installing clang-format..." && echo "请手动安装 clang-format")
	clang-format -i $$(find src include tests -name '*.cpp' -o -name '*.hpp') 2>/dev/null || true

check: fmt lint test

clean:
	rm -rf $(BUILD_DIR)

install: build
	install -d ~/.local/bin
	install $(BUILD_DIR)/vx_util_tests ~/.local/bin/

uninstall:
	rm -f ~/.local/bin/vx_util_tests

benchmark:
	cd $(BUILD_DIR) && ctest --output-on-failure --verbose

help:
	@echo "VX-ToolKit 构建系统 (C++)"
	@echo ""
	@echo "可用命令:"
	@echo "  make all         - 构建并运行所有测试"
	@echo "  make build       - 编译 Util 层与测试"
	@echo "  make test        - 运行 C++ 单元测试 (ctest)"
	@echo "  make lint        - 运行 cppcheck 代码检查"
	@echo "  make fmt         - 使用 clang-format 格式化代码"
	@echo "  make check       - 完整检查（格式化 + lint + 测试）"
	@echo "  make clean       - 清理构建产物"
	@echo "  make install     - 安装测试可执行文件到 ~/.local/bin/"
	@echo "  make uninstall   - 从系统卸载"
	@echo "  make benchmark    - 运行性能基准测试"
	@echo "  make help        - 显示此帮助信息"
	@echo ""
	@echo "构建方式: cmake -S . -B build -G Ninja && cmake --build build"