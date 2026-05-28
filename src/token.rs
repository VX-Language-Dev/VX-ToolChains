
// VX Language Compiler v3.0 - Token Module
// 词法分析器（Lexer）和 Token 定义

use std::collections::HashMap;
use std::fmt;

// ==================== 错误处理 ====================
#[derive(Debug)]
pub struct VXError {
    pub msg: String,
    pub line: usize,
    pub col: usize,
    pub source: Option<String>,
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

impl std::error::Error for VXError {}

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

// ==================== Token 类型 ====================
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum TokenType {
    If,
    Elif,
    Else,
    For,
    While,
    Break,
    Continue,
    Func,
    Return,
    Move,
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
pub struct Token {
    pub kind: TokenType,
    pub value: String,
    pub line: usize,
    pub col: usize,
}

pub const KEYWORDS: &[( &str, TokenType)] = &[
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
    ("move", TokenType::Move),
    ("this", TokenType::This),
    ("public", TokenType::Public),
    ("private", TokenType::Private),
    ("protected", TokenType::Protected),
    ("extends", TokenType::Extends),
    ("implements", TokenType::Implements),
];

// ==================== 词法分析器 ====================
pub struct Lexer {
    source: String,
    pos: usize,
    line: usize,
    col: usize,
    pub tokens: Vec<Token>,
    indent_stack: Vec<usize>,
}

impl Lexer {
    pub fn new(source: &str) -> Self {
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

    pub fn tokenize(mut self) -> Result<Vec<Token>, VXError> {
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
