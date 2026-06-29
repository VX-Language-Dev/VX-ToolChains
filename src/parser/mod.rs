// VX Language Compiler v3.0 - Parser Module
// 语法分析器入口：模块声明 + Parser 结构体 + 核心辅助方法
// 表达式解析 → expr.rs  |  语句解析 → stmt.rs  |  AST 定义 → ast.rs

mod ast;
mod expr;
mod stmt;
#[cfg(test)]
mod tests;

// 对外公开 AST 类型
pub use ast::{Expr, Stmt, get_src_line, expr_to_type_name};

use crate::token::{Token, TokenType, VXError};
use std::sync::OnceLock;

/// 返回 EOF 哨兵 Token 的静态引用，避免空 tokens 时 panic
fn eof_sentinel() -> &'static Token {
    static EOF: OnceLock<Token> = OnceLock::new();
    EOF.get_or_init(|| Token {
        kind: TokenType::EOF,
        value: String::new(),
        line: 0,
        col: 0,
    })
}

// ==================== 语法分析器 ====================
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
        self.tokens.get(self.pos).unwrap_or(eof_sentinel())
    }

    fn peek(&self, o: usize) -> &Token {
        self.tokens.get(self.pos + o).unwrap_or(eof_sentinel())
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

    /// 自举兼容：期望标识符，但允许关键字 token 作为标识符使用。
    ///
    /// 在自举编译器和现代 VX 代码中，关键字常被用作方法名/字段名/参数名/局部变量名
    /// （典型例子：`func new(...)` 构造器、`var type_name`、`param kind: int` 等）。
    /// 原生 lexer 仍把 `new/var/int/...` 识别为关键字 token，此处放宽解析器：
    /// 凡是带字面值（`value` 非空）且不在结构关键位置的 token，都接受为标识符。
    ///
    /// 注：以下 token 仍保留为控制结构关键字，绝不能降级：
    ///   - 块/语句分隔符 (Colon, Comma, Newline, Indent, Dedent, EOF, LParen/RParen, ...)
    ///   - 二元/一元运算符 (Plus, Minus, Star, ...)
    /// 这里仅对 KEYWORDS 表中的 token 做降级。
    fn expect_identifier_or_keyword(&mut self) -> Result<Token, VXError> {
        use crate::token::TokenType;
        let t = self.current().clone();
        match t.kind {
            TokenType::Identifier => Ok(self.advance()),
            // 关键字 token：仅当其字面值看起来像标识符（字母/下划线开头）时才接受
            kind if is_keyword_kind(kind) && is_identifier_like_value(&t.value) => Ok(self.advance()),
            _ => {
                let err = VXError {
                    msg: "期望标识符".to_string(),
                    line: t.line,
                    col: t.col,
                    source: Some(self.source.clone()),
                };
                Err(err)
            }
        }
    }

    fn match_kind(&self, kinds: &[TokenType]) -> bool {
        kinds.contains(&self.current().kind)
    }

    fn skip_newlines(&mut self) {
        while self.current().kind == TokenType::Newline {
            self.advance();
        }
    }

    /// 自举兼容: 二元运算符 (or/and/&&/||) 后是否还有合法表达式延续。
    ///
    /// 多行 if/while 条件如:
    ///   if a or b or
    ///      c or d:
    /// 其中 `or\n` 后下一行是 Indent+Identifier, 也算延续。
    /// 但若下一行是 Indent+`:` (条件结束, 块开始) 则不是延续, 不应消费 `or`。
    fn peek_continuation_after_binary_op(&self) -> bool {
        let mut look = self.pos + 1;
        // 跳过换行
        while look < self.tokens.len() && self.tokens[look].kind == TokenType::Newline {
            look += 1;
        }
        // 跳过 Dedent (回到上层缩进)
        while look < self.tokens.len() && self.tokens[look].kind == TokenType::Dedent {
            look += 1;
            while look < self.tokens.len() && self.tokens[look].kind == TokenType::Newline {
                look += 1;
            }
        }
        // 跳过 Indent (进入子缩进, 多行条件中下一行更深)
        while look < self.tokens.len() && self.tokens[look].kind == TokenType::Indent {
            look += 1;
            while look < self.tokens.len() && self.tokens[look].kind == TokenType::Newline {
                look += 1;
            }
        }
        if look >= self.tokens.len() {
            return false;
        }
        let nk = self.tokens[look].kind;
        // 块开始 `:` = 条件结束
        if nk == TokenType::Colon {
            return false;
        }
        // 缩进边界 / EOF = 表达式结束
        if matches!(nk, TokenType::EOF) {
            return false;
        }
        // 块结束关键字
        if matches!(nk,
            TokenType::Else | TokenType::Elif | TokenType::RParen
            | TokenType::RBracket | TokenType::RBrace | TokenType::Comma
        ) {
            return false;
        }
        true
    }

    /// 解析可选的泛型参数列表 `<T, U>`，用于函数/结构体/类声明。
    /// 如果当前 token 不是 `<`，返回空列表。
    fn parse_generic_params(&mut self) -> Result<Vec<String>, VXError> {
        let mut params = Vec::new();
        if self.current().kind == TokenType::Lt {
            self.advance();
            params.push(self.expect(TokenType::Identifier, Some("期望类型参数名"))?.value);
            while self.current().kind == TokenType::Comma {
                self.advance();
                params.push(self.expect(TokenType::Identifier, Some("期望类型参数名"))?.value);
            }
            self.expect(TokenType::Gt, Some("期望 '>'"))?;
        }
        Ok(params)
    }
}

/// 判断 TokenType 是否属于 KEYWORDS 表中定义的关键字
///
/// 关键字 token 拥有字面值（"new", "var", "int" 等），可以被降级为标识符。
/// 分隔符/运算符/标点 token 不在此列。
///
/// 注意: 控制流关键字 (If/Else/Elif/For/While/Break/Continue/Return)
/// 不可降级, 它们在 class/struct/enum 字段位置出现时表明结构边界
/// (例如 class body 结束), 不应被当作字段名。
pub(crate) fn is_keyword_kind(k: TokenType) -> bool {
    use crate::token::TokenType;
    matches!(k,
        TokenType::True | TokenType::False | TokenType::Nil
        | TokenType::In | TokenType::Import | TokenType::As
        | TokenType::VarT | TokenType::Struct | TokenType::Class
        | TokenType::Enum | TokenType::Union | TokenType::New | TokenType::Move
        | TokenType::Macro
        | TokenType::IntT | TokenType::FloatT | TokenType::DoubleT
        | TokenType::BoolT | TokenType::VoidT
        | TokenType::And | TokenType::Or | TokenType::Not
    )
}

/// 关键字字面值是否可作为标识符使用。
/// 简单校验：必须以字母或下划线开头。
pub(crate) fn is_identifier_like_value(s: &str) -> bool {
    let mut chars = s.chars();
    match chars.next() {
        Some(c) if c.is_alphabetic() || c == '_' => true,
        _ => false,
    }
}
