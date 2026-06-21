// VX Language Compiler v3.0 - Parser Module
// 语法分析器入口：模块声明 + Parser 结构体 + 核心辅助方法
// 表达式解析 → expr.rs  |  语句解析 → stmt.rs  |  AST 定义 → ast.rs

mod ast;
mod expr;
mod stmt;
#[cfg(test)]
mod tests;

// 对外公开 AST 类型
pub use ast::{Expr, Stmt, get_src_line};

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

    fn match_kind(&self, kinds: &[TokenType]) -> bool {
        kinds.contains(&self.current().kind)
    }

    fn skip_newlines(&mut self) {
        while self.current().kind == TokenType::Newline {
            self.advance();
        }
    }
}
