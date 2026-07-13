const std = @import("std");
const Allocator = std.mem.Allocator;

pub fn main() !void {
    const stdout = std.io.getStdOut().writer();

    // VX Language Server Protocol (LSP) 实现
    //
    // 功能列表:
    //   - 文档诊断 (diagnostics)
    //   - 自动补全 (completion)
    //   - 悬停提示 (hover)
    //   - 跳转定义 (goto definition)
    //   - 文档符号 (document symbols)
    //   - 内联提示 (inlay hints)
    //
    // 通信协议: JSON-RPC 2.0 over stdin/stdout

    var gpa = std.heap.GeneralPurposeAllocator(.{}){};
    defer _ = gpa.deinit();
    const allocator = gpa.allocator();

    // 初始化 LSP 后端
    const backend = @import("lsp/backend.zig").VxLspBackend.init(allocator);
    defer backend.deinit();

    try stdout.print("VX Language Server started\n", .{});

    // 主循环：读取 stdin，处理 JSON-RPC 请求
    // (这里应该实现完整的 JSON-RPC 消息循环)

    // 简化版本：仅输出启动信息后退出
    try stdout.writeAll("VX LSP server initialized\n");
}
