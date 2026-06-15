// VX Language LSP - 悬停提示引擎
// 在光标位置提供关键字、函数、变量、类型的详细说明

use tower_lsp::lsp_types::{Hover, HoverContents, MarkupContent, MarkupKind, Position, Range};
use vx_vm::parser::Stmt;
use vx_vm::token::{Token, TokenType};

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

    // 1. 关键字悬停
    if is_keyword_token(&token.kind) {
        if let Some(hover) = keyword_hover(&token.kind) {
            return Some(hover);
        }
    }

    // 2. 标识符悬停
    if token.kind == TokenType::Identifier {
        if let Some(name) = identifier_name_at(source, line, col) {
            // 优先查找函数
            if let Some(h) = function_hover(ast, &name) {
                return Some(h);
            }
            // 然后查找类型（struct/class/enum/union）
            if let Some(h) = type_hover(ast, &name) {
                return Some(h);
            }
            // 最后查找变量
            if let Some(h) = variable_hover(ast, &name) {
                return Some(h);
            }
        }
    }

    None
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
            | TokenType::Dirs
            | TokenType::Struct
            | TokenType::Class
            | TokenType::Enum
            | TokenType::Union
            | TokenType::Vector
            | TokenType::New
            | TokenType::Newz
            | TokenType::Free
            | TokenType::Move
            | TokenType::This
            | TokenType::Public
            | TokenType::Private
            | TokenType::Protected
            | TokenType::Extends
            | TokenType::Implements
            | TokenType::IntT
            | TokenType::FloatT
            | TokenType::DoubleT
            | TokenType::StringT
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
        TokenType::Import => ("import", "**import 导入模块**\n\n```vx\nimport module dirs \"path\"\n```"),
        TokenType::As => ("as", "**as 模块别名**"),
        TokenType::Dirs => ("dirs", "**dirs 模块搜索路径**"),
        TokenType::Struct => ("struct", "**struct 结构体**\n\n```vx\nstruct Name:\n    field: type\n```"),
        TokenType::Class => ("class", "**class 类**\n\n```vx\nclass Name extends Base:\n    method():\n        body\n```"),
        TokenType::Enum => ("enum", "**enum 枚举**\n\n```vx\nenum Name:\n    Variant = 0\n```"),
        TokenType::Union => ("union", "**union 联合类型**"),
        TokenType::Vector => ("vector", "**vector 向量类型**"),
        TokenType::New => ("new", "**new 创建实例（栈）**"),
        TokenType::Newz => ("newz", "**newz 堆分配（所有权）**\n\n分配堆内存，返回指针，遵循所有权规则"),
        TokenType::Free => ("free", "**free 释放堆内存**\n\n```vx\nfree(pointer)\n```"),
        TokenType::Move => ("move", "**move 所有权转移**\n\n```vx\nx = move y  # x 取得 y 的所有权\n```"),
        TokenType::This => ("this", "**this 当前实例引用**"),
        TokenType::Public => ("public", "**public 公开访问修饰符**"),
        TokenType::Private => ("private", "**private 私有访问修饰符**"),
        TokenType::Protected => ("protected", "**protected 受保护访问修饰符**"),
        TokenType::Extends => ("extends", "**extends 类继承**"),
        TokenType::Implements => ("implements", "**implements 接口实现**"),
        TokenType::IntT => ("int", "**int 整数类型**\n\n64位有符号整数"),
        TokenType::FloatT => ("float", "**float 单精度浮点**"),
        TokenType::DoubleT => ("double", "**double 双精度浮点**"),
        TokenType::StringT => ("string", "**string 字符串类型**"),
        TokenType::VarT => ("var", "**var 动态类型**"),
        TokenType::BoolT => ("bool", "**bool 布尔类型**"),
        TokenType::VoidT => ("void", "**void 空类型**"),
        TokenType::And => ("and", "**and 逻辑与**"),
        TokenType::Or => ("or", "**or 逻辑或**"),
        TokenType::Not => ("not", "**not 逻辑非**"),
        TokenType::In => ("in", "**in 属于（for-in 循环）**"),
        TokenType::True => ("true", "**true 布尔值真**"),
        TokenType::False => ("false", "**false 布尔值假**"),
        TokenType::Nil => ("nil", "**nil 空值**"),
        _ => return None,
    };
    let _ = kw; // suppress unused warning
    Some(make_hover(desc))
}

fn function_hover(ast: &[Stmt], name: &str) -> Option<Hover> {
    for stmt in ast {
        if let vx_vm::parser::Expr::FuncDecl(fname, params, ret, _, line, col) = stmt {
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
            vx_vm::parser::Expr::StructDecl(sname, fields, _, line, _col) => {
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
            vx_vm::parser::Expr::ClassDecl(cname, fields, _, parent, interfaces, line, _col) => {
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
            vx_vm::parser::Expr::UnionDecl(uname, fields, line, _col) => {
                if uname == name {
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
            }
            _ => {}
        }
    }
    None
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
                    "```vx\nvar {}{}\n```\n\n*变量，第 {} 行*",
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
        vx_vm::parser::Expr::FuncDecl(_, params, _, body, _, _) => {
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
        vx_vm::parser::Expr::StructDecl(_, _, methods, _, _) => {
            for m in methods {
                if let Some(h) = find_var_in_stmt(m, name, depth + 1) {
                    return Some(h);
                }
            }
        }
        vx_vm::parser::Expr::ClassDecl(_, _, methods, _, _, _, _) => {
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
                    "```vx\nvar {}{}\n```\n\n*变量，第 {} 行*",
                    vname, type_str, line
                );
                return Some(make_hover(&content));
            }
        }
        vx_vm::parser::Expr::FuncDecl(fname, params, _, body, _, _) => {
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
