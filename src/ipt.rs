// VX Language Compiler CLI — Stub
//
// 编译器核心已迁移到 Zig (src-zig/src/vxc.zig)。
// 此 Rust 占位符仅保留以维持 Cargo 构建通过，实际功能请使用 Zig 构建产物。
//
// TODO: 待 Zig vxc.zig 实现完整后，从 Cargo.toml [[bin]] 中移除此入口。

use std::env;
use std::process;

fn main() {
    let args: Vec<String> = env::args().collect();
    let prog = args.first().map(|s| s.as_str()).unwrap_or("vxc");

    eprintln!("[VX Deprecation] {} (Rust) is deprecated.", prog);
    eprintln!("  The compiler has been migrated to Zig.");
    eprintln!("  Please use the Zig build: zig build run-vxc");
    eprintln!("  Or run the Zig binary directly: zig-out/bin/vxc <input.vx>");
    process::exit(1);
}
