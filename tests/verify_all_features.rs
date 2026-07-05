// ==================== 验证 all_features.vx 的完整解析 ====================
// 本测试确保 all_features.vx 能被 VX 的词法分析器和语法分析器正确解析
// 覆盖所有语言功能: 字面量、变量声明、运算符、控制流、函数、
// 结构体、类、枚举、联合体、导入、宏、指针、所有权、数组、映射、泛型

use std::fs;
use vx_vm::token::Lexer;
use vx_vm::parser::Parser;

#[test]
fn test_all_features_lexer() {
    let source = fs::read_to_string("all_features.vx")
        .expect("无法读取 all_features.vx 文件");
    let tokens = Lexer::new(&source).tokenize()
        .expect("all_features.vx 词法分析失败");
    assert!(!tokens.is_empty(), "应产生 token");
    assert!(tokens.len() > 10, "应有足够多的 token");
}

#[test]
fn test_all_features_parser() {
    let source = fs::read_to_string("all_features.vx")
        .expect("无法读取 all_features.vx 文件");
    let tokens = Lexer::new(&source).tokenize()
        .expect("all_features.vx 词法分析失败");
    let ast = Parser::new(tokens, &source).parse()
        .expect("all_features.vx 语法分析失败");
    assert!(!ast.is_empty(), "应产生 AST 节点");
    // 预期有很多声明: 枚举(2) + 联合体(1) + 结构体(5) + 类(3) + 导入(4) + 函数定义(20+) + 宏定义(3)
    assert!(ast.len() > 30, "应有超过 30 个顶层声明，实际: {}", ast.len());
}
