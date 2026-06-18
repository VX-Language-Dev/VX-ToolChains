use std::env;
use std::fs;
use std::process;

use vx_vm::VM;

fn main() {
    let exe_path = env::current_exe().expect("[Runtime] 无法获取自身路径");

    let file_data = match fs::read(&exe_path) {
        Ok(d) => d,
        Err(_) => return,
    };

    let file_size = file_data.len();
    if file_size < 8 {
        return;
    }

    // 读取末尾的payload大小
    let payload_size_bytes = &file_data[file_size - 8..file_size];
    let payload_size = u64::from_le_bytes(
        payload_size_bytes
            .try_into()
            .expect("[Runtime] 末尾 8 字节对齐失败"),
    );

    // 校验 payload 范围，避免 OOM / 越界。
    //  - payload_size == 0 表示无附加字节码；
    //  - 强制 payload_size 转换为 usize 不会超过 file_size - 8；
    //  - 8 (大小字段) + payload_size <= file_size。
    let payload_size_usize = match usize::try_from(payload_size) {
        Ok(n) => n,
        Err(_) => {
            eprintln!("[Runtime] 附加字节码大小超出可寻址范围");
            process::exit(1);
        }
    };
    if payload_size == 0 {
        return;
    }
    if payload_size_usize
        .checked_add(8)
        .map(|n| n > file_size)
        .unwrap_or(true)
    {
        eprintln!("[Runtime] 附加字节码大小非法 (payload_size={}, file_size={})", payload_size, file_size);
        process::exit(1);
    }
    let stub_size = file_size - payload_size_usize - 8;
    let bytecode = &file_data[stub_size..stub_size + payload_size_usize];

    let mut vm = VM::new();
    vm.argv = env::args().collect();
    match vm.load_module(bytecode) {
        Ok(_) => match vm.run() {
            Ok(_) => {}
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
}
