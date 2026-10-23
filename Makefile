# VX-ToolKit Makefile（C++ 重写）
#
# 实际构建由 CMake + Ninja 驱动，本文件仅封装常用命令：
#   make          默认构建（MinSizeRel）
#   make build    构建全部目标
#   make test     运行全部单元测试
#   make fmt      clang-format 格式化源码
#   make lint     cppcheck 静态分析（若安装了 cppcheck）
#   make check    fmt + build + test 的完整检查流水线
#   make install  安装二进制到 ~/.local/bin
#   make clean    清理构建目录

.PHONY: all build test fmt lint check install clean help

BUILD_DIR ?= build
CMAKE_BUILD_TYPE ?= MinSizeRel
CMAKE_GENERATOR ?= Ninja

ifeq ($(VERBOSE),1)
	CMAKE_BUILD_ARGS += --verbose
endif

all: build

build:
	cmake -S . -B $(BUILD_DIR) -G $(CMAKE_GENERATOR) \
		-DCMAKE_BUILD_TYPE=$(CMAKE_BUILD_TYPE) \
		-DCMAKE_EXPORT_COMPILE_COMMANDS=ON
	cmake --build $(BUILD_DIR) --config $(CMAKE_BUILD_TYPE) $(CMAKE_BUILD_ARGS)

test: build
	cd $(BUILD_DIR) && ctest --output-on-failure --build-config $(CMAKE_BUILD_TYPE)

fmt:
	@command -v clang-format >/dev/null 2>&1 || { \
		echo "error: clang-format 未安装，无法格式化"; exit 1; \
	}
	find src include tests -name '*.cpp' -o -name '*.hpp' | \
		xargs clang-format -i

lint:
	@if command -v cppcheck >/dev/null 2>&1; then \
		cppcheck --enable=all --suppress=missingIncludeSystem \
			-I include --std=c++23 src include tests; \
	else \
		echo "warn: cppcheck 未安装，跳过静态分析"; \
	fi

check: fmt build test lint

install: build
	cmake --install $(BUILD_DIR) --prefix ~/.local

clean:
	rm -rf $(BUILD_DIR)

help:
	@echo "VX-ToolKit Makefile 目标："
	@echo "  build    构建全部目标（CMake + Ninja，$(CMAKE_BUILD_TYPE））"
	@echo "  test     运行 ctest 单元测试"
	@echo "  fmt      clang-format 格式化 src/include/tests"
	@echo "  lint     cppcheck 静态分析（可选）"
	@echo "  check    fmt + build + test + lint 完整检查"
	@echo "  install  安装到 ~/.local/bin"
	@echo "  clean    清理构建目录"
