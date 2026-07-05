// ==================== ELF64 可执行文件生成器 ====================
// 使用 object crate 直接构造静态链接的 ELF64 可执行文件
// 不依赖任何外部链接器

use object::write::*;
use object::*;

use super::{arch_endianness, arch_to_object};

/// 生成静态 ELF64 可执行文件
pub fn link_elf(
    text: &[u8],
    rodata: &[u8],
    data: &[u8],
    bss_size: u64,
    entry_offset: u64,
    output_path: &str,
    arch: &str,
) -> Result<(), Box<dyn std::error::Error>> {
    let architecture = arch_to_object(arch);
    let endianness = arch_endianness(arch);

    // ===== 创建 ELF 可执行文件写入器 =====
    let mut obj = Object::new(BinaryFormat::Elf, architecture, endianness);
    obj.set_file_type(FileType::Executable);

    // ===== 定义程序段 (PT_LOAD) =====
    // 代码段: .text + .rodata (R|X)
    let rx_flags = SegmentFlags::Elf {
        p_flags: PF_R | PF_X,
    };
    let rx_segment = obj.add_segment(rx_flags, 0x1000, 0, 0);

    // 数据段: .data + .bss (R|W)
    let rw_flags = SegmentFlags::Elf {
        p_flags: PF_R | PF_W,
    };
    let rw_segment = obj.add_segment(rw_flags, 0x2000, 0, 0);

    // ===== 添加节区 =====

    // .text 节: 代码
    let text_section_id = obj.add_section(rx_segment, b".text".to_vec(), SectionKind::Text);
    obj.append_section_data(text_section_id, text, 16); // 16 字节对齐

    // .rodata 节: 只读数据 (可选)
    let _rodata_section_id = if !rodata.is_empty() {
        let id = obj.add_section(rx_segment, b".rodata".to_vec(), SectionKind::ReadOnlyData);
        obj.append_section_data(id, rodata, 16);
        Some(id)
    } else {
        None
    };

    // .data 节: 初始化数据
    let data_section_id = obj.add_section(rw_segment, b".data".to_vec(), SectionKind::Data);
    obj.append_section_data(data_section_id, data, 16);

    // .bss 节: 未初始化数据
    let _bss_section_id = obj.add_section(rw_segment, b".bss".to_vec(), SectionKind::UninitializedData);
    // BSS 大小通过 append_bss 设置
    if bss_size > 0 {
        obj.append_bss(data_section_id, bss_size, 16)?;
    }

    // ===== 添加符号和入口点 =====

    // 添加 _start 全局符号
    let start_symbol = Symbol {
        name: b"_start".to_vec(),
        value: entry_offset,
        size: 0,
        kind: SymbolKind::Text,
        scope: SymbolScope::Global,
        weak: false,
        section: SymbolSection::Section(text_section_id),
        flags: SymbolFlags::None,
    };
    let start_sym_id = obj.add_symbol(start_symbol);
    obj.set_entry_point(start_sym_id);

    // ===== 写入文件 =====
    let exe_bytes = obj.write()?;
    std::fs::write(output_path, &exe_bytes)?;

    Ok(())
}
