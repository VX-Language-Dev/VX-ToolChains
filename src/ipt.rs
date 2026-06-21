// VX Language Compiler v3.0 (Rust Port)
// Token 处理和 AST 解析模块已拆分到 token.rs 和 parser.rs
// 编译器模块已拆分到 compiler_opcode / compiler_bytecode / compiler_ownership / compiler_core

use std::collections::HashMap;
use std::env;
use std::fs;
use std::path::Path;
use std::process;

// token/parser/compiler_ownership/opcode/compiler 已迁移到 vx_vm 库中共享给 LSP 等其他目标

use vx_vm::token::Lexer;
use vx_vm::parser::Parser;
use vx_vm::compiler_ownership::OwnershipChecker;
use vx_vm::compiler_core::Compiler;

// ==================== 主程序 ====================
fn main() {
    let args: Vec<String> = env::args().collect();
    if args.len() < 2 {
        eprintln!("Usage: vxcompiler <input.vx> [-o output.vxobj] [--dump-bytecode] [--dump-sections] [--target triple]");
        process::exit(1);
    }
    let input = &args[1];
    let mut output = String::new();
    let mut dump_bytecode = false;
    let mut dump_sections = false;
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
            "--dump-bytecode" => {
                dump_bytecode = true;
                i += 1;
            }
            "--dump-sections" => {
                dump_sections = true;
                i += 1;
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
        eprintln!("[VX 废弃警告] 检测到旧版配置文件 vxmodel / vxmodel.toml。");
        eprintln!("  该配置格式已废弃，请迁移至 vxsetting.toml。");
        eprintln!("  参考格式：");
        eprintln!("    [libraries]");
        eprintln!("    stdlib = \"/path/to/stdlib\"");
        process::exit(1);
    }

    if !vxsetting_path.exists() {
        eprintln!("VX Error: 缺少 vxsetting.toml 文件: {}", vxsetting_path.display());
        process::exit(1);
    }

    let settings = vx_vm::VxSettings::from_file(
        vxsetting_path.to_str().unwrap_or_else(|| {
            eprintln!("VX Error: 配置文件路径包含非 UTF-8 字符");
            process::exit(1);
        }),
    )
        .unwrap_or_else(|e| {
            eprintln!("VX Error: 解析 vxsetting.toml 失败: {}", e);
            process::exit(1);
        });

    let src = match fs::read_to_string(input) {
        Err(e) => {
            eprintln!("读取失败: {}", e);
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
            "所有权检查发现 {} 个问题，请修复后重新编译",
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

    if dump_bytecode {
        println!("=== Constants ===");
        for (i, c) in der.constants.iter().enumerate() {
            println!("  [{}] {:?}", i, c);
        }
        println!("=== Functions ===");
        for (i, f) in der.functions.iter().enumerate() {
            println!("  [{}] {} ({} instrs, {} params):", i, f.name, f.instructions.len(), f.num_params);
            for (j, inst) in f.instructions.iter().enumerate() {
                println!("    {:3}: {:?} {:?}", j, inst.op, inst.arg);
            }
        }
    }

    if dump_sections {
        // Write to temp buffer first to get v3 section data
        let mut buf = Vec::new();
        let constants: Vec<vx_vm::bytecode::SerializedConstant> = der
            .constants.iter()
            .map(|c| match c {
                vx_vm::compiler_bytecode::ConstantValue::Nil => vx_vm::bytecode::SerializedConstant::nil(),
                vx_vm::compiler_bytecode::ConstantValue::Bool(b) => vx_vm::bytecode::SerializedConstant::bool(*b),
                vx_vm::compiler_bytecode::ConstantValue::Int(v) => vx_vm::bytecode::SerializedConstant::int(*v),
                vx_vm::compiler_bytecode::ConstantValue::Float(v) => vx_vm::bytecode::SerializedConstant::float(*v),
                vx_vm::compiler_bytecode::ConstantValue::String(s) => vx_vm::bytecode::SerializedConstant::string(s),
            })
            .collect();
        let target = if der.target_triple.is_empty() { "x86_64-unknown-linux-gnu" } else { &der.target_triple };
        let mut bytecode_buf = Vec::new();
        if let Err(e) = vx_vm::bytecode::write_vxobj(&mut bytecode_buf, &constants, &[], &HashMap::new()) {
            eprintln!("写入字节码失败: {}", e);
            process::exit(1);
        }
        if let Err(e) = vx_vm::bytecode::write_vxobj_v3(&mut buf, target, &der.type_ir_data, &bytecode_buf, &[], &[], &[]) {
            eprintln!("写入 VXOBJ v3 失败: {}", e);
            process::exit(1);
        }
        vx_vm::bytecode::dump_section_stats(&buf);
        process::exit(0);
    }

    match comp.save(&der, &output) {
        Ok(_) => println!("Compiled: {} (VXOBJ v3)", output),
        Err(e) => {
            eprintln!("保存失败: {}", e);
            process::exit(1);
        }
    }
    println!("已内置 VPM 系统接口：sys_argv / os_system / file_read / file_write / file_exists");
    println!("已启用内存安全模式：new(堆分配) / move(所有权转移) / &(借用检查)");
    println!("关键字精简: 27 核心关键字, string/vector 入库, free/newz 标准化, and/or/not→&&/||/!");
}