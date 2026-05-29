// VX Language Compiler v3.0 (Rust Port)
// Token 处理和 AST 解析模块已拆分到 token.rs 和 parser.rs
// 编译器模块已拆分到 compiler_opcode / compiler_bytecode / compiler_ownership / compiler_core

use std::collections::HashMap;
use std::env;
use std::fs;
use std::path::Path;
use std::process;

// 子模块
mod token;
mod parser;
mod compiler_opcode;
mod compiler_bytecode;
mod compiler_ownership;
mod compiler_core;

use token::Lexer;
use parser::Parser;
use compiler_ownership::OwnershipChecker;
use compiler_core::Compiler;

// ==================== 主程序 ====================
fn parse_vxmodel<P: AsRef<Path>>(path: P) -> HashMap<String, String> {
    let mut map = HashMap::new();
    if let Ok(content) = fs::read_to_string(path.as_ref()) {
        for line in content.lines() {
            let line = line.trim();
            if line.is_empty() || line.starts_with('#') {
                continue;
            }
            if let Some((k, v)) = line.split_once(':') {
                map.insert(k.trim().to_string(), v.trim().to_string());
            }
        }
    }
    map
}

fn main() {
    let args: Vec<String> = env::args().collect();
    if args.len() < 2 {
        eprintln!("Usage: vxcompiler <input.vx> [-o output.vxobj]");
        process::exit(1);
    }
    let input = &args[1];
    let output = if args.len() > 3 && args[2] == "-o" {
        args[3].clone()
    } else {
        input.replacen(".vx", ".vxobj", 1)
    };

    let source_dir = fs::canonicalize(input)
        .ok()
        .and_then(|p| p.parent().map(|p| p.to_string_lossy().to_string()))
        .unwrap_or_default();
    let vxmodel_path = Path::new(&source_dir).join("vxmodel");
    if !fs::metadata(&vxmodel_path).is_ok() {
        eprintln!("VX Error: 缺少 vxmodel 文件: {}", vxmodel_path.display());
        process::exit(1);
    }
    let vxmodel = parse_vxmodel(&vxmodel_path);

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

    let mut comp = Compiler::new(vxmodel);
    let der = comp.compile(&ast);
    match comp.save(&der, &output) {
        Ok(_) => println!("Compiled: {}", output),
        Err(e) => {
            eprintln!("保存失败: {}", e);
            process::exit(1);
        }
    }
    println!("已内置 VPM 系统接口：sys_argv / os_system / file_read / file_write / file_exists");
    println!("已启用内存安全模式：newz(堆分配) / free(显式释放) / move(所有权转移) / &(借用检查)");
}
