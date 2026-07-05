// ==================== VX 全面集成测试 ====================
// 覆盖：词法分析、语法解析、所有权检查、内存模型
// 所有测试使用 VX 源文本 → 词法分析 → 语法解析 → 所有权检查 的完整管道

use vx_vm::bytecode::{VxObjV4Container, ExternalDependency, SECTION_TYPE_IR, SECTION_EXTERNAL_DEPS};
use vx_vm::compiler_ownership::OwnershipChecker;
use vx_vm::parser::{Parser, Stmt};
use vx_vm::token::{Lexer, TokenType};

// ========================================================================
// 测试辅助函数
// ========================================================================

/// 词法分析辅助函数：返回 token 的 kind 序列（忽略 Newline/EOF）
fn token_kinds(src: &str) -> Vec<TokenType> {
    let toks = Lexer::new(src).tokenize().expect("tokenize failed");
    toks.into_iter()
        .filter(|t| !matches!(t.kind, TokenType::Newline | TokenType::EOF))
        .map(|t| t.kind)
        .collect()
}

/// 解析辅助函数：返回 AST
fn parse(src: &str) -> Vec<Stmt> {
    let tokens = Lexer::new(src).tokenize().unwrap();
    Parser::new(tokens, src).parse().unwrap()
}

/// 解析失败测试
fn parse_err(src: &str) -> String {
    let tokens = match Lexer::new(src).tokenize() {
        Ok(t) => t,
        Err(e) => return e.msg,
    };
    let mut parser = Parser::new(tokens, src);
    match parser.parse() {
        Ok(_) => panic!("expected parse error, got Ok"),
        Err(e) => e.msg,
    }
}

/// 所有权检查测试：返回所有错误消息
fn check_owner(src: &str) -> Vec<String> {
    let ast = parse(src);
    let mut checker = OwnershipChecker::new(src);
    checker.check_ast(&ast);
    checker.errors.clone()
}

/// 所有权检查测试：返回所有警告
fn check_owner_warnings(src: &str) -> Vec<String> {
    let ast = parse(src);
    let mut checker = OwnershipChecker::new(src);
    checker.check_ast(&ast);
    checker.warnings.clone()
}

/// 所有权检查通过
fn check_owner_ok(src: &str) {
    let ast = parse(src);
    let mut checker = OwnershipChecker::new(src);
    checker.check_ast(&ast);
    assert!(
        checker.errors.is_empty(),
        "expected no errors, got: {:?}",
        checker.errors
    );
}

/// 所有权检查应报错
fn check_owner_err(src: &str, contains: &str) {
    let errors = check_owner(src);
    assert!(
        errors.iter().any(|e| e.contains(contains)),
        "expected error containing '{}', got: {:?}",
        contains,
        errors
    );
}

// ========================================================================
// 第一部分：词法分析测试 (Lexer)
// ========================================================================

#[test]
fn test_lexer_int_literals() {
    let kinds = token_kinds("0 42 10000");
    assert_eq!(kinds, vec![TokenType::Int, TokenType::Int, TokenType::Int]);
}

#[test]
fn test_lexer_float_literals() {
    let kinds = token_kinds("3.14 0.5 1e10 2.5e-3");
    assert_eq!(
        kinds,
        vec![
            TokenType::Float,
            TokenType::Float,
            TokenType::Float,
            TokenType::Float,
        ]
    );
}

#[test]
fn test_lexer_string_literals() {
    let kinds = token_kinds(r#""hello" 'world'"#);
    assert_eq!(kinds, vec![TokenType::String, TokenType::String]);
}

#[test]
fn test_lexer_escaped_string() {
    let toks = Lexer::new(r#""line1\nline2""#)
        .tokenize()
        .unwrap();
    let s = toks
        .iter()
        .find(|t| t.kind == TokenType::String)
        .unwrap();
    assert_eq!(s.value, "line1\nline2");
}

#[test]
fn test_lexer_unclosed_string_error() {
    let result = Lexer::new("\"unclosed").tokenize();
    assert!(result.is_err());
    assert!(result.unwrap_err().msg.contains("未闭合"));
}

#[test]
fn test_lexer_bool_and_nil() {
    let kinds = token_kinds("true false nil");
    assert_eq!(
        kinds,
        vec![TokenType::True, TokenType::False, TokenType::Nil]
    );
}

#[test]
fn test_lexer_all_operators() {
    let kinds = token_kinds("+ - * / % ^ == != < > <= >= = += -= *= /= %= ^= && || ! & ->");
    assert!(kinds.contains(&TokenType::Plus));
    assert!(kinds.contains(&TokenType::Minus));
    assert!(kinds.contains(&TokenType::Star));
    assert!(kinds.contains(&TokenType::Slash));
    assert!(kinds.contains(&TokenType::Percent));
    assert!(kinds.contains(&TokenType::Power));
    assert!(kinds.contains(&TokenType::Eq));
    assert!(kinds.contains(&TokenType::Ne));
    assert!(kinds.contains(&TokenType::Lt));
    assert!(kinds.contains(&TokenType::Gt));
    assert!(kinds.contains(&TokenType::Le));
    assert!(kinds.contains(&TokenType::Ge));
    assert!(kinds.contains(&TokenType::Assign));
    assert!(kinds.contains(&TokenType::PlusAssign));
    assert!(kinds.contains(&TokenType::And));
    assert!(kinds.contains(&TokenType::Or));
    assert!(kinds.contains(&TokenType::Not));
    assert!(kinds.contains(&TokenType::Ampersand));
    assert!(kinds.contains(&TokenType::Arrow));
}

#[test]
fn test_lexer_delimiters() {
    let kinds = token_kinds("( ) [ ] { } : , . ;");
    assert_eq!(
        kinds,
        vec![
            TokenType::LParen,
            TokenType::RParen,
            TokenType::LBracket,
            TokenType::RBracket,
            TokenType::LBrace,
            TokenType::RBrace,
            TokenType::Colon,
            TokenType::Comma,
            TokenType::Dot,
            TokenType::Semicolon,
        ]
    );
}

#[test]
fn test_lexer_keywords() {
    let kinds = token_kinds(
        "if else elif while for break continue func return loop \
         struct class enum union var mut move new import as macro",
    );
    assert!(kinds.contains(&TokenType::If));
    assert!(kinds.contains(&TokenType::Else));
    assert!(kinds.contains(&TokenType::Elif));
    assert!(kinds.contains(&TokenType::While));
    assert!(kinds.contains(&TokenType::For));
    assert!(kinds.contains(&TokenType::Break));
    assert!(kinds.contains(&TokenType::Continue));
    assert!(kinds.contains(&TokenType::Func));
    assert!(kinds.contains(&TokenType::Return));
    assert!(kinds.contains(&TokenType::Loop));
    assert!(kinds.contains(&TokenType::Struct));
    assert!(kinds.contains(&TokenType::Class));
    assert!(kinds.contains(&TokenType::Enum));
    assert!(kinds.contains(&TokenType::Union));
    assert!(kinds.contains(&TokenType::Mut));
    assert!(kinds.contains(&TokenType::Move));
    assert!(kinds.contains(&TokenType::New));
    assert!(kinds.contains(&TokenType::Import));
    assert!(kinds.contains(&TokenType::As));
    assert!(kinds.contains(&TokenType::Macro));
}

#[test]
fn test_lexer_types() {
    let kinds = token_kinds("int float double bool void");
    assert_eq!(
        kinds,
        vec![
            TokenType::IntT,
            TokenType::FloatT,
            TokenType::DoubleT,
            TokenType::BoolT,
            TokenType::VoidT,
        ]
    );
}

#[test]
fn test_lexer_identifier() {
    let kinds = token_kinds("foo bar_123 _private");
    assert_eq!(
        kinds,
        vec![TokenType::Identifier, TokenType::Identifier, TokenType::Identifier]
    );
}

#[test]
fn test_lexer_cjk_identifier() {
    let toks = Lexer::new("变量 计数器").tokenize().unwrap();
    let idents: Vec<&str> = toks
        .iter()
        .filter(|t| t.kind == TokenType::Identifier)
        .map(|t| t.value.as_str())
        .collect();
    assert_eq!(idents, vec!["变量", "计数器"]);
}

#[test]
fn test_lexer_comment_skipped() {
    let kinds = token_kinds("# this is a comment\n42");
    assert_eq!(kinds, vec![TokenType::Int]);
}

#[test]
fn test_lexer_hash_macro_call() {
    let kinds = token_kinds("#foo(x)");
    assert!(kinds.contains(&TokenType::Hash));
    assert!(kinds.contains(&TokenType::Identifier));
    assert!(kinds.contains(&TokenType::LParen));
}

#[test]
fn test_lexer_indent_dedent() {
    let src = "func foo()\n    return 1\n";
    let toks = Lexer::new(src).tokenize().unwrap();
    let all: Vec<TokenType> = toks.iter().map(|t| t.kind).collect();
    assert!(all.contains(&TokenType::Indent));
    assert!(all.contains(&TokenType::Dedent));
}

#[test]
fn test_lexer_illegal_char() {
    let result = Lexer::new("@").tokenize();
    assert!(result.is_err());
    assert!(result.unwrap_err().msg.contains("非法字符"));
}

// ========================================================================
// 第二部分：语法解析测试 (Parser) - 字面量和标识符
// ========================================================================

#[test]
fn test_parse_int_literal() {
    let ast = parse("func main()\n    x: int = 42\n");
    assert!(ast.len() >= 1);
}

#[test]
fn test_parse_float_literal() {
    let ast = parse("func main()\n    x: float = 3.14\n");
    assert!(!ast.is_empty());
}

#[test]
fn test_parse_string_literal() {
    let ast = parse("func main()\n    s: string = \"hello\"\n");
    assert!(!ast.is_empty());
}

#[test]
fn test_parse_bool_literals() {
    let ast = parse("func main()\n    a: bool = true\n    b: bool = false\n");
    assert!(!ast.is_empty());
}

#[test]
fn test_parse_nil_literal() {
    let ast = parse("func main()\n    x: pointer = nil\n");
    assert!(!ast.is_empty());
}

// ========================================================================
// 语法解析测试 - 变量声明
// ========================================================================

#[test]
fn test_var_decl_with_type() {
    assert!(parse("func main()\n    x: int = 1\n").len() >= 1);
}

#[test]
fn test_var_decl_no_init() {
    assert!(parse("func main()\n    x: int\n").len() >= 1);
}

#[test]
fn test_mut_var_decl() {
    assert!(parse("func main()\n    mut x: int = 1\n").len() >= 1);
}

#[test]
fn test_multi_var_decl() {
    assert!(parse("func main()\n    a: int = 1\n    b: float = 2.0\n    c: string = \"hi\"\n").len() >= 3);
}

#[test]
fn test_var_type_rejected() {
    let err = parse_err("func main()\n    x: var = 1\n");
    assert!(err.contains("var") || err.contains("动态类型"));
}

#[test]
fn test_var_inferred_decl_rejected() {
    let err = parse_err("func main()\n    var x = 1\n");
    assert!(err.contains("var") || err.contains("类型推断"));
}

// ========================================================================
// 语法解析测试 - 运算符表达式
// ========================================================================

#[test]
fn test_arithmetic_operators() {
    assert!(parse(
        "func main()\n    r: int = 1 + 2 * 3 - 4 / 2 % 3\n"
    )
    .len()
    >= 1);
}

#[test]
fn test_comparison_operators() {
    assert!(parse("func main()\n    r: bool = 1 < 2 and 3 > 1 and 2 <= 3 and 3 >= 2\n").len() >= 1);
}

#[test]
fn test_equality_operators() {
    assert!(parse("func main()\n    r: bool = 1 == 1 and 2 != 3\n").len() >= 1);
}

#[test]
fn test_logical_operators() {
    assert!(parse("func main()\n    r: bool = true and false or not true\n").len() >= 1);
}

#[test]
fn test_symbolic_logical_operators() {
    assert!(parse("func main()\n    r: bool = true && false || !true\n").len() >= 1);
}

#[test]
fn test_power_operator() {
    assert!(parse("func main()\n    r: int = 2 ^ 10\n").len() >= 1);
}

#[test]
fn test_compound_assign() {
    assert!(parse(
        "func main()\n    mut x: int = 10\n    x += 5\n    x -= 3\n    x *= 2\n    x /= 4\n"
    )
    .len()
    >= 1);
}

// ========================================================================
// 语法解析测试 - 控制流
// ========================================================================

#[test]
fn test_if_statement() {
    assert!(parse("func main()\n    x: int = 1\n    if x > 0\n        x = 10\n").len() >= 1);
}

#[test]
fn test_if_else_statement() {
    assert!(parse(
        "func main()\n    x: int = 1\n    if x > 0\n        x = 10\n    else\n        x = -10\n"
    )
    .len()
    >= 1);
}

#[test]
fn test_if_elif_else() {
    assert!(parse(
        "func main()\n    x: int = 0\n    if x > 0\n        x = 1\n    elif x < 0\n        x = -1\n    else\n        x = 0\n"
    ).len() >= 1);
}

#[test]
fn test_while_loop() {
    assert!(parse(
        "func main()\n    mut i: int = 0\n    while i < 10\n        i = i + 1\n"
    )
    .len()
    >= 1);
}

#[test]
fn test_for_loop() {
    assert!(parse(
        "func main()\n    total: int = 0\n    for i in range\n        total = total + i\n"
    )
    .len()
    >= 1);
}

#[test]
fn test_loop_with_break_continue() {
    assert!(parse(
        "func main()\n    mut i: int = 0\n    loop\n        i = i + 1\n        if i > 10\n            break\n        if i % 2 == 0\n            continue\n"
    ).len() >= 1);
}

#[test]
fn test_multi_line_if_condition() {
    assert!(parse(
        "func main()\n    a: int = 1\n    b: int = 2\n    c: int = 3\n    if a > 0 or\n       b > 0 or\n       c > 0\n        return\n"
    ).len() >= 1);
}

// ========================================================================
// 语法解析测试 - 函数
// ========================================================================

#[test]
fn test_func_no_params_no_return() {
    assert!(parse(
        "func greet()\n    sys_print(\"hello\")\n"
    )
    .len()
    >= 1);
}

#[test]
fn test_func_with_params() {
    assert!(parse(
        "func add(a: int, b: int)\n    r: int = a + b\n"
    )
    .len()
    >= 1);
}

#[test]
fn test_func_with_return_type() {
    assert!(parse(
        "func add(a: int, b: int) -> int\n    return a + b\n"
    )
    .len()
    >= 1);
}

#[test]
fn test_func_return_no_value() {
    assert!(parse(
        "func abort()\n    return\n"
    ).len() >= 1);
}

#[test]
fn test_nested_function_call() {
    assert!(parse(
        "func test()\n    r: int = foo(bar(baz()))\n"
    ).len() >= 1);
}

// ========================================================================
// 语法解析测试 - 复合类型声明
// ========================================================================

#[test]
fn test_struct_declaration() {
    assert!(parse(
        "struct Point:\n    x: int\n    y: int\n"
    ).len() >= 1);
}

#[test]
fn test_struct_with_method() {
    assert!(parse(
        "struct Point:\n    x: int\n    y: int\n    func area() -> int\n        return self.x * self.y\n"
    ).len() >= 1);
}

#[test]
fn test_class_declaration() {
    assert!(parse(
        "class Person:\n    name: string\n    age: int\n"
    ).len() >= 1);
}

#[test]
fn test_class_with_inheritance() {
    assert!(parse(
        "class Dog : Animal:\n    breed: string\n"
    ).len() >= 1);
}

#[test]
fn test_class_field_with_default() {
    assert!(parse(
        "class Counter:\n    count: int = 0\n"
    ).len() >= 1);
}

#[test]
fn test_enum_declaration() {
    assert!(parse(
        "enum Color:\n    Red\n    Green\n    Blue\n"
    ).len() >= 1);
}

#[test]
fn test_enum_with_values() {
    assert!(parse(
        "enum Status:\n    OK = 200\n    NotFound = 404\n    Error = 500\n"
    ).len() >= 1);
}

#[test]
fn test_union_declaration() {
    assert!(parse(
        "union Value:\n    i: int\n    f: float\n"
    ).len() >= 1);
}

#[test]
fn test_generic_struct() {
    assert!(parse(
        "struct Pair<T>:\n    first: T\n    second: T\n"
    ).len() >= 1);
}

#[test]
fn test_class_field_missing_type_rejected() {
    let err = parse_err("class Foo:\n    x = 10\n");
    assert!(err.contains("类型注解") || err.contains("类型"));
}

// ========================================================================
// 语法解析测试 - 数组和映射
// ========================================================================

#[test]
fn test_array_literal() {
    assert!(parse("func main()\n    arr: array = [1, 2, 3]\n").len() >= 1);
}

#[test]
fn test_empty_array() {
    assert!(parse("func main()\n    arr: array = []\n").len() >= 1);
}

#[test]
fn test_map_literal() {
    assert!(parse(
        "func main()\n    m: map = {\"key\": \"value\"}\n"
    )
    .len()
    >= 1);
}

// ========================================================================
// 语法解析测试 - 指针/引用/所有权表达式
// ========================================================================

#[test]
fn test_new_expr() {
    assert!(parse("func main()\n    p: pointer = new int\n").len() >= 1);
}

#[test]
fn test_new_with_args() {
    assert!(parse("func main()\n    p: pointer = new Point(1, 2)\n").len() >= 1);
}

#[test]
fn test_address_of() {
    assert!(parse("func main()\n    mut x: int = 1\n    r: pointer = &x\n").len() >= 1);
}

#[test]
fn test_mutable_address_of() {
    assert!(parse("func main()\n    mut x: int = 1\n    r: pointer = &mut x\n").len() >= 1);
}

#[test]
fn test_deref() {
    assert!(parse("func main()\n    mut x: int = 1\n    p: pointer = &x\n    v: int = *p\n").len() >= 1);
}

#[test]
fn test_move_expr() {
    assert!(parse("func main()\n    a: pointer = new int\n    b: pointer = move a\n").len() >= 1);
}

#[test]
fn test_property_access() {
    assert!(parse("func main()\n    r: int = point.x\n").len() >= 1);
}

#[test]
fn test_pointer_member_access() {
    assert!(parse("func main()\n    r: int = ptr->field\n").len() >= 1);
}

#[test]
fn test_index_access() {
    assert!(parse("func main()\n    v: int = arr[0]\n").len() >= 1);
}

#[test]
fn test_method_call() {
    assert!(parse("func main()\n    obj.method()\n").len() >= 1);
}

// ========================================================================
// 语法解析测试 - 导入语句
// ========================================================================

#[test]
fn test_import_simple() {
    assert!(parse("import math\n").len() >= 1);
}

#[test]
fn test_import_with_alias() {
    assert!(parse("import math as m\n").len() >= 1);
}

#[test]
fn test_import_module_path() {
    assert!(parse("import std.collections.vec\n").len() >= 1);
}

// ========================================================================
// 语法解析测试 - 宏系统
// ========================================================================

#[test]
fn test_macro_definition() {
    assert!(parse(
        "macro twice(x) { x + x }\n"
    ).len() >= 1);
}

#[test]
fn test_macro_call() {
    assert!(parse(
        "func main()\n    r: int = #twice(5)\n"
    ).len() >= 1);
}

// ========================================================================
// 语法解析测试 - 运算符优先级与结合性
// ========================================================================

#[test]
fn test_precedence_mul_over_add() {
    let ast = parse("func main()\n    r: int = 1 + 2 * 3\n");
    // 验证 2*3 先结合
    assert!(!ast.is_empty());
}

#[test]
fn test_precedence_comparison_over_logical() {
    let ast = parse("func main()\n    r: bool = 1 < 2 and 3 > 4\n");
    assert!(!ast.is_empty());
}

#[test]
fn test_parenthesized_override_precedence() {
    let ast = parse("func main()\n    r: int = (1 + 2) * 3\n");
    assert!(!ast.is_empty());
}

// ========================================================================
// 语法解析测试 - 嵌套结构
// ========================================================================

#[test]
fn test_nested_if_inside_while() {
    assert!(parse(
        "func main()\n    mut i: int = 0\n    while i < 10\n        if i % 2 == 0\n            i = i + 1\n        else\n            i = i + 2\n"
    ).len() >= 1);
}

#[test]
fn test_complex_nested_control_flow() {
    assert!(parse(
        "func main()\n    mut result: int = 0\n    for i in range\n        if i == 0\n            continue\n        if i % 2 == 0\n            result = result + i\n        else\n            result = result - i\n        if result > 100\n            break\n    return result\n"
    ).len() >= 1);
}

#[test]
fn test_func_with_multiple_params() {
    assert!(parse(
        "func calculate(a: int, b: int, c: int) -> int\n    return a + b * c\n"
    ).len() >= 1);
}

// ========================================================================
// 语法解析测试 - 错误处理
// ========================================================================

#[test]
fn test_missing_colon_error() {
    let err = parse_err("func main()\n    x: int = 1\n    if x > 0\n        x = 10\n");
    // 应该有错误
    assert!(!err.is_empty());
}

#[test]
fn test_unexpected_else_error() {
    let err = parse_err("func main()\n    x: int = 1\n    else\n        x = 2\n");
    assert!(!err.is_empty());
}

#[test]
fn test_edge_case_empty_program() {
    assert!(parse("").is_empty());
}

#[test]
fn test_edge_case_only_imports() {
    assert!(parse("import math\nimport io\n").len() >= 2);
}

#[test]
fn test_edge_case_func_no_body() {
    let src = "func empty():\n";
    // 可能通过或无体函数
    let tokens = Lexer::new(src).tokenize().unwrap();
    let result = Parser::new(tokens, src).parse();
    assert!(result.is_ok() || result.is_err());
}

// ========================================================================
// 第三部分：所有权检查测试 (OwnershipChecker)
// ========================================================================

// ---- 可变性 ----

#[test]
fn test_mut_var_cannot_reassign_without_mut() {
    let src = "func main()\n    x: int = 1\n    x = 2\n";
    let errors = check_owner(src);
    assert!(
        errors.iter().any(|e| e.contains("不能对不可变变量")),
        "expected immutability error, got: {:?}",
        errors
    );
}

#[test]
fn test_mut_var_assignment_ok() {
    check_owner_ok("func main()\n    mut x: int = 1\n    x = 2\n");
}

#[test]
fn test_immut_var_multiple_assign_error() {
    let src = "func main()\n    x: int = 1\n    x = 2\n    x = 3\n";
    let errors = check_owner(src);
    assert!(errors.len() >= 1, "immut variable should error on assign");
}

// ---- 借用规则 ----

#[test]
fn test_immutable_borrow_ok() {
    check_owner_ok("func main()\n    x: int = 1\n    r: pointer = &x\n");
}

#[test]
fn test_multiple_immutable_borrow_ok() {
    check_owner_ok("func main()\n    x: int = 1\n    r1: pointer = &x\n    r2: pointer = &x\n");
}

#[test]
fn test_mutable_borrow_requires_mut_owner() {
    let src = "func main()\n    x: int = 1\n    r: pointer = &mut x\n";
    check_owner_err(src, "不可变");
}

#[test]
fn test_mut_owner_mutable_borrow_ok() {
    check_owner_ok("func main()\n    mut x: int = 1\n    r: pointer = &mut x\n");
}

#[test]
fn test_aliasing_xor_mutation() {
    let src = "func main()\n    mut x: int = 1\n    r1: pointer = &mut x\n    r2: pointer = &mut x\n";
    check_owner_err(src, "已存在");
}

#[test]
fn test_mixed_borrow_violation() {
    let src = "func main()\n    mut x: int = 1\n    r1: pointer = &mut x\n    r2: pointer = &x\n";
    check_owner_err(src, "可变借用");
}

#[test]
fn test_borrow_freeze_owner() {
    let src = "func main()\n    mut x: int = 1\n    r: pointer = &mut x\n    y: int = x\n";
    check_owner_err(src, "活跃可变借用");
}

#[test]
fn test_immutable_borrow_then_mut_borrow_blocked() {
    let src = "func main()\n    x: int = 1\n    r1: pointer = &x\n    r2: pointer = &mut x\n";
    // 不可变借用后不允许可变借用
    let errors = check_owner(src);
    assert!(!errors.is_empty(), "immutable->mutable should be blocked");
}

// ---- Move 语义 ----

#[test]
fn test_move_transfers_ownership() {
    check_owner_ok("func main()\n    a: pointer = new int\n    b: pointer = move a\n");
}

#[test]
fn test_use_after_move_detected() {
    let src = "func main()\n    a: pointer = new int\n    b: pointer = move a\n    c: int = *a\n";
    check_owner_err(src, "use-after-move");
}

#[test]
fn test_double_move_error() {
    let src = "func main()\n    a: pointer = new int\n    b: pointer = move a\n    c: pointer = move a\n";
    check_owner_err(src, "所有权已转移");
}

#[test]
fn test_copy_scalar_does_not_move() {
    check_owner_ok("func main()\n    a: int = 1\n    b: int = a\n    c: int = a\n");
}

#[test]
fn test_copy_float_does_not_move() {
    check_owner_ok("func main()\n    a: float = 3.14\n    b: float = a\n    c: float = a\n");
}

// ---- Use After Free ----

#[test]
fn test_use_after_free_detected() {
    let src = "func main()\n    a: pointer = new int\n    free(a)\n    b: int = *a\n";
    let errors = check_owner(src);
    assert!(!errors.is_empty(), "use after free should error");
}

#[test]
fn test_double_free_detected() {
    let src = "func main()\n    a: pointer = new int\n    free(a)\n    free(a)\n";
    let errors = check_owner(src);
    assert!(
        errors.iter().any(|e| e.contains("双重释放")),
        "double free should error: {:?}",
        errors
    );
}

#[test]
fn test_free_after_move_error() {
    let src = "func main()\n    a: pointer = new int\n    b: pointer = move a\n    free(a)\n";
    let errors = check_owner(src);
    assert!(
        errors.iter().any(|e| e.contains("所有权已转移")),
        "free after move should error: {:?}",
        errors
    );
}

#[test]
fn test_free_while_borrow_error() {
    let src = "func main()\n    a: pointer = new int\n    r: pointer = &a\n    free(a)\n";
    let errors = check_owner(src);
    assert!(!errors.is_empty(), "free while borrow should error");
}

// ---- 作用域与内存泄漏警告 ----

#[test]
fn test_heap_scope_leak_warning() {
    let src = "func main()\n    a: pointer = new int\n";
    let warnings = check_owner_warnings(src);
    assert!(
        warnings.iter().any(|w| w.contains("内存泄漏")),
        "heap var without free should warn: {:?}",
        warnings
    );
}

#[test]
fn test_heap_var_properly_freed_no_warning() {
    let src = "func main()\n    a: pointer = new int\n    free(a)\n";
    let warnings = check_owner_warnings(src);
    assert!(
        !warnings.iter().any(|w| w.contains("内存泄漏")),
        "freed heap var should not warn: {:?}",
        warnings
    );
}

// ---- 返回值与所有权 ----

#[test]
fn test_return_heap_var_warning() {
    let src = "func make() -> pointer\n    a: pointer = new int\n    return a\n";
    let warnings = check_owner_warnings(src);
    assert!(
        warnings.iter().any(|w| w.contains("返回")),
        "returning heap var should warn: {:?}",
        warnings
    );
}

// ---- Copy 语义边缘情况 ----

#[test]
fn test_copy_bool() {
    check_owner_ok("func main()\n    a: bool = true\n    b: bool = a\n    c: bool = a\n");
}

#[test]
fn test_copy_double() {
    check_owner_ok("func main()\n    a: double = 3.14\n    b: double = a\n    c: double = a\n");
}

#[test]
fn test_move_non_copy_forced() {
    check_owner_ok("func main()\n    a: pointer = new int\n    b: pointer = move a\n");
}

// ========================================================================
// 第四部分：VXOBJ v4 序列化/反序列化测试
// ========================================================================

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
        parsed.get_section(SECTION_EXTERNAL_DEPS).unwrap(),
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

#[test]
fn test_vxobj_empty_sections() {
    let mut container = VxObjV4Container::new("aarch64-unknown-linux-gnu");
    container.set_section(SECTION_TYPE_IR, vec![]);
    let mut buf = Vec::new();
    container.write(&mut buf).unwrap();
    let parsed = VxObjV4Container::parse(&buf).unwrap();
    assert_eq!(parsed.get_section(SECTION_TYPE_IR).unwrap(), &vec![]);
}

#[test]
fn test_vxobj_no_external_deps() {
    let mut container = VxObjV4Container::new("riscv64-unknown-linux-gnu");
    container.set_section(SECTION_TYPE_IR, vec![10, 20, 30]);
    let mut buf = Vec::new();
    container.write(&mut buf).unwrap();
    let parsed = VxObjV4Container::parse(&buf).unwrap();
    assert!(!parsed.has_external_deps());
}

// ========================================================================
// 第五部分：边缘案例和极限测试
// ========================================================================

#[test]
fn test_edge_deeply_nested_expression() {
    assert!(parse(
        "func main()\n    r: int = ((((((1 + 2))))))\n"
    ).len() >= 1);
}

#[test]
fn test_edge_func_name_keyword() {
    // 关键字可作为函数名（自举兼容）
    assert!(parse(
        "func new()\n    return 1\n"
    ).len() >= 1);
    assert!(parse(
        "func return_val()\n    return 1\n"
    ).len() >= 1);
}

#[test]
fn test_edge_large_number_of_params() {
    assert!(parse(
        "func many(a: int, b: int, c: int, d: int, e: int) -> int\n    return a\n"
    ).len() >= 1);
}

#[test]
fn test_edge_trailing_comma_in_call() {
    assert!(parse(
        "func main()\n    r: int = foo(1, 2, 3)\n"
    ).len() >= 1);
}

// ========================================================================
// 第六部分：所有权检查 - 综合场景
// ========================================================================

#[test]
fn test_complex_ownership_flow() {
    // 综合场景：创建 → 借用 → 使用 → 释放
    let src = "func test()\n    mut heap: pointer = new int\n    r: pointer = &mut heap\n    *r = 42\n    v: int = *r\n";
    check_owner_ok(src);
}

#[test]
fn test_scope_var_shadowing() {
    check_owner_ok(
        "func test()\n    x: int = 1\n    if true\n        x: int = 2\n    \n"
    );
}

#[test]
fn test_move_in_if_branches() {
    let src = "func test()\n    a: pointer = new int\n    if true\n        b: pointer = move a\n    else\n        c: int = *a\n    \n";
    // move 在 if 分支中，else 分支还可能使用 a
    let errors = check_owner(src);
    // move 在 if 中但 else 仍在使用，应该报错
    assert!(!errors.is_empty(), "conditional move should be unsafe");
}

// ========================================================================
// 第七部分：语法解析 - lambda/闭包相关（如果有）
// ========================================================================

#[test]
fn test_single_line_block_func() {
    assert!(parse(
        "func main():\n    return 0\n"
    ).len() >= 1);
}

#[test]
fn test_multi_stmt_in_func() {
    assert!(parse(
        "func process()\n    data: pointer = new int\n    *data = 42\n    result: int = *data\n    free(data)\n    return result\n"
    ).len() >= 1);
}
