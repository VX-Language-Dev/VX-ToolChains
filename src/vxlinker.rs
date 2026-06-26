use std::env;
use std::fs;
use std::io::{self, Read, Write};
use std::path::Path;
use std::process::Command;

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
    /// 原生静态编译模式：直接从 VXCO 生成目标平台的原生可执行文件
    Native,
}

impl LinkMode {
    fn from_str(s: &str) -> Option<Self> {
        match s.to_lowercase().as_str() {
            "interpret" | "interp" | "i" => Some(LinkMode::Interpret),
            "jit" | "j" => Some(LinkMode::Jit),
            "aot" | "a" => Some(LinkMode::Aot),
            "native" | "n" | "static" | "s" => Some(LinkMode::Native),
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
            LinkMode::Native => Self::link_native(vxobj_path, output_path),
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

    // === Mode D: Native Static Compilation ===
    // 从 VXCO 中间文件直接生成目标平台的原生可执行文件
    //
    // 编译流程:
    //   1. 解析 VXCO 文件获取 TypeIR 和外部依赖
    //   2. 使用 Cranelift AOT 将 TypeIR 编译为原生对象文件
    //   3. 调用系统链接器生成最终的可执行文件
    //      - Linux/macOS: 使用 ld 或 cc
    //      - Windows: 使用 link.exe
    //   4. 若存在外部依赖且非可选，则采用动态链接
    //   5. 默认采用静态链接（无外部依赖时）
    fn link_native(vxco_path: &str, output_path: &str) -> Result<(), LinkerError> {
        let file_data = Self::read_file(vxco_path)?;

        // 尝试解析为 VXCO 格式（跨平台中间文件）
        let vxco_container = bytecode::VxcoContainer::parse(&file_data)
            .ok();

        let (type_ir_data, external_dep_names) = if let Some(ref vxco) = vxco_container {
            let deps = if let Some(deps_data) = vxco.get_section(bytecode::VXCO_SECTION_EXTERNAL_DEPS) {
                bytecode::deserialize_external_deps(deps_data)
                    .iter()
                    .map(|d| d.name.clone())
                    .collect::<Vec<_>>()
            } else {
                Vec::new()
            };
            (
                vxco.get_section(bytecode::VXCO_SECTION_TYPE_IR).cloned(),
                deps,
            )
        } else {
            // 兼容模式: 尝试作为 VXOBJ v3 解析（为向后兼容）
            let container = VxObjV3Container::parse(&file_data)
                .map_err(|_| LinkerError::InvalidFile("Not a valid VXCO or VXOBJ file".into()))?;
            (
                container.get_section(bytecode::SECTION_TYPE_IR),
                Vec::new(),
            )
        };

        let type_ir_data = type_ir_data
            .ok_or_else(|| LinkerError::NoTypeIr("No TypeIR section found".into()))?;

        let type_module = vx_vm::type_ir::deserialize_type_module(&type_ir_data)
            .ok_or_else(|| LinkerError::AotError("Failed to deserialize TypeIR".into()))?;

        let dynamic_link = !external_dep_names.is_empty();
        println!("[*] Native compilation: {} functions", type_module.functions.len());
        if dynamic_link {
            println!("[*] External dependencies: {:?} (dynamic linking)", external_dep_names);
        } else {
            println!("[*] External dependencies: none (static linking)");
        }

        // 使用 Cranelift AOT 编译到原生对象文件
        let obj_data = Self::aot_compile(&type_module)?;

        // 创建临时对象文件
        let temp_dir = std::env::temp_dir();
        let obj_file_path = temp_dir.join(format!("vx_native_{}.o", std::process::id()));
        fs::write(&obj_file_path, &obj_data)
            .map_err(|e| LinkerError::Io(e))?;

        // 调用系统链接器生成最终可执行文件
        let linker_result = Self::invoke_system_linker(&obj_file_path, output_path, dynamic_link, &external_dep_names);

        // 清理临时文件
        let _ = fs::remove_file(&obj_file_path);

        linker_result
    }

    /// 调用系统链接器生成最终可执行文件
    ///
    /// 平台适配:
    ///   - Linux: 使用 cc 或 ld
    ///   - macOS: 使用 cc 或 ld
    ///   - Windows: 使用 link.exe
    ///
    /// 链接模式:
    ///   - 静态链接（默认）: 不链接任何动态库，生成完全独立的可执行文件
    ///   - 动态链接: 仅当源代码明确引用外部动态库时采用
    fn invoke_system_linker(
        obj_path: &Path,
        output_path: &str,
        dynamic_link: bool,
        external_dep_names: &[String],
    ) -> Result<(), LinkerError> {
        let obj_path_str = obj_path.to_string_lossy();

        #[cfg(target_os = "windows")]
        {
            // Windows: 使用 link.exe
            let mut cmd = Command::new("link.exe");
            cmd.arg("/OUT:".to_string() + output_path);
            cmd.arg("/NOLOGO");
            
            if !dynamic_link {
                // 静态链接选项
                cmd.arg("/SUBSYSTEM:CONSOLE");
                cmd.arg("/ENTRY:mainCRTStartup");
                cmd.arg("/NODEFAULTLIB");
                cmd.arg("/DYNAMICBASE:NO");
                cmd.arg("/NXCOMPAT:NO");
                cmd.arg("/LTCG");  // 链接时代码生成优化
            } else {
                // 动态链接选项
                cmd.arg("/SUBSYSTEM:CONSOLE");
                cmd.arg("/ENTRY:mainCRTStartup");
            }
            
            // 添加外部动态库依赖
            for dep in external_dep_names {
                let lib_name = if dep.ends_with(".lib") || dep.ends_with(".dll") {
                    dep.clone()
                } else if dep.starts_with("lib") {
                    // 处理 "libc" -> "msvcrt.lib" 等
                    let base = dep.trim_start_matches("lib");
                    format!("{}.lib", base)
                } else {
                    format!("{}.lib", dep)
                };
                cmd.arg(lib_name);
            }
            
            // 添加默认库
            if !dynamic_link {
                cmd.arg("msvcrt.lib");
                cmd.arg("kernel32.lib");
            }
            
            cmd.arg(obj_path_str.as_ref());

            println!("[*] Invoking Windows linker: link.exe ...");

            let output = cmd.output().map_err(|e|
                LinkerError::AotError(format!("Failed to invoke linker: {}", e))
            )?;

            if !output.status.success() {
                let stderr = String::from_utf8_lossy(&output.stderr);
                return Err(LinkerError::AotError(format!(
                    "Windows linker failed: {}", stderr
                )));
            }
        }

        #[cfg(target_os = "macos")]
        {
            // macOS: 使用 cc 或 ld
            let linker = if Path::new("/usr/bin/ld").exists() { "ld" } else { "cc" };
            let mut cmd = Command::new(linker);

            if dynamic_link {
                cmd.arg("-o").arg(output_path);
                cmd.arg(obj_path_str.as_ref());
                // macOS 动态链接需要指定 dylib
                for dep in external_dep_names {
                    let lib_name = if dep.starts_with("lib") {
                        dep.trim_start_matches("lib")
                    } else {
                        dep
                    };
                    cmd.arg(format!("-l{}", lib_name));
                }
                cmd.arg("-lc");
                cmd.arg("-lSystem");  // macOS 系统库
            } else {
                // 静态链接: 生成完全独立的可执行文件
                cmd.arg("-static");
                cmd.arg("-o").arg(output_path);
                cmd.arg(obj_path_str.as_ref());
                cmd.arg("-e");
                cmd.arg("_main"); // 入口点符号
                cmd.arg("-no_pie");  // 非位置无关可执行文件
                cmd.arg("-O2");  // 优化
            }

            println!("[*] Invoking macOS linker: {} ...", linker);

            let output = cmd.output().map_err(|e|
                LinkerError::AotError(format!("Failed to invoke linker: {}", e))
            )?;

            if !output.status.success() {
                let stderr = String::from_utf8_lossy(&output.stderr);
                // 如果静态链接失败，尝试动态链接作为后备
                if !dynamic_link {
                    eprintln!("[!] Static linking failed, trying dynamic linking...");
                    return Self::invoke_system_linker(obj_path, output_path, true, external_dep_names);
                }
                return Err(LinkerError::AotError(format!(
                    "macOS linker failed: {}", stderr
                )));
            }
        }

        #[cfg(target_os = "linux")]
        {
            if dynamic_link {
                // 动态链接模式：使用 cc 以正确链接 C 运行时库
                let mut cmd = Command::new("cc");
                cmd.arg("-o").arg(output_path);
                cmd.arg(obj_path_str.as_ref());
                cmd.arg("-O2");  // 优化
                
                // 添加外部动态库
                for dep in external_dep_names {
                    // 处理库名：如果是 "libc" 则转换为 "c"
                    let lib_name = if dep.starts_with("lib") {
                        dep.trim_start_matches("lib")
                    } else {
                        dep
                    };
                    cmd.arg(format!("-l{}", lib_name));
                }
                
                // 添加默认库
                cmd.arg("-lc");  // C 标准库

                println!("[*] Invoking Linux linker: cc ...");

                let output = cmd.output().map_err(|e|
                    LinkerError::AotError(format!("Failed to invoke linker: {}", e))
                )?;

                if !output.status.success() {
                    let stderr = String::from_utf8_lossy(&output.stderr);
                    return Err(LinkerError::AotError(format!(
                        "Linux linker failed: {}", stderr
                    )));
                }
            } else {
                // 静态链接: 生成完全独立的可执行文件
                // 需要提供 _start 入口点调用 __main__ 并执行 exit syscall
                let temp_dir = std::env::temp_dir();
                let start_c_path = temp_dir.join(format!("vx_start_{}.c", std::process::id()));
                let start_o_path = temp_dir.join(format!("vx_start_{}.o", std::process::id()));
                let start_c = r#"
extern long __main__(void);
__attribute__((naked)) void _start(void) {
    __asm__ volatile (
        "call __main__\n\t"
        "movq %%rax, %%rdi\n\t"
        "movq $60, %%rax\n\t"
        "syscall\n\t"
        "hlt"
        :
        :
        : "rax", "rdi", "memory"
    );
}
"#;
                fs::write(&start_c_path, start_c).map_err(LinkerError::Io)?;

                // 编译 start.c 为对象文件
                let cc_output = Command::new("cc")
                    .arg("-c")
                    .arg("-fno-stack-protector")
                    .arg("-nostdlib")
                    .arg("-O2")
                    .arg("-o")
                    .arg(&start_o_path)
                    .arg(&start_c_path)
                    .output()
                    .map_err(|e| LinkerError::AotError(format!("Failed to compile start stub: {}", e)))?;

                if !cc_output.status.success() {
                    let _ = fs::remove_file(&start_c_path);
                    let stderr = String::from_utf8_lossy(&cc_output.stderr);
                    return Err(LinkerError::AotError(format!(
                        "Failed to compile start stub: {}", stderr
                    )));
                }

                let linker = if Path::new("/usr/bin/ld").exists() { "ld" } else { "cc" };
                let mut cmd = Command::new(linker);
                cmd.arg("-static");
                cmd.arg("-o").arg(output_path);
                cmd.arg(obj_path_str.as_ref());
                cmd.arg(start_o_path.to_string_lossy().as_ref());
                
                // 添加优化选项
                if linker == "ld" {
                    cmd.arg("--gc-sections");  // 移除未使用的段
                } else {
                    cmd.arg("-Wl,--gc-sections");  // 通过 cc 传递 ld 选项
                }

                println!("[*] Invoking Linux linker: {} ...", linker);

                let output = cmd.output().map_err(|e| {
                    let _ = fs::remove_file(&start_c_path);
                    let _ = fs::remove_file(&start_o_path);
                    LinkerError::AotError(format!("Failed to invoke linker: {}", e))
                })?;

                let _ = fs::remove_file(&start_c_path);
                let _ = fs::remove_file(&start_o_path);

                if !output.status.success() {
                    let stderr = String::from_utf8_lossy(&output.stderr);
                    return Err(LinkerError::AotError(format!(
                        "Linux linker failed: {}", stderr
                    )));
                }
            }
        }

        // 设置可执行权限 (Unix)
        #[cfg(unix)]
        {
            let metadata = fs::metadata(output_path)?;
            let mut perms = metadata.permissions();
            perms.set_mode(0o755);
            fs::set_permissions(output_path, perms)?;
        }

        println!("[+] Native linked: {} (static={})", output_path, !dynamic_link);
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
    eprintln!("VX Linker v4 - Multi-mode linker");
    eprintln!("Usage: {} <input.vxobj/.vxco> [options]", prog_name);
    eprintln!("Options:");
    eprintln!("  -o <path>      Output path (default: input with .out/.exe)");
    eprintln!("  -s <path>      Runtime stub path (for interpret/jit mode)");
    eprintln!("  --mode <mode>  Link mode: interpret (default), jit, aot, native");
    eprintln!("  --dump         Dump VXOBJ/VXCO section info");
    eprintln!();
    eprintln!("Modes:");
    eprintln!("  interpret  VM interpreter + bytecode (compatible, slowest)");
    eprintln!("  jit        Cranelift JIT stub + type info (balanced)");
    eprintln!("  aot        AOT compile to native machine code (fastest)");
    eprintln!("  native     Static compile to native executable (default for .vxco)");
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
    // 优化等级 (由 vpm 构建器透传, 链接核心暂作记录)
    let mut opt_level: u8 = 20;

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
            "--opt-level" if i + 1 < args.len() => {
                opt_level = args[i + 1].parse().unwrap_or(20);
                i += 2;
            }
            // 死代码诊断标志由编译器处理, 链接器忽略
            "--warn-dead-code" | "--error-dead-code" => {
                i += 1;
            }
            _ => {
                eprintln!("Unknown arg: {}", args[i]);
                print_usage(&args[0]);
                std::process::exit(1);
            }
        }
    }
    // 预留: opt_level 供链接核心后续接入优化通路
    let _ = opt_level;

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
