// VX Language Compiler CLI

use std::env;
use std::fs;
use std::path::Path;
use std::process;

// token/parser/compiler_ownership/compiler 已迁移到 vx_vm 库中共享给 LSP 等其他目标

use vx_vm::token::Lexer;
use vx_vm::parser::Parser;
use vx_vm::compiler_ownership::OwnershipChecker;
use vx_vm::compiler_core::Compiler;

// ==================== 主程序 ====================
fn main() {
    let args: Vec<String> = env::args().collect();
    if args.len() < 2 {
        eprintln!("Usage: vxcompiler <input.vx> [-o output.vxobj] [--target triple]");
        process::exit(1);
    }
    let input = &args[1];
    let mut output = String::new();
    let mut target_triple = String::new();
    // 优化等级 (由 vpm 构建器透传, 编译核心暂作记录)
    let mut opt_level: u8 = 20;
    let mut warn_dead_code = false;
    let mut error_dead_code = false;
    let mut i = 2;
    while i < args.len() {
        match args[i].as_str() {
            "-o" => {
                if i + 1 < args.len() {
                    output = args[i + 1].clone();
                    i += 2;
                } else {
                    eprintln!("Missing output file for -o");
                    process::exit(1);
                }
            }
            "--target" if i + 1 < args.len() => {
                target_triple = args[i + 1].clone();
                i += 2;
            }
            "--opt-level" if i + 1 < args.len() => {
                opt_level = args[i + 1].parse().unwrap_or(20);
                i += 2;
            }
            "--warn-dead-code" => {
                warn_dead_code = true;
                i += 1;
            }
            "--error-dead-code" => {
                error_dead_code = true;
                i += 1;
            }
            _ => {
                eprintln!("Unknown argument: {}", args[i]);
                process::exit(1);
            }
        }
    }
    // 预留: opt_level / warn_dead_code / error_dead_code 供编译核心后续接入优化与诊断通路
    //
    // 优先级: CLI 参数 > VX_OPT_LEVEL 等环境变量 > 编译核心默认值 (0 / false / false)
    // 这使得 vpm VxBuilder 在不修改 CLI 的情况下, 也能通过环境变量把构建器侧的
    // [vxset] 优化/死代码策略注入到编译器实例, 避免 vxcompiler 内部丢失配置。
    let env_opt = env::var("VX_OPT_LEVEL")
        .ok()
        .and_then(|s| s.parse::<u8>().ok());
    let env_warn_dc = env::var("VX_WARN_DEAD_CODE")
        .map(|v| v == "1" || v.eq_ignore_ascii_case("true"))
        .unwrap_or(false);
    let env_error_dc = env::var("VX_ERROR_DEAD_CODE")
        .map(|v| v == "1" || v.eq_ignore_ascii_case("true"))
        .unwrap_or(false);
    let final_opt_level = env_opt.unwrap_or(opt_level);
    let final_warn_dc = env_warn_dc || warn_dead_code;
    let final_error_dc = env_error_dc || error_dead_code;
    // final_* 在下面构造 Compiler 时通过 with_options 注入
    if output.is_empty() {
        output = input.replacen(".vx", ".vxobj", 1);
    }

    let source_dir = fs::canonicalize(input)
        .ok()
        .and_then(|p| p.parent().map(|p| p.to_string_lossy().to_string()))
        .unwrap_or_default();

    let vxsetting_path = Path::new(&source_dir).join("vxsetting.toml");
    let vxmodel_path = Path::new(&source_dir).join("vxmodel");
    let vxmodel_toml_path = Path::new(&source_dir).join("vxmodel.toml");

    if vxmodel_path.exists() || vxmodel_toml_path.exists() {
        eprintln!("[VX Deprecation Warning] Legacy config file detected: vxmodel / vxmodel.toml");
        eprintln!("  This format is deprecated. Please migrate to vxsetting.toml.");
        eprintln!("  Reference format:");
        eprintln!("    [libraries]");
        eprintln!("    stdlib = \"/path/to/stdlib\"");
        process::exit(1);
    }

    if !vxsetting_path.exists() {
        eprintln!("VX Error: Missing vxsetting.toml: {}", vxsetting_path.display());
        process::exit(1);
    }

    let settings = vx_vm::VxSettings::from_file(
        vxsetting_path.to_str().unwrap_or_else(|| {
            eprintln!("VX Error: Config file path contains non-UTF-8 characters");
            process::exit(1);
        }),
    )
        .unwrap_or_else(|e| {
            eprintln!("VX Error: Failed to parse vxsetting.toml: {}", e);
            process::exit(1);
        });

    let src = match fs::read_to_string(input) {
        Err(e) => {
            eprintln!("Read failed: {}", e);
            process::exit(1);
        }
        Ok(s) => s,
    };
    let lexer = Lexer::new(&src);
    let tokens = match lexer.tokenize() {
        Err(e) => {
            eprintln!("{}", e);
            process::exit(1);
        }
        Ok(t) => t,
    };
    let mut parser = Parser::new(tokens, &src);
    let ast = match parser.parse() {
        Err(e) => {
            eprintln!("{}", e);
            process::exit(1);
        }
        Ok(a) => a,
    };

    let mut checker = OwnershipChecker::new(&src);
    checker.check_ast(&ast);
    if !checker.errors.is_empty() {
        for err in &checker.errors {
            eprintln!("[Ownership Warning] {}", err);
        }
        eprintln!(
            "Ownership checker found {} issue(s). Please fix and recompile.",
            checker.errors.len()
        );
        process::exit(1);
    }

    let mut comp = Compiler::new(settings).with_options(final_opt_level, final_warn_dc, final_error_dc);
    let mut der = match comp.compile(&ast) {
        Ok(d) => d,
        Err(e) => {
            eprintln!("{}", e);
            process::exit(1);
        }
    };
    if !target_triple.is_empty() {
        der.target_triple = target_triple;
    }

    if let Err(e) = comp.save(&der, &output) {
        eprintln!("[Error] Save failed: {}", e);
        process::exit(1);
    }
}
