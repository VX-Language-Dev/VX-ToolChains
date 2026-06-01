// VX Language LSP - 全局状态管理
// 维护打开文档的解析缓存、符号索引、工作区符号表

use dashmap::DashMap;
use lsp_types::Url;
use std::sync::Mutex;
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
    /// 语法错误（来自 Lexer/Parser）
    pub syntax_errors: Vec<String>,
    /// 所有权检查错误
    pub ownership_errors: Vec<String>,
    /// 解析是否成功（语法层面）
    pub parse_ok: bool,
    /// 所有权检查是否通过
    pub ownership_ok: bool,
}

impl DocumentState {
    pub fn new(source: String) -> Self {
        Self {
            source,
            tokens: Vec::new(),
            ast: Vec::new(),
            syntax_errors: Vec::new(),
            ownership_errors: Vec::new(),
            parse_ok: false,
            ownership_ok: false,
        }
    }
}

/// LSP 后端的全局状态
#[derive(Debug, Default)]
pub struct BackendState {
    /// 文档 URL -> 文档解析状态
    pub documents: DashMap<Url, DocumentState>,
    /// 工作区符号索引（跨文件）
    pub workspace_symbols: Mutex<Vec<lsp_types::SymbolInformation>>,
}

impl BackendState {
    pub fn new() -> Self {
        Self {
            documents: DashMap::new(),
            workspace_symbols: Mutex::new(Vec::new()),
        }
    }
}
