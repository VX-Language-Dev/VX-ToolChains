// ==================== VX 集成测试 ====================
// 验证 VXOBJ v4 中间格式序列化/反序列化，外部依赖解析，以及内存模型检查。

use vx_vm::bytecode::{VxObjV4Container, ExternalDependency, SECTION_TYPE_IR, SECTION_EXTERNAL_DEPS};
use vx_vm::compiler_ownership::OwnershipChecker;
use vx_vm::parser::Parser;
use vx_vm::token::Lexer;

#[test]
fn test_vxobj_v4_roundtrip() {
    let mut container = VxObjV4Container::new("x86_64-unknown-linux-gnu");
    container.set_section(SECTION_TYPE_IR, vec![1, 2, 3, 4]);
    container.set_section(SECTION_EXTERNAL_DEPS, vec![
        b'm', b'y', b'l', b'i', b'b', 0, 0, b'0', 0,
    ]);
    container.set_external_deps_flag(true);

    let mut buf = Vec::new();
    container.write(&mut buf).unwrap();

    let parsed = VxObjV4Container::parse(&buf).unwrap();
    assert_eq!(parsed.header.target_triple, "x86_64-unknown-linux-gnu");
    assert!(parsed.has_external_deps());
    assert_eq!(parsed.get_section(SECTION_TYPE_IR).unwrap(), &vec![1, 2, 3, 4]);

    let deps = vx_vm::bytecode::deserialize_external_deps(
        parsed.get_section(SECTION_EXTERNAL_DEPS).unwrap()
    );
    assert_eq!(deps.len(), 1);
    assert_eq!(deps[0].name, "mylib");
    assert!(!deps[0].is_optional);
}

#[test]
fn test_external_dependency_serialization() {
    let dep = ExternalDependency::new("libc")
        .with_path("/usr/lib/libc.so")
        .set_optional(true);
    let bytes = dep.to_bytes();
    let parsed = ExternalDependency::from_bytes(&bytes).unwrap();
    assert_eq!(parsed.name, "libc");
    assert_eq!(parsed.path, Some("/usr/lib/libc.so".to_string()));
    assert!(parsed.is_optional);
}

fn parse(src: &str) -> Vec<vx_vm::parser::Stmt> {
    let tokens = Lexer::new(src).tokenize().unwrap();
    Parser::new(tokens, src).parse().unwrap()
}

#[test]
fn test_mut_var_cannot_reassign_without_mut() {
    let src = "func main()\n    x: int = 1\n    x = 2\n";
    let ast = parse(src);
    let mut checker = OwnershipChecker::new(src);
    checker.check_ast(&ast);
    assert!(
        checker.errors.iter().any(|e| e.contains("不能对不可变变量 'x' 赋值")),
        "应为不可变变量赋值报错: {:?}",
        checker.errors
    );
}

#[test]
fn test_mut_var_assignment_ok() {
    let src = "func main()\n    mut x: int = 1\n    x = 2\n";
    let ast = parse(src);
    let mut checker = OwnershipChecker::new(src);
    checker.check_ast(&ast);
    assert!(checker.errors.is_empty(), "可变变量赋值应通过: {:?}", checker.errors);
}

#[test]
fn test_mutable_borrow_requires_mut_owner() {
    let src = "func main()\n    x: int = 1\n    r: pointer = &mut x\n";
    let ast = parse(src);
    let mut checker = OwnershipChecker::new(src);
    checker.check_ast(&ast);
    assert!(
        checker.errors.iter().any(|e| e.contains("不能从不可变变量 'x' 创建可变借用")),
        "应为不可变变量可变借用报错: {:?}",
        checker.errors
    );
}

#[test]
fn test_aliasing_xor_mutation() {
    let src = "func main()\n    mut x: int = 1\n    r1: pointer = &mut x\n    r2: pointer = &mut x\n";
    let ast = parse(src);
    let mut checker = OwnershipChecker::new(src);
    checker.check_ast(&ast);
    assert!(
        checker.errors.iter().any(|e| e.contains("已存在借用")),
        "应为两次可变借用报错: {:?}",
        checker.errors
    );
}

#[test]
fn test_mixed_borrow_violation() {
    let src = "func main()\n    mut x: int = 1\n    r1: pointer = &mut x\n    r2: pointer = &x\n";
    let ast = parse(src);
    let mut checker = OwnershipChecker::new(src);
    checker.check_ast(&ast);
    assert!(
        checker.errors.iter().any(|e| e.contains("已存在可变借用")),
        "可变借用后不可再不可变借用: {:?}",
        checker.errors
    );
}

#[test]
fn test_use_after_move_detected() {
    let src = "func main()\n    a: pointer = new int\n    b: pointer = move a\n    c: pointer = a\n";
    let ast = parse(src);
    let mut checker = OwnershipChecker::new(src);
    checker.check_ast(&ast);
    assert!(
        checker.errors.iter().any(|e| e.contains("use-after-move")),
        "应为移动后使用报错: {:?}",
        checker.errors
    );
}

#[test]
fn test_copy_scalar_does_not_move() {
    let src = "func main()\n    a: int = 1\n    b: int = a\n    c: int = a\n";
    let ast = parse(src);
    let mut checker = OwnershipChecker::new(src);
    checker.check_ast(&ast);
    assert!(checker.errors.is_empty(), "标量 Copy 语义应允许多次使用: {:?}", checker.errors);
}

#[test]
fn test_borrow_freeze_owner() {
    let src = "func main()\n    mut x: int = 1\n    r: pointer = &mut x\n    y: int = x\n";
    let ast = parse(src);
    let mut checker = OwnershipChecker::new(src);
    checker.check_ast(&ast);
    assert!(
        checker.errors.iter().any(|e| e.contains("存在活跃可变借用，无法使用")),
        "可变借用期间原变量应被冻结: {:?}",
        checker.errors
    );
}

#[test]
fn test_var_inferred_decl_rejected() {
    let src = "func main():\n    var x = 1\n";
    let tokens = Lexer::new(src).tokenize().unwrap();
    let mut parser = Parser::new(tokens, src);
    assert!(parser.parse().is_err(), "var 推断声明应被拒绝");
}

#[test]
fn test_var_type_rejected() {
    let src = "func main():\n    x: var = 1\n";
    let tokens = Lexer::new(src).tokenize().unwrap();
    let mut parser = Parser::new(tokens, src);
    assert!(parser.parse().is_err(), "var 类型应被拒绝");
}

#[test]
fn test_explicit_type_required_in_class_field() {
    let src = "class Point:\n    x = 10\n";
    let tokens = Lexer::new(src).tokenize().unwrap();
    let mut parser = Parser::new(tokens, src);
    assert!(parser.parse().is_err(), "类字段缺少类型注解应被拒绝");
}
