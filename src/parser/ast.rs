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
    WhileStmt(Box<Expr>, Vec<Box<Expr>>, usize, usize),
    ForStmt(String, Box<Expr>, Vec<Box<Expr>>, usize, usize),
    BreakStmt(usize, usize),
    ContinueStmt(usize, usize),
    FuncDecl(
        String,
        Vec<(String, String)>,
        Option<String>,
        Vec<Box<Expr>>,
        usize,
        usize,
    ),
    ReturnStmt(Option<Box<Expr>>, usize, usize),
    CallExpr(Box<Expr>, Vec<Box<Expr>>, usize, usize),
    StructDecl(String, Vec<(String, String)>, Vec<Box<Expr>>, usize, usize),
    ClassDecl(
        String,
        Vec<(String, String, String)>,
        Vec<Box<Expr>>,
        Option<String>,
        Vec<String>,
        usize,
        usize,
    ),
    EnumDecl(String, Vec<(String, i64)>, usize, usize),
    UnionDecl(String, Vec<(String, String)>, usize, usize),
    VectorLiteral(Option<Box<Expr>>, Vec<Box<Expr>>, usize, usize),
    NewExpr(String, Vec<Box<Expr>>, Vec<Box<Expr>>, usize, usize),
    NewzExpr(String, Vec<Box<Expr>>, Vec<Box<Expr>>, usize, usize),
    FreeStmt(Box<Expr>, usize, usize),
    MoveExpr(Box<Expr>, usize, usize),
    ExprStmt(Box<Expr>, usize, usize),
    ImportStmt(String, Option<String>, Option<String>, usize, usize),
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

pub fn e_line(e: &Expr) -> usize {
    match e {
        Expr::IntLiteral(_, l, _) => *l,
        Expr::FloatLiteral(_, l, _) => *l,
        Expr::StringLiteral(_, l, _) => *l,
        Expr::BoolLiteral(_, l, _) => *l,
        Expr::NilLiteral(l, _) => *l,
        Expr::Identifier(_, l, _) => *l,
        Expr::ArrayLiteral(_, l, _) => *l,
        Expr::MapLiteral(_, l, _) => *l,
        Expr::AddressOf(_, l, _) => *l,
        Expr::Deref(_, l, _) => *l,
        Expr::PointerMember(_, _, l, _) => *l,
        Expr::TypeExpr(_, l, _) => *l,
        Expr::BinaryOp(_, _, _, l, _) => *l,
        Expr::UnaryOp(_, _, l, _) => *l,
        Expr::VarDecl(_, _, _, _, l, _) => *l,
        Expr::Assign(_, _, _, l, _) => *l,
        Expr::IndexAccess(_, _, l, _) => *l,
        Expr::PropertyAccess(_, _, l, _) => *l,
        Expr::IfStmt(_, _, _, _, l, _) => *l,
        Expr::WhileStmt(_, _, l, _) => *l,
        Expr::ForStmt(_, _, _, l, _) => *l,
        Expr::BreakStmt(l, _) => *l,
        Expr::ContinueStmt(l, _) => *l,
        Expr::FuncDecl(_, _, _, _, l, _) => *l,
        Expr::ReturnStmt(_, l, _) => *l,
        Expr::CallExpr(_, _, l, _) => *l,
        Expr::StructDecl(_, _, _, l, _) => *l,
        Expr::ClassDecl(_, _, _, _, _, l, _) => *l,
        Expr::EnumDecl(_, _, l, _) => *l,
        Expr::UnionDecl(_, _, l, _) => *l,
        Expr::VectorLiteral(_, _, l, _) => *l,
        Expr::NewExpr(_, _, _, l, _) => *l,
        Expr::NewzExpr(_, _, _, l, _) => *l,
        Expr::FreeStmt(_, l, _) => *l,
        Expr::MoveExpr(_, l, _) => *l,
        Expr::ExprStmt(_, l, _) => *l,
        Expr::ImportStmt(_, _, _, l, _) => *l,
    }
}

pub fn e_col(e: &Expr) -> usize {
    match e {
        Expr::IntLiteral(_, _, c) => *c,
        Expr::FloatLiteral(_, _, c) => *c,
        Expr::StringLiteral(_, _, c) => *c,
        Expr::BoolLiteral(_, _, c) => *c,
        Expr::NilLiteral(_, c) => *c,
        Expr::Identifier(_, _, c) => *c,
        Expr::ArrayLiteral(_, _, c) => *c,
        Expr::MapLiteral(_, _, c) => *c,
        Expr::AddressOf(_, _, c) => *c,
        Expr::Deref(_, _, c) => *c,
        Expr::PointerMember(_, _, _, c) => *c,
        Expr::TypeExpr(_, _, c) => *c,
        Expr::BinaryOp(_, _, _, _, c) => *c,
        Expr::UnaryOp(_, _, _, c) => *c,
        Expr::VarDecl(_, _, _, _, _, c) => *c,
        Expr::Assign(_, _, _, _, c) => *c,
        Expr::IndexAccess(_, _, _, c) => *c,
        Expr::PropertyAccess(_, _, _, c) => *c,
        Expr::IfStmt(_, _, _, _, _, c) => *c,
        Expr::WhileStmt(_, _, _, c) => *c,
        Expr::ForStmt(_, _, _, _, c) => *c,
        Expr::BreakStmt(_, c) => *c,
        Expr::ContinueStmt(_, c) => *c,
        Expr::FuncDecl(_, _, _, _, _, c) => *c,
        Expr::ReturnStmt(_, _, c) => *c,
        Expr::CallExpr(_, _, _, c) => *c,
        Expr::StructDecl(_, _, _, _, c) => *c,
        Expr::ClassDecl(_, _, _, _, _, _, c) => *c,
        Expr::EnumDecl(_, _, _, c) => *c,
        Expr::UnionDecl(_, _, _, c) => *c,
        Expr::VectorLiteral(_, _, _, c) => *c,
        Expr::NewExpr(_, _, _, _, c) => *c,
        Expr::NewzExpr(_, _, _, _, c) => *c,
        Expr::FreeStmt(_, _, c) => *c,
        Expr::MoveExpr(_, _, c) => *c,
        Expr::ExprStmt(_, _, c) => *c,
        Expr::ImportStmt(_, _, _, _, c) => *c,
    }
}
