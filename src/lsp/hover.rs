// VX Language LSP - 悬停提示引擎
// 在光标位置提供关键字、函数、变量、类型的详细说明

use tower_lsp::lsp_types::{Hover, HoverContents, MarkupContent, MarkupKind, Position, Range};
use vx_vm::parser::{Expr, Stmt};
use vx_vm::token::{Token, TokenType};

/// 内置函数/标准库 hover 信息表
fn builtin_hover(name: &str) -> Option<String> {
    match name {
        "out" => Some("**out(value: int)**\n\n内置输出函数，将整数打印到标准输出并追加换行。".to_string()),
        "sys_argv" => Some("**sys_argv() -> [string]**\n\n返回命令行参数数组。".to_string()),
        "len" => Some("**len(collection) -> int**\n\n返回数组或字符串的长度。".to_string()),
        "panic" => Some("**panic(message: string)**\n\n触发运行时 panic 并终止程序。".to_string()),
        _ => None,
    }
}

/// 从 AST 和源码中查找光标所在位置的相关信息
pub fn hover(
    ast: &[Stmt],
    tokens: &[Token],
    source: &str,
    position: Position,
) -> Option<Hover> {
    let (line, col) = ((position.line + 1) as usize, (position.character + 1) as usize);

    // 找到光标处的 token
    let token = find_token_at(tokens, line, col)?;
    let range = token_range(&token);

    // 1. 关键字悬停
    if is_keyword_token(&token.kind) {
        if let Some(hover) = keyword_hover(&token.kind) {
            return Some(with_range(hover, range));
        }
    }

    // 2. 标识符悬停
    if token.kind == TokenType::Identifier {
        if let Some(name) = identifier_name_at(source, line, col) {
            // 优先查找函数
            if let Some(h) = function_hover(ast, &name) {
                return Some(with_range(h, range));
            }
            // 然后查找类型（struct/class/enum/union）
            if let Some(h) = type_hover(ast, &name) {
                return Some(with_range(h, range));
            }
            // 查找成员字段 / 方法（property access）
            if let Some(h) = member_hover(ast, tokens, line, col, &name) {
                return Some(with_range(h, range));
            }
            // 查找调用表达式的函数签名
            if let Some(h) = call_hover(ast, tokens, line, col, &name) {
                return Some(with_range(h, range));
            }
            // 最后查找变量
            if let Some(h) = variable_hover(ast, &name) {
                return Some(with_range(h, range));
            }
            // 内置函数兜底
            if let Some(content) = builtin_hover(&name) {
                return Some(with_range(make_hover(&content), range));
            }
        }
    }

    None
}

/// 根据 token 生成精确 hover range
fn token_range(token: &Token) -> Option<Range> {
    let line0 = token.line.saturating_sub(1) as u32;
    let col0 = token.col.saturating_sub(1) as u32;
    let width = token.value.chars().count() as u32;
    Some(Range {
        start: Position { line: line0, character: col0 },
        end: Position { line: line0, character: col0 + width },
    })
}

fn with_range(hover: Hover, range: Option<Range>) -> Hover {
    Hover {
        contents: hover.contents,
        range,
    }
}

/// 在 token 流中查找指定位置的 token
fn find_token_at(tokens: &[Token], line: usize, col: usize) -> Option<Token> {
    for token in tokens {
        if token.line == line && token.col == col {
            return Some(token.clone());
        }
        // 处理多字符 token（如 Identifier, String, Int 等），col 是开始列
        if token.line == line
            && col >= token.col
            && col < token.col + token_value_width(&token.value)
        {
            return Some(token.clone());
        }
    }
    None
}

fn token_value_width(s: &str) -> usize {
    s.chars().count()
}

fn is_keyword_token(kind: &TokenType) -> bool {
    matches!(
        kind,
        TokenType::If
            | TokenType::Elif
            | TokenType::Else
            | TokenType::For
            | TokenType::While
            | TokenType::Break
            | TokenType::Continue
            | TokenType::Func
            | TokenType::Return
            | TokenType::Import
            | TokenType::As
            | TokenType::Struct
            | TokenType::Class
            | TokenType::Enum
            | TokenType::Union
            | TokenType::New
            | TokenType::Move
            | TokenType::IntT
            | TokenType::FloatT
            | TokenType::DoubleT
            | TokenType::VarT
            | TokenType::BoolT
            | TokenType::VoidT
            | TokenType::And
            | TokenType::Or
            | TokenType::Not
            | TokenType::In
            | TokenType::True
            | TokenType::False
            | TokenType::Nil
    )
}

fn keyword_hover(kind: &TokenType) -> Option<Hover> {
    let (kw, desc) = match kind {
        TokenType::If => ("if", "**if 条件判断**\n\n```vx\nif condition:\n    body\n```"),
        TokenType::Elif => ("elif", "**elif 条件分支**\n\n```vx\nif x:\n    a\nelif y:\n    b\n```"),
        TokenType::Else => ("else", "**else 否则分支**"),
        TokenType::For => ("for", "**for 循环**\n\n```vx\nfor x in collection:\n    body\n```"),
        TokenType::While => ("while", "**while 循环**\n\n```vx\nwhile condition:\n    body\n```"),
        TokenType::Break => ("break", "**break 跳出循环**"),
        TokenType::Continue => ("continue", "**continue 跳过当前循环**"),
        TokenType::Func => ("func", "**func 函数声明**\n\n```vx\nfunc name(param: type) -> ret:\n    body\n```"),
        TokenType::Return => ("return", "**return 函数返回**"),
        TokenType::Import => ("import", "**import 导入模块**\n\n```vx\nimport module as alias\nimport(\"p1\",\"p2\") as mod\n```"),
        TokenType::As => ("as", "**as 模块别名**"),
        TokenType::Struct => ("struct", "**struct 结构体**\n\n```vx\nstruct Name:\n    field: type\n```"),
        TokenType::Class => ("class", "**class 类** (冒号继承语法)\n\n```vx\nclass Name : Parent, Trait:\n    field: type\n```"),
        TokenType::Enum => ("enum", "**enum 枚举**\n\n```vx\nenum Name:\n    Variant = 0\n```"),
        TokenType::Union => ("union", "**union 联合类型**"),
        TokenType::New => ("new", "**new 创建实例（栈）**"),
        TokenType::Move => ("move", "**move 所有权转移**\n\n```vx\nx = move y  # x 取得 y 的所有权\n```"),
        TokenType::IntT => ("int", "**int 整数类型**\n\n64位有符号整数"),
        TokenType::FloatT => ("float", "**float 单精度浮点**"),
        TokenType::DoubleT => ("double", "**double 双精度浮点**"),
        TokenType::VarT => ("var", "**var 已移除**\n\nVX 现为纯静态类型语言，请使用 `name: Type = value`"),
        TokenType::BoolT => ("bool", "**bool 布尔类型**"),
        TokenType::VoidT => ("void", "**void 空类型**"),
        TokenType::And => ("&&", "**&& 逻辑与** (关键字 and 已裁减)"),
        TokenType::Or => ("||", "**|| 逻辑或** (关键字 or 已裁减)"),
        TokenType::Not => ("!", "**! 逻辑非** (关键字 not 已裁减)"),
        TokenType::In => ("in", "**in 属于（for-in 循环）**"),
        TokenType::True => ("true", "**true 布尔值真**"),
        TokenType::False => ("false", "**false 布尔值假**"),
        TokenType::Nil => ("nil", "**nil 空值**"),
        // 以下关键字已裁减:
        //   string → std::String (标准库)  |  vector → std::Vec<T> (标准库)
        //   public/private/protected → #[pub]/#[priv] 注解
        //   extends/implements → 冒号语法 class A : Parent, Trait
        //   dirs → import("a","b") 可变参数  |  newz → mem::zeroed<T>()  |  free → mem::free(ptr)
        //   this → 解析器语法糖 (自动替换为实例变量)
        _ => return None,
    };
    let _ = kw; // suppress unused warning
    Some(make_hover(desc))
}

fn function_hover(ast: &[Stmt], name: &str) -> Option<Hover> {
    for stmt in ast {
        if let vx_vm::parser::Expr::FuncDecl(fname, _type_params, params, ret, _body, line, col) = stmt {
            if fname == name {
                let param_str = params
                    .iter()
                    .map(|(n, t)| format!("{}: {}", n, t))
                    .collect::<Vec<_>>()
                    .join(", ");
                let ret_str = ret
                    .as_ref()
                    .map(|r| format!(" -> {}", r))
                    .unwrap_or_default();
                let content = format!(
                    "```vx\nfunc {}({}){}\n```\n\n*第 {} 行*",
                    fname, param_str, ret_str, line
                );
                return Some(Hover {
                    contents: HoverContents::Markup(MarkupContent {
                        kind: MarkupKind::Markdown,
                        value: content,
                    }),
                    range: Some(Range {
                        start: Position {
                            line: (*line as u32).saturating_sub(1),
                            character: (*col as u32).saturating_sub(1),
                        },
                        end: Position {
                            line: (*line as u32).saturating_sub(1),
                            character: (*col as u32).saturating_sub(1) + fname.chars().count() as u32,
                        },
                    }),
                });
            }
        }
    }
    None
}

fn type_hover(ast: &[Stmt], name: &str) -> Option<Hover> {
    for stmt in ast {
        match stmt {
            vx_vm::parser::Expr::StructDecl(sname, _type_params, fields, _methods, line, _col) => {
                if sname == name {
                    let field_str = fields
                        .iter()
                        .map(|(n, t)| format!("  {}: {}", n, t))
                        .collect::<Vec<_>>()
                        .join("\n");
                    let content = format!(
                        "```vx\nstruct {}:\n{}\n```\n\n*结构体，第 {} 行*",
                        sname, field_str, line
                    );
                    return Some(make_hover(&content));
                }
            }
            vx_vm::parser::Expr::ClassDecl(cname, _type_params, fields, _methods, parent, interfaces, line, _col) => {
                if cname == name {
                    let field_str = fields
                        .iter()
                        .map(|(n, t, v)| format!("  [{}] {}: {}", v, n, t))
                        .collect::<Vec<_>>()
                        .join("\n");
                    let mut header = format!("class {}", cname);
                    if let Some(p) = parent {
                        header.push_str(&format!(" extends {}", p));
                    }
                    if !interfaces.is_empty() {
                        header.push_str(&format!(" implements {}", interfaces.join(", ")));
                    }
                    let content = format!(
                        "```vx\n{}:\n{}\n```\n\n*类，第 {} 行*",
                        header, field_str, line
                    );
                    return Some(make_hover(&content));
                }
            }
            vx_vm::parser::Expr::EnumDecl(ename, variants, line, _col) => {
                if ename == name {
                    let v_str = variants
                        .iter()
                        .map(|(n, v)| format!("  {} = {}", n, v))
                        .collect::<Vec<_>>()
                        .join("\n");
                    let content = format!(
                        "```vx\nenum {}:\n{}\n```\n\n*枚举，第 {} 行*",
                        ename, v_str, line
                    );
                    return Some(make_hover(&content));
                }
            }
            vx_vm::parser::Expr::UnionDecl(uname, fields, line, _col) if uname == name => {
                let field_str = fields
                    .iter()
                    .map(|(n, t)| format!("  {}: {}", n, t))
                    .collect::<Vec<_>>()
                    .join("\n");
                let content = format!(
                    "```vx\nunion {}:\n{}\n```\n\n*联合类型，第 {} 行*",
                    uname, field_str, line
                );
                return Some(make_hover(&content));
            }
            _ => {}
        }
    }
    None
}

/// 成员字段 / 方法悬停：当光标位于 `obj.field` 的 field 上时
fn member_hover(
    ast: &[Stmt],
    tokens: &[Token],
    line: usize,
    col: usize,
    name: &str,
) -> Option<Hover> {
    // 向后找到最近的 `.` 或 `->`，确定其左侧对象名
    let access_idx = tokens.iter().rposition(|t| {
        let is_dot = t.kind == TokenType::Dot;
        let is_arrow = t.kind == TokenType::Arrow
            || (t.kind == TokenType::Minus && t.value == "->");
        (is_dot || is_arrow)
            && t.line == line
            && t.col >= col.saturating_sub(2)
            && t.col <= col
    })?;

    let mut obj_name: Option<String> = None;
    for i in (0..access_idx).rev() {
        let t = &tokens[i];
        if t.kind == TokenType::Identifier {
            obj_name = Some(t.value.clone());
            break;
        }
        if t.value == "this" {
            obj_name = Some("this".to_string());
            break;
        }
        if t.kind == TokenType::RParen || t.kind == TokenType::RBracket || t.kind == TokenType::RBrace {
            break;
        }
    }

    let obj_name = obj_name?;
    let type_name = infer_type_of_identifier(ast, &obj_name)?;
    find_member_hover(ast, &type_name, name)
}

/// 调用表达式悬停：当光标位于函数调用的 callee 上时显示签名
fn call_hover(
    ast: &[Stmt],
    tokens: &[Token],
    line: usize,
    col: usize,
    name: &str,
) -> Option<Hover> {
    // 检查光标后紧邻 '(' token
    let is_call = tokens.iter().any(|t| {
        t.kind == TokenType::LParen
            && t.line == line
            && t.col >= col
            && t.col <= col + 2
    });
    if !is_call {
        return None;
    }
    function_hover(ast, name)
}

/// 推断标识符在 AST 中的声明类型
fn infer_type_of_identifier(ast: &[Stmt], name: &str) -> Option<String> {
    for stmt in ast {
        if let Some(t) = find_type_in_stmt(stmt, name, 0) {
            return Some(t);
        }
    }
    None
}

fn find_type_in_stmt(stmt: &Stmt, name: &str, depth: usize) -> Option<String> {
    if depth > 16 {
        return None;
    }
    match stmt {
        Expr::VarDecl(vname, type_ann, _, _, _, _) => {
            if vname == name {
                return type_ann.as_ref().map(|t| vx_vm::parser::expr_to_type_name(t));
            }
        }
        Expr::ForStmt(var, _, _, _, _) => {
            if var == name {
                return Some("int".to_string());
            }
        }
        Expr::FuncDecl(_, _, params, _, body, _, _) => {
            for (pname, ptype) in params {
                if pname == name {
                    return Some(ptype.clone());
                }
            }
            for s in body {
                if let Some(t) = find_type_in_stmt(s, name, depth + 1) {
                    return Some(t);
                }
            }
        }
        Expr::IfStmt(_, body, elifs, else_body, _, _) => {
            for s in body {
                if let Some(t) = find_type_in_stmt(s, name, depth + 1) {
                    return Some(t);
                }
            }
            for (_, b) in elifs {
                for s in b {
                    if let Some(t) = find_type_in_stmt(s, name, depth + 1) {
                        return Some(t);
                    }
                }
            }
            if let Some(b) = else_body {
                for s in b {
                    if let Some(t) = find_type_in_stmt(s, name, depth + 1) {
                        return Some(t);
                    }
                }
            }
        }
        Expr::WhileStmt(_, body, _, _) => {
            for s in body {
                if let Some(t) = find_type_in_stmt(s, name, depth + 1) {
                    return Some(t);
                }
            }
        }
        Expr::StructDecl(_, _, _, methods, _, _) => {
            for m in methods {
                if let Some(t) = find_type_in_stmt(m, name, depth + 1) {
                    return Some(t);
                }
            }
        }
        Expr::ClassDecl(_, _, _, methods, _, _, _, _) => {
            for m in methods {
                if let Some(t) = find_type_in_stmt(m, name, depth + 1) {
                    return Some(t);
                }
            }
        }
        _ => {}
    }
    None
}

fn find_member_hover(ast: &[Stmt], type_name: &str, member_name: &str) -> Option<Hover> {
    for stmt in ast {
        match stmt {
            Expr::StructDecl(sname, _, fields, methods, _, _) if sname == type_name => {
                for (fname, ftype) in fields {
                    if fname == member_name {
                        let content = format!("```vx\n{}: {}\n```\n\n*结构体字段*", fname, ftype);
                        return Some(make_hover(&content));
                    }
                }
                for m in methods {
                    if let Expr::FuncDecl(fname, _, params, ret, _, _, _) = m.as_ref() {
                        if fname == member_name {
                            let detail = format_func_signature(fname, params, ret);
                            let content = format!("```vx\n{}\n```\n\n*结构体方法*", detail);
                            return Some(make_hover(&content));
                        }
                    }
                }
            }
            Expr::ClassDecl(cname, _, fields, methods, _, _, _, _) if cname == type_name => {
                for (fname, ftype, vis) in fields {
                    if fname == member_name {
                        let content = format!(
                            "```vx\n[{}] {}: {}\n```\n\n*类字段*",
                            vis, fname, ftype
                        );
                        return Some(make_hover(&content));
                    }
                }
                for m in methods {
                    if let Expr::FuncDecl(fname, _, params, ret, _, _, _) = m.as_ref() {
                        if fname == member_name {
                            let detail = format_func_signature(fname, params, ret);
                            let content = format!("```vx\n{}\n```\n\n*类方法*", detail);
                            return Some(make_hover(&content));
                        }
                    }
                }
            }
            _ => {}
        }
    }
    None
}

fn format_func_signature(name: &str, params: &[(String, String)], ret: &Option<String>) -> String {
    let param_str = params
        .iter()
        .map(|(n, t)| format!("{}: {}", n, t))
        .collect::<Vec<_>>()
        .join(", ");
    let ret_str = ret.as_ref().map(|r| format!(" -> {}", r)).unwrap_or_default();
    format!("func {}({}){}", name, param_str, ret_str)
}

fn variable_hover(ast: &[Stmt], name: &str) -> Option<Hover> {
    for stmt in ast {
        find_var_in_stmt(stmt, name, 0)?;
    }
    None
}

fn find_var_in_stmt(stmt: &Stmt, name: &str, depth: usize) -> Option<Hover> {
    if depth > 8 {
        return None;
    }
    match stmt {
        vx_vm::parser::Expr::VarDecl(vname, type_ann, _, _, line, col) => {
            if vname == name {
                let type_str = if let Some(t) = type_ann {
                    if let vx_vm::parser::Expr::TypeExpr(tname, _, _) = t.as_ref() {
                        format!(": {}", tname)
                    } else {
                        String::new()
                    }
                } else {
                    String::new()
                };
                let content = format!(
                    "```vx\n{}{}\n```\n\n*变量，第 {} 行*",
                    vname, type_str, line
                );
                return Some(make_hover(&content));
            }
            let _ = col;
        }
        vx_vm::parser::Expr::ForStmt(var, _, body, _, _) => {
            if var == name {
                let content = format!(
                    "```vx\n# 循环变量 {}\n```\n\n*for-in 循环变量*",
                    var
                );
                return Some(make_hover(&content));
            }
            for s in body {
                if let Some(h) = find_var_in_stmt(s, name, depth + 1) {
                    return Some(h);
                }
            }
        }
        vx_vm::parser::Expr::FuncDecl(_, _type_params, params, _ret, body, _, _) => {
            for (pname, ptype) in params {
                if pname == name {
                    let content = format!(
                        "```vx\nfunc ({}: {})\n```\n\n*函数参数*",
                        pname, ptype
                    );
                    return Some(make_hover(&content));
                }
            }
            for s in body {
                if let Some(h) = find_var_in_stmt(s, name, depth + 1) {
                    return Some(h);
                }
            }
        }
        vx_vm::parser::Expr::IfStmt(cond, body, elifs, else_body, _, _) => {
            find_var_in_expr(cond, name, depth + 1)?;
            for s in body {
                if let Some(h) = find_var_in_stmt(s, name, depth + 1) {
                    return Some(h);
                }
            }
            for (c, b) in elifs {
                find_var_in_expr(c, name, depth + 1)?;
                for s in b {
                    if let Some(h) = find_var_in_stmt(s, name, depth + 1) {
                        return Some(h);
                    }
                }
            }
            if let Some(b) = else_body {
                for s in b {
                    if let Some(h) = find_var_in_stmt(s, name, depth + 1) {
                        return Some(h);
                    }
                }
            }
        }
        vx_vm::parser::Expr::WhileStmt(cond, body, _, _) => {
            find_var_in_expr(cond, name, depth + 1)?;
            for s in body {
                if let Some(h) = find_var_in_stmt(s, name, depth + 1) {
                    return Some(h);
                }
            }
        }
        vx_vm::parser::Expr::StructDecl(_, _type_params, _fields, methods, _, _) => {
            for m in methods {
                if let Some(h) = find_var_in_stmt(m, name, depth + 1) {
                    return Some(h);
                }
            }
        }
        vx_vm::parser::Expr::ClassDecl(_, _type_params, _fields, methods, _, _, _, _) => {
            for m in methods {
                if let Some(h) = find_var_in_stmt(m, name, depth + 1) {
                    return Some(h);
                }
            }
        }
        _ => {}
    }
    None
}

fn find_var_in_expr(
    e: &vx_vm::parser::Expr,
    name: &str,
    depth: usize,
) -> Option<Hover> {
    if depth > 8 {
        return None;
    }
    match e {
        vx_vm::parser::Expr::VarDecl(vname, type_ann, _, _, line, _col) => {
            if vname == name {
                let type_str = if let Some(t) = type_ann {
                    if let vx_vm::parser::Expr::TypeExpr(tname, _, _) = t.as_ref() {
                        format!(": {}", tname)
                    } else {
                        String::new()
                    }
                } else {
                    String::new()
                };
                let content = format!(
                    "```vx\n{}{}\n```\n\n*变量，第 {} 行*",
                    vname, type_str, line
                );
                return Some(make_hover(&content));
            }
        }
        vx_vm::parser::Expr::FuncDecl(fname, _type_params, params, _ret, body, _, _) => {
            if fname == name {
                let param_str = params
                    .iter()
                    .map(|(n, t)| format!("{}: {}", n, t))
                    .collect::<Vec<_>>()
                    .join(", ");
                let content = format!("```vx\nfunc {}({})\n```", fname, param_str);
                return Some(make_hover(&content));
            }
            for s in body {
                if let Some(h) = find_var_in_stmt(s, name, depth + 1) {
                    return Some(h);
                }
            }
        }
        _ => {}
    }
    None
}

/// 从源码中提取光标位置处的标识符名称
fn identifier_name_at(source: &str, line: usize, col: usize) -> Option<String> {
    let src_line = vx_vm::parser::get_src_line(source, line);
    let chars: Vec<char> = src_line.chars().collect();
    if col < 1 || col > chars.len() + 1 {
        return None;
    }
    let idx = col.saturating_sub(1);
    if idx >= chars.len() {
        return None;
    }
    if !chars[idx].is_alphanumeric() && chars[idx] != '_' {
        return None;
    }
    // 向前找到标识符开始
    let mut start = idx;
    while start > 0 && (chars[start - 1].is_alphanumeric() || chars[start - 1] == '_') {
        start -= 1;
    }
    // 向后找到标识符结束
    let mut end = idx;
    while end < chars.len() && (chars[end].is_alphanumeric() || chars[end] == '_') {
        end += 1;
    }
    if start == end {
        return None;
    }
    Some(chars[start..end].iter().collect())
}

fn make_hover(content: &str) -> Hover {
    Hover {
        contents: HoverContents::Markup(MarkupContent {
            kind: MarkupKind::Markdown,
            value: content.to_string(),
        }),
        range: None,
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use vx_vm::token::TokenType;

    #[test]
    fn test_identifier_name_at() {
        let source = "func main():\n    out(\"hello\")";
        assert_eq!(identifier_name_at(source, 1, 1).as_deref(), Some("func"));
        assert_eq!(identifier_name_at(source, 2, 5).as_deref(), Some("out"));
        assert_eq!(identifier_name_at(source, 1, 5).as_deref(), None);
        assert_eq!(identifier_name_at(source, 2, 20).as_deref(), None);
    }

    #[test]
    fn test_is_keyword_token() {
        assert!(is_keyword_token(&TokenType::Func));
        assert!(is_keyword_token(&TokenType::If));
        assert!(is_keyword_token(&TokenType::Return));
        assert!(!is_keyword_token(&TokenType::Identifier));
        assert!(!is_keyword_token(&TokenType::Plus));
    }

    #[test]
    fn test_keyword_hover_func() {
        let h = keyword_hover(&TokenType::Func);
        assert!(h.is_some());
        let content = h.unwrap();
        let markdown = match content.contents {
            HoverContents::Markup(m) => m.value,
            _ => String::new(),
        };
        assert!(markdown.contains("func"));
    }
}
