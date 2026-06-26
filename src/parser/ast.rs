// VX Language Compiler v3.0 - AST 定义
// 语法树节点类型与辅助函数

// ==================== AST ====================
#[derive(Debug, Clone)]
#[allow(dead_code)]
pub enum Expr {
    IntLiteral(i64, usize, usize),
    FloatLiteral(f64, usize, usize),
    StringLiteral(String, usize, usize),
    BoolLiteral(bool, usize, usize),
    NilLiteral(usize, usize),
    Identifier(String, usize, usize),
    ArrayLiteral(Vec<Box<Expr>>, usize, usize),
    MapLiteral(Vec<(Box<Expr>, Box<Expr>)>, usize, usize),
    AddressOf(Box<Expr>, usize, usize),
    Deref(Box<Expr>, usize, usize),
    PointerMember(Box<Expr>, String, usize, usize),
    TypeExpr(String, usize, usize),
    BinaryOp(String, Box<Expr>, Box<Expr>, usize, usize),
    UnaryOp(String, Box<Expr>, usize, usize),
    VarDecl(String, Option<Box<Expr>>, Box<Expr>, bool, usize, usize),
    Assign(Box<Expr>, String, Box<Expr>, usize, usize),
    IndexAccess(Box<Expr>, Box<Expr>, usize, usize),
    PropertyAccess(Box<Expr>, String, usize, usize),
    IfStmt(
        Box<Expr>,
        Vec<Box<Expr>>,
        Vec<(Box<Expr>, Vec<Box<Expr>>)>,
        Option<Vec<Box<Expr>>>,
        usize,
        usize,
    ),
    MatchStmt(Box<Expr>, Vec<(Box<Expr>, Vec<Box<Expr>>)>, usize, usize),
    WhileStmt(Box<Expr>, Vec<Box<Expr>>, usize, usize),
    ForStmt(String, Box<Expr>, Vec<Box<Expr>>, usize, usize),
    LoopStmt(Option<String>, Vec<Box<Expr>>, usize, usize),
    BreakStmt(Option<String>, usize, usize),
    ContinueStmt(Option<String>, usize, usize),
    FuncDecl(
        String,
        Vec<String>,
        Vec<(String, String)>,
        Option<String>,
        Vec<Box<Expr>>,
        usize,
        usize,
    ),
    ReturnStmt(Option<Box<Expr>>, usize, usize),
    CallExpr(Box<Expr>, Vec<Box<Expr>>, usize, usize),
    StructDecl(String, Vec<String>, Vec<(String, String)>, Vec<Box<Expr>>, usize, usize),
    // 冒号继承语法: class Dog : Animal, Canine { ... }
    ClassDecl(
        String,
        Vec<String>,
        Vec<(String, String, String)>,
        Vec<Box<Expr>>,
        Option<String>,       // 父类 (extends)
        Vec<String>,          // 接口列表 (implements)
        usize,
        usize,
    ),
    EnumDecl(String, Vec<(String, i64)>, usize, usize),
    UnionDecl(String, Vec<(String, String)>, usize, usize),
    // VectorLiteral 已裁减 → 数组字面量自动转为 std::Vec<T>
    NewExpr(String, Vec<Box<Expr>>, Vec<Box<Expr>>, usize, usize),
    // NewzExpr 已裁减 → 编译期展开为 NewExpr + zero 标记
    // FreeStmt 已裁减 → mem::free(ptr) 标准库函数调用
    MoveExpr(Box<Expr>, usize, usize),
    ExprStmt(Box<Expr>, usize, usize),
    // dirs 已裁减 → ImportStmt 支持可变路径列表
    ImportStmt(String, Option<String>, Vec<String>, usize, usize),
    
    // 宏系统相关节点
    MacroDef(String, Vec<String>, Vec<Box<Expr>>, usize, usize),  // macro name(params) { body }
    MacroCall(String, Vec<Box<Expr>>, usize, usize),              // #macro_name(args)
}

pub type Stmt = Expr;

// ==================== 辅助函数 ====================
pub fn expr_to_type_name(e: &Expr) -> String {
    if let Expr::TypeExpr(name, _, _) = e {
        name.clone()
    } else {
        String::new()
    }
}

/// 从源码中按 1-indexed 行号提取源代码行；行号越界返回空串
pub fn get_src_line(source: &str, line: usize) -> String {
    if line > 0 {
        source.lines().nth(line - 1).unwrap_or("").to_string()
    } else {
        String::new()
    }
}

/// 提取任意 Expr 节点的 (行, 列) 位置，作为 e_line/e_col 的单一事实源
pub fn pos(e: &Expr) -> (usize, usize) {
    match e {
        Expr::IntLiteral(_, l, c) => (*l, *c),
        Expr::FloatLiteral(_, l, c) => (*l, *c),
        Expr::StringLiteral(_, l, c) => (*l, *c),
        Expr::BoolLiteral(_, l, c) => (*l, *c),
        Expr::NilLiteral(l, c) => (*l, *c),
        Expr::Identifier(_, l, c) => (*l, *c),
        Expr::ArrayLiteral(_, l, c) => (*l, *c),
        Expr::MapLiteral(_, l, c) => (*l, *c),
        Expr::AddressOf(_, l, c) => (*l, *c),
        Expr::Deref(_, l, c) => (*l, *c),
        Expr::PointerMember(_, _, l, c) => (*l, *c),
        Expr::TypeExpr(_, l, c) => (*l, *c),
        Expr::BinaryOp(_, _, _, l, c) => (*l, *c),
        Expr::UnaryOp(_, _, l, c) => (*l, *c),
        Expr::VarDecl(_, _, _, _, l, c) => (*l, *c),
        Expr::Assign(_, _, _, l, c) => (*l, *c),
        Expr::IndexAccess(_, _, l, c) => (*l, *c),
        Expr::PropertyAccess(_, _, l, c) => (*l, *c),
        Expr::IfStmt(_, _, _, _, l, c) => (*l, *c),
        Expr::MatchStmt(_, _, l, c) => (*l, *c),
        Expr::WhileStmt(_, _, l, c) => (*l, *c),
        Expr::ForStmt(_, _, _, l, c) => (*l, *c),
        Expr::LoopStmt(_, _, l, c) => (*l, *c),
        Expr::BreakStmt(_, l, c) => (*l, *c),
        Expr::ContinueStmt(_, l, c) => (*l, *c),
        Expr::FuncDecl(_, _, _, _, _, l, c) => (*l, *c),
        Expr::ReturnStmt(_, l, c) => (*l, *c),
        Expr::CallExpr(_, _, l, c) => (*l, *c),
        Expr::StructDecl(_, _, _, _, l, c) => (*l, *c),
        Expr::ClassDecl(_, _, _, _, _, _, l, c) => (*l, *c),
        Expr::EnumDecl(_, _, l, c) => (*l, *c),
        Expr::UnionDecl(_, _, l, c) => (*l, *c),
        Expr::NewExpr(_, _, _, l, c) => (*l, *c),
        Expr::MoveExpr(_, l, c) => (*l, *c),
        Expr::ExprStmt(_, l, c) => (*l, *c),
        Expr::ImportStmt(_, _, _, l, c) => (*l, *c),
        
        // 宏系统节点
        Expr::MacroDef(_, _, _, l, c) => (*l, *c),
        Expr::MacroCall(_, _, l, c) => (*l, *c),
    }
}

pub fn e_line(e: &Expr) -> usize {
    pos(e).0
}

pub fn e_col(e: &Expr) -> usize {
    pos(e).1
}
