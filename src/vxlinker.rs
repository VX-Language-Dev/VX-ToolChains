use std::env;
use std::fs;
use std::io::{self, Read, Write};
use std::path::Path;

#[cfg(unix)]
use std::os::unix::fs::PermissionsExt;

use vx_vm::bytecode;

// 平台相关的默认配置
#[cfg(target_os = "windows")]
const DEFAULT_STUB: &str = "vx_runtime_x64.exe";
#[cfg(not(target_os = "windows"))]
const DEFAULT_STUB: &str = "vx_runtime";

#[cfg(target_os = "windows")]
const DEFAULT_OUTPUT_EXT: &str = "exe";
#[cfg(not(target_os = "windows"))]
const DEFAULT_OUTPUT_EXT: &str = "out";

#[derive(Debug)]
pub enum LinkerError {
    IoError(io::Error),
    InvalidFile(String),
    UnsupportedVersion(u32),
    FileNotFound(String),
}

impl From<io::Error> for LinkerError {
    fn from(err: io::Error) -> Self {
        LinkerError::IoError(err)
    }
}

impl std::fmt::Display for LinkerError {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match self {
            LinkerError::IoError(e) => write!(f, "IO错误: {}", e),
            LinkerError::InvalidFile(msg) => write!(f, "无效的 .vxobj 文件: {}", msg),
            LinkerError::UnsupportedVersion(v) => write!(f, "不支持的 .vxobj 版本: {}", v),
            LinkerError::FileNotFound(path) => write!(f, "找不到文件: {}", path),
        }
    }
}

impl std::error::Error for LinkerError {}

pub struct VXLinker;

impl VXLinker {
    /// 将 .vxobj 链接为 x64 可执行文件
    pub fn link(vxobj_path: &str, output_path: &str, stub_path: &str) -> Result<(), LinkerError> {
        // 1. 验证并读取 VXOBJ 载荷
        let payload = Self::read_vxobj_payload(vxobj_path)?;
        println!("[*] 成功加载字节码载荷: {} bytes", payload.len());

        // 2. 验证并读取 x64 存根
        let stub = Self::read_runtime_stub(stub_path)?;
        println!("[*] 成功加载 x64 运行时存根: {} bytes", stub.len());

        // 3. 拼接并生成 EXE (Stub + Payload)
        Self::write_final_executable(&stub, &payload, output_path)?;
        println!("[+] 链接成功生成: {} (x64 架构)", output_path);

        Ok(())
    }

    /// 高效读取整个文件到内存向量
    fn read_file_to_vec<P: AsRef<Path>>(path: P) -> Result<Vec<u8>, LinkerError> {
        let mut file = fs::File::open(&path)?;
        let mut buffer = Vec::new();
        file.read_to_end(&mut buffer)?;
        Ok(buffer)
    }

    /// 读取并验证 VXOBJ 载荷
    fn read_vxobj_payload(path: &str) -> Result<Vec<u8>, LinkerError> {
        let file_data = Self::read_file_to_vec(path)?;

        // 使用 bytecode 模块验证 VXOBJ 格式
        bytecode::parse_vxobj(&file_data).map_err(|e| {
            LinkerError::InvalidFile(format!("VXOBJ 解析失败: {}", e))
        })?;

        // V2 格式：保留完整文件内容（含头部），由运行时 VM 直接解析
        Ok(file_data)
    }

    /// 读取运行时存根（自动搜索 common 构建输出目录）
    fn read_runtime_stub(path: &str) -> Result<Vec<u8>, LinkerError> {
        // 候选搜索路径列表
        let mut search_paths = vec![path.to_string()];

        // 添加 cargo 构建输出目录中的候选路径
        let cargo_dirs = [
            format!("target/debug/{}", DEFAULT_STUB),
            format!("target/release/{}", DEFAULT_STUB),
        ];
        for dir in &cargo_dirs {
            if !search_paths.contains(dir) && Path::new(dir).exists() {
                search_paths.push(dir.clone());
            }
        }

        // 依次尝试每个路径
        for p in &search_paths {
            if Path::new(p).exists() {
                let stub = Self::read_file_to_vec(p)?;
                println!("[*] 找到运行时存根: {}", p);
                return Ok(stub);
            }
        }

        // 所有路径都不存在
        Err(LinkerError::FileNotFound(format!(
            "{};\n请确保 {} 存在（通过 `cargo build --bin vx_runtime` 编译生成）",
            search_paths[0], DEFAULT_STUB
        )))
    }

    /// 写入最终可执行文件
    fn write_final_executable(
        stub: &[u8],
        payload: &[u8],
        out_path: &str,
    ) -> Result<(), LinkerError> {
        let mut out_file = fs::File::create(out_path)?;

        // 写入存根
        out_file.write_all(stub)?;

        // 追加字节码载荷
        out_file.write_all(payload)?;

        // 在文件末尾写入载荷大小 (8 字节 uint64_t)，供运行时解析
        let payload_size = payload.len() as u64;
        out_file.write_all(&payload_size.to_le_bytes())?;

        out_file.flush()?;

        // Unix 平台：设置可执行权限
        #[cfg(unix)]
        {
            let metadata = out_file.metadata()?;
            let mut perms = metadata.permissions();
            perms.set_mode(0o755);
            std::fs::set_permissions(out_path, perms)?;
        }

        Ok(())
    }
}

fn print_usage(prog_name: &str) {
    eprintln!("VX Language Linker (x64) v1.0");
    eprintln!("用法: {} <input.vxobj> [-o output.exe] [-s stub.exe]", prog_name);
    eprintln!("选项:");
    eprintln!("  -o <path>  指定输出的 exe 路径 (默认: 与输入同名)");
    eprintln!("  -s <path>  指定 x64 运行时存根路径 (默认: vx_runtime_x64.exe)");
}

fn main() {
    // 确保 64 位运行环境校验
    if std::mem::size_of::<usize>() != 8 {
        eprintln!("错误: 此链接器必须针对 x64 架构编译");
        std::process::exit(1);
    }

    let args: Vec<String> = env::args().collect();
    if args.len() < 2 {
        print_usage(&args[0]);
        std::process::exit(1);
    }

    let input_file = args[1].clone();
    let mut output_file = String::new();
    let mut stub_file = DEFAULT_STUB.to_string();

    // 简易参数解析
    let mut i = 2;
    while i < args.len() {
        match args[i].as_str() {
            "-o" if i + 1 < args.len() => {
                output_file = args[i + 1].clone();
                i += 2;
            }
            "-s" if i + 1 < args.len() => {
                stub_file = args[i + 1].clone();
                i += 2;
            }
            _ => {
                eprintln!("未知参数: {}", args[i]);
                print_usage(&args[0]);
                std::process::exit(1);
            }
        }
    }

    // 默认输出文件名处理
    if output_file.is_empty() {
        let path = Path::new(&input_file);
        let output = path.with_extension(DEFAULT_OUTPUT_EXT);
        output_file = output.to_string_lossy().to_string();
    }

    match VXLinker::link(&input_file, &output_file, &stub_file) {
        Ok(_) => (),
        Err(e) => {
            eprintln!("[错误] {}", e);
            std::process::exit(1);
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::io::Write;
    use tempfile::TempDir;

    #[test]
    fn test_read_vxobj_payload_v2() {
        // 创建临时的 V2 格式 .vxobj 文件
        let dir = TempDir::new().unwrap();
        let vxobj_path = dir.path().join("test.vxobj");

        let mut file = fs::File::create(&vxobj_path).unwrap();
        // Magic: "VXOBJ"
        file.write_all(b"VXOBJ").unwrap();
        // Version: 2 (大端序)
        file.write_all(&[0, 0, 0, 2]).unwrap();
        // 数量字段必须先于常量数据，否则 parse_vxobj 会触发巨大分配
        // NumConstants: 0
        file.write_all(&[0, 0, 0, 0]).unwrap();
        // NumFunctions: 0
        file.write_all(&[0, 0, 0, 0]).unwrap();
        file.flush().unwrap();

        let result = VXLinker::read_vxobj_payload(vxobj_path.to_str().unwrap());
        assert!(result.is_ok());
        let payload = result.unwrap();
        assert_eq!(&payload[0..5], b"VXOBJ");
        assert_eq!(payload.len(), 5 + 4 + 4 + 4);
    }

    #[test]
    fn test_read_vxobj_payload_invalid_magic() {
        let dir = TempDir::new().unwrap();
        let vxobj_path = dir.path().join("test.vxobj");

        let mut file = fs::File::create(&vxobj_path).unwrap();
        file.write_all(b"INVALID").unwrap();
        file.flush().unwrap();

        let result = VXLinker::read_vxobj_payload(vxobj_path.to_str().unwrap());
        assert!(result.is_err());
    }

    #[test]
    fn test_link_integration() {
        let dir = TempDir::new().unwrap();

        // 创建模拟的 .vxobj 文件 (最小有效格式: magic + version + 0常量 + 0函数)
        let vxobj_path = dir.path().join("test.vxobj");
        let mut vxobj_file = fs::File::create(&vxobj_path).unwrap();
        vxobj_file.write_all(b"VXOBJ").unwrap();
        vxobj_file.write_all(&[0, 0, 0, 2]).unwrap();
        // NumConstants: 0
        vxobj_file.write_all(&[0, 0, 0, 0]).unwrap();
        // NumFunctions: 0
        vxobj_file.write_all(&[0, 0, 0, 0]).unwrap();
        vxobj_file.flush().unwrap();

        // 创建模拟的存根文件
        let stub_path = dir.path().join("stub.exe");
        let mut stub_file = fs::File::create(&stub_path).unwrap();
        stub_file.write_all(&[0x4D, 0x5A]).unwrap(); // MZ header
        stub_file.flush().unwrap();

        let output_path = dir.path().join("output.exe");

        let result = VXLinker::link(
            vxobj_path.to_str().unwrap(),
            output_path.to_str().unwrap(),
            stub_path.to_str().unwrap(),
        );

        assert!(result.is_ok());

        // 验证输出文件
        let output_data = fs::read(output_path).unwrap();
        assert_eq!(&output_data[0..2], &[0x4D, 0x5A]); // MZ header
        // 检查末尾的 payload 大小
        let len = output_data.len();
        let payload_size = u64::from_le_bytes([
            output_data[len - 8],
            output_data[len - 7],
            output_data[len - 6],
            output_data[len - 5],
            output_data[len - 4],
            output_data[len - 3],
            output_data[len - 2],
            output_data[len - 1],
        ]);
        // payload 长度 = magic(5) + version(4) + num_constants(4) + num_functions(4)
        assert_eq!(payload_size as usize, 5 + 4 + 4 + 4);
    }
}
