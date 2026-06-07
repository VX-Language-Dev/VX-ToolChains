use std::env;
use std::fs;
use std::process;

use vx_vm::VM;

fn main() {
    let exe_path = env::current_exe().expect("[Runtime] 无法获取自身路径");

    let file_data = fs::read(&exe_path).expect("[Runtime] 无法打开自身EXE");

    let file_size = file_data.len();
    if file_size < 8 {
        return;
    }

    // 读取末尾的payload大小
    let payload_size_bytes = &file_data[file_size - 8..file_size];
    let payload_size = u64::from_le_bytes(payload_size_bytes.try_into().unwrap());

    if payload_size > 0 && (payload_size as usize) <= file_size - 8 {
        let stub_size = file_size - (payload_size as usize) - 8;
        let bytecode = &file_data[stub_size..stub_size + (payload_size as usize)];

        let mut vm = VM::new();
        match vm.load_module(bytecode) {
            Ok(_) => match vm.run() {
                Ok(_) => {
                    // println!("[Runtime] 执行完成");
                }
                Err(e) => {
                    eprintln!("[Runtime Error] {}", e);
                    process::exit(1);
                }
            },
            Err(e) => {
                eprintln!("[Runtime Error] 加载失败: {}", e);
                process::exit(1);
            }
        }
    } else {
        // 无附加字节码，静默返回
    }
}
