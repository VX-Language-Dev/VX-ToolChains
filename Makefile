.PHONY: all build test lint clean install help

# VX-ToolKit Makefile
# 使用 Go 实现的工具链

GO := go
BUILD_DIR := bin
CMD_DIR := go/cmd
INTERNAL_DIR := go/internal

BINS := vxc vlnk vpm vxde vdlnk vxlsp vfmt

all: build test

build: $(addprefix $(BUILD_DIR)/,$(BINS))

$(BUILD_DIR)/%: $(CMD_DIR)/*/main.go
	@mkdir -p $(BUILD_DIR)
	$(GO) build -ldflags "-s -w" -trimpath -o $@ $(CMD_DIR)$*/main.go

test:
	cd go && $(GO) test ./internal/... -v -race -coverprofile=coverage.out
	cd go && $(GO) tool cover -func=coverage.out | tail -1

lint:
	@which golangci-lint > /dev/null || (echo "Installing golangci-lint..." && $(GO) install github.com/golangci/golangci-lint/cmd/golangci-lint@latest)
	cd go && golangci-lint run --config ../.golangci.yml ./...

fmt:
	cd go && $(GO) fmt ./...
	cd go && $(GO)imports -local github.com/vx-lang/vxc -w .

check: fmt lint test

clean:
	rm -rf $(BUILD_DIR)
	rm -f go/coverage.out
	rm -rf .zig-cache zig-out

install: build
	install -d ~/.local/bin
	for bin in $(BINS); do \
		install $(BUILD_DIR)/$$bin ~/.local/bin/; \
	done

uninstall:
	for bin in $(BINS); do \
		rm -f ~/.local/bin/$$bin; \
	done

benchmark:
	cd go && $(GO) test ./internal/... -bench=. -benchmem -run=^$$

race:
	cd go && $(GO) test ./internal/... -race

help:
	@echo "VX-ToolKit 构建系统"
	@echo ""
	@echo "可用命令:"
	@echo "  make all         - 构建并运行所有测试"
	@echo "  make build       - 编译所有工具"
	@echo "  make test        - 运行单元测试（含覆盖率）"
	@echo "  make lint        - 运行 golangci-lint 代码检查"
	@echo "  make fmt         - 格式化代码"
	@echo "  make check       - 完整检查（格式化 + lint + 测试）"
	@echo "  make clean       - 清理构建产物"
	@echo "  make install     - 安装到 ~/.local/bin/"
	@echo "  make uninstall   - 从系统卸载"
	@echo "  make benchmark    - 运行性能基准测试"
	@echo "  make race        - 运行竞态检测"
	@echo "  make help        - 显示此帮助信息"
	@echo ""
	@echo "工具列表: $(BINS)"