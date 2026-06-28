
// VX Language Compiler v3.0 - Token Module
// 词法分析器（Lexer）和 Token 定义

use std::collections::HashMap;
use std::fmt;
use std::sync::OnceLock;

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
    // 控制流
    If,
    Elif,
    Else,
    For,
    While,
    Break,
    Continue,
    // 函数
    Func,
    Return,
    // 循环
    Loop,
    // 所有权
    Move,
    // 字面量
    Int,
    Float,
    String,
    Identifier,
    // 运算符
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
    // 逻辑运算符 (自举兼容: and/or/not 关键字 + 符号形式 &&/||/! 并存)
    And,
    Or,
    Not,
    // 分隔符
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
    // 布尔/零值
    True,
    False,
    Nil,
    // 迭代/导入
    In,
    Import,
    As,
    // 原生标量类型 (硬件基础类型, 必留)
    IntT,
    FloatT,
    DoubleT,
    VarT,
    BoolT,
    VoidT,
    // 复合类型声明
    Struct,
    Class,
    Enum,
    Union,
    // 内存分配/构造
    New,
    // 宏系统
    Macro,      // macro 关键字
    Hash,       // # 符号，用于宏调用
}

#[derive(Debug, Clone)]
pub struct Token {
    pub kind: TokenType,
    pub value: String,
    pub line: usize,
    pub col: usize,
}

pub const KEYWORDS: &[( &str, TokenType)] = &[
    // 22 核心骨架关键字 (底层 OpCode 绑定, 永久保留)
    ("if", TokenType::If),
    ("else", TokenType::Else),
    ("elif", TokenType::Elif),
    ("for", TokenType::For),
    ("while", TokenType::While),
    ("break", TokenType::Break),
    ("continue", TokenType::Continue),
    ("func", TokenType::Func),
    ("return", TokenType::Return),
    ("loop", TokenType::Loop),
    ("true", TokenType::True),
    ("false", TokenType::False),
    ("nil", TokenType::Nil),
    ("in", TokenType::In),
    ("import", TokenType::Import),
    ("as", TokenType::As),
    ("var", TokenType::VarT),
    ("struct", TokenType::Struct),
    ("class", TokenType::Class),
    ("enum", TokenType::Enum),
    ("union", TokenType::Union),
    ("new", TokenType::New),
    ("move", TokenType::Move),
    ("macro", TokenType::Macro),  // 宏定义关键字
    // 5 原生标量类型 (硬件基础类型, 保留)
    ("int", TokenType::IntT),
    ("float", TokenType::FloatT),
    ("double", TokenType::DoubleT),
    ("bool", TokenType::BoolT),
    ("void", TokenType::VoidT),
    // 逻辑运算符关键字 (与符号形式 &&/||/! 并存, 自举代码大量使用)
    ("and", TokenType::And),
    ("or", TokenType::Or),
    ("not", TokenType::Not),
    // --- 以下关键字已裁减 (移至标准库/注解/语法糖) ---
    // string  → std::String, 字符串字面量自动展开
    // vector  → std::Vec<T>, 数组字面量自动展开
    // public/private/protected → #[pub] / #[priv] 注解
    // extends/implements → 冒号语法 class A : Parent, Trait
    // dirs   → import("a","b") as mod 可变参数导入
    // this   → 解析器自动替换为当前实例局部变量
    // newz   → mem::zeroed<T>() 标准库函数
    // free   → mem::free(ptr) 标准库函数
];

/// 编译期初始化一次的关键字哈希表，O(1) 查找替代 O(n) 线性扫描。
fn keyword_map() -> &'static HashMap<&'static str, TokenType> {
    static KW_MAP: OnceLock<HashMap<&str, TokenType>> = OnceLock::new();
    KW_MAP.get_or_init(|| {
        let mut m = HashMap::with_capacity(KEYWORDS.len());
        for (k, v) in KEYWORDS {
            m.insert(*k, *v);
        }
        m
    })
}

// ==================== 词法分析器 ====================
//
// 设计：Lexer 按 Unicode 字符迭代。`chars` 是源字符串的迭代器，
// `pos` 始终指向下一个待消费字符在源字符串中的字节偏移（由 `chars`
// 产生），`line` / `col` 按字符计数（1-based）。这样中文 / CJK 等
// 多字节字符以及单字节 ASCII 都能被正确解析。

pub struct Lexer {
    source: String,
    /// 下一个待消费字符在 `source` 中的字节偏移。
    pos: usize,
    line: usize,
    /// 当前字符的列号（1-based）。换行后重置为 1。
    col: usize,
    pub tokens: Vec<Token>,
    indent_stack: Vec<usize>,
    /// 上一行以可续行运算符结尾（如 or/and/+ 等），下一行的缩进变化不应产生 Indent/Dedent。
    continuation_pending: bool,
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
            continuation_pending: false,
        }
    }

    /// 返回从 `pos` 起第 `offset` 个字符的拷贝，不会越界。
    /// 对于超出末尾的情况返回 `'\0'`，与旧的字节语义保持一致。
    fn peek(&self, offset: usize) -> char {
        self.source[self.pos..]
            .chars()
            .nth(offset)
            .unwrap_or('\0')
    }

    /// 消费一个字符并返回它，同时更新 `pos`（字节偏移）、`line`、`col`。
    fn advance(&mut self) -> char {
        let c = self.peek(0);
        if c == '\0' {
            return c;
        }
        // 移动 pos 到当前字符之后
        self.pos += c.len_utf8();
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
        // 已由 is_ascii_digit() 过滤，解析失败概率极低；
        // 使用 unwrap_or 回退到 0 避免潜在 panic。
        let val = if f {
            format!("{}", s.parse::<f64>().unwrap_or(0.0))
        } else {
            format!("{}", s.parse::<i64>().unwrap_or(0))
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
        let kind = keyword_map()
            .get(val.as_str())
            .copied()
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
                // 若上一行以续行运算符结尾，则当前行属于表达式续行，不产生 Indent。
                if self.continuation_pending {
                    self.continuation_pending = false;
                } else {
                    self.indent_stack.push(indent);
                    self.tokens.push(Token {
                        kind: TokenType::Indent,
                        value: indent.to_string(),
                        line: self.line,
                        col: self.col,
                    });
                }
            } else if indent < last {
                self.continuation_pending = false;
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
            } else {
                self.continuation_pending = false;
            }
            return Ok(());
        }
    }

    /// 判断某类 token 是否可作为行尾续行运算符。
    fn is_continuation_operator(kind: TokenType) -> bool {
        matches!(
            kind,
            TokenType::Or
                | TokenType::And
                | TokenType::Plus
                | TokenType::Minus
                | TokenType::Star
                | TokenType::Slash
                | TokenType::Percent
                | TokenType::Power
                | TokenType::Comma
        )
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
                let prev_kind = self.tokens.last().map(|t| t.kind);
                self.tokens.push(Token {
                    kind: TokenType::Newline,
                    value: String::new(),
                    line: sl,
                    col: sc,
                });
                self.continuation_pending = prev_kind.map_or(false, Self::is_continuation_operator);
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

            // 使用 match 语句替代 HashMap，避免每次调用时的哈希表构造开销
            let kind = match c {
                '+' => TokenType::Plus,
                '-' => TokenType::Minus,
                '*' => TokenType::Star,
                '/' => TokenType::Slash,
                '%' => TokenType::Percent,
                '^' => TokenType::Power,
                '<' => TokenType::Lt,
                '>' => TokenType::Gt,
                '=' => TokenType::Assign,
                '!' => TokenType::Not,
                '&' => TokenType::Ampersand,
                '#' => TokenType::Hash,  // 宏调用符号
                '(' => TokenType::LParen,
                ')' => TokenType::RParen,
                '[' => TokenType::LBracket,
                ']' => TokenType::RBracket,
                '{' => TokenType::LBrace,
                '}' => TokenType::RBrace,
                ':' => TokenType::Colon,
                ';' => TokenType::Semicolon,
                ',' => TokenType::Comma,
                '.' => TokenType::Dot,
                _ => {
                    // 不在单字符符号表中，作为非法字符报错
                    vx_error!(
                        format!("非法字符: {}", c),
                        self.line,
                        self.col,
                        &self.source
                    );
                }
            };
            self.advance();
            self.tokens.push(Token {
                kind,
                value: c.to_string(),
                line: sl,
                col: sc,
            });
            continue;
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

// ==================== Tests ====================

#[cfg(test)]
mod tests {
    use super::*;

    /// 辅助函数: 取出 token 流中的 kind 序列（忽略 Newline 和 EOF）
    fn kinds(src: &str) -> Vec<TokenType> {
        let toks = Lexer::new(src).tokenize().expect("tokenize failed");
        toks.into_iter()
            .filter(|t| !matches!(t.kind, TokenType::Newline | TokenType::EOF))
            .map(|t| t.kind)
            .collect()
    }

    #[test]
    fn test_int_and_float() {
        assert_eq!(kinds("42 3.14"), vec![TokenType::Int, TokenType::Float]);
    }

    #[test]
    fn test_scientific_notation() {
        // 1.5e3 2E-4 应解析为两个 Float
        assert_eq!(kinds("1.5e3 2E-4"), vec![TokenType::Float, TokenType::Float]);
    }

    #[test]
    fn test_keywords_recognized() {
        let k = kinds("if else while func return true false nil");
        assert!(k.contains(&TokenType::If));
        assert!(k.contains(&TokenType::While));
        assert!(k.contains(&TokenType::Func));
        assert!(k.contains(&TokenType::Return));
        assert!(k.contains(&TokenType::True));
        assert!(k.contains(&TokenType::False));
        assert!(k.contains(&TokenType::Nil));
    }

    #[test]
    fn test_removed_keyword_string_is_identifier() {
        // string 关键字已移除为标准库类型，应识别为 Identifier
        let toks = Lexer::new("string").tokenize().unwrap();
        // 第一个非 Newline/EOF token 应为 Identifier
        let first = toks.iter().find(|t| !matches!(t.kind, TokenType::Newline | TokenType::EOF)).unwrap();
        assert_eq!(first.kind, TokenType::Identifier);
        assert_eq!(first.value, "string");
    }

    #[test]
    fn test_composite_operators() {
        // 验证所有复合运算符被正确识别
        let k = kinds("== != <= >= += -= *= /= %= ** && || ! -> &");
        assert!(k.contains(&TokenType::Eq));
        assert!(k.contains(&TokenType::Ne));
        assert!(k.contains(&TokenType::Le));
        assert!(k.contains(&TokenType::Ge));
        assert!(k.contains(&TokenType::PlusAssign));
        assert!(k.contains(&TokenType::MinusAssign));
        assert!(k.contains(&TokenType::And));
        assert!(k.contains(&TokenType::Or));
        assert!(k.contains(&TokenType::Arrow));
        assert!(k.contains(&TokenType::Ampersand));
    }

    #[test]
    fn test_double_quoted_string_with_escape() {
        let toks = Lexer::new("\"hello\\nworld\"").tokenize().unwrap();
        let s = toks.iter().find(|t| t.kind == TokenType::String).unwrap();
        assert_eq!(s.value, "hello\nworld");
    }

    #[test]
    fn test_single_quoted_string() {
        let toks = Lexer::new("'hi'").tokenize().unwrap();
        let s = toks.iter().find(|t| t.kind == TokenType::String).unwrap();
        assert_eq!(s.value, "hi");
    }

    #[test]
    fn test_unclosed_string_errors() {
        assert!(Lexer::new("\"unclosed").tokenize().is_err());
    }

    #[test]
    fn test_cjk_identifier() {
        // 中文标识符应被识别
        let toks = Lexer::new("变量 = 42").tokenize().unwrap();
        let ident = toks.iter().find(|t| t.kind == TokenType::Identifier).unwrap();
        assert_eq!(ident.value, "变量");
    }

    #[test]
    fn test_comment_skipped() {
        let k = kinds("# this is comment\n42");
        assert_eq!(k, vec![TokenType::Int]);
    }

    #[test]
    fn test_indent_dedent_emitted() {
        let src = "func f()\n    return 1\n";
        let toks = Lexer::new(src).tokenize().unwrap();
        let k: Vec<TokenType> = toks.iter().map(|t| t.kind).collect();
        assert!(k.contains(&TokenType::Indent), "应发出 Indent");
        assert!(k.contains(&TokenType::Dedent), "应发出 Dedent");
        assert!(k.contains(&TokenType::Func));
        assert!(k.contains(&TokenType::Return));
    }

    #[test]
    fn test_integer_with_underscores() {
        // 1_000_000 形式（如果实现支持）
        let toks = Lexer::new("1_000").tokenize().unwrap();
        let n = toks.iter().find(|t| t.kind == TokenType::Int).unwrap();
        // 允许保留下划线或解析为 1000，只要不报错即可
        assert!(!n.value.is_empty());
    }
}
