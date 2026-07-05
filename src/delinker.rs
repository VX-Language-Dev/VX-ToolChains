// ==================== VX De-Linker ====================
//
// 功能: 从原生可执行文件中提取嵌入的 VXOBJ v4 数据。
//
// 工作流程:
//   1. 扫描可执行文件中的 VXOBJ magic 字节 ("VXOBJ")
//   2. 找到后尝试解析 VXOBJ v4 容器
//   3. 提取各段数据
//   4. 可选的: 将提取的 VXOBJ 保存为 .vxobj 文件
//
// 前提: 链接器 (vxlinker) 已将 VXOBJ 数据嵌入到可执行文件的 .vxobj section 中。
//
// 嵌入方式:
//   - ELF (Linux): 在链接时通过 objcopy 添加 .vxobj section
//   - Mach-O (macOS): 通过 ld 的 -sectcreate 选项
//   - PE (Windows): 通过链接器指令添加 .vxobj section
//
// 为简化实现，当前采用 "尾部追加" 策略:
//   链接器在生成可执行文件后，将完整的 VXOBJ v4 数据追加到文件末尾，
//   并在文件末尾写入一个 8 字节的偏移量标记（指向 VXOBJ 数据起始位置）。
//
// 文件格式:
//   [可执行文件内容]
//   [VXOBJ v4 数据] (自描述，包含 magic + version)
//   [8 bytes: offset from end to VXOBJ start] (小端 u64)
//   [4 bytes: magic "VXOB"]

use std::io::{Read, Seek, SeekFrom};

use crate::bytecode::VxObjV4Container;

/// 用于在可执行文件末尾查找 VXOBJ 数据的尾部标记
const TRAILER_MAGIC: &[u8; 4] = b"VXOB";
const TRAILER_SIZE: u64 = 12; // 8 bytes offset + 4 bytes magic

/// 从原生可执行文件中提取嵌入的 VXOBJ v4 容器
pub fn extract_vxobj_from_executable(path: &str) -> Result<VxObjV4Container, String> {
    let mut file = std::fs::File::open(path)
        .map_err(|e| format!("无法打开文件: {}", e))?;

    let file_len = file.metadata()
        .map_err(|e| format!("获取文件大小失败: {}", e))?
        .len();

    if file_len < TRAILER_SIZE {
        return Err("文件太小，不包含 VXOBJ 尾部标记".to_string());
    }

    // 读取尾部标记
    file.seek(SeekFrom::End(-(TRAILER_SIZE as i64)))
        .map_err(|e| format!("文件定位失败: {}", e))?;

    let mut trailer = [0u8; 12];
    file.read_exact(&mut trailer)
        .map_err(|e| format!("读取尾部标记失败: {}", e))?;

    // 验证 magic
    let magic = &trailer[8..12];
    if magic != TRAILER_MAGIC {
        return Err(format!(
            "未找到 VXOBJ 尾部标记 (期望 {:02x?}, 实际 {:02x?})",
            TRAILER_MAGIC, magic
        ));
    }

    // 读取偏移量 (小端 u64)
    let offset = u64::from_le_bytes([
        trailer[0], trailer[1], trailer[2], trailer[3],
        trailer[4], trailer[5], trailer[6], trailer[7],
    ]);

    if offset as u64 >= file_len {
        return Err(format!("无效的 VXOBJ 偏移量: {}", offset));
    }

    // 读取 VXOBJ 数据
    let vxobj_size = (file_len - TRAILER_SIZE) - offset;
    file.seek(SeekFrom::Start(offset))
        .map_err(|e| format!("文件定位到 VXOBJ 数据失败: {}", e))?;

    let mut vxobj_data = vec![0u8; vxobj_size as usize];
    file.read_exact(&mut vxobj_data)
        .map_err(|e| format!("读取 VXOBJ 数据失败: {}", e))?;

    // 解析 VXOBJ v4 容器
    let container = VxObjV4Container::parse(&vxobj_data)
        .map_err(|e| format!("VXOBJ 解析失败: {}", e))?;

    Ok(container)
}

/// 将 VXOBJ 容器保存为文件
pub fn save_vxobj(container: &VxObjV4Container, output_path: &str) -> Result<(), String> {
    let mut file = std::fs::File::create(output_path)
        .map_err(|e| format!("创建文件失败: {}", e))?;

    container.write(&mut file)
        .map_err(|e| format!("写入 VXOBJ 失败: {}", e))?;

    Ok(())
}

/// 将 VXOBJ 数据追加到可执行文件末尾
pub fn append_vxobj_to_executable(exec_path: &str, vxobj_data: &[u8]) -> Result<(), String> {
    let mut file = std::fs::OpenOptions::new()
        .append(true)
        .open(exec_path)
        .map_err(|e| format!("打开文件失败: {}", e))?;

    let current_len = file.metadata()
        .map_err(|e| format!("获取文件大小失败: {}", e))?
        .len();

    // 写入 VXOBJ 数据
    file.write_all(vxobj_data)
        .map_err(|e| format!("写入 VXOBJ 数据失败: {}", e))?;

    // 写入尾部标记: offset (小端 u64) + magic
    let offset_bytes = current_len.to_le_bytes();
    file.write_all(&offset_bytes)
        .map_err(|e| format!("写入偏移量失败: {}", e))?;
    file.write_all(TRAILER_MAGIC)
        .map_err(|e| format!("写入尾部 magic 失败: {}", e))?;

    file.flush()
        .map_err(|e| format!("刷新文件失败: {}", e))?;

    Ok(())
}

use std::io::Write;

/// 显示 VXOBJ 容器的摘要信息
pub fn print_container_info(container: &VxObjV4Container) {
    println!("VXOBJ v4 容器信息:");
    println!("  目标平台: {}", container.header.target_triple);
    println!("  Flags: {:#010b}", container.header.flags);
    if container.has_external_deps() {
        println!("  外部依赖: 是");
    }
    println!("  段列表:");
    for sec in &container.header.sections {
        println!("    {:12} {} 字节", sec.name, sec.size);
    }
}

/// 从指定路径提取 VXOBJ 并保存为文件
pub fn extract_and_save(exec_path: &str, output_path: &str) -> Result<(), String> {
    let container = extract_vxobj_from_executable(exec_path)?;
    print_container_info(&container);
    save_vxobj(&container, output_path)?;
    println!("VXOBJ 已保存到: {}", output_path);
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::bytecode::VxObjV4Container;

    #[test]
    fn test_roundtrip() {
        // 创建一个临时文件
        let dir = std::env::temp_dir();
        let exec_path = dir.join("vx_delinker_test.bin");
        let exec_str = exec_path.to_string_lossy().to_string();

        // 创建可执行文件内容
        let exec_content = b"\x7fELF...some executable content here...";
        std::fs::write(&exec_path, exec_content).unwrap();

        // 创建 VXOBJ 容器
        let mut container = VxObjV4Container::new("x86_64-unknown-linux-gnu");
        container.set_section("TypeIR", vec![0, 1, 2, 3]);
        container.set_section("Debug", b"test debug".to_vec());

        // 序列化 VXOBJ
        let mut vxobj_data = Vec::new();
        container.write(&mut vxobj_data).unwrap();

        // 追加到可执行文件
        append_vxobj_to_executable(&exec_str, &vxobj_data).unwrap();

        // 提取 VXOBJ
        let extracted = extract_vxobj_from_executable(&exec_str).unwrap();
        assert_eq!(extracted.header.target_triple, "x86_64-unknown-linux-gnu");
        assert!(extracted.get_section("TypeIR").is_some());
        assert_eq!(extracted.get_section("TypeIR").unwrap(), &vec![0, 1, 2, 3]);
        assert_eq!(extracted.get_section("Debug").unwrap(), b"test debug");

        // 清理
        std::fs::remove_file(&exec_path).unwrap();
    }
}