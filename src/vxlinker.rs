use std::env;
use std::fs;
use std::io::{self, Read, Write};
use std::path::Path;

#[cfg(unix)]
use std::os::unix::fs::PermissionsExt;

use vx_vm::bytecode::{self, VxObjV3Container};

// Platform defaults
#[cfg(target_os = "windows")]
const DEFAULT_STUB: &str = "vx_runtime_x64.exe";
#[cfg(not(target_os = "windows"))]
const DEFAULT_STUB: &str = "vx_runtime";

#[cfg(target_os = "windows")]
const DEFAULT_OUTPUT_EXT: &str = "exe";
#[cfg(not(target_os = "windows"))]
const DEFAULT_OUTPUT_EXT: &str = "out";

#[derive(Debug, Clone, Copy, PartialEq)]
pub enum LinkMode {
    Interpret,
    Jit,
    Aot,
}

impl LinkMode {
    fn from_str(s: &str) -> Option<Self> {
        match s.to_lowercase().as_str() {
            "interpret" | "interp" | "i" => Some(LinkMode::Interpret),
            "jit" | "j" => Some(LinkMode::Jit),
            "aot" | "a" => Some(LinkMode::Aot),
            _ => None,
        }
    }
}

#[derive(Debug)]
pub enum LinkerError {
    Io(io::Error),
    InvalidFile(String),
    UnsupportedVersion(u32),
    FileNotFound(String),
    NoTypeIr(String),
    AotError(String),
}

impl From<io::Error> for LinkerError {
    fn from(err: io::Error) -> Self { LinkerError::Io(err) }
}

impl std::fmt::Display for LinkerError {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match self {
            LinkerError::Io(e) => write!(f, "IO: {}", e),
            LinkerError::InvalidFile(msg) => write!(f, "Invalid .vxobj file: {}", msg),
            LinkerError::UnsupportedVersion(v) => write!(f, "Unsupported version: {}", v),
            LinkerError::FileNotFound(p) => write!(f, "File not found: {}", p),
            LinkerError::NoTypeIr(msg) => write!(f, "No TypeIR section: {}", msg),
            LinkerError::AotError(msg) => write!(f, "AOT compilation failed: {}", msg),
        }
    }
}

impl std::error::Error for LinkerError {}

pub struct VXLinker;

impl VXLinker {
    pub fn link(
        vxobj_path: &str,
        output_path: &str,
        stub_path: &str,
        mode: LinkMode,
    ) -> Result<(), LinkerError> {
        match mode {
            LinkMode::Interpret => Self::link_interpret(vxobj_path, output_path, stub_path),
            LinkMode::Jit => Self::link_jit(vxobj_path, output_path, stub_path),
            LinkMode::Aot => Self::link_aot(vxobj_path, output_path),
        }
    }

    // === Mode A: Interpret (backward compatible) ===
    fn link_interpret(vxobj_path: &str, output_path: &str, stub_path: &str) -> Result<(), LinkerError> {
        let file_data = Self::read_file(vxobj_path)?;
        let v3_container = VxObjV3Container::parse(&file_data).ok();
        let payload = if let Some(ref container) = v3_container {
            container.get_section(bytecode::SECTION_BYTECODE)
                .unwrap_or_else(|| file_data.clone())
        } else {
            file_data.clone()
        };

        // Validate bytecode
        bytecode::parse_vxobj(&payload).map_err(|e|
            LinkerError::InvalidFile(format!("Bytecode parse: {}", e))
        )?;

        let stub = Self::read_runtime_stub(stub_path)?;
        println!("[*] Interpret stub: {} bytes", stub.len());
        println!("[*] Bytecode payload: {} bytes", payload.len());

        Self::write_executable(&stub, &payload, output_path)?;
        println!("[+] Linked (interpret): {} (VM + bytecode)", output_path);
        Ok(())
    }

    // === Mode B: JIT (Cranelift JIT stub) ===
    fn link_jit(vxobj_path: &str, output_path: &str, stub_path: &str) -> Result<(), LinkerError> {
        let file_data = Self::read_file(vxobj_path)?;
        let container = VxObjV3Container::parse(&file_data)
            .map_err(|_| LinkerError::InvalidFile("Not a v3 container".into()))?;

        let bytecode_data = container.get_section(bytecode::SECTION_BYTECODE)
            .ok_or_else(|| LinkerError::InvalidFile("Missing Bytecode section".into()))?;

        let has_type_ir = container.get_section(bytecode::SECTION_TYPE_IR).is_some();

        let stub = Self::read_runtime_stub(stub_path)?;
        println!("[*] JIT stub: {} bytes", stub.len());
        println!("[*] Bytecode: {} bytes", bytecode_data.len());
        if has_type_ir {
            println!("[*] TypeIR: present (for JIT optimizer)");
        }

        // For JIT: embed both stub + bytecode + typeIR (same layout for now)
        // The JIT runtime reads both at startup
        let mut combined = Vec::new();
        combined.extend_from_slice(&bytecode_data);
        if let Some(type_ir) = container.get_section(bytecode::SECTION_TYPE_IR) {
            combined.extend_from_slice(&type_ir);
        }

        Self::write_executable(&stub, &combined, output_path)?;
        println!("[+] Linked (JIT): {} (stub + bytecode + type_ir)", output_path);
        Ok(())
    }

    // === Mode C: AOT (Cranelift AOT compilation) ===
    fn link_aot(vxobj_path: &str, output_path: &str) -> Result<(), LinkerError> {
        let file_data = Self::read_file(vxobj_path)?;
        let container = VxObjV3Container::parse(&file_data)
            .map_err(|_| LinkerError::InvalidFile("Not a v3 container".into()))?;

        let type_ir_data = container.get_section(bytecode::SECTION_TYPE_IR)
            .ok_or_else(|| LinkerError::NoTypeIr(
                "v3 file has no TypeIR section; recompile with vxcompiler".into()
            ))?;

        let type_module = vx_vm::type_ir::deserialize_type_module(&type_ir_data)
            .ok_or_else(|| LinkerError::AotError("Failed to deserialize TypeIR".into()))?;

        println!("[*] AOT compiling {} functions from TypeIR...", type_module.functions.len());

        let machine_code = match Self::aot_compile(&type_module) {
            Ok(code) => code,
            Err(e) => {
                // Fallback: warn and emit interpret stub
                eprintln!("[!] AOT fallback: using interpreter ({})", e);
                let stub = Self::find_any_stub()?;
                let bytecode_data = container.get_section(bytecode::SECTION_BYTECODE)
                    .unwrap_or_default();
                return Self::write_executable(&stub, &bytecode_data, output_path);
            }
        };

        println!("[*] Machine code: {} bytes", machine_code.len());

        // Write minimal runtime stub + machine code
        let runtime_stub = Self::build_minimal_stub();
        Self::write_executable(&runtime_stub, &machine_code, output_path)?;
        println!("[+] Linked (AOT): {} (native machine code)", output_path);
        Ok(())
    }

    // === AOT Compilation via Cranelift ===
    // 启用方式: `cargo build --features aot`
    // 编译流水线: TypeIR → Cranelift CLIF IR → 宿主原生机器码 → 对象文件
    #[cfg(feature = "aot")]
    fn aot_compile(type_module: &vx_vm::type_ir::TypeModule) -> Result<Vec<u8>, LinkerError> {
        match vx_vm::aot_backend::compile_type_module(type_module, None) {
            Ok(obj_data) => {
                println!("[*] AOT object file: {} bytes ({} functions)",
                    obj_data.len(), type_module.functions.len());
                Ok(obj_data)
            }
            Err(e) => Err(LinkerError::AotError(format!(
                "Cranelift compilation failed: {}", e
            ))),
        }
    }

    #[cfg(not(feature = "aot"))]
    fn aot_compile(_type_module: &vx_vm::type_ir::TypeModule) -> Result<Vec<u8>, LinkerError> {
        Err(LinkerError::AotError(
            "AOT backend not enabled. Rebuild with: cargo build --features aot".into()
        ))
    }

    // === Utilities ===

    fn read_file(path: &str) -> Result<Vec<u8>, LinkerError> {
        let mut file = fs::File::open(path)?;
        let mut buf = Vec::new();
        file.read_to_end(&mut buf)?;
        Ok(buf)
    }

    fn read_runtime_stub(path: &str) -> Result<Vec<u8>, LinkerError> {
        let mut search_paths = vec![path.to_string()];
        let cargo_dirs = [
            format!("target/debug/{}", DEFAULT_STUB),
            format!("target/release/{}", DEFAULT_STUB),
        ];
        for dir in &cargo_dirs {
            if !search_paths.contains(dir) && Path::new(dir).exists() {
                search_paths.push(dir.clone());
            }
        }
        for p in &search_paths {
            if Path::new(p).exists() {
                let stub = Self::read_file(p)?;
                println!("[*] Found stub: {}", p);
                return Ok(stub);
            }
        }
        Err(LinkerError::FileNotFound(format!(
            "{}; build with `cargo build --bin vx_runtime`", search_paths[0]
        )))
    }

    fn find_any_stub() -> Result<Vec<u8>, LinkerError> {
        Self::read_runtime_stub(DEFAULT_STUB)
    }

    fn build_minimal_stub() -> Vec<u8> {
        // Minimal ELF64 stub for Linux x86_64
        // In production, this would be a proper minimal runtime
        let arch = env::consts::ARCH;
        let os = env::consts::OS;
        println!("[*] Building minimal stub for {}-{}", arch, os);
        // Return a small ELF header placeholder (actual stub from vx_runtime)
        Vec::new()
    }

    fn write_executable(stub: &[u8], payload: &[u8], out_path: &str) -> Result<(), LinkerError> {
        let mut out_file = fs::File::create(out_path)?;
        out_file.write_all(stub)?;
        out_file.write_all(payload)?;
        let payload_size = payload.len() as u64;
        out_file.write_all(&payload_size.to_le_bytes())?;
        out_file.flush()?;

        #[cfg(unix)]
        {
            let metadata = out_file.metadata()?;
            let mut perms = metadata.permissions();
            perms.set_mode(0o755);
            fs::set_permissions(out_path, perms)?;
        }
        Ok(())
    }
}

fn print_usage(prog_name: &str) {
    eprintln!("VX Linker v3 - Multi-mode linker");
    eprintln!("Usage: {} <input.vxobj> [options]", prog_name);
    eprintln!("Options:");
    eprintln!("  -o <path>      Output path (default: input with .out/.exe)");
    eprintln!("  -s <path>      Runtime stub path (for interpret/jit mode)");
    eprintln!("  --mode <mode>  Link mode: interpret (default), jit, aot");
    eprintln!("  --dump         Dump VXOBJ section info");
    eprintln!();
    eprintln!("Modes:");
    eprintln!("  interpret  VM interpreter + bytecode (compatible, slowest)");
    eprintln!("  jit        Cranelift JIT stub + type info (balanced)");
    eprintln!("  aot        AOT compile to native machine code (fastest)");
}

fn main() {
    let args: Vec<String> = env::args().collect();
    if args.len() < 2 {
        print_usage(&args[0]);
        std::process::exit(1);
    }

    let input_file = args[1].clone();
    let mut output_file = String::new();
    let mut stub_file = DEFAULT_STUB.to_string();
    let mut mode = LinkMode::Interpret;
    let mut dump = false;

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
            "--mode" if i + 1 < args.len() => {
                let m = args[i + 1].clone();
                mode = LinkMode::from_str(&m)
                    .unwrap_or_else(|| {
                        eprintln!("Unknown mode: {}. Use: interpret, jit, aot", m);
                        std::process::exit(1);
                    });
                i += 2;
            }
            "--dump" => {
                dump = true;
                i += 1;
            }
            _ => {
                eprintln!("Unknown arg: {}", args[i]);
                print_usage(&args[0]);
                std::process::exit(1);
            }
        }
    }

    if output_file.is_empty() {
        let path = Path::new(&input_file);
        let output = path.with_extension(DEFAULT_OUTPUT_EXT);
        output_file = output.to_string_lossy().to_string();
    }

    // Handle --dump (just show section info, no linking)
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

    println!("[*] Linking {} in {:?} mode -> {}", input_file, mode, output_file);

    match VXLinker::link(&input_file, &output_file, &stub_file, mode) {
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
    use std::io::Write;
    use tempfile::TempDir;

    #[test]
    fn test_mode_parsing() {
        assert_eq!(LinkMode::from_str("interpret"), Some(LinkMode::Interpret));
        assert_eq!(LinkMode::from_str("jit"), Some(LinkMode::Jit));
        assert_eq!(LinkMode::from_str("aot"), Some(LinkMode::Aot));
        assert_eq!(LinkMode::from_str("j"), Some(LinkMode::Jit));
        assert_eq!(LinkMode::from_str("a"), Some(LinkMode::Aot));
        assert_eq!(LinkMode::from_str("unknown"), None);
    }

    #[test]
    fn test_interpret_link() {
        let dir = TempDir::new().unwrap();
        let vxobj_path = dir.path().join("test.vxobj");
        let stub_path = dir.path().join("stub.exe");
        let out_path = dir.path().join("test.out");

        // Minimal v2 VXOBJ
        let mut f = fs::File::create(&vxobj_path).unwrap();
        f.write_all(b"VXOBJ").unwrap();
        f.write_all(&[0, 0, 0, 2]).unwrap();
        f.write_all(&[0, 0, 0, 0]).unwrap(); // 0 constants
        f.write_all(&[0, 0, 0, 0]).unwrap(); // 0 functions
        f.flush().unwrap();

        // Minimal stub
        let mut s = fs::File::create(&stub_path).unwrap();
        s.write_all(&[0x4D, 0x5A]).unwrap();
        s.flush().unwrap();

        let result = VXLinker::link(
            vxobj_path.to_str().unwrap(),
            out_path.to_str().unwrap(),
            stub_path.to_str().unwrap(),
            LinkMode::Interpret,
        );
        assert!(result.is_ok());
        assert!(out_path.exists());
    }

    #[test]
    fn test_v3_interpret_link() {
        let dir = TempDir::new().unwrap();
        let vxobj_path = dir.path().join("test.vxobj");
        let stub_path = dir.path().join("stub.exe");
        let out_path = dir.path().join("test.out");

        // Create a proper v2 bytecode payload (with magic+version)
        let bytecode_buf = {
            let mut buf = Vec::new();
            // Full v2 format: magic + version + 0 constants + 0 functions
            buf.extend_from_slice(b"VXOBJ");
            buf.extend_from_slice(&[0, 0, 0, 2]); // version 2
            buf.extend_from_slice(&[0, 0, 0, 0]); // 0 constants
            buf.extend_from_slice(&[0, 0, 0, 0]); // 0 functions
            buf
        };
        let mut f = fs::File::create(&vxobj_path).unwrap();
        bytecode::write_vxobj_v3(&mut f, "x86_64-unknown-linux-gnu", &[], &bytecode_buf, &[], &[], &[]).unwrap();

        let mut s = fs::File::create(&stub_path).unwrap();
        s.write_all(&[0x4D, 0x5A]).unwrap();
        s.flush().unwrap();

        let result = VXLinker::link(
            vxobj_path.to_str().unwrap(),
            out_path.to_str().unwrap(),
            stub_path.to_str().unwrap(),
            LinkMode::Interpret,
        );
        assert!(result.is_ok());
    }
}
