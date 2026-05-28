// VX Language Compiler v3.0 - Parser Module
// AST 定义和语法分析器（Parser）

use super::token::{Token, TokenType, VXError};

// ==================== AST ====================
#[derive(Debug, Clone)]
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

// ==================== 语法分析 ====================
pub struct Parser {
    pub tokens: Vec<Token>,
    pub source: String,
    pub pos: usize,
}

impl Parser {
    pub fn new(tokens: Vec<Token>, source: &str) -> Self {
        Self {
            tokens,
            source: source.to_string(),
            pos: 0,
        }
    }

    fn current(&self) -> &Token {
        self.tokens
            .get(self.pos)
            .unwrap_or(self.tokens.last().unwrap())
    }

    fn peek(&self, o: usize) -> &Token {
        self.tokens
            .get(self.pos + o)
            .unwrap_or(self.tokens.last().unwrap())
    }

    fn advance(&mut self) -> Token {
        if self.pos < self.tokens.len() - 1 {
            self.pos += 1;
        }
        self.tokens[self.pos - 1].clone()
    }

    fn expect(&mut self, t: TokenType, m: Option<&str>) -> Result<Token, VXError> {
        if self.current().kind != t {
            let err = VXError {
                msg: m.unwrap_or(&format!("期望 {:?}", t)).to_string(),
                line: self.current().line,
                col: self.current().col,
                source: Some(self.source.clone()),
            };
            return Err(err);
        }
        Ok(self.advance())
    }

    fn match_kind(&self, kinds: &[TokenType]) -> bool {
        kinds.contains(&self.current().kind)
    }

    fn skip_newlines(&mut self) {
        while self.current().kind == TokenType::Newline {
            self.advance();
        }
    }

    pub fn parse_expression(&mut self) -> Result<Expr, VXError> {
        self.parse_assignment()
    }

    fn parse_assignment(&mut self) -> Result<Expr, VXError> {
        let e = self.parse_or()?;
        if self.match_kind(&[
            TokenType::Assign,
            TokenType::PlusAssign,
            TokenType::MinusAssign,
            TokenType::StarAssign,
            TokenType::SlashAssign,
            TokenType::PercentAssign,
            TokenType::PowerAssign,
        ]) {
            let (el, ec) = (e_line(&e), e_col(&e));
            if !matches!(
                &e,
                Expr::Identifier(..) | Expr::IndexAccess(..) | Expr::PropertyAccess(..)
            ) {
                return Err(VXError {
                    msg: "赋值目标必须是变量/索引/属性".to_string(),
                    line: el,
                    col: ec,
                    source: Some(self.source.clone()),
                });
            }
            let op = self.advance().value;
            let r = self.parse_assignment()?;
            return Ok(Expr::Assign(Box::new(e), op, Box::new(r), el, ec));
        }
        Ok(e)
    }

    fn parse_or(&mut self) -> Result<Expr, VXError> {
        let mut l = self.parse_and()?;
        while self.current().kind == TokenType::Or {
            let op = self.advance().value;
            l = Expr::BinaryOp(
                op,
                Box::new(l.clone()),
                Box::new(self.parse_and()?),
                e_line(&l),
                e_col(&l),
            );
        }
        Ok(l)
    }

    fn parse_and(&mut self) -> Result<Expr, VXError> {
        let mut l = self.parse_equality()?;
        while self.current().kind == TokenType::And {
            let op = self.advance().value;
            l = Expr::BinaryOp(
                op,
                Box::new(l.clone()),
                Box::new(self.parse_equality()?),
                e_line(&l),
                e_col(&l),
            );
        }
        Ok(l)
    }

    fn parse_equality(&mut self) -> Result<Expr, VXError> {
        let mut l = self.parse_comparison()?;
        while self.match_kind(&[TokenType::Eq, TokenType::Ne]) {
            let op = self.advance().value;
            l = Expr::BinaryOp(
                op,
                Box::new(l.clone()),
                Box::new(self.parse_comparison()?),
                e_line(&l),
                e_col(&l),
            );
        }
        Ok(l)
    }

    fn parse_comparison(&mut self) -> Result<Expr, VXError> {
        let mut l = self.parse_additive()?;
        while self.match_kind(&[TokenType::Lt, TokenType::Gt, TokenType::Le, TokenType::Ge]) {
            let op = self.advance().value;
            l = Expr::BinaryOp(
                op,
                Box::new(l.clone()),
                Box::new(self.parse_additive()?),
                e_line(&l),
                e_col(&l),
            );
        }
        Ok(l)
    }

    fn parse_additive(&mut self) -> Result<Expr, VXError> {
        let mut l = self.parse_multiplicative()?;
        while self.match_kind(&[TokenType::Plus, TokenType::Minus]) {
            let op = self.advance().value;
            l = Expr::BinaryOp(
                op,
                Box::new(l.clone()),
                Box::new(self.parse_multiplicative()?),
                e_line(&l),
                e_col(&l),
            );
        }
        Ok(l)
    }

    fn parse_multiplicative(&mut self) -> Result<Expr, VXError> {
        let mut l = self.parse_power()?;
        while self.match_kind(&[TokenType::Star, TokenType::Slash, TokenType::Percent]) {
            let op = self.advance().value;
            l = Expr::BinaryOp(
                op,
                Box::new(l.clone()),
                Box::new(self.parse_power()?),
                e_line(&l),
                e_col(&l),
            );
        }
        Ok(l)
    }

    fn parse_power(&mut self) -> Result<Expr, VXError> {
        let l = self.parse_unary()?;
        if self.current().kind == TokenType::Power {
            let op = self.advance().value;
            return Ok(Expr::BinaryOp(
                op,
                Box::new(l.clone()),
                Box::new(self.parse_power()?),
                e_line(&l),
                e_col(&l),
            ));
        }
        Ok(l)
    }

    fn parse_unary(&mut self) -> Result<Expr, VXError> {
        let (l, c) = (self.current().line, self.current().col);
        if self.current().kind == TokenType::Ampersand {
            self.advance();
            return Ok(Expr::AddressOf(Box::new(self.parse_unary()?), l, c));
        }
        if self.current().kind == TokenType::Star {
            self.advance();
            return Ok(Expr::Deref(Box::new(self.parse_unary()?), l, c));
        }
        if self.match_kind(&[TokenType::Minus, TokenType::Not]) {
            let op = self.advance().value;
            return Ok(Expr::UnaryOp(op, Box::new(self.parse_unary()?), l, c));
        }
        self.parse_postfix()
    }

    fn parse_postfix(&mut self) -> Result<Expr, VXError> {
        let mut e = self.parse_primary()?;
        loop {
            if self.current().kind == TokenType::LParen {
                e = self.parse_call(e)?;
            } else if self.current().kind == TokenType::LBracket {
                e = self.parse_index(e)?;
            } else if self.current().kind == TokenType::Dot {
                self.advance();
                let p = self.expect(TokenType::Identifier, None)?;
                e = Expr::PropertyAccess(Box::new(e), p.value, p.line, p.col);
            } else if self.current().kind == TokenType::Arrow {
                self.advance();
                let m = self.expect(TokenType::Identifier, None)?;
                e = Expr::PointerMember(Box::new(e), m.value, m.line, m.col);
            } else {
                break;
            }
        }
        Ok(e)
    }

    fn parse_call(&mut self, callee: Expr) -> Result<Expr, VXError> {
        let (l, c) = (self.current().line, self.current().col);
        self.advance();
        let mut a = vec![];
        if !self.match_kind(&[TokenType::RParen]) {
            a.push(Box::new(self.parse_expression()?));
            while self.current().kind == TokenType::Comma {
                self.advance();
                a.push(Box::new(self.parse_expression()?));
            }
        }
        self.expect(TokenType::RParen, None)?;
        Ok(Expr::CallExpr(Box::new(callee), a, l, c))
    }

    fn parse_index(&mut self, e: Expr) -> Result<Expr, VXError> {
        let (l, c) = (self.current().line, self.current().col);
        self.advance();
        let i = self.parse_expression()?;
        self.expect(TokenType::RBracket, None)?;
        Ok(Expr::IndexAccess(Box::new(e), Box::new(i), l, c))
    }

    fn parse_primary(&mut self) -> Result<Expr, VXError> {
        let t = self.current().clone();
        match t.kind {
            TokenType::Int => {
                self.advance();
                Ok(Expr::IntLiteral(t.value.parse().unwrap(), t.line, t.col))
            }
            TokenType::Float => {
                self.advance();
                Ok(Expr::FloatLiteral(t.value.parse().unwrap(), t.line, t.col))
            }
            TokenType::String => {
                self.advance();
                Ok(Expr::StringLiteral(t.value.clone(), t.line, t.col))
            }
            TokenType::True | TokenType::False => {
                self.advance();
                Ok(Expr::BoolLiteral(t.kind == TokenType::True, t.line, t.col))
            }
            TokenType::Nil => {
                self.advance();
                Ok(Expr::NilLiteral(t.line, t.col))
            }
            TokenType::This => {
                self.advance();
                Ok(Expr::Identifier("this".into(), t.line, t.col))
            }
            TokenType::New => self.parse_new_expr(),
            TokenType::Newz => self.parse_newz_expr(),
            TokenType::Move => self.parse_move_expr(),
            TokenType::Vector => self.parse_vector_literal(),
            TokenType::In => {
                self.advance();
                Ok(Expr::Identifier(t.value.clone(), t.line, t.col))
            }
            TokenType::Identifier => {
                self.advance();
                Ok(Expr::Identifier(t.value.clone(), t.line, t.col))
            }
            TokenType::LBracket => self.parse_array(),
            TokenType::LBrace => self.parse_map(),
            TokenType::LParen => {
                self.advance();
                let e = self.parse_expression()?;
                self.expect(TokenType::RParen, None)?;
                Ok(e)
            }
            _ => Err(VXError {
                msg: format!("意外token: {:?}", t.kind),
                line: t.line,
                col: t.col,
                source: Some(self.source.clone()),
            }),
        }
    }

    fn parse_new_expr(&mut self) -> Result<Expr, VXError> {
        let t = self.advance();
        let (l, c) = (t.line, t.col);
        let tn = self.expect(TokenType::Identifier, None)?.value;
        let mut ta = vec![];
        if self.current().kind == TokenType::Lt {
            self.advance();
            ta.push(Box::new(self.parse_type()?));
            while self.current().kind == TokenType::Comma {
                self.advance();
                ta.push(Box::new(self.parse_type()?));
            }
            self.expect(TokenType::Gt, None)?;
        }
        let mut a = vec![];
        if self.current().kind == TokenType::LParen {
            self.advance();
            if !self.match_kind(&[TokenType::RParen]) {
                a.push(Box::new(self.parse_expression()?));
                while self.current().kind == TokenType::Comma {
                    self.advance();
                    a.push(Box::new(self.parse_expression()?));
                }
            }
            self.expect(TokenType::RParen, None)?;
        }
        Ok(Expr::NewExpr(tn, ta, a, l, c))
    }

    fn parse_newz_expr(&mut self) -> Result<Expr, VXError> {
        let t = self.advance();
        let (l, c) = (t.line, t.col);
        let tn = self.expect(TokenType::Identifier, None)?.value;
        let mut ta = vec![];
        if self.current().kind == TokenType::Lt {
            self.advance();
            ta.push(Box::new(self.parse_type()?));
            while self.current().kind == TokenType::Comma {
                self.advance();
                ta.push(Box::new(self.parse_type()?));
            }
            self.expect(TokenType::Gt, None)?;
        }
        let mut a = vec![];
        if self.current().kind == TokenType::LParen {
            self.advance();
            if !self.match_kind(&[TokenType::RParen]) {
                a.push(Box::new(self.parse_expression()?));
                while self.current().kind == TokenType::Comma {
                    self.advance();
                    a.push(Box::new(self.parse_expression()?));
                }
            }
            self.expect(TokenType::RParen, None)?;
        }
        Ok(Expr::NewzExpr(tn, ta, a, l, c))
    }

    fn parse_move_expr(&mut self) -> Result<Expr, VXError> {
        let t = self.advance();
        let (l, c) = (t.line, t.col);
        Ok(Expr::MoveExpr(Box::new(self.parse_unary()?), l, c))
    }

    fn parse_vector_literal(&mut self) -> Result<Expr, VXError> {
        let t = self.advance();
        let (l, c) = (t.line, t.col);
        let mut ta = None;
        if self.current().kind == TokenType::Lt {
            self.advance();
            ta = Some(Box::new(self.parse_type()?));
            self.expect(TokenType::Gt, None)?;
        }
        self.expect(TokenType::LBrace, None)?;
        let mut e = vec![];
        if !self.match_kind(&[TokenType::RBrace]) {
            e.push(Box::new(self.parse_expression()?));
            while self.current().kind == TokenType::Comma {
                self.advance();
                e.push(Box::new(self.parse_expression()?));
            }
        }
        self.expect(TokenType::RBrace, None)?;
        Ok(Expr::VectorLiteral(ta, e, l, c))
    }

    fn parse_array(&mut self) -> Result<Expr, VXError> {
        let (l, c) = (self.current().line, self.current().col);
        self.advance();
        let mut e = vec![];
        if !self.match_kind(&[TokenType::RBracket]) {
            e.push(Box::new(self.parse_expression()?));
            while self.current().kind == TokenType::Comma {
                self.advance();
                e.push(Box::new(self.parse_expression()?));
            }
        }
        self.expect(TokenType::RBracket, None)?;
        Ok(Expr::ArrayLiteral(e, l, c))
    }

    fn parse_map(&mut self) -> Result<Expr, VXError> {
        let (l, c) = (self.current().line, self.current().col);
        self.advance();
        let mut p = vec![];
        let mut e: Vec<Box<Expr>> = vec![];
        if !self.match_kind(&[TokenType::RBrace]) {
            let k = self.parse_expression()?;
            if self.current().kind == TokenType::Colon {
                self.advance();
                let v = self.parse_expression()?;
                p.push((Box::new(k), Box::new(v)));
                while self.current().kind == TokenType::Comma {
                    self.advance();
                    let kk = self.parse_expression()?;
                    self.expect(TokenType::Colon, None)?;
                    let vv = self.parse_expression()?;
                    p.push((Box::new(kk), Box::new(vv)));
                }
                self.expect(TokenType::RBrace, None)?;
                return Ok(Expr::MapLiteral(p, l, c));
            } else {
                e.push(Box::new(k));
            }
            while self.current().kind == TokenType::Comma {
                self.advance();
                e.push(Box::new(self.parse_expression()?));
            }
            self.expect(TokenType::RBrace, None)?;
            return Ok(Expr::ArrayLiteral(e, l, c));
        }
        Ok(Expr::MapLiteral(p, l, c))
    }

    fn parse_type(&mut self) -> Result<Expr, VXError> {
        let (l, c) = (self.current().line, self.current().col);
        let nm = if self.match_kind(&[
            TokenType::IntT,
            TokenType::FloatT,
            TokenType::DoubleT,
            TokenType::StringT,
            TokenType::VarT,
            TokenType::BoolT,
            TokenType::VoidT,
        ]) {
            let t = self.advance().value;
            match t.as_str() {
                "int" | "float" | "double" | "string" | "var" | "bool" | "void" => t,
                _ => {
                    return Err(VXError {
                        msg: format!("未知类型: {}", t),
                        line: l,
                        col: c,
                        source: Some(self.source.clone()),
                    });
                }
            }
        } else if self.current().kind == TokenType::Vector {
            self.advance();
            "vector".into()
        } else if self.current().kind == TokenType::Identifier {
            self.advance().value
        } else {
            return Err(VXError {
                msg: "期望类型".to_string(),
                line: l,
                col: c,
                source: Some(self.source.clone()),
            });
        };

        let mut ta = vec![];
        if self.current().kind == TokenType::Lt {
            self.advance();
            ta.push(Box::new(self.parse_type()?));
            while self.current().kind == TokenType::Comma {
                self.advance();
                ta.push(Box::new(self.parse_type()?));
            }
            self.expect(TokenType::Gt, None)?;
        }
        if !ta.is_empty() {
            Ok(Expr::TypeExpr(
                format!(
                    "{}<{}>",
                    nm,
                    ta.iter()
                        .map(|t| expr_to_type_name(t.as_ref()))
                        .collect::<Vec<_>>()
                        .join(", ")
                ),
                l,
                c,
            ))
        } else {
            Ok(Expr::TypeExpr(nm, l, c))
        }
    }

    pub fn parse_statement(&mut self) -> Result<Stmt, VXError> {
        self.skip_newlines();
        match self.current().kind {
            TokenType::Struct => self.parse_struct_decl(),
            TokenType::Class => self.parse_class_decl(),
            TokenType::Enum => self.parse_enum_decl(),
            TokenType::Union => self.parse_union_decl(),
            TokenType::Identifier if self.peek(1).kind == TokenType::Colon => self.parse_var_decl(),
            TokenType::Import => self.parse_import_stmt(),
            TokenType::Func => self.parse_func_decl(),
            TokenType::If => self.parse_if_stmt(),
            TokenType::While => self.parse_while_stmt(),
            TokenType::For => self.parse_for_stmt(),
            TokenType::Return => self.parse_return_stmt(),
            TokenType::Break => {
                let t = self.advance();
                Ok(Expr::BreakStmt(t.line, t.col))
            }
            TokenType::Continue => {
                let t = self.advance();
                Ok(Expr::ContinueStmt(t.line, t.col))
            }
            TokenType::Free => self.parse_free_stmt(),
            _ => {
                let e = self.parse_expression()?;
                Ok(Expr::ExprStmt(Box::new(e.clone()), e_line(&e), e_col(&e)))
            }
        }
    }

    fn parse_struct_decl(&mut self) -> Result<Stmt, VXError> {
        let t = self.advance();
        let (l, c) = (t.line, t.col);
        let n = self.expect(TokenType::Identifier, None)?.value;
        self.expect(TokenType::Colon, None)?;
        self.skip_newlines();
        let mut f = vec![];
        let mut m = vec![];
        self.expect(TokenType::Indent, None)?;
        while !self.match_kind(&[TokenType::Dedent, TokenType::EOF]) {
            self.skip_newlines();
            if self.match_kind(&[TokenType::Dedent, TokenType::EOF]) {
                break;
            }
            if self.current().kind == TokenType::Func {
                m.push(Box::new(self.parse_func_decl()?));
            } else {
                let fn_name = self.expect(TokenType::Identifier, None)?.value;
                self.expect(TokenType::Colon, None)?;
                let ft = self.parse_type()?;
                f.push((expr_to_type_name(&ft), fn_name));
            }
        }
        self.expect(TokenType::Dedent, None)?;
        Ok(Expr::StructDecl(n, f, m, l, c))
    }

    fn parse_class_decl(&mut self) -> Result<Stmt, VXError> {
        let t = self.advance();
        let (l, c) = (t.line, t.col);
        let n = self.expect(TokenType::Identifier, None)?.value;
        let (mut p, mut ii) = (None, vec![]);
        if self.current().kind == TokenType::Extends {
            self.advance();
            p = Some(self.expect(TokenType::Identifier, None)?.value);
        }
        if self.current().kind == TokenType::Implements {
            self.advance();
            ii.push(self.expect(TokenType::Identifier, None)?.value);
            while self.current().kind == TokenType::Comma {
                self.advance();
                ii.push(self.expect(TokenType::Identifier, None)?.value);
            }
        }
        self.expect(TokenType::Colon, None)?;
        self.skip_newlines();
        self.expect(TokenType::Indent, None)?;
        let mut f = vec![];
        let mut m = vec![];
        while !self.match_kind(&[TokenType::Dedent, TokenType::EOF]) {
            self.skip_newlines();
            if self.match_kind(&[TokenType::Dedent, TokenType::EOF]) {
                break;
            }
            let mut acc = "public".to_string();
            match self.current().kind {
                TokenType::Public => {
                    self.advance();
                }
                TokenType::Private => {
                    self.advance();
                    acc = "private".into();
                }
                TokenType::Protected => {
                    self.advance();
                    acc = "protected".into();
                }
                _ => {}
            }
            if self.current().kind == TokenType::Func {
                m.push(Box::new(self.parse_func_decl()?));
            } else {
                let fn_name = self.expect(TokenType::Identifier, None)?.value;
                self.expect(TokenType::Colon, None)?;
                let ft = self.parse_type()?;
                f.push((expr_to_type_name(&ft), fn_name, acc));
            }
        }
        self.expect(TokenType::Dedent, None)?;
        Ok(Expr::ClassDecl(n, f, m, p, ii, l, c))
    }

    fn parse_enum_decl(&mut self) -> Result<Stmt, VXError> {
        let t = self.advance();
        let (l, c) = (t.line, t.col);
        let n = self.expect(TokenType::Identifier, None)?.value;
        self.expect(TokenType::Colon, None)?;
        self.skip_newlines();
        self.expect(TokenType::Indent, None)?;
        let mut v = vec![];
        let mut auto = 0;
        while !self.match_kind(&[TokenType::Dedent, TokenType::EOF]) {
            self.skip_newlines();
            if self.match_kind(&[TokenType::Dedent, TokenType::EOF]) {
                break;
            }
            let vn = self.expect(TokenType::Identifier, None)?.value;
            let mut vv = auto;
            if self.current().kind == TokenType::Assign {
                self.advance();
                vv = self.expect(TokenType::Int, None)?.value.parse().unwrap();
            }
            v.push((vn, vv));
            auto = vv + 1;
        }
        self.expect(TokenType::Dedent, None)?;
        Ok(Expr::EnumDecl(n, v, l, c))
    }

    fn parse_union_decl(&mut self) -> Result<Stmt, VXError> {
        let t = self.advance();
        let (l, c) = (t.line, t.col);
        let n = self.expect(TokenType::Identifier, None)?.value;
        self.expect(TokenType::Colon, None)?;
        self.skip_newlines();
        self.expect(TokenType::Indent, None)?;
        let mut f = vec![];
        while !self.match_kind(&[TokenType::Dedent, TokenType::EOF]) {
            self.skip_newlines();
            if self.match_kind(&[TokenType::Dedent, TokenType::EOF]) {
                break;
            }
            let fn_name = self.expect(TokenType::Identifier, None)?.value;
            self.expect(TokenType::Colon, None)?;
            f.push((expr_to_type_name(&self.parse_type()?), fn_name));
        }
        self.expect(TokenType::Dedent, None)?;
        Ok(Expr::UnionDecl(n, f, l, c))
    }

    fn parse_var_decl(&mut self) -> Result<Stmt, VXError> {
        let nm = self.expect(TokenType::Identifier, None)?.value;
        self.expect(TokenType::Colon, None)?;
        let mut th = self.parse_type()?;
        let (l, c) = (e_line(&th), e_col(&th));
        while self.current().kind == TokenType::Star {
            self.advance();
            th = Expr::TypeExpr("pointer".into(), l, c);
        }
        let mut v = Expr::NilLiteral(l, c);
        if self.current().kind == TokenType::Assign {
            self.advance();
            v = self.parse_expression()?;
        }
        Ok(Expr::VarDecl(
            nm,
            Some(Box::new(th)),
            Box::new(v),
            false,
            l,
            c,
        ))
    }

    fn parse_func_decl(&mut self) -> Result<Stmt, VXError> {
        let t = self.advance();
        let (l, c) = (t.line, t.col);
        let n = self.expect(TokenType::Identifier, None)?.value;
        self.expect(TokenType::LParen, None)?;
        let mut p = vec![];
        if !self.match_kind(&[TokenType::RParen]) {
            let pn = self.expect(TokenType::Identifier, None)?.value;
            self.expect(TokenType::Colon, None)?;
            let pt = expr_to_type_name(&self.parse_type()?);
            p.push((pn, pt));
            while self.current().kind == TokenType::Comma {
                self.advance();
                let pn = self.expect(TokenType::Identifier, None)?.value;
                self.expect(TokenType::Colon, None)?;
                let pt = expr_to_type_name(&self.parse_type()?);
                p.push((pn, pt));
            }
        }
        self.expect(TokenType::RParen, None)?;
        let mut rt = None;
        if self.current().kind == TokenType::Arrow {
            self.advance();
            rt = Some(expr_to_type_name(&self.parse_type()?));
        }
        let b = self.parse_block()?;
        Ok(Expr::FuncDecl(n, p, rt, b, l, c))
    }

    fn parse_if_stmt(&mut self) -> Result<Stmt, VXError> {
        let t = self.advance();
        let (l, c) = (t.line, t.col);
        let cond = self.parse_expression()?;
        let body = self.parse_block()?;
        let mut elifs: Vec<(Box<Expr>, Vec<Box<Stmt>>)> = vec![];
        self.skip_newlines();
        while self.current().kind == TokenType::Elif {
            self.advance();
            let ec = self.parse_expression()?;
            let eb = self.parse_block()?;
            elifs.push((Box::new(ec), eb));
            self.skip_newlines();
        }
        let mut ebody: Option<Vec<Box<Stmt>>> = None;
        if self.current().kind == TokenType::Else {
            self.advance();
            self.skip_newlines();
            ebody = Some(self.parse_block()?);
        }
        Ok(Expr::IfStmt(Box::new(cond), body, elifs, ebody, l, c))
    }

    fn parse_for_stmt(&mut self) -> Result<Stmt, VXError> {
        let t = self.advance();
        let (l, c) = (t.line, t.col);
        let var = self.expect(TokenType::Identifier, None)?.value;
        self.expect(TokenType::In, None)?;
        let it = self.parse_expression()?;
        let body = self.parse_block()?;
        Ok(Expr::ForStmt(var, Box::new(it), body, l, c))
    }

    fn parse_while_stmt(&mut self) -> Result<Stmt, VXError> {
        let t = self.advance();
        let (l, c) = (t.line, t.col);
        let cond = self.parse_expression()?;
        let body = self.parse_block()?;
        Ok(Expr::WhileStmt(Box::new(cond), body, l, c))
    }

    fn parse_return_stmt(&mut self) -> Result<Stmt, VXError> {
        let t = self.advance();
        let (l, c) = (t.line, t.col);
        let v = if !self.match_kind(&[TokenType::Newline, TokenType::Dedent, TokenType::EOF]) {
            Some(Box::new(self.parse_expression()?))
        } else {
            None
        };
        Ok(Expr::ReturnStmt(v, l, c))
    }

    fn parse_free_stmt(&mut self) -> Result<Stmt, VXError> {
        let t = self.advance();
        let (l, c) = (t.line, t.col);
        let target = self.parse_expression()?;
        Ok(Expr::FreeStmt(Box::new(target), l, c))
    }

    fn parse_import_stmt(&mut self) -> Result<Stmt, VXError> {
        let t = self.advance();
        let (l, c) = (t.line, t.col);
        let nm = self.expect(TokenType::Identifier, None)?.value;
        let (mut al, mut di) = (None, None);
        while self.match_kind(&[TokenType::As, TokenType::Dirs]) {
            if self.current().kind == TokenType::As {
                self.advance();
                al = Some(self.expect(TokenType::Identifier, None)?.value);
            } else {
                self.advance();
                di = Some(self.expect(TokenType::String, None)?.value);
            }
        }
        Ok(Expr::ImportStmt(nm, al, di, l, c))
    }

    pub fn parse_block(&mut self) -> Result<Vec<Box<Stmt>>, VXError> {
        let mut st: Vec<Box<Stmt>> = vec![];
        self.skip_newlines();
        if self.current().kind == TokenType::Colon {
            self.advance();
            self.skip_newlines();
            if !self.match_kind(&[TokenType::Indent]) {
                st.push(Box::new(self.parse_statement()?));
                return Ok(st);
            }
            self.advance();
        }
        while !self.match_kind(&[TokenType::Dedent, TokenType::EOF]) {
            self.skip_newlines();
            if self.match_kind(&[TokenType::Dedent, TokenType::EOF]) {
                break;
            }
            st.push(Box::new(self.parse_statement()?));
        }
        if self.current().kind == TokenType::Dedent {
            self.advance();
        }
        Ok(st)
    }

    pub fn parse(&mut self) -> Result<Vec<Stmt>, VXError> {
        let mut st = vec![];
        while !self.match_kind(&[TokenType::EOF]) {
            self.skip_newlines();
            if self.match_kind(&[TokenType::EOF]) {
                break;
            }
            st.push(self.parse_statement()?);
        }
        Ok(st)
    }
}
