use std::env;
use std::fs;
use std::path::{Path, PathBuf};
use std::process::Command;

use vx_vm::bytecode;
use vx_vm::delinker;

#[derive(Debug, Clone, Copy, PartialEq)]
pub enum LinkMode {
    Native,
}

impl LinkMode {
    fn from_str(s: &str) -> Option<Self> {
        match s.to_lowercase().as_str() {
            "native" | "n" | "static" | "s" => Some(LinkMode::Native),
            _ => None,
        }
    }
}

#[derive(Debug)]
pub enum LinkerError {
    Io(std::io::Error),
    InvalidFile(String),
    VlnkNotFound,
    VlnkFailed(String),
}

impl From<std::io::Error> for LinkerError {
    fn from(err: std::io::Error) -> Self { LinkerError::Io(err) }
}

impl std::fmt::Display for LinkerError {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match self {
            LinkerError::Io(e) => write!(f, "IO: {}", e),
            LinkerError::InvalidFile(msg) => write!(f, "Invalid file: {}", msg),
            LinkerError::VlnkNotFound => write!(f, "VX linker (vxlinker) not found. Set VLNK_PATH or ensure it is in PATH."),
            LinkerError::VlnkFailed(msg) => write!(f, "VX linker failed: {}", msg),
        }
    }
}

impl std::error::Error for LinkerError {}

pub struct VXLinker;

impl VXLinker {
    pub fn link(
        vxobj_path: &str,
        output_path: &str,
        _mode: LinkMode,
        embed_vxobj: bool,
    ) -> Result<(), LinkerError> {
        // 验证输入文件
        let file_data = fs::read(vxobj_path)?;
        bytecode::VxObjV4Container::parse(&file_data)
            .map_err(|e| LinkerError::InvalidFile(format!("Not a valid VXOBJ v4 file: {}", e)))?;

        // 查找 Zig 版 vxlinker
        let vlnk_path = Self::find_vxlinker()?;

        let final_output = if output_path.is_empty() {
            let path = Path::new(vxobj_path);
            let output = path.with_extension("out");
            output.to_string_lossy().to_string()
        } else {
            output_path.to_string()
        };

        // 调用 Zig vxlinker 完成全部链接
        let status = Command::new(&vlnk_path)
            .arg(vxobj_path)
            .arg("-o")
            .arg(&final_output)
            .status()
            .map_err(LinkerError::Io)?;

        if !status.success() {
            return Err(LinkerError::VlnkFailed(
                "vxlinker exited with non-zero status".into(),
            ));
        }

        // 可选：嵌入 VXOBJ 数据供未来反链接使用
        if embed_vxobj {
            if let Err(e) = delinker::append_vxobj_to_executable(&final_output, &file_data) {
                eprintln!("[!] Failed to embed VXOBJ data: {}", e);
            }
        }

        Ok(())
    }

    /// 查找 Zig 版 vxlinker 可执行文件
    fn find_vxlinker() -> Result<PathBuf, LinkerError> {
        // 1. 环境变量 VLNK_PATH
        if let Ok(path) = env::var("VLNK_PATH") {
            let p = Path::new(&path).to_path_buf();
            if p.exists() {
                return Ok(p);
            }
        }

        // 2. 已知安装位置
        let candidates = [
            "/run/media/max4075/DOTNET/VX/Vlnk/zig-out/bin/vxlinker",
            "/tmp/vlnk_build/out/bin/vxlinker",
        ];
        for c in &candidates {
            let p = Path::new(c);
            if p.exists() {
                return Ok(p.to_path_buf());
            }
        }

        // 3. PATH 搜索
        if let Some(path) = env::var("PATH").ok() {
            for dir in path.split(':') {
                for name in &["vxlinker", "vlnk"] {
                    let p = Path::new(dir).join(name);
                    if p.exists() {
                        return Ok(p);
                    }
                }
            }
        }

        Err(LinkerError::VlnkNotFound)
    }
}

fn print_usage(prog_name: &str) {
    eprintln!("VX Linker v4 - Native static linker (Zig Vlnk backend)");
    eprintln!("Usage: {} <input.vxobj> [options]", prog_name);
    eprintln!("Options:");
    eprintln!("  -o <path>         Output path (default: input with .out extension)");
    eprintln!("  --mode <mode>     Link mode: native (default)");
    eprintln!("  --dump            Dump VXOBJ v4 section info");
    eprintln!("  --embed-vxobj     Embed VXOBJ data in output executable (for de-linking)");
    eprintln!();
    eprintln!("Modes:");
    eprintln!("  native     Static compile to native executable via VX native linker (Vlnk)");
}

fn main() {
    let args: Vec<String> = env::args().collect();
    if args.len() < 2 {
        print_usage(&args[0]);
        std::process::exit(1);
    }

    let input_file = args[1].clone();
    let mut output_file = String::new();
    let mut mode = LinkMode::Native;
    let mut dump = false;
    let mut embed_vxobj = false;

    let mut i = 2;
    while i < args.len() {
        match args[i].as_str() {
            "-o" if i + 1 < args.len() => {
                output_file = args[i + 1].clone();
                i += 2;
            }
            "--mode" if i + 1 < args.len() => {
                let m = args[i + 1].clone();
                mode = LinkMode::from_str(&m)
                    .unwrap_or_else(|| {
                        eprintln!("Unknown mode: {}. Use: native", m);
                        std::process::exit(1);
                    });
                i += 2;
            }
            "--dump" => {
                dump = true;
                i += 1;
            }
            "--embed-vxobj" => {
                embed_vxobj = true;
                i += 1;
            }
            _ => {
                eprintln!("Unknown arg: {}", args[i]);
                print_usage(&args[0]);
                std::process::exit(1);
            }
        }
    }

    if dump {
        let data = match fs::read(&input_file) {
            Ok(d) => d,
            Err(e) => {
                eprintln!("Read error: {}", e);
                std::process::exit(1);
            }
        };
        bytecode::dump_section_stats(&data);
        return;
    }

    match VXLinker::link(&input_file, &output_file, mode, embed_vxobj) {
        Ok(_) => {}
        Err(e) => {
            eprintln!("[Error] {}", e);
            std::process::exit(1);
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_mode_parsing() {
        assert_eq!(LinkMode::from_str("native"), Some(LinkMode::Native));
        assert_eq!(LinkMode::from_str("n"), Some(LinkMode::Native));
        assert_eq!(LinkMode::from_str("static"), Some(LinkMode::Native));
        assert_eq!(LinkMode::from_str("unknown"), None);
    }
}
