// VX Language LSP - 跳转到定义引擎
// 在光标位置查找标识符的定义位置

use tower_lsp::lsp_types::{GotoDefinitionResponse, Position, Range, Url};
use vx_vm::parser::{Expr, Stmt};

type BoxExpr = Box<Expr>;

/// 跳转到定义：查找光标处标识符的声明位置
pub fn goto_definition(
    ast: &[Stmt],
    source: &str,
    position: Position,
    uri: &Url,
) -> Option<GotoDefinitionResponse> {
    let (line, col) = ((position.line + 1) as usize, (position.character + 1) as usize);

    let name = identifier_name_at(source, line, col)?;

    // 1. 首先找到光标所在的函数
    if let Some(enclosing) = find_enclosing_function(ast, line) {
        // 2. 在该函数内搜索定义
        if let Some(loc) = find_local_definition(enclosing, &name, line) {
            return Some(location_to_response(uri, loc, &name));
        }
    }

    // 3. 全局搜索
    let mut defs = Vec::new();
    collect_definitions(ast, &name, &mut defs);

    if let Some((def_line, def_col)) = defs.first() {
        return Some(location_to_response(uri, (*def_line, *def_col), &name));
    }

    None
}

/// 找到包含指定行的最内层函数
fn find_enclosing_function(ast: &[Stmt], target_line: usize) -> Option<&Stmt> {
    for stmt in ast {
        if let Some(found) = find_enclosing_in_stmt(stmt, target_line) {
            return Some(found);
        }
    }
    None
}

fn find_enclosing_in_stmt(stmt: &Stmt, target_line: usize) -> Option<&Stmt> {
    match stmt {
        Expr::FuncDecl(_, _, _, _, body, func_line, _) => {
            if *func_line > target_line {
                return None;
            }
            // 检查是否在函数体内部
            for s in body {
                if let Some(found) = find_enclosing_in_stmt(s, target_line) {
                    return Some(found);
                }
            }
            // 如果不在任何嵌套函数内，则这个函数就是最内层的
            Some(stmt)
        }
        Expr::StructDecl(_, _, _, methods, decl_line, _) => {
            if *decl_line > target_line {
                return None;
            }
            for m in methods {
                if let Some(found) = find_enclosing_in_stmt(m, target_line) {
                    return Some(found);
                }
            }
            None
        }
        Expr::ClassDecl(_, _, _, methods, _, _, decl_line, _) => {
            if *decl_line > target_line {
                return None;
            }
            for m in methods {
                if let Some(found) = find_enclosing_in_stmt(m, target_line) {
                    return Some(found);
                }
            }
            None
        }
        _ => None,
    }
}

/// 在函数内搜索局部定义
fn find_local_definition(func_stmt: &Stmt, name: &str, target_line: usize) -> Option<(usize, usize)> {
    if let Expr::FuncDecl(_, _, params, _, body, _, _) = func_stmt {
        // 首先检查参数
        for (pname, _) in params {
            if pname == name {
                // 参数定义在函数声明行
                return None;
            }
        }
        // 在函数体内搜索
        return search_for_definition(body, name, target_line, 0);
    }
    None
}

/// 在语句块中搜索定义，优先返回最近的定义
fn search_for_definition(
    stmts: &[BoxExpr],
    name: &str,
    target_line: usize,
    depth: usize,
) -> Option<(usize, usize)> {
    for stmt in stmts {
        if let Some(loc) = search_stmt_for_definition(stmt, name, target_line, depth) {
            return Some(loc);
        }
    }
    None
}

fn search_stmt_for_definition(
    stmt: &Expr,
    name: &str,
    target_line: usize,
    depth: usize,
) -> Option<(usize, usize)> {
    match stmt {
        Expr::VarDecl(vname, _, _, _, vline, _) => {
            if vname == name && *vline <= target_line {
                return Some((*vline, 1));
            }
        }
        Expr::ForStmt(varname, _, body, for_line, _) => {
            if varname == name {
                return Some((*for_line, 1));
            }
            if let Some(loc) = search_for_definition(body, name, target_line, depth + 1) {
                return Some(loc);
            }
        }
        Expr::IfStmt(_, body, elifs, else_body, _, _) => {
            if let Some(loc) = search_for_definition(body, name, target_line, depth + 1) {
                return Some(loc);
            }
            for (_, b) in elifs {
                if let Some(loc) = search_for_definition(b, name, target_line, depth + 1) {
                    return Some(loc);
                }
            }
            if let Some(b) = else_body {
                if let Some(loc) = search_for_definition(b, name, target_line, depth + 1) {
                    return Some(loc);
                }
            }
        }
        Expr::WhileStmt(_, body, _, _) => {
            if let Some(loc) = search_for_definition(body, name, target_line, depth + 1) {
                return Some(loc);
            }
        }
        Expr::ExprStmt(inner, _, _) => {
            if let Some(loc) = search_expr_for_reference(inner, name, target_line) {
                return Some(loc);
            }
        }
        _ => {}
    }
    None
}

fn search_expr_for_reference(
    e: &Expr,
    name: &str,
    target_line: usize,
) -> Option<(usize, usize)> {
    match e {
        Expr::Assign(_, _, value, _, _) => {
            search_expr_for_reference(value, name, target_line)
        }
        Expr::CallExpr(callee, args, _, _) => {
            if let Some(loc) = search_expr_for_reference(callee, name, target_line) {
                return Some(loc);
            }
            for a in args {
                if let Some(loc) = search_expr_for_reference(a, name, target_line) {
                    return Some(loc);
                }
            }
            None
        }
        Expr::BinaryOp(_, left, right, _, _) => {
            if let Some(loc) = search_expr_for_reference(left, name, target_line) {
                return Some(loc);
            }
            search_expr_for_reference(right, name, target_line)
        }
        Expr::UnaryOp(_, inner, _, _) => {
            search_expr_for_reference(inner, name, target_line)
        }
        _ => None,
    }
}

fn location_to_response(uri: &Url, loc: (usize, usize), name: &str) -> GotoDefinitionResponse {
    let line = loc.0.saturating_sub(1) as u32;
    let col = loc.1.saturating_sub(1) as u32;
    let end_col = col + name.chars().count() as u32;
    GotoDefinitionResponse::Scalar(tower_lsp::lsp_types::Location {
        uri: uri.clone(),
        range: Range {
            start: Position { line, character: col },
            end: Position { line, character: end_col },
        },
    })
}

/// 从 AST 收集指定名称的定义位置
fn collect_definitions(ast: &[Stmt], name: &str, defs: &mut Vec<(usize, usize)>) {
    for stmt in ast {
        collect_from_stmt(stmt, name, defs, 0);
    }
}

fn collect_from_stmt(stmt: &Stmt, name: &str, defs: &mut Vec<(usize, usize)>, depth: usize) {
    if depth > 16 {
        return;
    }
    match stmt {
        Expr::FuncDecl(fname, _, params, _, body, line, col) => {
            if fname == name {
                defs.push((*line, *col));
            }
            for (pname, _) in params {
                if pname == name {
                    defs.push((*line, *col));
                }
            }
            for s in body {
                collect_from_stmt(s, name, defs, depth + 1);
            }
        }
        Expr::StructDecl(sname, _, fields, methods, line, col) => {
            if sname == name {
                defs.push((*line, *col));
            }
            for (fname, _) in fields {
                if fname == name {
                    defs.push((*line, *col));
                }
            }
            for m in methods {
                collect_from_stmt(m, name, defs, depth + 1);
            }
        }
        Expr::ClassDecl(cname, _, fields, methods, _, _, line, col) => {
            if cname == name {
                defs.push((*line, *col));
            }
            for (fname, _, _) in fields {
                if fname == name {
                    defs.push((*line, *col));
                }
            }
            for m in methods {
                collect_from_stmt(m, name, defs, depth + 1);
            }
        }
        Expr::EnumDecl(ename, _, line, col) => {
            if ename == name {
                defs.push((*line, *col));
            }
        }
        Expr::UnionDecl(uname, _, line, col) => {
            if uname == name {
                defs.push((*line, *col));
            }
        }
        Expr::VarDecl(vname, _, _, _, line, col) => {
            if vname == name {
                defs.push((*line, *col));
            }
        }
        Expr::ForStmt(varname, _, body, line, col) => {
            if varname == name {
                defs.push((*line, *col));
            }
            for s in body {
                collect_from_stmt(s, name, defs, depth + 1);
            }
        }
        Expr::IfStmt(_, body, elifs, else_body, _, _) => {
            for s in body {
                collect_from_stmt(s, name, defs, depth + 1);
            }
            for (_, b) in elifs {
                for s in b {
                    collect_from_stmt(s, name, defs, depth + 1);
                }
            }
            if let Some(b) = else_body {
                for s in b {
                    collect_from_stmt(s, name, defs, depth + 1);
                }
            }
        }
        Expr::WhileStmt(_, body, _, _) => {
            for s in body {
                collect_from_stmt(s, name, defs, depth + 1);
            }
        }
        _ => {}
    }
}

/// 从源文本中提取指定位置的标识符名称
fn identifier_name_at(source: &str, line: usize, col: usize) -> Option<String> {
    let lines: Vec<&str> = source.lines().collect();
    if line == 0 || line > lines.len() {
        return None;
    }
    let line_str = lines[line - 1];

    let chars: Vec<(usize, char)> = line_str.char_indices().map(|(i, c)| (i, c)).collect();
    if chars.is_empty() {
        return None;
    }

    // 找到列位置对应的字符
    for (idx, (char_idx, c)) in chars.iter().enumerate() {
        if idx + 1 == col {
            if c.is_alphanumeric() || *c == '_' {
                let start = *char_idx;
                let mut end = start + c.len_utf8();

                // 向后扫描以找到标识符的结束位置
                while end < line_str.len() {
                    let next_char = line_str[end..].chars().next()?;
                    if next_char.is_alphanumeric() || next_char == '_' {
                        end += next_char.len_utf8();
                    } else {
                        break;
                    }
                }

                if end > start {
                    return Some(line_str[start..end].to_string());
                }
            }
        }
    }

    None
}