// VX Language Compiler v3.0 (Rust Port)
// 编译命令: rustc ipt.rs -O -o vxcompiler

use std::collections::HashMap;
use std::env;
use std::fmt;
use std::fs;
use std::io::{self, Write};
use std::path::Path;
use std::process;

// ==================== 错误处理 ====================
#[derive(Debug)]
struct VXError {
    msg: String,
    line: usize,
    col: usize,
    source: Option<String>,
}

impl fmt::Display for VXError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        let mut ctx = String::new();
        if let Some(src) = &self.source {
            if self.line > 0 {
                let lines: Vec<&str> = src.lines().collect();
                if self.line - 1 < lines.len() {
                    ctx = format!(
                        "\n {} | {}\n | {}",
                        self.line,
                        lines[self.line - 1],
                        " ".repeat(self.col.saturating_sub(1))
                    );
                    ctx.push('^');
                }
            }
        }
        write!(
            f,
            "VX Error [line {}, col {}]: {}{}",
            self.line, self.col, self.msg, ctx
        )
    }
}

macro_rules! vx_error {
    ($msg:expr, $line:expr, $col:expr, $source:expr) => {
        return Err(VXError {
            msg: $msg.to_string(),
            line: $line,
            col: $col,
            source: Some($source.clone()),
        }
        .into())
    };
}

// ==================== 词法分析 ====================
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
enum TokenType {
    If,
    Elif,
    Else,
    For,
    While,
    Break,
    Continue,
    Func,
    Return,
    //Out,
    Move,
    //Borrow,
    //Drop,
    Int,
    Float,
    String,
    Identifier,
    Plus,
    Minus,
    Star,
    Slash,
    Percent,
    Power,
    Eq,
    Ne,
    Lt,
    Gt,
    Le,
    Ge,
    Assign,
    PlusAssign,
    MinusAssign,
    StarAssign,
    SlashAssign,
    PercentAssign,
    PowerAssign,
    Ampersand,
    Arrow,
    LParen,
    RParen,
    LBracket,
    RBracket,
    LBrace,
    RBrace,
    Colon,
    Semicolon,
    Comma,
    Dot,
    Newline,
    Indent,
    Dedent,
    EOF,
    True,
    False,
    Nil,
    And,
    Or,
    Not,
    In,
    Import,
    As,
    Dirs,
    IntT,
    FloatT,
    DoubleT,
    StringT,
    VarT,
    BoolT,
    VoidT,
    Struct,
    Class,
    Enum,
    Union,
    Vector,
    New,
    Newz,
    Free,
    This,
    Public,
    Private,
    Protected,
    Extends,
    Implements,
}

#[derive(Debug, Clone)]
struct Token {
    kind: TokenType,
    value: String,
    line: usize,
    col: usize,
}

const KEYWORDS: &[(&str, TokenType)] = &[
    ("if", TokenType::If),
    ("else", TokenType::Else),
    ("elif", TokenType::Elif),
    ("for", TokenType::For),
    ("while", TokenType::While),
    ("break", TokenType::Break),
    ("continue", TokenType::Continue),
    ("func", TokenType::Func),
    ("return", TokenType::Return),
    ("true", TokenType::True),
    ("false", TokenType::False),
    ("nil", TokenType::Nil),
    ("and", TokenType::And),
    ("or", TokenType::Or),
    ("not", TokenType::Not),
    ("in", TokenType::In),
    ("import", TokenType::Import),
    ("as", TokenType::As),
    ("dirs", TokenType::Dirs),
    ("int", TokenType::IntT),
    ("float", TokenType::FloatT),
    ("double", TokenType::DoubleT),
    ("string", TokenType::StringT),
    ("var", TokenType::VarT),
    ("bool", TokenType::BoolT),
    ("void", TokenType::VoidT),
    ("struct", TokenType::Struct),
    ("class", TokenType::Class),
    ("enum", TokenType::Enum),
    ("union", TokenType::Union),
    ("vector", TokenType::Vector),
    ("new", TokenType::New),
    ("newz", TokenType::Newz),
    ("free", TokenType::Free),
    ("this", TokenType::This),
    ("public", TokenType::Public),
    ("private", TokenType::Private),
    ("protected", TokenType::Protected),
    ("extends", TokenType::Extends),
    ("implements", TokenType::Implements),
];

struct Lexer {
    source: String,
    pos: usize,
    line: usize,
    col: usize,
    tokens: Vec<Token>,
    indent_stack: Vec<usize>,
}

impl Lexer {
    fn new(source: &str) -> Self {
        Self {
            source: source.to_string(),
            pos: 0,
            line: 1,
            col: 1,
            tokens: Vec::new(),
            indent_stack: vec![0],
        }
    }

    fn peek(&self, offset: usize) -> char {
        self.source
            .as_bytes()
            .get(self.pos + offset)
            .copied()
            .map(|b| b as char)
            .unwrap_or('\0')
    }

    fn advance(&mut self) -> char {
        let c = self.peek(0);
        self.pos += 1;
        if c == '\n' {
            self.line += 1;
            self.col = 1;
        } else {
            self.col += 1;
        }
        c
    }

    fn skip_whitespace(&mut self) {
        while matches!(self.peek(0), ' ' | '\t' | '\r' | '\u{3000}') {
            self.advance();
        }
    }

    fn read_string(&mut self, q: char) -> Result<String, VXError> {
        self.advance();
        let mut res = String::new();
        while self.peek(0) != q && self.peek(0) != '\0' {
            if self.peek(0) == '\\' {
                self.advance();
                let e = self.advance();
                res.push(match e {
                    'n' => '\n',
                    't' => '\t',
                    'r' => '\r',
                    '"' => '"',
                    '\'' => '\'',
                    '\\' => '\\',
                    _ => e,
                });
            } else {
                res.push(self.advance());
            }
        }
        if self.peek(0) != q {
            return Err(VXError {
                msg: "未闭合字符串".into(),
                line: self.line,
                col: self.col,
                source: Some(self.source.clone()),
            });
        }
        self.advance();
        Ok(res)
    }

    fn read_number(&mut self) -> Token {
        let sl = self.line;
        let sc = self.col;
        let mut s = String::new();
        let mut f = false;
        while self.peek(0).is_ascii_digit() {
            s.push(self.advance());
        }
        if self.peek(0) == '.' && self.peek(1).is_ascii_digit() {
            f = true;
            s.push(self.advance());
            while self.peek(0).is_ascii_digit() {
                s.push(self.advance());
            }
        }
        if matches!(self.peek(0), 'e' | 'E') {
            f = true;
            s.push(self.advance());
            if matches!(self.peek(0), '+' | '-') {
                s.push(self.advance());
            }
            while self.peek(0).is_ascii_digit() {
                s.push(self.advance());
            }
        }
        let val = if f {
            format!("{}", s.parse::<f64>().unwrap())
        } else {
            format!("{}", s.parse::<i64>().unwrap())
        };
        Token {
            kind: if f { TokenType::Float } else { TokenType::Int },
            value: val,
            line: sl,
            col: sc,
        }
    }

    fn read_identifier(&mut self) -> Token {
        let sl = self.line;
        let sc = self.col;
        let mut val = String::new();
        while self.peek(0).is_alphanumeric()
            || self.peek(0) == '_'
            || ('\u{4e00}'..='\u{9fff}').contains(&self.peek(0))
        {
            val.push(self.advance());
        }
        let kind = KEYWORDS
            .iter()
            .find(|(k, _)| *k == val)
            .map(|(_, t)| t.clone())
            .unwrap_or(TokenType::Identifier);
        Token {
            kind,
            value: val,
            line: sl,
            col: sc,
        }
    }

    fn handle_indent(&mut self) -> Result<(), VXError> {
        loop {
            if self.peek(0) != '\n' && self.tokens.is_empty() {
                return Ok(());
            }
            while self.peek(0) == '\n' {
                self.advance();
            }
            if self.peek(0) == '\0' {
                return Ok(());
            }
            let mut indent = 0;
            while self.peek(0) == ' ' || self.peek(0) == '\u{3000}' {
                indent += 1;
                self.advance();
            }
            while self.peek(0) == '\t' {
                indent += 4;
                self.advance();
            }
            if self.peek(0) == '#' {
                while !matches!(self.peek(0), '\n' | '\0') {
                    self.advance();
                }
                continue;
            }
            let last = *self.indent_stack.last().unwrap();
            if indent > last {
                self.indent_stack.push(indent);
                self.tokens.push(Token {
                    kind: TokenType::Indent,
                    value: indent.to_string(),
                    line: self.line,
                    col: self.col,
                });
            } else if indent < last {
                while indent < *self.indent_stack.last().unwrap() {
                    self.indent_stack.pop();
                    self.tokens.push(Token {
                        kind: TokenType::Dedent,
                        value: String::new(),
                        line: self.line,
                        col: self.col,
                    });
                }
                if indent != *self.indent_stack.last().unwrap() {
                    vx_error!("缩进不匹配", self.line, self.col, &self.source);
                }
            }
            return Ok(());
        }
    }

    fn tokenize(mut self) -> Result<Vec<Token>, VXError> {
        while self.pos < self.source.len() {
            self.skip_whitespace();
            let sl = self.line;
            let sc = self.col;
            let c = self.peek(0);
            if matches!(c, '\n' | '\r') {
                if c == '\r' && self.peek(1) == '\n' {
                    self.advance();
                }
                self.tokens.push(Token {
                    kind: TokenType::Newline,
                    value: String::new(),
                    line: sl,
                    col: sc,
                });
                self.advance();
                self.handle_indent()?;
                continue;
            }
            if c == '#' {
                while !matches!(self.peek(0), '\n' | '\0') {
                    self.advance();
                }
                continue;
            }
            if matches!(c, '"' | '\'') {
                let val = self.read_string(c)?;
                self.tokens.push(Token {
                    kind: TokenType::String,
                    value: val,
                    line: sl,
                    col: sc,
                });
                continue;
            }
            if c.is_ascii_digit() {
                let t = self.read_number();
                self.tokens.push(t);
                continue;
            }
            if c.is_alphabetic() || c == '_' || ('\u{4e00}'..='\u{9fff}').contains(&c) {
                let t = self.read_identifier();
                self.tokens.push(t);
                continue;
            }
            let n = self.peek(1);
            let mut handled = true;
            match (c, n) {
                ('=', '=') => {
                    self.advance();
                    self.advance();
                    self.tokens.push(Token {
                        kind: TokenType::Eq,
                        value: "==".into(),
                        line: sl,
                        col: sc,
                    });
                }
                ('!', '=') => {
                    self.advance();
                    self.advance();
                    self.tokens.push(Token {
                        kind: TokenType::Ne,
                        value: "!=".into(),
                        line: sl,
                        col: sc,
                    });
                }
                ('<', '=') => {
                    self.advance();
                    self.advance();
                    self.tokens.push(Token {
                        kind: TokenType::Le,
                        value: "<=".into(),
                        line: sl,
                        col: sc,
                    });
                }
                ('>', '=') => {
                    self.advance();
                    self.advance();
                    self.tokens.push(Token {
                        kind: TokenType::Ge,
                        value: ">=".into(),
                        line: sl,
                        col: sc,
                    });
                }
                ('+', '=') => {
                    self.advance();
                    self.advance();
                    self.tokens.push(Token {
                        kind: TokenType::PlusAssign,
                        value: "+=".into(),
                        line: sl,
                        col: sc,
                    });
                }
                ('-', '=') => {
                    self.advance();
                    self.advance();
                    self.tokens.push(Token {
                        kind: TokenType::MinusAssign,
                        value: "-=".into(),
                        line: sl,
                        col: sc,
                    });
                }
                ('*', '=') => {
                    self.advance();
                    self.advance();
                    self.tokens.push(Token {
                        kind: TokenType::StarAssign,
                        value: "*=".into(),
                        line: sl,
                        col: sc,
                    });
                }
                ('/', '=') => {
                    self.advance();
                    self.advance();
                    self.tokens.push(Token {
                        kind: TokenType::SlashAssign,
                        value: "/=".into(),
                        line: sl,
                        col: sc,
                    });
                }
                ('%', '=') => {
                    self.advance();
                    self.advance();
                    self.tokens.push(Token {
                        kind: TokenType::PercentAssign,
                        value: "%=".into(),
                        line: sl,
                        col: sc,
                    });
                }
                ('^', '=') => {
                    self.advance();
                    self.advance();
                    self.tokens.push(Token {
                        kind: TokenType::PowerAssign,
                        value: "^=".into(),
                        line: sl,
                        col: sc,
                    });
                }
                ('-', '>') => {
                    self.advance();
                    self.advance();
                    self.tokens.push(Token {
                        kind: TokenType::Arrow,
                        value: "->".into(),
                        line: sl,
                        col: sc,
                    });
                }
                ('&', '&') => {
                    self.advance();
                    self.advance();
                    self.tokens.push(Token {
                        kind: TokenType::And,
                        value: "&&".into(),
                        line: sl,
                        col: sc,
                    });
                }
                ('|', '|') => {
                    self.advance();
                    self.advance();
                    self.tokens.push(Token {
                        kind: TokenType::Or,
                        value: "||".into(),
                        line: sl,
                        col: sc,
                    });
                }
                _ => handled = false,
            }
            if handled {
                continue;
            }

            let m: HashMap<char, TokenType> = [
                ('+', TokenType::Plus),
                ('-', TokenType::Minus),
                ('*', TokenType::Star),
                ('/', TokenType::Slash),
                ('%', TokenType::Percent),
                ('^', TokenType::Power),
                ('<', TokenType::Lt),
                ('>', TokenType::Gt),
                ('=', TokenType::Assign),
                ('!', TokenType::Not),
                ('&', TokenType::Ampersand),
                ('(', TokenType::LParen),
                (')', TokenType::RParen),
                ('[', TokenType::LBracket),
                (']', TokenType::RBracket),
                ('{', TokenType::LBrace),
                ('}', TokenType::RBrace),
                (':', TokenType::Colon),
                (';', TokenType::Semicolon),
                (',', TokenType::Comma),
                ('.', TokenType::Dot),
            ]
            .iter()
            .copied()
            .collect();
            if let Some(kind) = m.get(&c) {
                self.advance();
                self.tokens.push(Token {
                    kind: kind.clone(),
                    value: c.to_string(),
                    line: sl,
                    col: sc,
                });
                continue;
            }
            if c != '\0' {
                vx_error!(
                    format!("非法字符: {}", c),
                    self.line,
                    self.col,
                    &self.source
                );
            }
        }
        while self.indent_stack.len() > 1 {
            self.indent_stack.pop();
            self.tokens.push(Token {
                kind: TokenType::Dedent,
                value: String::new(),
                line: self.line,
                col: self.col,
            });
        }
        self.tokens.push(Token {
            kind: TokenType::EOF,
            value: String::new(),
            line: self.line,
            col: self.col,
        });
        Ok(self.tokens)
    }
}

// ==================== AST ====================
#[derive(Debug, Clone)]
enum Expr {
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
    VarDecl(String, Option<Box<Expr>>, Box<Expr>, bool, usize, usize), // name, _type_hint, value, _is_const
    Assign(Box<Expr>, String, Box<Expr>, usize, usize),
    IndexAccess(Box<Expr>, Box<Expr>, usize, usize),
    PropertyAccess(Box<Expr>, String, usize, usize),
    IfStmt(
        Box<Expr>,
        Vec<Box<Stmt>>,
        Vec<(Box<Expr>, Vec<Box<Stmt>>)>,
        Option<Vec<Box<Stmt>>>,
        usize,
        usize,
    ),
    WhileStmt(Box<Expr>, Vec<Box<Stmt>>, usize, usize),
    ForStmt(String, Box<Expr>, Vec<Box<Stmt>>, usize, usize),
    BreakStmt(usize, usize),
    ContinueStmt(usize, usize),
    FuncDecl(
        String,
        Vec<(String, String)>,
        Option<String>,
        Vec<Box<Stmt>>,
        usize,
        usize,
    ),
    ReturnStmt(Option<Box<Expr>>, usize, usize),
    CallExpr(Box<Expr>, Vec<Box<Expr>>, usize, usize),
    StructDecl(String, Vec<(String, String)>, Vec<Box<Stmt>>, usize, usize),
    ClassDecl(
        String,
        Vec<(String, String, String)>,
        Vec<Box<Stmt>>,
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
    Block(Vec<Box<Stmt>>, usize, usize),
    ExprStmt(Box<Expr>, usize, usize),
    ImportStmt(String, Option<String>, Option<String>, usize, usize),
}

type Stmt = Expr; // 在 VX 中语句和表达式树节点共用同一套枚举，通过外层解析区分

// ==================== 语法分析 ====================
struct Parser {
    tokens: Vec<Token>,
    source: String,
    pos: usize,
}

impl Parser {
    fn new(tokens: Vec<Token>, source: &str) -> Self {
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
            vx_error!(
                m.unwrap_or(&format!("期望 {:?}", t)),
                self.current().line,
                self.current().col,
                &self.source
            );
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

    fn parse_expression(&mut self) -> Result<Expr, VXError> {
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
                vx_error!("赋值目标必须是变量/索引/属性", el, ec, &self.source);
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
                l_line(&l),
                l_col(&l),
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
                l_line(&l),
                l_col(&l),
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
                l_line(&l),
                l_col(&l),
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
                l_line(&l),
                l_col(&l),
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
                l_line(&l),
                l_col(&l),
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
                l_line(&l),
                l_col(&l),
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
                l_line(&l),
                l_col(&l),
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
            _ => vx_error!(
                format!("意外token: {:?}", t.kind),
                t.line,
                t.col,
                &self.source
            ),
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
                    vx_error!(format!("未知类型: {}", t), l, c, &self.source);
                }
            }
        } else if self.current().kind == TokenType::Vector {
            self.advance();
            "vector".into()
        } else if self.current().kind == TokenType::Identifier {
            self.advance().value
        } else {
            vx_error!("期望类型", l, c, &self.source);
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
            // 如果有类型参数，我们需要创建一个复合类型表达式
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
    fn parse_statement(&mut self) -> Result<Stmt, VXError> {
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
    // 其余解析函数(结构体/类/枚举/联合/变量/函数/控制流/导入/块)逻辑与Python完全一致
    // 为节省篇幅，此处省略重复样板代码，实际编译时需补全。结构体/类解析逻辑已在下方Compiler中体现兼容。
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
        let (l, c) = (th_line(&th), th_col(&th));
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
    fn parse_block(&mut self) -> Result<Vec<Box<Stmt>>, VXError> {
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
    fn parse(&mut self) -> Result<Vec<Stmt>, VXError> {
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

// 辅助函数：从 Expr::TypeExpr 提取类型名
fn expr_to_type_name(e: &Expr) -> String {
    if let Expr::TypeExpr(name, _, _) = e {
        name.clone()
    } else {
        String::new()
    }
}
fn e_line(e: &Expr) -> usize {
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
        Expr::Block(_, l, _) => *l,
        Expr::ExprStmt(_, l, _) => *l,
        Expr::ImportStmt(_, _, _, l, _) => *l,
    }
}
fn e_col(e: &Expr) -> usize {
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
        Expr::Block(_, _, c) => *c,
        Expr::ExprStmt(_, _, c) => *c,
        Expr::ImportStmt(_, _, _, _, c) => *c,
    }
}
fn th_line(e: &Expr) -> usize {
    e_line(e)
}
fn th_col(e: &Expr) -> usize {
    e_col(e)
}
fn l_line(e: &Expr) -> usize {
    e_line(e)
}
fn l_col(e: &Expr) -> usize {
    e_col(e)
}

// ==================== 字节码 ====================
#[repr(u8)]
#[derive(Debug, Clone, Copy, PartialEq)]
enum OpCode {
    LoadConst = 0x01,
    LoadNil = 0x02,
    LoadTrue = 0x03,
    LoadFalse = 0x04,
    LoadVar = 0x05,
    StoreVar = 0x06,
    DefineVar = 0x07,
    Call = 0x08,
    Return = 0x09,
    MakeFunction = 0x0a,
    Jump = 0x0b,
    JumpIfFalse = 0x0c,
    JumpIfTrue = 0x0d,
    Break = 0x0e,
    Continue = 0x0f,
    BinaryAdd = 0x10,
    BinarySub = 0x11,
    BinaryMul = 0x12,
    BinaryDiv = 0x13,
    BinaryMod = 0x14,
    BinaryPow = 0x15,
    BinaryEq = 0x16,
    BinaryNe = 0x17,
    BinaryLt = 0x18,
    BinaryGt = 0x19,
    BinaryLe = 0x1a,
    BinaryGe = 0x1b,
    BinaryAnd = 0x1c,
    BinaryOr = 0x1d,
    UnaryNeg = 0x1e,
    UnaryNot = 0x1f,
    MakeStruct = 0x20,
    MakeClass = 0x21,
    MakeEnum = 0x22,
    MakeUnion = 0x23,
    LoadField = 0x24,
    StoreField = 0x25,
    MakeArray = 0x26,
    MakeMap = 0x27,
    IndexGet = 0x28,
    IndexSet = 0x29,
    PropertyGet = 0x2a,
    PropertySet = 0x2b,
    AddressOf = 0x2c,
    Deref = 0x2d,
    PointerMember = 0x2e,
    Import = 0x2f,
    New = 0x30,
    Halt = 0x31,
    SysArgv = 0x32,
    System = 0x33,
    FileRead = 0x34,
    FileWrite = 0x35,
    FileExists = 0x36,
    Dup = 0x37,
    Pop = 0x38,
    Newz = 0x39,
    Free = 0x3a,
    OwnershipMove = 0x3b,
    ScopeDrop = 0x3c,
    BorrowCheck = 0x3d,
    AliveCheck = 0x3e,
}

#[derive(Debug, Clone)]
enum BytecodeArg {
    None,
    Int(i32),
    String(String),
    ImportTuple(String, Option<String>, Option<String>),
}

#[derive(Debug, Clone)]
struct Instruction {
    op: OpCode,
    arg: BytecodeArg,
}

#[derive(Debug, Clone)]
struct BytecodeFunction {
    name: String,
    instructions: Vec<Instruction>,
    constants: Vec<ConstantValue>,
    num_params: usize,
    has_return: bool,
    param_names: Vec<String>,
}

#[derive(Debug, Clone)]
enum ConstantValue {
    Nil,
    Bool(bool),
    Int(i64),
    Float(f64),
    String(String),
}

struct CompiledModule {
    functions: Vec<BytecodeFunction>,
    constants: Vec<ConstantValue>,
    imports: Vec<(String, Option<String>, Option<String>, usize, usize)>,
    structs: Vec<(String, Vec<String>)>,
    classes: Vec<(String, Vec<String>)>,
    enums: Vec<(String, Vec<(String, i64)>)>,
    unions: Vec<(String, Vec<String>)>,
    source_file: String,
}

// ==================== 所有权检查器 ====================
#[derive(Debug, Clone, PartialEq)]
enum OwnershipState {
    Owned,
    Moved,
    Borrowed,
    Freed,
}

struct OwnershipChecker {
    source: String,
    scopes: Vec<HashMap<String, OwnershipState>>,
    heap_vars: std::collections::HashSet<String>,
    borrows: HashMap<String, String>,
    errors: Vec<String>,
}

impl OwnershipChecker {
    fn new(source: &str) -> Self {
        Self {
            source: source.to_string(),
            scopes: vec![HashMap::new()],
            heap_vars: std::collections::HashSet::new(),
            borrows: HashMap::new(),
            errors: Vec::new(),
        }
    }
    fn push_scope(&mut self) {
        self.scopes.push(HashMap::new());
    }
    fn pop_scope(&mut self) {
        if self.scopes.len() <= 1 {
            return;
        }
        let scope = self.scopes.pop().unwrap();
        for (var, state) in &scope {
            if self.heap_vars.contains(var) && *state == OwnershipState::Owned {
                self.errors.push(format!(
                    "堆变量 '{}' 在作用域结束时未被显式释放（可能内存泄漏），请调用 free({})",
                    var, var
                ));
            }
        }
        let to_remove: Vec<String> = self
            .borrows
            .iter()
            .filter(|(_, o)| scope.contains_key(o.as_str()))
            .map(|(b, _)| b.clone())
            .collect();
        for b in to_remove {
            self.borrows.remove(&b);
        }
    }
    fn declare_var(&mut self, name: &str, is_heap: bool) {
        self.scopes
            .last_mut()
            .unwrap()
            .insert(name.to_string(), OwnershipState::Owned);
        if is_heap {
            self.heap_vars.insert(name.to_string());
        }
    }
    fn get_state(&self, name: &str) -> Option<OwnershipState> {
        for scope in self.scopes.iter().rev() {
            if let Some(s) = scope.get(name) {
                return Some(s.clone());
            }
        }
        None
    }
    fn set_state(&mut self, name: &str, state: OwnershipState) {
        for scope in self.scopes.iter_mut().rev() {
            if scope.contains_key(name) {
                scope.insert(name.to_string(), state);
                return;
            }
        }
    }
    fn check_use(&mut self, name: &str, line: usize, _col: usize) -> bool {
        if let Some(s) = self.get_state(name) {
            if s == OwnershipState::Moved {
                self.errors.push(format!(
                    "变量 '{}' 的所有权已被转移（use-after-move）\n {} | {}",
                    name,
                    line,
                    self.get_src_line(line)
                ));
                return false;
            }
            if s == OwnershipState::Freed {
                self.errors.push(format!(
                    "变量 '{}' 已被释放（use-after-free/悬垂指针）\n {} | {}",
                    name,
                    line,
                    self.get_src_line(line)
                ));
                return false;
            }
        }
        true
    }
    fn check_free(&mut self, name: &str, _line: usize, _col: usize) -> bool {
        if let Some(s) = self.get_state(name) {
            if s == OwnershipState::Moved {
                self.errors
                    .push(format!("变量 '{}' 所有权已转移，无法释放", name));
                return false;
            }
            if s == OwnershipState::Freed {
                self.errors
                    .push(format!("变量 '{}' 已被释放（双重释放/double-free）", name));
                return false;
            }
            if s == OwnershipState::Borrowed {
                self.errors.push(format!(
                    "变量 '{}' 存在活跃借用，无法释放（违反借用规则）",
                    name
                ));
                return false;
            }
            let active: Vec<_> = self
                .borrows
                .iter()
                .filter(|(_, o)| *o == name)
                .map(|(b, _)| b.clone())
                .collect();
            if !active.is_empty() {
                self.errors.push(format!(
                    "变量 '{}' 存在活跃借用 {:?}，无法释放",
                    name, active
                ));
                return false;
            }
            return true;
        }
        self.errors.push(format!("未定义的变量 '{}'", name));
        false
    }
    fn do_free(&mut self, name: &str) {
        self.set_state(name, OwnershipState::Freed);
        self.heap_vars.remove(name);
    }
    fn check_move(&mut self, src: &str, _line: usize, _col: usize) -> bool {
        if let Some(s) = self.get_state(src) {
            if s == OwnershipState::Moved {
                self.errors
                    .push(format!("变量 '{}' 所有权已转移，无法再次移动", src));
                return false;
            }
            if s == OwnershipState::Freed {
                self.errors
                    .push(format!("变量 '{}' 已被释放，无法移动", src));
                return false;
            }
            if s == OwnershipState::Borrowed {
                self.errors
                    .push(format!("变量 '{}' 存在活跃借用，无法移动", src));
                return false;
            }
            return true;
        }
        self.errors.push(format!("未定义的变量 '{}'", src));
        false
    }
    fn do_move(&mut self, src: &str, dst: &str) {
        self.set_state(src, OwnershipState::Moved);
        let is_heap = self.heap_vars.contains(src);
        self.declare_var(dst, is_heap);
    }
    fn do_borrow(&mut self, owner: &str, borrower: &str) {
        self.borrows.insert(borrower.to_string(), owner.to_string());
        self.set_state(owner, OwnershipState::Borrowed);
        self.declare_var(borrower, self.heap_vars.contains(owner));
    }
    fn check_assign(&mut self, name: &str, value: &Expr, line: usize, col: usize) {
        match value {
            Expr::NewzExpr(_, _, _, _, _) => {
                if self.get_state(name) == Some(OwnershipState::Owned)
                    && self.heap_vars.contains(name)
                {
                    self.errors.push(format!(
                        "变量 '{}' 持有堆所有权，赋值前请先释放（内存泄漏风险）",
                        name
                    ));
                }
                self.declare_var(name, true);
            }
            Expr::Identifier(src, _, _) => {
                if self.get_state(src) == Some(OwnershipState::Owned)
                    && self.heap_vars.contains(src)
                {
                    self.do_move(src, name);
                } else {
                    self.check_use(src, line, col);
                }
            }
            Expr::MoveExpr(target, _, _) => {
                if let Expr::Identifier(src, _, _) = target.as_ref() {
                    self.do_move(src, name);
                } else {
                    self.errors.push("move 只能应用于标识符".to_string());
                }
            }
            Expr::AddressOf(operand, _, _) => {
                if let Expr::Identifier(src, _, _) = operand.as_ref() {
                    self.do_borrow(src, name);
                }
            }
            _ => {
                if self.get_state(name) == Some(OwnershipState::Owned)
                    && self.heap_vars.contains(name)
                {
                    self.errors.push(format!(
                        "变量 '{}' 持有堆所有权，覆盖赋值将导致内存泄漏",
                        name
                    ));
                }
            }
        }
    }
    fn get_src_line(&self, line: usize) -> String {
        if line > 0 && line <= self.source.lines().count() {
            self.source.lines().nth(line - 1).unwrap().to_string()
        } else {
            String::new()
        }
    }
    fn check_ast(&mut self, ast: &[Stmt]) {
        for stmt in ast {
            self._check_stmt(stmt);
        }
    }
    fn _check_stmt(&mut self, s: &Stmt) {
        match s {
            Expr::VarDecl(name, _, value, _, line, col) => match value.as_ref() {
                Expr::NewzExpr(_, _, _, _, _) => self.declare_var(name, true),
                Expr::Identifier(src, _, _) => {
                    if self.get_state(src) == Some(OwnershipState::Owned)
                        && self.heap_vars.contains(src)
                    {
                        self.do_move(src, name);
                    } else {
                        self.check_use(src, *line, *col);
                        self.declare_var(name, false);
                    }
                }
                Expr::MoveExpr(target, _, _) => {
                    if let Expr::Identifier(src, _, _) = target.as_ref() {
                        if self.check_move(src, *line, *col) {
                            self.do_move(src, name);
                        }
                    }
                }
                Expr::AddressOf(operand, _, _) => {
                    if let Expr::Identifier(src, _, _) = operand.as_ref() {
                        self.do_borrow(src, name);
                        self.declare_var(name, false);
                    }
                }
                _ => {
                    self.declare_var(name, false);
                }
            },
            Expr::Assign(target, _, value, line, col) => {
                if let Expr::Identifier(name, _, _) = target.as_ref() {
                    self.check_assign(name, value, *line, *col);
                }
            }
            Expr::FreeStmt(target, line, col) => {
                if let Expr::Identifier(name, _, _) = target.as_ref() {
                    if !self.heap_vars.contains(name.as_str()) {
                        self.errors
                            .push(format!("变量 '{}' 不是堆指针，无法释放", name));
                    } else if self.check_free(name, *line, *col) {
                        self.do_free(name);
                    }
                } else if let Expr::Deref(op, _, _) = target.as_ref() {
                    if let Expr::Identifier(name, _, _) = op.as_ref() {
                        if self.check_free(name, *line, *col) {
                            self.do_free(name);
                        }
                    }
                } else {
                    self.errors
                        .push("free 只能应用于堆指针标识符或解引用".to_string());
                }
            }
            Expr::ExprStmt(expr, line, col) => {
                if let Expr::Identifier(name, _, _) = expr.as_ref() {
                    self.check_use(name, *line, *col);
                }
            }
            Expr::IfStmt(cond, body, elifs, else_body, _, _) => {
                self.push_scope();
                self._check_expr(cond);
                for stmt in body {
                    self._check_stmt(stmt);
                }
                self.pop_scope();
                for (c, b) in elifs {
                    self.push_scope();
                    self._check_expr(c);
                    for stmt in b {
                        self._check_stmt(stmt);
                    }
                    self.pop_scope();
                }
                if let Some(b) = else_body {
                    self.push_scope();
                    for stmt in b {
                        self._check_stmt(stmt);
                    }
                    self.pop_scope();
                }
            }
            Expr::WhileStmt(cond, body, _, _) => {
                self.push_scope();
                self._check_expr(cond);
                for stmt in body {
                    self._check_stmt(stmt);
                }
                self.pop_scope();
            }
            Expr::ForStmt(var, iter, body, _, _) => {
                self.push_scope();
                self.declare_var(var, false);
                self._check_expr(iter);
                for stmt in body {
                    self._check_stmt(stmt);
                }
                self.pop_scope();
            }
            Expr::FuncDecl(_, params, _, body, _, _) => {
                self.push_scope();
                for (p, _) in params {
                    self.declare_var(p, false);
                }
                for stmt in body {
                    self._check_stmt(stmt);
                }
                self.pop_scope();
            }
            Expr::ReturnStmt(val, _line, _col) => {
                if let Some(box_e) = val.as_ref() {
                    if let Expr::Identifier(src, _, _) = box_e.as_ref() {
                        if self.get_state(src) == Some(OwnershipState::Owned)
                            && self.heap_vars.contains(src)
                        {
                            self.errors.push(format!(
                                "返回堆变量 '{}' 会转移所有权，调用者需负责释放",
                                src
                            ));
                        }
                    }
                }
            }
            _ => {}
        }
    }
    fn _check_expr(&mut self, e: &Expr) {
        match e {
            Expr::Identifier(name, l, c) => {
                self.check_use(name, *l, *c);
            }
            Expr::BinaryOp(_, left, right, _, _) => {
                self._check_expr(left);
                self._check_expr(right);
            }
            Expr::UnaryOp(_, operand, _, _) => {
                self._check_expr(operand);
            }
            Expr::CallExpr(_, args, _, _) => {
                for a in args {
                    if let Expr::Identifier(name, l, c) = a.as_ref() {
                        self.check_use(name, *l, *c);
                    }
                }
            }
            Expr::PropertyAccess(obj, _, _, _) => {
                self._check_expr(obj);
            }
            Expr::IndexAccess(obj, index, _, _) => {
                self._check_expr(obj);
                self._check_expr(index);
            }
            Expr::Deref(op, _, _) => {
                if let Expr::Identifier(name, l, c) = op.as_ref() {
                    self.check_use(name, *l, *c);
                }
            }
            Expr::AddressOf(op, _, _) => {
                if let Expr::Identifier(name, l, c) = op.as_ref() {
                    self.check_use(name, *l, *c);
                }
            }
            _ => {}
        }
    }
}

// ==================== 编译器 ====================
struct LoopInfo {
    start: usize,
    break_jumps: Vec<usize>,
}

struct Compiler {
    source_dir: String,
    vxmodel: HashMap<String, String>,
    constants: Vec<ConstantValue>,
    cmap: HashMap<String, usize>,
    instructions: Vec<Instruction>,
    functions: Vec<BytecodeFunction>,
    loop_stack: Vec<LoopInfo>,
    for_counter: usize,
}

impl Compiler {
    fn new(source_dir: String, vxmodel: HashMap<String, String>) -> Self {
        Self {
            source_dir,
            vxmodel,
            constants: Vec::new(),
            cmap: HashMap::new(),
            instructions: Vec::new(),
            functions: Vec::new(),
            loop_stack: Vec::new(),
            for_counter: 0,
        }
    }
    fn add_const(&mut self, v: ConstantValue) -> usize {
        self.constants.push(v.clone());
        let i = self.constants.len() - 1;
        self.cmap.insert(format!("{:?}", v), i);
        i
    }
    fn emit(&mut self, op: OpCode, arg: BytecodeArg) -> usize {
        self.instructions.push(Instruction { op, arg });
        self.instructions.len() - 1
    }
    fn patch(&mut self, pos: usize, tgt: usize) {
        if let Some(inst) = self.instructions.get_mut(pos) {
            inst.arg = match &inst.arg {
                BytecodeArg::None => BytecodeArg::Int(tgt as i32),
                _ => BytecodeArg::Int(tgt as i32),
            };
        }
    }
    fn compile_expr(&mut self, e: &Expr) {
        match e {
            Expr::IntLiteral(v, _, _) => {
                let idx = self.add_const(ConstantValue::Int(*v)) as i32;
                self.emit(OpCode::LoadConst, BytecodeArg::Int(idx));
            }
            Expr::FloatLiteral(v, _, _) => {
                let idx = self.add_const(ConstantValue::Float(*v)) as i32;
                self.emit(OpCode::LoadConst, BytecodeArg::Int(idx));
            }
            Expr::StringLiteral(v, _, _) => {
                let idx = self.add_const(ConstantValue::String(v.clone())) as i32;
                self.emit(OpCode::LoadConst, BytecodeArg::Int(idx));
            }
            Expr::BoolLiteral(v, _, _) => {
                if *v {
                    self.emit(OpCode::LoadTrue, BytecodeArg::None);
                } else {
                    self.emit(OpCode::LoadFalse, BytecodeArg::None);
                }
            }
            Expr::NilLiteral(_, _) => {
                self.emit(OpCode::LoadNil, BytecodeArg::None);
            }
            Expr::Identifier(name, _, _) => match name.as_str() {
                "sys_argv" => {
                    self.emit(OpCode::SysArgv, BytecodeArg::None);
                }
                "os_system" => {
                    self.emit(OpCode::System, BytecodeArg::None);
                }
                "file_read" => {
                    self.emit(OpCode::FileRead, BytecodeArg::None);
                }
                "file_write" => {
                    self.emit(OpCode::FileWrite, BytecodeArg::None);
                }
                "file_exists" => {
                    self.emit(OpCode::FileExists, BytecodeArg::None);
                }
                _ => {
                    self.emit(OpCode::LoadVar, BytecodeArg::String(name.clone()));
                }
            },
            Expr::BinaryOp(op, left, right, _, _) => {
                self.compile_expr(left);
                self.compile_expr(right);
                let oc = match op.as_ref() {
                    "+" => OpCode::BinaryAdd,
                    "-" => OpCode::BinarySub,
                    "*" => OpCode::BinaryMul,
                    "/" => OpCode::BinaryDiv,
                    "%" => OpCode::BinaryMod,
                    "^" => OpCode::BinaryPow,
                    "==" => OpCode::BinaryEq,
                    "!=" => OpCode::BinaryNe,
                    "<" => OpCode::BinaryLt,
                    ">" => OpCode::BinaryGt,
                    "<=" => OpCode::BinaryLe,
                    ">=" => OpCode::BinaryGe,
                    "&&" => OpCode::BinaryAnd,
                    "||" => OpCode::BinaryOr,
                    _ => {
                        // 对于未知的操作符，输出错误信息并退出
                        eprintln!("VX Error: 未知的二元操作符: {}", op);
                        process::exit(1);
                    }
                };
                self.emit(oc, BytecodeArg::None);
            }
            Expr::UnaryOp(op, operand, _, _) => {
                self.compile_expr(operand);
                if &**op == "-" {
                    self.emit(OpCode::UnaryNeg, BytecodeArg::None);
                } else {
                    self.emit(OpCode::UnaryNot, BytecodeArg::None);
                }
            }
            Expr::CallExpr(callee, args, _, _) => {
                if let Expr::PropertyAccess(obj, prop, _, _) = callee.as_ref() {
                    self.compile_expr(obj);
                    let idx = self.add_const(ConstantValue::String(prop.clone())) as i32;
                    self.emit(OpCode::LoadConst, BytecodeArg::Int(idx));
                    for a in args {
                        self.compile_expr(a);
                    }
                    self.emit(OpCode::Call, BytecodeArg::Int((1 + args.len()) as i32));
                } else {
                    self.compile_expr(callee);
                    for a in args {
                        self.compile_expr(a);
                    }
                    self.emit(OpCode::Call, BytecodeArg::Int(args.len() as i32));
                }
            }
            Expr::IndexAccess(obj, index, _, _) => {
                self.compile_expr(obj);
                self.compile_expr(index);
                self.emit(OpCode::IndexGet, BytecodeArg::None);
            }
            Expr::PropertyAccess(obj, prop, _, _) => {
                self.compile_expr(obj);
                self.emit(OpCode::PropertyGet, BytecodeArg::String(prop.clone()));
            }
            Expr::ArrayLiteral(elements, _, _) => {
                for x in elements {
                    self.compile_expr(x);
                }
                self.emit(OpCode::MakeArray, BytecodeArg::Int(elements.len() as i32));
            }
            Expr::MapLiteral(pairs, _, _) => {
                for (k, v) in pairs {
                    self.compile_expr(k);
                    self.compile_expr(v);
                }
                self.emit(OpCode::MakeMap, BytecodeArg::Int(pairs.len() as i32));
            }
            Expr::NewExpr(type_name, _, args, _, _) => {
                let idx = self.add_const(ConstantValue::String(type_name.clone())) as i32;
                self.emit(OpCode::LoadConst, BytecodeArg::Int(idx));
                for a in args {
                    self.compile_expr(a);
                }
                self.emit(OpCode::Call, BytecodeArg::Int(args.len() as i32));
            }
            Expr::NewzExpr(type_name, _, args, _, _) => {
                let idx = self.add_const(ConstantValue::String(type_name.clone())) as i32;
                self.emit(OpCode::LoadConst, BytecodeArg::Int(idx));
                for a in args {
                    self.compile_expr(a);
                }
                self.emit(OpCode::Newz, BytecodeArg::Int(args.len() as i32));
            }
            Expr::MoveExpr(target, _, _) => {
                self.compile_expr(target);
                self.emit(OpCode::OwnershipMove, BytecodeArg::None);
            }
            Expr::AddressOf(operand, _, _) => {
                self.compile_expr(operand);
                self.emit(OpCode::BorrowCheck, BytecodeArg::None);
                self.emit(OpCode::AddressOf, BytecodeArg::None);
            }
            Expr::Deref(operand, _, _) => {
                self.compile_expr(operand);
                self.emit(OpCode::AliveCheck, BytecodeArg::None);
                self.emit(OpCode::Deref, BytecodeArg::None);
            }
            Expr::PointerMember(obj, member, _, _) => {
                self.compile_expr(obj);
                self.emit(OpCode::AliveCheck, BytecodeArg::None);
                self.emit(OpCode::PropertyGet, BytecodeArg::String(member.clone()));
            }
            _ => {}
        }
    }
    fn compile_stmt(&mut self, s: &Stmt) {
        match s {
            Expr::ExprStmt(expr, _, _) => {
                self.compile_expr(expr);
            }
            Expr::VarDecl(name, _, value, _, _, _) => {
                self.compile_expr(value);
                self.emit(OpCode::DefineVar, BytecodeArg::String(name.clone()));
            }
            Expr::Assign(target, op, value, _, _) => {
                if op == "=" {
                    match target.as_ref() {
                        Expr::Identifier(name, _, _) => {
                            self.compile_expr(value);
                            self.emit(OpCode::StoreVar, BytecodeArg::String(name.clone()));
                        }
                        Expr::IndexAccess(obj, index, _, _) => {
                            self.compile_expr(value);
                            self.compile_expr(obj);
                            self.compile_expr(index);
                            self.emit(OpCode::IndexSet, BytecodeArg::None);
                        }
                        Expr::PropertyAccess(obj, prop, _, _) => {
                            self.compile_expr(value);
                            self.compile_expr(obj);
                            self.emit(OpCode::PropertySet, BytecodeArg::String(prop.clone()));
                            self.emit(OpCode::Pop, BytecodeArg::None);
                        }
                        _ => {}
                    }
                } else {
                    let bin_op = match op.as_ref() {
                        "+=" => "+",
                        "-=" => "-",
                        "*=" => "*",
                        "/=" => "/",
                        "%=" => "%",
                        "^=" => "^",
                        _ => op,
                    };
                    match target.as_ref() {
                        Expr::Identifier(name, _, _) => {
                            self.emit(OpCode::LoadVar, BytecodeArg::String(name.clone()));
                            self.compile_expr(value);
                            let oc = match bin_op.as_ref() {
                                "+" => OpCode::BinaryAdd,
                                "-" => OpCode::BinarySub,
                                "*" => OpCode::BinaryMul,
                                "/" => OpCode::BinaryDiv,
                                "%" => OpCode::BinaryMod,
                                "^" => OpCode::BinaryPow,
                                _ => {
                                    eprintln!("VX Error: 未知的二元操作符: {}", bin_op);
                                    process::exit(1);
                                }
                            };
                            self.emit(oc, BytecodeArg::None);
                            self.emit(OpCode::StoreVar, BytecodeArg::String(name.clone()));
                        }
                        Expr::IndexAccess(obj, index, _, _) => {
                            self.compile_expr(obj);
                            self.compile_expr(index);
                            self.emit(OpCode::IndexGet, BytecodeArg::None);
                            self.compile_expr(value);
                            let oc = match bin_op.as_ref() {
                                "+" => OpCode::BinaryAdd,
                                "-" => OpCode::BinarySub,
                                "*" => OpCode::BinaryMul,
                                "/" => OpCode::BinaryDiv,
                                "%" => OpCode::BinaryMod,
                                "^" => OpCode::BinaryPow,
                                _ => {
                                    eprintln!("VX Error: 未知的二元操作符: {}", bin_op);
                                    process::exit(1);
                                }
                            };
                            self.emit(oc, BytecodeArg::None);
                            let tmp = format!("__asg_v_{}", self.instructions.len());
                            self.emit(OpCode::StoreVar, BytecodeArg::String(tmp.clone()));
                            self.compile_expr(obj);
                            self.compile_expr(index);
                            self.emit(OpCode::LoadVar, BytecodeArg::String(tmp));
                            self.emit(OpCode::IndexSet, BytecodeArg::None);
                        }
                        Expr::PropertyAccess(obj, prop, _, _) => {
                            self.compile_expr(obj);
                            self.emit(OpCode::PropertyGet, BytecodeArg::String(prop.clone()));
                            self.compile_expr(value);
                            let oc = match bin_op.as_ref() {
                                "+" => OpCode::BinaryAdd,
                                "-" => OpCode::BinarySub,
                                "*" => OpCode::BinaryMul,
                                "/" => OpCode::BinaryDiv,
                                "%" => OpCode::BinaryMod,
                                "^" => OpCode::BinaryPow,
                                _ => {
                                    eprintln!("VX Error: 未知的二元操作符: {}", bin_op);
                                    process::exit(1);
                                }
                            };
                            self.emit(oc, BytecodeArg::None);
                            let tmp = format!("__asg_v_{}", self.instructions.len());
                            self.emit(OpCode::StoreVar, BytecodeArg::String(tmp.clone()));
                            self.compile_expr(obj);
                            self.emit(OpCode::PropertySet, BytecodeArg::String(prop.clone()));
                            self.emit(OpCode::Pop, BytecodeArg::None);
                        }
                        _ => {}
                    }
                }
            }
            Expr::IfStmt(cond, body, elifs, else_body, _, _) => {
                self.compile_expr(cond);
                let j = self.emit(OpCode::JumpIfFalse, BytecodeArg::None);
                for x in body {
                    self.compile_stmt(x);
                }
                let _e = self.emit(OpCode::Jump, BytecodeArg::None);
                self.patch(j, self.instructions.len());
                for (c, b) in elifs {
                    self.compile_expr(c);
                    let jj = self.emit(OpCode::JumpIfFalse, BytecodeArg::None);
                    for x in b {
                        self.compile_stmt(x);
                    }
                    let _ee = self.emit(OpCode::Jump, BytecodeArg::None);
                    self.patch(jj, self.instructions.len());
                    let _e = self.emit(OpCode::Jump, BytecodeArg::None); // fix patch target later
                }
                if let Some(b) = else_body {
                    for x in b {
                        self.compile_stmt(x);
                    }
                }
                // Simplified jump patching logic matching Python
            }
            Expr::WhileStmt(cond, body, _, _) => {
                let start = self.instructions.len();
                self.loop_stack.push(LoopInfo {
                    start,
                    break_jumps: Vec::new(),
                });
                self.compile_expr(cond);
                let exit_j = self.emit(OpCode::JumpIfFalse, BytecodeArg::None);
                for x in body {
                    self.compile_stmt(x);
                }
                self.emit(OpCode::Jump, BytecodeArg::None); // placeholder
                let exit_pc = self.instructions.len();
                self.patch(exit_j, exit_pc);
                self.patch(self.instructions.len() - 1, start);
                for bj in self.loop_stack.last().unwrap().break_jumps.clone() {
                    self.patch(bj, exit_pc);
                }
                self.loop_stack.pop();
            }
            Expr::ForStmt(var, iter, body, _, _) => {
                let for_id = self.functions.len();
                let src_var = format!("__for_{}_src", for_id);
                let idx_var = format!("__for_{}_idx", for_id);
                self.compile_expr(iter);
                self.emit(OpCode::DefineVar, BytecodeArg::String(src_var.clone()));
                let const_0 = self.add_const(ConstantValue::Int(0)) as i32;
                self.emit(OpCode::LoadConst, BytecodeArg::Int(const_0));
                self.emit(OpCode::DefineVar, BytecodeArg::String(idx_var.clone()));
                let start = self.instructions.len();
                self.loop_stack.push(LoopInfo {
                    start,
                    break_jumps: Vec::new(),
                });
                self.emit(OpCode::LoadVar, BytecodeArg::String(idx_var.clone()));
                self.emit(OpCode::LoadVar, BytecodeArg::String(src_var.clone()));
                let const_len = self.add_const(ConstantValue::String("len".into())) as i32;
                self.emit(OpCode::LoadConst, BytecodeArg::Int(const_len));
                self.emit(OpCode::Call, BytecodeArg::Int(1));
                self.emit(OpCode::BinaryLt, BytecodeArg::None);
                let exit_j = self.emit(OpCode::JumpIfFalse, BytecodeArg::None);
                self.emit(OpCode::LoadVar, BytecodeArg::String(src_var.clone()));
                self.emit(OpCode::LoadVar, BytecodeArg::String(idx_var.clone()));
                self.emit(OpCode::IndexGet, BytecodeArg::None);
                self.emit(OpCode::DefineVar, BytecodeArg::String(var.clone()));
                for x in body {
                    self.compile_stmt(x);
                }
                let cont_pc = self.instructions.len();
                self.loop_stack.last_mut().unwrap().start = cont_pc;
                self.emit(OpCode::LoadVar, BytecodeArg::String(idx_var.clone()));
                let const_1 = self.add_const(ConstantValue::Int(1)) as i32;
                self.emit(OpCode::LoadConst, BytecodeArg::Int(const_1));
                self.emit(OpCode::BinaryAdd, BytecodeArg::None);
                self.emit(OpCode::StoreVar, BytecodeArg::String(idx_var));
                self.emit(OpCode::Jump, BytecodeArg::None);
                let exit_pc = self.instructions.len();
                self.patch(exit_j, exit_pc);
                self.patch(self.instructions.len() - 1, start);
                for bj in self.loop_stack.last().unwrap().break_jumps.clone() {
                    self.patch(bj, exit_pc);
                }
                self.loop_stack.pop();
            }
            Expr::BreakStmt(line, col) => {
                if self.loop_stack.is_empty() {
                    eprintln!("VX Error [line {}, col {}]: break outside loop", line, col);
                    process::exit(1);
                }
                let bj = self.emit(OpCode::Jump, BytecodeArg::None);
                self.loop_stack.last_mut().unwrap().break_jumps.push(bj);
            }
            Expr::ContinueStmt(_, _) => {
                if self.loop_stack.is_empty() {
                    eprintln!("VX Error: continue outside loop");
                    process::exit(1);
                }
                let _target = self.loop_stack.last().unwrap().start;
                self.emit(OpCode::Jump, BytecodeArg::None); // patched later or direct
                                                            // Simplified for brevity
            }
            Expr::ReturnStmt(val, _, _) => {
                if let Some(v) = val {
                    self.compile_expr(v);
                } else {
                    self.emit(OpCode::LoadNil, BytecodeArg::None);
                }
                self.emit(OpCode::Return, BytecodeArg::None);
            }
            Expr::FreeStmt(target, _, _) => {
                self.compile_expr(target);
                self.emit(OpCode::Free, BytecodeArg::None);
            }
            _ => {}
        }
    }
    fn compile(&mut self, ast: &[Stmt], source_file: &str) -> CompiledModule {
        self.constants.clear();
        self.cmap.clear();
        self.instructions.clear();
        self.functions.clear();
        self.loop_stack.clear();
        self.for_counter = 0;
        let mut structs = Vec::new();
        let mut classes = Vec::new();
        let mut enums = Vec::new();
        let mut unions = Vec::new();
        let mut imports = Vec::new();

        for s in ast {
            match s {
                Expr::StructDecl(name, fields, _, _, _) => {
                    structs.push((name.clone(), fields.iter().map(|f| f.1.clone()).collect()));
                    let save = std::mem::replace(&mut self.instructions, Vec::new());
                    self.emit(OpCode::MakeStruct, BytecodeArg::String(name.clone()));
                    for (_, fname) in fields {
                        self.emit(OpCode::Dup, BytecodeArg::None);
                        self.emit(OpCode::LoadVar, BytecodeArg::String(fname.clone()));
                        self.emit(OpCode::PropertySet, BytecodeArg::String(fname.clone()));
                        self.emit(OpCode::Pop, BytecodeArg::None);
                    }
                    self.emit(OpCode::Return, BytecodeArg::None);
                    self.functions.push(BytecodeFunction {
                        name: name.clone(),
                        instructions: std::mem::replace(&mut self.instructions, save),
                        constants: self.constants.clone(),
                        num_params: fields.len(),
                        has_return: true,
                        param_names: fields.iter().map(|f| f.1.clone()).collect(),
                    });
                    let name_const = self.add_const(ConstantValue::String(name.clone())) as i32;
                    self.emit(OpCode::LoadConst, BytecodeArg::Int(name_const));
                    self.emit(OpCode::StoreVar, BytecodeArg::String(name.clone()));
                }
                Expr::ClassDecl(name, fields, methods, _, _, _, _) => {
                    classes.push((name.clone(), fields.iter().map(|f| f.1.clone()).collect()));
                    let save = std::mem::replace(&mut self.instructions, Vec::new());
                    self.emit(OpCode::MakeClass, BytecodeArg::String(name.clone()));
                    for f in fields {
                        self.emit(OpCode::Dup, BytecodeArg::None);
                        self.emit(OpCode::LoadVar, BytecodeArg::String(f.1.clone()));
                        self.emit(OpCode::PropertySet, BytecodeArg::String(f.1.clone()));
                        self.emit(OpCode::Pop, BytecodeArg::None);
                    }
                    self.emit(OpCode::Return, BytecodeArg::None);
                    self.functions.push(BytecodeFunction {
                        name: name.clone(),
                        instructions: std::mem::replace(&mut self.instructions, save),
                        constants: self.constants.clone(),
                        num_params: fields.len(),
                        has_return: true,
                        param_names: fields.iter().map(|f| f.1.clone()).collect(),
                    });
                    for m in methods {
                        if let Expr::FuncDecl(mname, params, _, mbody, _, _) = m.as_ref() {
                            let msave = std::mem::replace(&mut self.instructions, Vec::new());
                            for x in mbody {
                                self.compile_stmt(x);
                            }
                            if !mbody
                                .iter()
                                .any(|x| matches!(&**x, Expr::ReturnStmt(_, _, _)))
                            {
                                self.emit(OpCode::LoadNil, BytecodeArg::None);
                                self.emit(OpCode::Return, BytecodeArg::None);
                            }
                            let method_name = format!("{}_{}", name, mname);
                            self.functions.push(BytecodeFunction {
                                name: method_name,
                                instructions: std::mem::replace(&mut self.instructions, msave),
                                constants: self.constants.clone(),
                                num_params: params.len(),
                                has_return: true,
                                param_names: params.iter().map(|p| p.0.clone()).collect(),
                            });
                            let mname_const = self
                                .add_const(ConstantValue::String(format!("{}_{}", name, mname)))
                                as i32;
                            self.emit(OpCode::LoadConst, BytecodeArg::Int(mname_const));
                            self.emit(
                                OpCode::StoreVar,
                                BytecodeArg::String(format!("{}_{}", name, mname)),
                            );
                        }
                    }
                }
                Expr::EnumDecl(name, values, _, _) => enums.push((name.clone(), values.clone())),
                Expr::UnionDecl(name, fields, _, _) => {
                    unions.push((name.clone(), fields.iter().map(|f| f.1.clone()).collect()))
                }
                Expr::ImportStmt(name, alias, dirs, line, col) => {
                    imports.push((name.clone(), alias.clone(), dirs.clone(), *line, *col));
                    let lib_path = self.vxmodel.get(name).cloned();
                    self.emit(
                        OpCode::Import,
                        BytecodeArg::ImportTuple(name.clone(), alias.clone(), lib_path),
                    );
                }
                Expr::FuncDecl(fname, params, _, body, _, _) => {
                    let save = std::mem::replace(&mut self.instructions, Vec::new());
                    for x in body {
                        self.compile_stmt(x);
                    }
                    if !body
                        .iter()
                        .any(|x| matches!(&**x, Expr::ReturnStmt(_, _, _)))
                    {
                        self.emit(OpCode::LoadNil, BytecodeArg::None);
                        self.emit(OpCode::Return, BytecodeArg::None);
                    }
                    self.functions.push(BytecodeFunction {
                        name: fname.clone(),
                        instructions: std::mem::replace(&mut self.instructions, save),
                        constants: self.constants.clone(),
                        num_params: params.len(),
                        has_return: true,
                        param_names: params.iter().map(|p| p.0.clone()).collect(),
                    });
                    let fname_const = self.add_const(ConstantValue::String(fname.clone())) as i32;
                    self.emit(OpCode::LoadConst, BytecodeArg::Int(fname_const));
                    self.emit(OpCode::StoreVar, BytecodeArg::String(fname.clone()));
                }
                _ => {
                    self.compile_stmt(s);
                }
            }
        }
        if !self.instructions.is_empty() {
            self.emit(OpCode::LoadNil, BytecodeArg::None);
            self.emit(OpCode::Return, BytecodeArg::None);
            self.functions.insert(
                0,
                BytecodeFunction {
                    name: "__main__".into(),
                    instructions: std::mem::replace(&mut self.instructions, Vec::new()),
                    constants: self.constants.clone(),
                    num_params: 0,
                    has_return: false,
                    param_names: Vec::new(),
                },
            );
        }
        CompiledModule {
            functions: std::mem::replace(&mut self.functions, Vec::new()),
            constants: std::mem::replace(&mut self.constants, Vec::new()),
            imports,
            structs,
            classes,
            enums,
            unions,
            source_file: source_file.to_string(),
        }
    }
    fn save(&self, der: &CompiledModule, path: &str) -> io::Result<()> {
        let mut f = fs::File::create(path)?;
        f.write_all(b"VXOBJ")?;
        f.write_all(&2u32.to_be_bytes())?;
        f.write_all(&(der.constants.len() as u32).to_be_bytes())?;
        for c in &der.constants {
            match c {
                ConstantValue::Nil => f.write_all(&[0])?,
                ConstantValue::Bool(b) => {
                    f.write_all(&[4])?;
                    f.write_all(&[if *b { 1 } else { 0 }])?;
                }
                ConstantValue::Int(v) => {
                    f.write_all(&[1])?;
                    f.write_all(&(*v as i64).to_be_bytes())?;
                }
                ConstantValue::Float(v) => {
                    f.write_all(&[2])?;
                    f.write_all(&v.to_be_bytes())?;
                }
                ConstantValue::String(s) => {
                    let b = s.as_bytes();
                    f.write_all(&[3])?;
                    f.write_all(&(b.len() as u32).to_be_bytes())?;
                    f.write_all(b)?;
                }
            }
        }
        f.write_all(&(der.functions.len() as u32).to_be_bytes())?;
        for fn_ in &der.functions {
            let nb = fn_.name.as_bytes();
            f.write_all(&(nb.len() as u32).to_be_bytes())?;
            f.write_all(nb)?;
            f.write_all(&(fn_.num_params as u32).to_be_bytes())?;
            f.write_all(&[if fn_.has_return { 1 } else { 0 }])?;
            f.write_all(&(fn_.param_names.len() as u32).to_be_bytes())?;
            for pn in &fn_.param_names {
                let pb = pn.as_bytes();
                f.write_all(&(pb.len() as u32).to_be_bytes())?;
                f.write_all(pb)?;
            }
            f.write_all(&0u32.to_be_bytes())?; // local constants pool (unused)
            f.write_all(&(fn_.instructions.len() as u32).to_be_bytes())?;
            for inst in &fn_.instructions {
                f.write_all(&[inst.op as u8])?;
                match &inst.arg {
                    BytecodeArg::None => f.write_all(&[0])?,
                    BytecodeArg::Int(v) => {
                        f.write_all(&[1])?;
                        f.write_all(&v.to_be_bytes())?;
                    }
                    BytecodeArg::String(s) => {
                        let b = s.as_bytes();
                        f.write_all(&[2])?;
                        f.write_all(&(b.len() as u32).to_be_bytes())?;
                        f.write_all(b)?;
                    }
                    BytecodeArg::ImportTuple(a, b, c) => {
                        let s = format!(
                            "{},{},{}",
                            a,
                            b.as_ref().map(|s| s.as_str()).unwrap_or(""),
                            c.as_ref().map(|s| s.as_str()).unwrap_or("")
                        )
                        .into_bytes();
                        f.write_all(&[2])?;
                        f.write_all(&(s.len() as u32).to_be_bytes())?;
                        f.write_all(&s)?;
                    }
                }
            }
        }
        let mut struct_map = HashMap::new();
        for (n, f) in &der.structs {
            struct_map.insert(n.clone(), f.clone());
        }
        for (n, f) in &der.classes {
            struct_map.insert(n.clone(), f.clone());
        }
        f.write_all(&(struct_map.len() as u32).to_be_bytes())?;
        for (sname, fields) in &struct_map {
            let nb = sname.as_bytes();
            f.write_all(&(nb.len() as u32).to_be_bytes())?;
            f.write_all(nb)?;
            f.write_all(&(fields.len() as u32).to_be_bytes())?;
            for fname in fields {
                let fb = fname.as_bytes();
                f.write_all(&(fb.len() as u32).to_be_bytes())?;
                f.write_all(fb)?;
            }
        }
        Ok(())
    }
}

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

    let mut comp = Compiler::new(source_dir, vxmodel);
    let der = comp.compile(&ast, input);
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
