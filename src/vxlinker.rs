use std::env;
use std::fs;
use std::io::{self, Read};
use std::path::Path;
use std::process::Command;

#[cfg(unix)]
use std::os::unix::fs::PermissionsExt;

use vx_vm::bytecode::{self, VxObjV4Container};

#[cfg(target_os = "windows")]
const DEFAULT_OUTPUT_EXT: &str = "exe";
#[cfg(not(target_os = "windows"))]
const DEFAULT_OUTPUT_EXT: &str = "out";

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
    Io(io::Error),
    InvalidFile(String),
    UnsupportedVersion(u32),
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
        _stub_path: &str,
        mode: LinkMode,
    ) -> Result<(), LinkerError> {
        match mode {
            LinkMode::Native => Self::link_native(vxobj_path, output_path),
        }
    }

    fn link_native(vxobj_path: &str, output_path: &str) -> Result<(), LinkerError> {
        let file_data = Self::read_file(vxobj_path)?;

        let vxobj_container = VxObjV4Container::parse(&file_data)
            .map_err(|e| LinkerError::InvalidFile(format!("Not a valid VXOBJ v4 file: {}", e)))?;

        let type_ir_data = vxobj_container.get_section(bytecode::SECTION_TYPE_IR)
            .ok_or_else(|| LinkerError::NoTypeIr("No TypeIR section found".into()))?;

        let external_dep_names: Vec<String> = if let Some(deps_data) = vxobj_container.get_section(bytecode::SECTION_EXTERNAL_DEPS) {
            bytecode::deserialize_external_deps(deps_data)
                .iter()
                .map(|d| d.name.clone())
                .collect()
        } else {
            Vec::new()
        };

        let type_module = vx_vm::type_ir::deserialize_type_module_result(type_ir_data)
            .map_err(|e| LinkerError::AotError(format!("Failed to deserialize TypeIR: {}", e)))?;

        let dynamic_link = !external_dep_names.is_empty();

        let obj_data = Self::aot_compile(&type_module)?;

        let temp_dir = std::env::temp_dir();
        let obj_file_path = temp_dir.join(format!("vx_native_{}.o", std::process::id()));
        fs::write(&obj_file_path, &obj_data)
            .map_err(LinkerError::Io)?;

        let linker_result = Self::invoke_system_linker(&obj_file_path, output_path, dynamic_link, &external_dep_names);

        if let Err(e) = fs::remove_file(&obj_file_path) {
            eprintln!("[!] Failed to remove temporary object file {}: {}", obj_file_path.display(), e);
        }

        linker_result
    }

    fn invoke_system_linker(
        obj_path: &Path,
        output_path: &str,
        dynamic_link: bool,
        external_dep_names: &[String],
    ) -> Result<(), LinkerError> {
        let obj_path_str = obj_path.to_string_lossy();

        #[cfg(target_os = "windows")]
        {
            let mut cmd = Command::new("link.exe");
            cmd.arg("/OUT:".to_string() + output_path);
            cmd.arg("/NOLOGO");
            if !dynamic_link {
                cmd.arg("/SUBSYSTEM:CONSOLE");
                cmd.arg("/ENTRY:mainCRTStartup");
                cmd.arg("/NODEFAULTLIB");
                cmd.arg("/DYNAMICBASE:NO");
                cmd.arg("/NXCOMPAT:NO");
                cmd.arg("/LTCG");
            } else {
                cmd.arg("/SUBSYSTEM:CONSOLE");
                cmd.arg("/ENTRY:mainCRTStartup");
            }
            for dep in external_dep_names {
                let lib_name = if dep.ends_with(".lib") || dep.ends_with(".dll") {
                    dep.clone()
                } else if dep.starts_with("lib") {
                    format!("{}.lib", dep.trim_start_matches("lib"))
                } else {
                    format!("{}.lib", dep)
                };
                cmd.arg(lib_name);
            }
            if !dynamic_link {
                cmd.arg("msvcrt.lib");
                cmd.arg("kernel32.lib");
            }
            cmd.arg(obj_path_str.as_ref());
            let output = cmd.output().map_err(|e|
                LinkerError::AotError(format!("Failed to invoke linker: {}", e))
            )?;
            if !output.status.success() {
                let stderr = String::from_utf8_lossy(&output.stderr);
                return Err(LinkerError::AotError(format!("Windows linker failed: {}", stderr)));
            }
        }

        #[cfg(target_os = "macos")]
        {
            let linker = if Path::new("/usr/bin/ld").exists() { "ld" } else { "cc" };
            let mut cmd = Command::new(linker);
            if dynamic_link {
                cmd.arg("-o").arg(output_path);
                cmd.arg(obj_path_str.as_ref());
                for dep in external_dep_names {
                    let lib_name = if dep.starts_with("lib") { dep.trim_start_matches("lib") } else { dep };
                    cmd.arg(format!("-l{}", lib_name));
                }
                cmd.arg("-lc");
                cmd.arg("-lSystem");
            } else {
                cmd.arg("-static");
                cmd.arg("-o").arg(output_path);
                cmd.arg(obj_path_str.as_ref());
                cmd.arg("-e").arg("_main");
                cmd.arg("-no_pie");
                cmd.arg("-O2");
            }
            let output = cmd.output().map_err(|e|
                LinkerError::AotError(format!("Failed to invoke linker: {}", e))
            )?;
            if !output.status.success() {
                let stderr = String::from_utf8_lossy(&output.stderr);
                if !dynamic_link {
                    return Self::invoke_system_linker(obj_path, output_path, true, external_dep_names);
                }
                return Err(LinkerError::AotError(format!("macOS linker failed: {}", stderr)));
            }
        }

        #[cfg(target_os = "linux")]
        {
            if dynamic_link {
                let mut cmd = Command::new("cc");
                cmd.arg("-o").arg(output_path);
                cmd.arg(obj_path_str.as_ref());
                cmd.arg("-O2");
                for dep in external_dep_names {
                    let lib_name = if dep.starts_with("lib") { dep.trim_start_matches("lib") } else { dep };
                    cmd.arg(format!("-l{}", lib_name));
                }
                cmd.arg("-lc");
                let output = cmd.output().map_err(|e|
                    LinkerError::AotError(format!("Failed to invoke linker: {}", e))
                )?;
                if !output.status.success() {
                    let stderr = String::from_utf8_lossy(&output.stderr);
                    return Err(LinkerError::AotError(format!("Linux linker failed: {}", stderr)));
                }
            } else {
                let temp_dir = std::env::temp_dir();
                let start_c_path = temp_dir.join(format!("vx_start_{}.c", std::process::id()));
                let start_o_path = temp_dir.join(format!("vx_start_{}.o", std::process::id()));
                let start_c = r#"
extern long __main__(void);

void vx_out(long value) {
    char buf[32];
    int i = 0;
    int is_negative = 0;
    if (value < 0) {
        is_negative = 1;
        value = -value;
    }
    if (value == 0) {
        buf[i++] = '0';
    } else {
        while (value > 0) {
            buf[i++] = '0' + (value % 10);
            value /= 10;
        }
    }
    if (is_negative) {
        buf[i++] = '-';
    }
    // reverse
    int start = 0;
    int end = i - 1;
    while (start < end) {
        char tmp = buf[start];
        buf[start] = buf[end];
        buf[end] = tmp;
        start++;
        end--;
    }
    buf[i++] = '\n';
    __asm__ volatile (
        "movq $1, %%rax\n\t"
        "movq $1, %%rdi\n\t"
        "movq %0, %%rsi\n\t"
        "movq %1, %%rdx\n\t"
        "syscall\n\t"
        :
        : "r" (buf), "r" ((long)i)
        : "rax", "rdi", "rsi", "rdx", "memory"
    );
}

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
                let cleanup_stubs = |c: &Path, o: &Path| {
                    let _ = fs::remove_file(c);
                    let _ = fs::remove_file(o);
                };
                fs::write(&start_c_path, start_c).map_err(|e| {
                    cleanup_stubs(&start_c_path, &start_o_path);
                    LinkerError::Io(e)
                })?;
                let cc_output = Command::new("cc")
                    .arg("-c")
                    .arg("-fno-stack-protector")
                    .arg("-nostdlib")
                    .arg("-O2")
                    .arg("-o").arg(&start_o_path)
                    .arg(&start_c_path)
                    .output()
                    .map_err(|e| {
                        cleanup_stubs(&start_c_path, &start_o_path);
                        LinkerError::AotError(format!("Failed to compile start stub: {}", e))
                    })?;
                if !cc_output.status.success() {
                    cleanup_stubs(&start_c_path, &start_o_path);
                    let stderr = String::from_utf8_lossy(&cc_output.stderr);
                    return Err(LinkerError::AotError(format!("Failed to compile start stub: {}", stderr)));
                }
                let linker = if Path::new("/usr/bin/ld").exists() { "ld" } else { "cc" };
                let mut cmd = Command::new(linker);
                cmd.arg("-static");
                cmd.arg("-o").arg(output_path);
                cmd.arg(obj_path_str.as_ref());
                cmd.arg(start_o_path.to_string_lossy().as_ref());
                if linker == "ld" {
                    cmd.arg("--gc-sections");
                } else {
                    cmd.arg("-Wl,--gc-sections");
                }
                let output = cmd.output().map_err(|e| {
                    cleanup_stubs(&start_c_path, &start_o_path);
                    LinkerError::AotError(format!("Failed to invoke linker: {}", e))
                })?;
                cleanup_stubs(&start_c_path, &start_o_path);
                if !output.status.success() {
                    let stderr = String::from_utf8_lossy(&output.stderr);
                    return Err(LinkerError::AotError(format!("Linux linker failed: {}", stderr)));
                }
            }
        }

        #[cfg(unix)]
        {
            let metadata = fs::metadata(output_path)?;
            let mut perms = metadata.permissions();
            perms.set_mode(0o755);
            fs::set_permissions(output_path, perms)?;
        }

        Ok(())
    }

    #[cfg(feature = "aot")]
    fn aot_compile(type_module: &vx_vm::type_ir::TypeModule) -> Result<Vec<u8>, LinkerError> {
        vx_vm::aot_backend::compile_type_module(type_module, None)
            .map_err(|e| LinkerError::AotError(format!(
                "Cranelift compilation failed: {}", e
            )))
    }

    #[cfg(not(feature = "aot"))]
    fn aot_compile(_type_module: &vx_vm::type_ir::TypeModule) -> Result<Vec<u8>, LinkerError> {
        Err(LinkerError::AotError(
            "AOT backend not enabled. Rebuild with: cargo build --features aot".into()
        ))
    }

    fn read_file(path: &str) -> Result<Vec<u8>, LinkerError> {
        let mut file = fs::File::open(path)?;
        let mut buf = Vec::new();
        file.read_to_end(&mut buf)?;
        Ok(buf)
    }
}

fn print_usage(prog_name: &str) {
    eprintln!("VX Linker v4 - Native static linker");
    eprintln!("Usage: {} <input.vxobj> [options]", prog_name);
    eprintln!("Options:");
    eprintln!("  -o <path>      Output path (default: input with .out/.exe)");
    eprintln!("  --mode <mode>  Link mode: native (default)");
    eprintln!("  --dump         Dump VXOBJ v4 section info");
    eprintln!();
    eprintln!("Modes:");
    eprintln!("  native     Static compile to native executable");
}

fn main() {
    let args: Vec<String> = env::args().collect();
    if args.len() < 2 {
        print_usage(&args[0]);
        std::process::exit(1);
    }

    let input_file = args[1].clone();
    let mut output_file = String::new();
    let mut stub_file = String::new();
    let mut mode = LinkMode::Native;
    let mut dump = false;
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
                        eprintln!("Unknown mode: {}. Use: native", m);
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
    let _ = opt_level;

    if output_file.is_empty() {
        let path = Path::new(&input_file);
        let output = path.with_extension(DEFAULT_OUTPUT_EXT);
        output_file = output.to_string_lossy().to_string();
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

    #[test]
    fn test_mode_parsing() {
        assert_eq!(LinkMode::from_str("native"), Some(LinkMode::Native));
        assert_eq!(LinkMode::from_str("n"), Some(LinkMode::Native));
        assert_eq!(LinkMode::from_str("unknown"), None);
    }
}
