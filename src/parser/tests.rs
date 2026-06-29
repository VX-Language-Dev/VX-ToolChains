// VX Language Compiler - Parser 单元测试
// 覆盖表达式解析的核心分支，避免重构时静默回归

use super::*;
use crate::token::Lexer;

fn parse_expr(src: &str) -> Expr {
    let tokens = Lexer::new(src).tokenize().expect("lex failed");
    let mut parser = Parser::new(tokens, src);
    parser.parse_expression().expect("parse_expression failed")
}

#[test]
fn test_int_literal() {
    let e = parse_expr("42");
    assert!(matches!(e, Expr::IntLiteral(42, _, _)), "got {:?}", e);
}

#[test]
fn test_float_literal() {
    let e = parse_expr("3.14");
    match e {
        Expr::FloatLiteral(v, _, _) => assert!((v - 3.14).abs() < 1e-9, "got {}", v),
        other => panic!("expected FloatLiteral, got {:?}", other),
    }
}

#[test]
fn test_string_literal() {
    let e = parse_expr("\"hi\"");
    match e {
        Expr::StringLiteral(s, _, _) => assert_eq!(s, "hi"),
        other => panic!("expected StringLiteral, got {:?}", other),
    }
}

#[test]
fn test_bool_literals() {
    assert!(matches!(parse_expr("true"), Expr::BoolLiteral(true, _, _)));
    assert!(matches!(parse_expr("false"), Expr::BoolLiteral(false, _, _)));
}

#[test]
fn test_nil_literal() {
    assert!(matches!(parse_expr("nil"), Expr::NilLiteral(_, _)));
}

#[test]
fn test_identifier() {
    match parse_expr("foo_bar") {
        Expr::Identifier(name, _, _) => assert_eq!(name, "foo_bar"),
        other => panic!("expected Identifier, got {:?}", other),
    }
}

#[test]
fn test_cjk_identifier() {
    match parse_expr("变量") {
        Expr::Identifier(name, _, _) => assert_eq!(name, "变量"),
        other => panic!("expected Identifier, got {:?}", other),
    }
}

#[test]
fn test_binary_add() {
    let e = parse_expr("1 + 2");
    match e {
        Expr::BinaryOp(op, _, _, _, _) => assert_eq!(op, "+"),
        other => panic!("expected BinaryOp, got {:?}", other),
    }
}

#[test]
fn test_var_type_rejected() {
    let src = "func main():\n    x: var = 1\n";
    let tokens = Lexer::new(src).tokenize().expect("lex failed");
    let mut parser = Parser::new(tokens, src);
    assert!(parser.parse().is_err(), "var 类型应被拒绝");
}

#[test]
fn test_var_decl_rejected() {
    let src = "func main():\n    var x = 1\n";
    let tokens = Lexer::new(src).tokenize().expect("lex failed");
    let mut parser = Parser::new(tokens, src);
    assert!(parser.parse().is_err(), "var 推断声明应被拒绝");
}

#[test]
fn test_explicit_type_var_decl_ok() {
    let src = "func main():\n    x: int = 1\n";
    let tokens = Lexer::new(src).tokenize().expect("lex failed");
    let mut parser = Parser::new(tokens, src);
    assert!(parser.parse().is_ok(), "显式类型声明应通过");
}

#[test]
fn test_operator_precedence() {
    // 1 + 2 * 3  应解析为  1 + (2 * 3)
    let e = parse_expr("1 + 2 * 3");
    match e {
        Expr::BinaryOp(op, _, rhs, _, _) => {
            assert_eq!(op, "+");
            assert!(matches!(*rhs, Expr::BinaryOp(ref op2, _, _, _, _) if op2 == "*"),
                "右侧应为 *，实际为 {:?}", rhs);
        }
        other => panic!("expected BinaryOp, got {:?}", other),
    }
}

#[test]
fn test_unary_neg() {
    let e = parse_expr("-x");
    match e {
        Expr::UnaryOp(op, _, _, _) => assert_eq!(op, "-"),
        other => panic!("expected UnaryOp, got {:?}", other),
    }
}

#[test]
fn test_parenthesized() {
    // (1 + 2) * 3
    let e = parse_expr("(1 + 2) * 3");
    match e {
        Expr::BinaryOp(op, lhs, _, _, _) => {
            assert_eq!(op, "*");
            assert!(matches!(*lhs, Expr::BinaryOp(ref op2, _, _, _, _) if op2 == "+"),
                "左侧应为 +，实际为 {:?}", lhs);
        }
        other => panic!("expected BinaryOp, got {:?}", other),
    }
}
