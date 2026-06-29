// VX Language LSP - 自动补全引擎
// 提供关键字、符号（函数/变量/类型）和成员（结构体字段/类方法）补全

use tower_lsp::lsp_types::{
    CompletionItem, CompletionItemKind, CompletionResponse,
    CompletionTriggerKind, InsertTextFormat, Position,
};
use vx_vm::parser::{Expr, Stmt};
use vx_vm::token::{KEYWORDS, Token, TokenType};

/// 作用域层级，数值越大表示越局部
#[derive(Debug, Clone, Copy, PartialEq, Eq, PartialOrd, Ord)]
pub struct ScopeDepth(pub usize);

/// 带作用域深度的符号信息，用于补全排序
#[derive(Debug, Clone)]
pub struct ScopedSymbolInfo {
    pub info: SymbolInfo,
    pub depth: ScopeDepth,
}

/// 符号信息：用于补全和跳转定义
#[derive(Debug, Clone)]
pub struct SymbolInfo {
    pub name: String,
    pub kind: SymbolKind,
    pub detail: Option<String>,
}

/// 符号分类
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum SymbolKind {
    Function,
    Variable,
    Struct,
    Class,
    Enum,
    Union,
    TypeAlias,
    LoopVar,
    Param,
}

impl From<SymbolKind> for CompletionItemKind {
    fn from(k: SymbolKind) -> Self {
        match k {
            SymbolKind::Function => CompletionItemKind::FUNCTION,
            SymbolKind::Variable => CompletionItemKind::VARIABLE,
            SymbolKind::Struct => CompletionItemKind::STRUCT,
            SymbolKind::Class => CompletionItemKind::CLASS,
            SymbolKind::Enum => CompletionItemKind::ENUM,
            SymbolKind::Union => CompletionItemKind::ENUM,
            SymbolKind::TypeAlias => CompletionItemKind::TYPE_PARAMETER,
            SymbolKind::LoopVar => CompletionItemKind::VARIABLE,
            SymbolKind::Param => CompletionItemKind::VARIABLE,
        }
    }
}

/// 从 AST 遍历收集当前文档中所有声明的符号
#[allow(dead_code)]
pub fn collect_symbols(ast: &[Stmt]) -> Vec<SymbolInfo> {
    let mut symbols = Vec::new();
    for stmt in ast {
        collect_from_expr(stmt, &mut symbols);
    }
    symbols
}

#[allow(dead_code)]
fn collect_from_expr(e: &Expr, symbols: &mut Vec<SymbolInfo>) {
    match e {
        Expr::FuncDecl(name, _type_params, params, ret_type, body, _line, _col) => {
            let detail = format_func_signature(name, params, ret_type);
            symbols.push(SymbolInfo {
                name: name.clone(),
                kind: SymbolKind::Function,
                detail: Some(detail),
            });
            for stmt in body {
                collect_from_expr(stmt, symbols);
            }
        }
        Expr::StructDecl(name, _type_params, fields, methods, _line, _col) => {
            let detail = format_struct_detail(name, fields);
            symbols.push(SymbolInfo {
                name: name.clone(),
                kind: SymbolKind::Struct,
                detail: Some(detail),
            });
            for m in methods {
                collect_from_expr(m, symbols);
            }
        }
        Expr::ClassDecl(name, _type_params, _fields, methods, parent, interfaces, _line, _col) => {
            let detail = format_class_detail(name, parent, interfaces);
            symbols.push(SymbolInfo {
                name: name.clone(),
                kind: SymbolKind::Class,
                detail: Some(detail),
            });
            for m in methods {
                collect_from_expr(m, symbols);
            }
        }
        Expr::EnumDecl(name, variants, _line, _col) => {
            let detail = format_enum_detail(name, variants);
            symbols.push(SymbolInfo {
                name: name.clone(),
                kind: SymbolKind::Enum,
                detail: Some(detail),
            });
        }
        Expr::UnionDecl(name, _fields, _line, _col) => {
            symbols.push(SymbolInfo {
                name: name.clone(),
                kind: SymbolKind::Union,
                detail: Some(format!("union {}", name)),
            });
        }
        Expr::VarDecl(name, _type_ann, value, _is_mut, _line, _col) => {
            symbols.push(SymbolInfo {
                name: name.clone(),
                kind: SymbolKind::Variable,
                detail: None,
            });
            collect_from_expr(value.as_ref(), symbols);
        }
        Expr::ForStmt(var, iter, body, _line, _col) => {
            symbols.push(SymbolInfo {
                name: var.clone(),
                kind: SymbolKind::LoopVar,
                detail: None,
            });
            collect_from_expr(iter, symbols);
            for stmt in body {
                collect_from_expr(stmt, symbols);
            }
        }
        Expr::IfStmt(cond, body, elifs, else_body, _, _) => {
            collect_from_expr(cond, symbols);
            for stmt in body {
                collect_from_expr(stmt, symbols);
            }
            for (c, b) in elifs {
                collect_from_expr(c, symbols);
                for stmt in b {
                    collect_from_expr(stmt, symbols);
                }
            }
            if let Some(b) = else_body {
                for stmt in b {
                    collect_from_expr(stmt, symbols);
                }
            }
        }
        Expr::WhileStmt(cond, body, _, _) => {
            collect_from_expr(cond, symbols);
            for stmt in body {
                collect_from_expr(stmt, symbols);
            }
        }
        Expr::ImportStmt(module, Some(alias), _dirs, _, _) => {
            symbols.push(SymbolInfo {
                name: alias.clone(),
                kind: SymbolKind::TypeAlias,
                detail: Some(format!("import {} as {}", module, alias)),
            });
        }
        Expr::ImportStmt(_, None, _, _, _) => {}
        Expr::ExprStmt(inner, _, _) => {
            collect_from_expr(inner, symbols);
        }
        Expr::CallExpr(callee, args, _, _) => {
            collect_from_expr(callee, symbols);
            for a in args {
                collect_from_expr(a, symbols);
            }
        }
        Expr::BinaryOp(_, left, right, _, _) => {
            collect_from_expr(left, symbols);
            collect_from_expr(right, symbols);
        }
        _ => {}
    }
}

/// 生成函数签名字符串
fn format_func_signature(name: &str, params: &[(String, String)], ret_type: &Option<String>) -> String {
    let param_str = params
        .iter()
        .map(|(n, t)| format!("{}: {}", n, t))
        .collect::<Vec<_>>()
        .join(", ");
    let ret = ret_type
        .as_ref()
        .map(|r| format!(" -> {}", r))
        .unwrap_or_default();
    format!("func {}({}){}", name, param_str, ret)
}

fn format_struct_detail(name: &str, fields: &[(String, String)]) -> String {
    let field_str = fields
        .iter()
        .map(|(n, t)| format!("{}: {}", n, t))
        .collect::<Vec<_>>()
        .join(", ");
    format!("struct {} {{ {} }}", name, field_str)
}

fn format_class_detail(name: &str, parent: &Option<String>, interfaces: &[String]) -> String {
    let mut detail = format!("class {}", name);
    if let Some(p) = parent {
        detail.push_str(&format!(" extends {}", p));
    }
    if !interfaces.is_empty() {
        detail.push_str(&format!(" implements {}", interfaces.join(", ")));
    }
    detail
}

fn format_enum_detail(name: &str, variants: &[(String, i64)]) -> String {
    let v_str = variants
        .iter()
        .map(|(n, v)| format!("{} = {}", n, v))
        .collect::<Vec<_>>()
        .join(", ");
    format!("enum {} {{ {} }}", name, v_str)
}

/// 从 AST 查找指定类型名的成员（用于 `.` 补全）
pub fn find_type_members(ast: &[Stmt], type_name: &str) -> Vec<SymbolInfo> {
    for stmt in ast {
        if let Expr::StructDecl(name, _type_params, fields, methods, _, _) = stmt {
            if name == type_name {
                let mut members = Vec::new();
                for (fname, ftype) in fields {
                    members.push(SymbolInfo {
                        name: fname.clone(),
                        kind: SymbolKind::Variable,
                        detail: Some(format!("{}: {}", fname, ftype)),
                    });
                }
                for m in methods {
                    if let Expr::FuncDecl(fname, _type_params, params, ret, _, _l, _c) = m.as_ref() {
                        members.push(SymbolInfo {
                            name: fname.clone(),
                            kind: SymbolKind::Function,
                            detail: Some(format_func_signature(fname, params, ret)),
                        });
                    }
                }
                return members;
            }
        }
        if let Expr::ClassDecl(name, _type_params, fields, methods, _, _, _, _) = stmt {
            if name == type_name {
                let mut members = Vec::new();
                for (fname, ftype, vis) in fields {
                    members.push(SymbolInfo {
                        name: fname.clone(),
                        kind: SymbolKind::Variable,
                        detail: Some(format!("{}: {} [{}]", fname, ftype, vis)),
                    });
                }
                for m in methods {
                    if let Expr::FuncDecl(fname, _type_params, params, ret, _, _l, _c) = m.as_ref() {
                        members.push(SymbolInfo {
                            name: fname.clone(),
                            kind: SymbolKind::Function,
                            detail: Some(format_func_signature(fname, params, ret)),
                        });
                    }
                }
                return members;
            }
        }
    }
    Vec::new()
}

/// 执行补全：根据触发方式和上下文返回补全列表
pub fn complete(
    ast: &[Stmt],
    tokens: &[Token],
    source: &str,
    position: Position,
    trigger_kind: CompletionTriggerKind,
    trigger_character: Option<&str>,
) -> Option<CompletionResponse> {
    let line = position.line as usize + 1;
    let col = position.character as usize + 1;

    let mut items = Vec::new();

    // 成员补全：触发字符 `.` 或 `->`
    if trigger_kind == CompletionTriggerKind::TRIGGER_CHARACTER {
        let ch = trigger_character.unwrap_or("");
        if ch == "." || ch == ">" {
            // 对于 `->`，tokens 中通常产生 Minus 和 GreaterThan（Arrow 视词法器而定）
            // 这里统一查找箭头或点之前的类型名
            let type_name = find_type_before_member_access(tokens, line, col);
            if let Some(tn) = type_name {
                let members = find_type_members(ast, &tn);
                for m in members {
                    items.push(symbol_to_completion_item(&m));
                }
                if !items.is_empty() {
                    return Some(CompletionResponse::Array(items));
                }
            }
        }
    }

    // import 上下文优先只显示模块路径
    if is_import_context(source, line, col) {
        add_import_completions(&mut items, source, line, col);
        return if items.is_empty() { None } else { Some(CompletionResponse::Array(items)) };
    }

    // 通用补全：关键字 + 当前文档符号
    add_keyword_completions(&mut items, source, line, col);
    add_scoped_symbol_completions(&mut items, ast, line, col);

    // 内置类型补全
    add_builtin_type_completions(&mut items);

    // import 路径补全（非 import 行也提供，但排序靠后）
    add_import_completions(&mut items, source, line, col);

    if items.is_empty() {
        None
    } else {
        Some(CompletionResponse::Array(items))
    }
}

/// 从 token 流中推断 `.` 或 `->` 前的类型名
/// 向后扫描找到最近的 Identifier，然后从 AST 中查找其声明类型
fn find_type_before_member_access(tokens: &[Token], line: usize, col: usize) -> Option<String> {
    // 找到位置附近的 `.` token 或 Arrow 的索引
    let access_idx = tokens.iter().rposition(|t| {
        let is_dot = t.kind == TokenType::Dot;
        let is_arrow = t.kind == TokenType::Arrow
            || (t.kind == TokenType::Minus && t.value == "->");
        (is_dot || is_arrow) && t.line == line && t.col >= col.saturating_sub(3) && t.col <= col
    })?;
    // 从访问符向前扫描找到最近的 Identifier 或 This
    for i in (0..access_idx).rev() {
        let t = &tokens[i];
        if t.kind == TokenType::Identifier {
            return Some(t.value.clone());
        }
        // this 关键字已裁减为语法糖 → 标识符 "this" 统一处理
        if t.value == "this" {
            return Some("this".to_string());
        }
        // 跳过函数调用 / 下标等封闭结构
        if t.kind == TokenType::RParen || t.kind == TokenType::RBracket || t.kind == TokenType::RBrace {
            let mut depth = 1;
            let mut j = i.saturating_sub(1);
            let open = match t.kind {
                TokenType::RParen => TokenType::LParen,
                TokenType::RBracket => TokenType::LBracket,
                TokenType::RBrace => TokenType::LBrace,
                _ => unreachable!(),
            };
            while j > 0 && depth > 0 {
                if tokens[j].kind == t.kind {
                    depth += 1;
                }
                if tokens[j].kind == open {
                    depth -= 1;
                }
                j = j.saturating_sub(1);
            }
            continue;
        }
    }
    None
}

/// 兼容旧名（保持测试可用）
#[allow(dead_code)]
fn find_type_before_dot(tokens: &[Token], line: usize, col: usize) -> Option<String> {
    find_type_before_member_access(tokens, line, col)
}

/// 收集带作用域深度的符号，用于补全排序
fn collect_scoped_symbols(ast: &[Stmt], line: usize, _col: usize) -> Vec<ScopedSymbolInfo> {
    let mut result = Vec::new();
    for stmt in ast {
        collect_scoped_from_expr(stmt, &mut result, ScopeDepth(0), line);
    }
    result
}

fn collect_scoped_from_expr(
    e: &Expr,
    symbols: &mut Vec<ScopedSymbolInfo>,
    depth: ScopeDepth,
    target_line: usize,
) {
    match e {
        Expr::FuncDecl(name, _type_params, params, ret_type, body, line, _col) => {
            let detail = format_func_signature(name, params, ret_type);
            symbols.push(ScopedSymbolInfo {
                info: SymbolInfo {
                    name: name.clone(),
                    kind: SymbolKind::Function,
                    detail: Some(detail),
                },
                depth,
            });
            for (pname, ptype) in params {
                symbols.push(ScopedSymbolInfo {
                    info: SymbolInfo {
                        name: pname.clone(),
                        kind: SymbolKind::Param,
                        detail: Some(format!("{}: {}", pname, ptype)),
                    },
                    depth: ScopeDepth(depth.0 + 1),
                });
            }
            for stmt in body {
                collect_scoped_from_expr(stmt, symbols, ScopeDepth(depth.0 + 1), target_line);
            }
            let _ = line;
        }
        Expr::StructDecl(name, _type_params, fields, methods, _line, _col) => {
            symbols.push(ScopedSymbolInfo {
                info: SymbolInfo {
                    name: name.clone(),
                    kind: SymbolKind::Struct,
                    detail: Some(format_struct_detail(name, fields)),
                },
                depth,
            });
            for m in methods {
                collect_scoped_from_expr(m, symbols, ScopeDepth(depth.0 + 1), target_line);
            }
        }
        Expr::ClassDecl(name, _type_params, _fields, methods, parent, interfaces, _line, _col) => {
            symbols.push(ScopedSymbolInfo {
                info: SymbolInfo {
                    name: name.clone(),
                    kind: SymbolKind::Class,
                    detail: Some(format_class_detail(name, parent, interfaces)),
                },
                depth,
            });
            for m in methods {
                collect_scoped_from_expr(m, symbols, ScopeDepth(depth.0 + 1), target_line);
            }
        }
        Expr::EnumDecl(name, variants, _line, _col) => {
            symbols.push(ScopedSymbolInfo {
                info: SymbolInfo {
                    name: name.clone(),
                    kind: SymbolKind::Enum,
                    detail: Some(format_enum_detail(name, variants)),
                },
                depth,
            });
        }
        Expr::UnionDecl(name, _fields, _line, _col) => {
            symbols.push(ScopedSymbolInfo {
                info: SymbolInfo {
                    name: name.clone(),
                    kind: SymbolKind::Union,
                    detail: Some(format!("union {}", name)),
                },
                depth,
            });
        }
        Expr::VarDecl(name, type_ann, value, _is_mut, _line, _col) => {
            let detail = type_ann.as_ref().map(|t| format!("{}: {}", name, expr_to_type_name(t)));
            symbols.push(ScopedSymbolInfo {
                info: SymbolInfo {
                    name: name.clone(),
                    kind: SymbolKind::Variable,
                    detail,
                },
                depth,
            });
            collect_scoped_from_expr(value.as_ref(), symbols, depth, target_line);
        }
        Expr::ForStmt(var, iter, body, _line, _col) => {
            symbols.push(ScopedSymbolInfo {
                info: SymbolInfo {
                    name: var.clone(),
                    kind: SymbolKind::LoopVar,
                    detail: Some(format!("for-in 循环变量 {}", var)),
                },
                depth: ScopeDepth(depth.0 + 1),
            });
            collect_scoped_from_expr(iter, symbols, depth, target_line);
            for stmt in body {
                collect_scoped_from_expr(stmt, symbols, ScopeDepth(depth.0 + 1), target_line);
            }
        }
        Expr::ImportStmt(module, Some(alias), _dirs, _line, _col) => {
            symbols.push(ScopedSymbolInfo {
                info: SymbolInfo {
                    name: alias.clone(),
                    kind: SymbolKind::TypeAlias,
                    detail: Some(format!("import {} as {}", module, alias)),
                },
                depth,
            });
        }
        Expr::ImportStmt(_, None, _, _, _) => {}
        Expr::IfStmt(cond, body, elifs, else_body, _, _) => {
            collect_scoped_from_expr(cond, symbols, depth, target_line);
            for stmt in body {
                collect_scoped_from_expr(stmt, symbols, depth, target_line);
            }
            for (c, b) in elifs {
                collect_scoped_from_expr(c, symbols, depth, target_line);
                for stmt in b {
                    collect_scoped_from_expr(stmt, symbols, depth, target_line);
                }
            }
            if let Some(b) = else_body {
                for stmt in b {
                    collect_scoped_from_expr(stmt, symbols, depth, target_line);
                }
            }
        }
        Expr::WhileStmt(cond, body, _, _) => {
            collect_scoped_from_expr(cond, symbols, depth, target_line);
            for stmt in body {
                collect_scoped_from_expr(stmt, symbols, depth, target_line);
            }
        }
        Expr::ExprStmt(inner, _, _) => {
            collect_scoped_from_expr(inner, symbols, depth, target_line);
        }
        Expr::CallExpr(callee, args, _, _) => {
            collect_scoped_from_expr(callee, symbols, depth, target_line);
            for a in args {
                collect_scoped_from_expr(a, symbols, depth, target_line);
            }
        }
        Expr::BinaryOp(_, left, right, _, _) => {
            collect_scoped_from_expr(left, symbols, depth, target_line);
            collect_scoped_from_expr(right, symbols, depth, target_line);
        }
        Expr::UnaryOp(_, inner, _, _) => {
            collect_scoped_from_expr(inner, symbols, depth, target_line);
        }
        Expr::Assign(target, _, value, _, _) => {
            collect_scoped_from_expr(target, symbols, depth, target_line);
            collect_scoped_from_expr(value, symbols, depth, target_line);
        }
        Expr::ReturnStmt(inner, _, _) => {
            if let Some(e) = inner {
                collect_scoped_from_expr(e, symbols, depth, target_line);
            }
        }
        Expr::MatchStmt(cond, arms, _, _) => {
            collect_scoped_from_expr(cond, symbols, depth, target_line);
            for (_, body) in arms {
                for stmt in body {
                    collect_scoped_from_expr(stmt, symbols, depth, target_line);
                }
            }
        }
        Expr::LoopStmt(_label, body, _, _) => {
            for stmt in body {
                collect_scoped_from_expr(stmt, symbols, depth, target_line);
            }
        }
        Expr::BreakStmt(_, _, _) | Expr::ContinueStmt(_, _, _) => {}
        _ => {}
    }
}

fn expr_to_type_name(e: &Expr) -> String {
    vx_vm::parser::expr_to_type_name(e)
}

/// 添加当前文档符号补全项（带作用域排序）
fn add_scoped_symbol_completions(items: &mut Vec<CompletionItem>, ast: &[Stmt], line: usize, col: usize) {
    let mut scoped = collect_scoped_symbols(ast, line, col);
    // 按作用域深度降序、去重，局部符号排在前面
    scoped.sort_by(|a, b| b.depth.cmp(&a.depth).then_with(|| a.info.name.cmp(&b.info.name)));
    let mut seen = std::collections::HashSet::new();
    for s in scoped {
        if seen.insert(s.info.name.clone()) {
            items.push(scoped_symbol_to_completion_item(&s));
        }
    }
}

fn scoped_symbol_to_completion_item(s: &ScopedSymbolInfo) -> CompletionItem {
    let mut item = symbol_to_completion_item(&s.info);
    // 局部变量排序靠前：用 sort_text 控制，局部变量用 '0'，全局用 '1'
    let prefix = if s.depth.0 >= 1 { '0' } else { '1' };
    item.sort_text = Some(format!("{}{}", prefix, item.label));
    item
}

/// 添加关键字补全项
fn add_keyword_completions(items: &mut Vec<CompletionItem>, source: &str, line: usize, col: usize) {
    let src_line = get_src_line(source, line);
    let prefix = extract_prefix(&src_line, col);

    for (kw, _tt) in KEYWORDS {
        if kw.starts_with(&prefix) || prefix.is_empty() {
            items.push(CompletionItem {
                label: kw.to_string(),
                kind: Some(CompletionItemKind::KEYWORD),
                detail: Some(keyword_detail(kw)),
                insert_text: Some(kw.to_string()),
                insert_text_format: Some(InsertTextFormat::PLAIN_TEXT),
                ..Default::default()
            });
        }
    }
}

/// 添加当前文档符号补全项（按声明顺序，不做作用域排序）
#[allow(dead_code)]
fn add_symbol_completions(items: &mut Vec<CompletionItem>, ast: &[Stmt]) {
    let symbols = collect_symbols(ast);
    for s in &symbols {
        items.push(symbol_to_completion_item(s));
    }
}

/// 添加内置类型补全项
fn add_builtin_type_completions(items: &mut Vec<CompletionItem>) {
    let builtin_types = [
        ("int", "整数类型"),
        ("float", "浮点数类型"),
        ("double", "双精度浮点类型"),
        ("string", "字符串类型"),
        ("bool", "布尔类型"),
        ("void", "空类型"),
        ("pointer", "指针类型"),
    ];
    for (name, desc) in builtin_types {
        items.push(CompletionItem {
            label: name.to_string(),
            kind: Some(CompletionItemKind::TYPE_PARAMETER),
            detail: Some(desc.to_string()),
            insert_text: Some(name.to_string()),
            insert_text_format: Some(InsertTextFormat::PLAIN_TEXT),
            ..Default::default()
        });
    }
}

fn symbol_to_completion_item(s: &SymbolInfo) -> CompletionItem {
    let insert_text = match s.kind {
        SymbolKind::Function => build_function_snippet(&s.name, &s.detail),
        SymbolKind::Struct => build_new_snippet(&s.name),
        SymbolKind::Class => build_new_snippet(&s.name),
        _ => s.name.clone(),
    };
    let insert_text_format = if insert_text.contains("$") {
        Some(InsertTextFormat::SNIPPET)
    } else {
        Some(InsertTextFormat::PLAIN_TEXT)
    };
    CompletionItem {
        label: s.name.clone(),
        kind: Some(CompletionItemKind::from(s.kind)),
        detail: s.detail.clone(),
        insert_text: Some(insert_text),
        insert_text_format,
        ..Default::default()
    }
}

/// 为函数生成 snippet：func(a: ${1:int}, b: ${2:int})
fn build_function_snippet(name: &str, detail: &Option<String>) -> String {
    if let Some(sig) = detail {
        // 从 "func name(a: int, b: string)" 提取参数名
        if let Some(start) = sig.find('(') {
            if let Some(end) = sig.rfind(')') {
                let inner = &sig[start + 1..end];
                if inner.trim().is_empty() {
                    return format!("{}()", name);
                }
                let params: Vec<&str> = inner.split(',').collect();
                let mut snippet_params = Vec::new();
                for (i, p) in params.iter().enumerate() {
                    let parts: Vec<&str> = p.split(':').collect();
                    let pname = parts.first().map(|s| s.trim()).unwrap_or("");
                    let ptype = parts.get(1).map(|s| s.trim()).unwrap_or("");
                    snippet_params.push(format!("{}: ${{{}:{}}}", pname, i + 1, ptype));
                }
                return format!("{}({})${{0}}", name, snippet_params.join(", "));
            }
        }
    }
    format!("{}(${{1}})${{0}}", name)
}

/// 为 struct/class 生成 new snippet：new Type($1, $2)
fn build_new_snippet(name: &str) -> String {
    format!("new {}(${{1}})${{0}}", name)
}

/// 检测当前行是否在 import 语句上下文中
fn is_import_context(source: &str, line: usize, col: usize) -> bool {
    let src_line = get_src_line(source, line);
    let prefix = src_line[..col.saturating_sub(1).min(src_line.len())].trim_start();
    prefix.starts_with("import") || prefix.starts_with("as")
}

/// 添加 import 路径补全（基于当前文档已出现的 import 模块名）
fn add_import_completions(items: &mut Vec<CompletionItem>, _source: &str, _line: usize, _col: usize) {
    // 当前仅提供常见内置模块路径建议；后续可扫描 workspace 中的 vx 文件/模块
    let builtin_modules = [
        ("std", "标准库根模块"),
        ("std.mem", "内存管理模块"),
        ("std.io", "输入输出模块"),
        ("std.fs", "文件系统模块"),
        ("std.string", "字符串工具模块"),
        ("std.vec", "动态数组模块"),
    ];
    for (name, desc) in builtin_modules {
        items.push(CompletionItem {
            label: name.to_string(),
            kind: Some(CompletionItemKind::MODULE),
            detail: Some(desc.to_string()),
            insert_text: Some(name.to_string()),
            insert_text_format: Some(InsertTextFormat::PLAIN_TEXT),
            ..Default::default()
        });
    }
}

/// 从源码行中提取当前光标前的标识符前缀
fn extract_prefix(line: &str, col: usize) -> String {
    let chars: Vec<char> = line.chars().collect();
    let end = col.saturating_sub(1).min(chars.len());
    let start = chars[..end]
        .iter()
        .rposition(|c| !c.is_alphanumeric() && *c != '_')
        .map(|p| p + 1)
        .unwrap_or(0);
    chars[start..end].iter().collect()
}

/// 关键字说明
fn keyword_detail(kw: &str) -> String {
    match kw {
        "if" => "条件判断语句".to_string(),
        "elif" => "条件分支（else if）".to_string(),
        "else" => "条件分支（else）".to_string(),
        "for" => "循环遍历语句".to_string(),
        "while" => "循环条件语句".to_string(),
        "break" => "跳出循环".to_string(),
        "continue" => "跳过当前循环".to_string(),
        "func" => "函数声明".to_string(),
        "return" => "函数返回值".to_string(),
        "import" => "导入模块".to_string(),
        "as" => "模块别名".to_string(),
        "dirs" => "模块搜索路径".to_string(),
        "struct" => "结构体声明".to_string(),
        "class" => "类声明".to_string(),
        "enum" => "枚举声明".to_string(),
        "union" => "联合类型声明".to_string(),
        "vector" => "向量类型".to_string(),
        "new" => "创建实例".to_string(),
        "newz" => "堆分配（所有权）".to_string(),
        "free" => "释放堆内存".to_string(),
        "move" => "所有权转移".to_string(),
        "this" => "当前实例引用".to_string(),
        "public" => "公开访问修饰符".to_string(),
        "private" => "私有访问修饰符".to_string(),
        "protected" => "受保护访问修饰符".to_string(),
        "extends" => "类继承".to_string(),
        "implements" => "接口实现".to_string(),
        "true" => "布尔值 真".to_string(),
        "false" => "布尔值 假".to_string(),
        "nil" => "空值".to_string(),
        "and" => "逻辑与".to_string(),
        "or" => "逻辑或".to_string(),
        "not" => "逻辑非".to_string(),
        "in" => "属于（for-in）".to_string(),
        "var" => "已移除：VX 为纯静态类型语言，请使用 `name: Type = value`".to_string(),
        _ => kw.to_string(),
    }
}

/// 获取源码指定行（1-indexed）
fn get_src_line(source: &str, line: usize) -> String {
    vx_vm::parser::get_src_line(source, line)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_keyword_detail() {
        assert_eq!(keyword_detail("func"), "函数声明");
        assert_eq!(keyword_detail("newz"), "堆分配（所有权）");
        assert_eq!(keyword_detail("struct"), "结构体声明");
        assert_eq!(keyword_detail("unknown"), "unknown");
    }

    #[test]
    fn test_extract_prefix() {
        assert_eq!(extract_prefix("func hello", 5), "func");
        assert_eq!(extract_prefix("  int x = 1", 6), "int");
        assert_eq!(extract_prefix("out(result)", 4), "out");
        assert_eq!(extract_prefix("", 1), "");
        assert_eq!(extract_prefix("123abc", 1), "");
    }

    #[test]
    fn test_collect_symbols_func() {
        let ast = vec![
            Expr::FuncDecl("add".to_string(), vec![], vec![("a".into(), "int".into()), ("b".into(), "int".into())], Some("int".into()), vec![], 1, 1)
        ];
        let syms = collect_symbols(&ast);
        assert_eq!(syms.len(), 1);
        assert_eq!(syms[0].name, "add");
        assert_eq!(syms[0].kind, SymbolKind::Function);
        assert!(syms[0].detail.as_deref().unwrap().contains("add"));
    }

    #[test]
    fn test_find_type_members() {
        let ast = vec![
            Expr::StructDecl("Point".to_string(), vec![], vec![("x".into(), "int".into()), ("y".into(), "int".into())], vec![], 1, 1)
        ];
        let members = find_type_members(&ast, "Point");
        assert_eq!(members.len(), 2);
        assert_eq!(members[0].name, "x");
        assert_eq!(members[1].name, "y");
    }

    #[test]
    fn test_identifier_name_at_completion() {
        // "    func hello" — n at col 7 (1-indexed) means prefix = "fun"
        let result = extract_prefix("    func hello", 9);
        assert_eq!(result, "func");
    }
}