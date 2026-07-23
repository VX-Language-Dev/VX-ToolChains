const std = @import("std");

pub fn build(b: *std.Build) void {
    _ = b;
    // VX-ToolKit 已迁移到 Go 实现
    // Zig 构建系统已废弃，仅保留此文件以兼容旧版本
    //
    // 请使用以下命令构建：
    //   cd go && go build -o ../bin/ ./cmd/vxc/main.go
    //   cd go && go build -o ../bin/ ./cmd/vlnk/main.go
    //   cd go && go build -o ../bin/ ./cmd/vpm/main.go
    //   cd go && go build -o ../bin/ ./cmd/vxde/main.go
    //   cd go && go build -o ../bin/ ./cmd/vxlsp/main.go
    //   cd go && go build -o ../bin/ ./cmd/vfmt/main.go
    //   cd go && go build -o ../bin/ ./cmd/vdlnk/main.go
    //
    // 或使用 Makefile（如果存在）:
    //   make all
}
