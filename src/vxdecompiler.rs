// ==================== VX Decompiler CLI ====================
//
// 反编译 VXOBJ v4 文件为可读的 VX 源代码。
//
// 用法:
//   vxdecompiler <input.vxobj> [-o output.vx]
//   vxdecompiler <input.vxobj> --stdout      (输出到 stdout)
//
// 工作原理:
//   1. 解析 VXOBJ v4 容器，提取 TypeIR 段
//   2. 反序列化 TypeModule
//   3. 重建 VX 源码结构（函数、类、控制流、表达式）
//   4. 输出可读的 .vx 源码文件

use std::env;
use std::fs;
use std::process;

fn print_usage(prog_name: &str) {
    eprintln!("VX Decompiler v4 - VXOBJ → VX Source");
    eprintln!("Usage: {} <input.vxobj> [options]", prog_name);
    eprintln!("Options:");
    eprintln!("  -o <path>      Output path (default: input with .vx)");
    eprintln!("  --stdout       Write output to stdout instead of file");
    eprintln!("  --info         Show VXOBJ section info only");
    eprintln!();
    eprintln!("Examples:");
    eprintln!("  {} test.vxobj                  # → test.vx", prog_name);
    eprintln!("  {} test.vxobj -o out.vx        # → out.vx", prog_name);
    eprintln!("  {} test.vxobj --stdout         # → stdout", prog_name);
    eprintln!("  {} test.vxobj --info           # show section info", prog_name);
}

fn main() {
    let args: Vec<String> = env::args().collect();
    if args.len() < 2 {
        print_usage(&args[0]);
        process::exit(1);
    }

    let input = &args[1];
    let mut output = String::new();
    let mut to_stdout = false;
    let mut info_only = false;

    let mut i = 2;
    while i < args.len() {
        match args[i].as_str() {
            "-o" if i + 1 < args.len() => {
                output = args[i + 1].clone();
                i += 2;
            }
            "--stdout" => {
                to_stdout = true;
                i += 1;
            }
            "--info" => {
                info_only = true;
                i += 1;
            }
            _ => {
                eprintln!("Unknown argument: {}", args[i]);
                print_usage(&args[0]);
                process::exit(1);
            }
        }
    }

    // 读取并解析 VXOBJ
    let data = match fs::read(input) {
        Ok(d) => d,
        Err(e) => {
            eprintln!("[Error] Failed to read '{}': {}", input, e);
            process::exit(1);
        }
    };

    let _container = match vx_vm::bytecode::VxObjV4Container::parse(&data) {
        Ok(c) => c,
        Err(e) => {
            eprintln!("[Error] Invalid VXOBJ v4 file: {}", e);
            process::exit(1);
        }
    };

    // 仅显示信息
    if info_only {
        vx_vm::bytecode::dump_section_stats(&data);
        return;
    }

    // 反编译
    let source = match vx_vm::decompiler::Decompiler::decompile_file(input) {
        Ok(s) => s,
        Err(e) => {
            eprintln!("[Error] Decompilation failed: {}", e);
            process::exit(1);
        }
    };

    // 输出
    if to_stdout {
        print!("{}", source);
    } else {
        if output.is_empty() {
            output = input.replacen(".vxobj", ".vx", 1);
            if output == *input {
                output = format!("{}.vx", input);
            }
        }
        match fs::write(&output, &source) {
            Ok(_) => {
                // no status output
            }
            Err(e) => {
                eprintln!("[Error] Failed to write '{}': {}", output, e);
                process::exit(1);
            }
        }
    }
}