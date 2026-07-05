// ==================== VX De-Linker CLI ====================
//
// 从原生可执行文件中提取嵌入的 VXOBJ v4 数据。
//
// 用法:
//   vxdelinker <executable> [-o output.vxobj]
//   vxdelinker <executable> --info        (仅显示 VXOBJ 信息)
//
// 工作原理:
//   1. 扫描可执行文件尾部，查找 VXOBJ 数据标记
//   2. 提取 VXOBJ v4 容器
//   3. 保存为 .vxobj 文件或显示信息
//
// 前提: 可执行文件必须是通过 vxlinker --embed-vxobj 生成的。

use std::env;
use std::fs;
use std::process;

use vx_vm::delinker;

fn print_usage(prog_name: &str) {
    eprintln!("VX De-Linker v4 - Extract VXOBJ from native executables");
    eprintln!("Usage: {} <executable> [options]", prog_name);
    eprintln!("Options:");
    eprintln!("  -o <path>      Output .vxobj path (default: input with .vxobj)");
    eprintln!("  --info         Show VXOBJ container info only");
    eprintln!("  --decompile    Extract and decompile in one step");
    eprintln!();
    eprintln!("Examples:");
    eprintln!("  {} test.out                    # → test.vxobj", prog_name);
    eprintln!("  {} test.out -o test.vxobj      # → test.vxobj", prog_name);
    eprintln!("  {} test.out --info             # show container info", prog_name);
    eprintln!("  {} test.out --decompile        # extract + decompile to .vx", prog_name);
    eprintln!();
    eprintln!("Note: Only works with executables linked with '--embed-vxobj' flag.");
}

fn main() {
    let args: Vec<String> = env::args().collect();
    if args.len() < 2 {
        print_usage(&args[0]);
        process::exit(1);
    }

    let input = &args[1];
    let mut output = String::new();
    let mut info_only = false;
    let mut decompile = false;

    let mut i = 2;
    while i < args.len() {
        match args[i].as_str() {
            "-o" if i + 1 < args.len() => {
                output = args[i + 1].clone();
                i += 2;
            }
            "--info" => {
                info_only = true;
                i += 1;
            }
            "--decompile" => {
                decompile = true;
                i += 1;
            }
            _ => {
                eprintln!("Unknown argument: {}", args[i]);
                print_usage(&args[0]);
                process::exit(1);
            }
        }
    }

    // 从可执行文件提取 VXOBJ
    let container = match delinker::extract_vxobj_from_executable(input) {
        Ok(c) => c,
        Err(e) => {
            eprintln!("[Error] De-linking failed: {}", e);
            eprintln!();
            eprintln!("Make sure the executable was linked with '--embed-vxobj' flag.");
            process::exit(1);
        }
    };

    // 仅显示信息
    if info_only {
        delinker::print_container_info(&container);
        return;
    }

    // 确定输出路径
    if output.is_empty() {
        output = input.to_string();
        // 替换扩展名
        if let Some(dot) = output.rfind('.') {
            let ext = &output[dot..];
            if ext == ".out" || ext == ".exe" || ext == "" {
                output.replace_range(dot.., ".vxobj");
            } else {
                output.push_str(".vxobj");
            }
        } else {
            output.push_str(".vxobj");
        }
    }

    // 保存 VXOBJ 文件
    let vxobj_data = {
        let mut buf = Vec::new();
        container.write(&mut buf).map_err(|e| {
            eprintln!("[Error] Failed to serialize VXOBJ: {}", e);
            process::exit(1);
        }).ok();
        buf
    };

    match fs::write(&output, &vxobj_data) {
        Ok(_) => {
            // no status output
        }
        Err(e) => {
            eprintln!("[Error] Failed to write '{}': {}", output, e);
            process::exit(1);
        }
    }

    // 可选的: 提取后自动反编译
    if decompile {
        let vx_output = output.replacen(".vxobj", ".vx", 1);
        match vx_vm::decompiler::Decompiler::decompile_file(&output) {
            Ok(source) => {
                match fs::write(&vx_output, &source) {
                    Ok(_) => {
                        // no status output
                    }
                    Err(e) => {
                        eprintln!("[Error] Failed to write decompiled source '{}': {}", vx_output, e);
                    }
                }
            }
            Err(e) => {
                eprintln!("[Error] Decompilation failed: {}", e);
            }
        }
    }
}