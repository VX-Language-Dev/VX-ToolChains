// ==================== 内置链接器主入口 ====================
// 将 TypeIR AOT 编译产生的机器码段直接链接为原生可执行文件
// 不依赖任何外部工具 (ld/link.exe/cc)
//
// 平台分发:
//   - Linux:   ELF64 (builtin_linker/elf.rs)
//   - macOS:   Mach-O 64 (builtin_linker/macho.rs)
//   - Windows: PE32+ (builtin_linker/pe.rs)
//
// 使用方式:
//   builtin_linker::link_direct(&text, &rodata, &data, bss_size,
//                                entry_offset, output_path, arch)

mod elf;
mod macho;
mod pe;

use std::path::Path;

/// 将机器码段直接链接为原生可执行文件
///
/// # 参数
/// - `text`: .text 段机器码 (已包含 _start 入口)
/// - `rodata`: .rodata 段只读数据
/// - `data`: .data 段读写数据
/// - `bss_size`: .bss 段大小 (未初始化数据, 初始为 0)
/// - `entry_offset`: _start 函数在 text 段内的偏移量
/// - `output_path`: 输出可执行文件路径
/// - `arch`: 目标架构 (如 "x86_64", "aarch64")
pub fn link_direct(
    text: &[u8],
    rodata: &[u8],
    data: &[u8],
    bss_size: u64,
    entry_offset: u64,
    output_path: &str,
    arch: &str,
) -> Result<(), Box<dyn std::error::Error>> {
    #[cfg(target_os = "linux")]
    {
        elf::link_elf(text, rodata, data, bss_size, entry_offset, output_path, arch)?;
    }

    #[cfg(target_os = "macos")]
    {
        macho::link_macho(text, rodata, data, bss_size, entry_offset, output_path, arch)?;
    }

    #[cfg(target_os = "windows")]
    {
        pe::link_pe(text, rodata, data, bss_size, entry_offset, output_path, arch)?;
    }

    // 设置可执行权限
    #[cfg(unix)]
    {
        use std::fs;
        use std::os::unix::fs::PermissionsExt;
        let metadata = fs::metadata(output_path)?;
        let mut perms = metadata.permissions();
        perms.set_mode(0o755);
        fs::set_permissions(output_path, perms)?;
    }

    Ok(())
}

/// 解析架构字符串为 object crate 的 Architecture 枚举
fn arch_to_object(arch: &str) -> object::Architecture {
    match arch {
        "x86_64" | "amd64" | "x64" => object::Architecture::X86_64,
        "aarch64" | "arm64" => object::Architecture::Aarch64,
        "riscv64" => object::Architecture::Riscv64,
        s if s.starts_with("armv7") => object::Architecture::Arm,
        _ => {
            // 默认 x86_64
            eprintln!("[builtin_linker] Unknown arch '{}', defaulting to x86_64", arch);
            object::Architecture::X86_64
        }
    }
}

/// 判断架构是否为小端序
fn arch_endianness(_arch: &str) -> object::Endianness {
    // 目前所有支持的架构都是小端
    object::Endianness::Little
}
