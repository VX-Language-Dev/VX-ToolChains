// VX Debugger - CLI 调试器
// 使用 VM 内置的 handle_breakpoint REPL 进行交互调试

use std::io::{self, Write};
use vx_vm::VM;

fn main() {
    let args: Vec<String> = std::env::args().collect();
    if args.len() < 2 {
        eprintln!("用法: vxdbg <bytecode.vxobj>");
        std::process::exit(1);
    }

    let bytecode_path = &args[1];
    let bytecode = std::fs::read(bytecode_path).expect("无法读取字节码文件");

    let mut vm = VM::new();
    vm.load_module(&bytecode).expect("无法加载字节码模块");

    println!("已加载模块，函数列表:");
    for (i, func) in vm.module.functions.iter().enumerate() {
        println!("  [{}] {} ({} 条指令)", i, func.name, func.instructions.len());
    }
    println!();

    // Pre-run REPL: set breakpoints before execution starts
    println!("输入 break <pc> 设置断点，然后输入 continue 开始执行。");
    loop {
        print!("(vxdbg) ");
        io::stdout().flush().unwrap();
        let mut input = String::new();
        if io::stdin().read_line(&mut input).unwrap_or(0) == 0 {
            break;
        }
        let input = input.trim();
        if input.is_empty() {
            continue;
        }

        let parts: Vec<&str> = input.split_whitespace().collect();
        let cmd = parts.first().copied().unwrap_or("");

        match cmd {
            "break" | "b" => {
                if parts.len() < 2 {
                    println!("用法: break <pc>");
                    continue;
                }
                if let Ok(pc) = parts[1].parse::<usize>() {
                    vm.set_breakpoint(pc);
                    println!("在 PC {} 设置断点", pc);
                } else {
                    println!("PC 必须是数字");
                }
            }
            "clear" => {
                if parts.len() < 2 {
                    println!("用法: clear <pc>");
                    continue;
                }
                if let Ok(pc) = parts[1].parse::<usize>() {
                    vm.clear_breakpoint(pc);
                    println!("已清除 PC {} 的断点", pc);
                }
            }
            "list" | "l" => {
                println!("函数列表:");
                for (i, func) in vm.module.functions.iter().enumerate() {
                    println!("  [{}] {} ({} 条指令)", i, func.name, func.instructions.len());
                }
            }
            "continue" | "c" | "run" | "r" => {
                println!("开始执行...");
                let result = vm.run();
                match &result {
                    Ok(val) => println!("执行完成，返回值: {:?}", val),
                    Err(e) => println!("执行错误: {}", e),
                }
                break;
            }
            "help" | "h" => {
                println!("VX Debugger 预运行命令:");
                println!("  break/b <pc> - 设置断点");
                println!("  clear <pc>   - 清除断点");
                println!("  list/l       - 列出函数");
                println!("  run/r        - 开始执行");
                println!("  continue/c   - 开始执行");
                println!("  help/h       - 显示此帮助");
                println!("  quit/q       - 退出");
            }
            "quit" | "q" => {
                break;
            }
            _ => {
                println!("未知命令: {}", cmd);
            }
        }
    }
}