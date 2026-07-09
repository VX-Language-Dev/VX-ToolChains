// VX De-Linker CLI — Stub
//
// 反链接器核心已迁移到 Zig (src-zig/src/vdlnk.zig)。
// 此 Rust 占位符仅保留以维持 Cargo 构建通过，实际功能请使用 Zig 构建产物。
//
// TODO: 待 Zig vdlnk.zig 实现完整后，从 Cargo.toml [[bin]] 中移除此入口。

use std::env;
use std::process;

fn main() {
    let args: Vec<String> = env::args().collect();
    let prog = args.first().map(|s| s.as_str()).unwrap_or("vdlnk");

    eprintln!("[VX Deprecation] {} (Rust) is deprecated.", prog);
    eprintln!("  The de-linker has been migrated to Zig.");
    eprintln!("  Please use the Zig build: zig build run-vdlnk");
    eprintln!("  Or run the Zig binary directly: zig-out/bin/vdlnk <executable>");
    process::exit(1);
}
