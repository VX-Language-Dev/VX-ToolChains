// VX Language LSP - 全局状态管理
// 维护打开文档的解析缓存、符号索引、工作区符号表

use dashmap::DashMap;
use lsp_types::Url;
use vx_vm::parser::Stmt;
use vx_vm::token::Token;

/// 单个文档的完整解析状态
#[derive(Debug, Clone, Default)]
pub struct DocumentState {
    /// 文档的源代码
    pub source: String,
    /// 词法分析结果
    pub tokens: Vec<Token>,
    /// 语法树
    pub ast: Vec<Stmt>,
}

/// LSP 后端的全局状态
#[derive(Debug, Default)]
pub struct BackendState {
    /// 文档 URL -> 文档解析状态
    pub documents: DashMap<Url, DocumentState>,
}

impl BackendState {
    pub fn new() -> Self {
        Self {
            documents: DashMap::new(),
        }
    }
}
