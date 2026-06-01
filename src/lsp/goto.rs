// VX Language LSP - 跳转到定义引擎
// 在光标位置查找标识符的定义位置

use tower_lsp::lsp_types::{GotoDefinitionResponse, Position, Range, Url};
use vx_vm::parser::Stmt;

/// 跳转到定义：查找光标处标识符的声明位置
pub fn goto_definition(
    ast: &[Stmt],
    source: &str,
    position: Position,
    uri: &Url,
) -> Option<GotoDefinitionResponse> {
    let (line, col) = ((position.line + 1) as usize, (position.character + 1) as usize);

    let name = identifier_name_at(source, line, col)?;

    let mut defs = Vec::new();
    collect_definitions(ast, &name, &mut defs);

    if let Some((def_line, def_col)) = defs.first() {
        let line = (*def_line as u32).saturating_sub(1);
        let col = (*def_col as u32).saturating_sub(1);
        let end_col = col + name.chars().count() as u32;

        Some(GotoDefinitionResponse::Scalar(
            tower_lsp::lsp_types::Location {
                uri: uri.clone(),
                range: Range {
                    start: Position {
                        line,
                        character: col,
                    },
                    end: Position {
                        line,
                        character: end_col,
                    },
                },
            },
        ))
    } else {
        None
    }
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
        vx_vm::parser::Expr::FuncDecl(fname, params, _, body, line, col) => {
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
        vx_vm::parser::Expr::StructDecl(sname, _, _, line, col) => {
            if sname == name {
                defs.push((*line, *col));
            }
        }
        vx_vm::parser::Expr::ClassDecl(cname, _, _, _, _, line, col) => {
            if cname == name {
                defs.push((*line, *col));
            }
        }
        vx_vm::parser::Expr::EnumDecl(ename, _, line, col) => {
            if ename == name {
                defs.push((*line, *col));
            }
        }
        vx_vm::parser::Expr::UnionDecl(uname, _, line, col) => {
            if uname == name {
                defs.push((*line, *col));
            }
        }
        vx_vm::parser::Expr::VarDecl(vname, _, _, _, line, col) => {
            if vname == name {
                defs.push((*line, *col));
            }
        }
        vx_vm::parser::Expr::ForStmt(var, _, body, line, col) => {
            if var == name {
                defs.push((*line, *col));
            }
            for s in body {
                collect_from_stmt(s, name, defs, depth + 1);
            }
        }
        vx_vm::parser::Expr::IfStmt(_, body, elifs, else_body, _, _) => {
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
        vx_vm::parser::Expr::WhileStmt(_, body, _, _) => {
            for s in body {
                collect_from_stmt(s, name, defs, depth + 1);
            }
        }
        vx_vm::parser::Expr::ExprStmt(inner, _, _) => {
            collect_from_expr(inner, name, defs, depth + 1);
        }
        _ => {}
    }
}

fn collect_from_expr(
    e: &vx_vm::parser::Expr,
    name: &str,
    defs: &mut Vec<(usize, usize)>,
    depth: usize,
) {
    if depth > 16 {
        return;
    }
    match e {
        vx_vm::parser::Expr::FuncDecl(fname, _, _, body, line, col) => {
            if fname == name {
                defs.push((*line, *col));
            }
            for s in body {
                collect_from_stmt(s, name, defs, depth + 1);
            }
        }
        vx_vm::parser::Expr::CallExpr(callee, args, _, _) => {
            collect_from_expr(callee, name, defs, depth + 1);
            for a in args {
                collect_from_expr(a, name, defs, depth + 1);
            }
        }
        vx_vm::parser::Expr::BinaryOp(_, left, right, _, _) => {
            collect_from_expr(left, name, defs, depth + 1);
            collect_from_expr(right, name, defs, depth + 1);
        }
        vx_vm::parser::Expr::Assign(target, _, value, _, _) => {
            if let vx_vm::parser::Expr::Identifier(vname, _, _) = target.as_ref() {
                if vname == name {
                    // 赋值目标不是定义，跳过
                }
            }
            collect_from_expr(value, name, defs, depth + 1);
        }
        _ => {}
    }
}

fn identifier_name_at(source: &str, line: usize, col: usize) -> Option<String> {
    let src_line = vx_vm::parser::get_src_line(source, line);
    let chars: Vec<char> = src_line.chars().collect();
    let idx = col.saturating_sub(1);
    if idx >= chars.len() {
        return None;
    }
    if !chars[idx].is_alphanumeric() && chars[idx] != '_' {
        return None;
    }
    let mut start = idx;
    while start > 0 && (chars[start - 1].is_alphanumeric() || chars[start - 1] == '_') {
        start -= 1;
    }
    let mut end = idx;
    while end < chars.len() && (chars[end].is_alphanumeric() || chars[end] == '_') {
        end += 1;
    }
    if start == end {
        return None;
    }
    Some(chars[start..end].iter().collect())
}

#[cfg(test)]
mod tests {
    use super::*;
    use vx_vm::parser::Expr;

    #[test]
    fn test_collect_definitions_func() {
        let ast = vec![Expr::FuncDecl(
            "hello".to_string(),
            vec![],
            Some("int".to_string()),
            vec![],
            1,
            1,
        )];
        let mut defs = Vec::new();
        collect_definitions(&ast, "hello", &mut defs);
        assert_eq!(defs.len(), 1);
        assert_eq!(defs[0], (1, 1));
    }

    #[test]
    fn test_collect_definitions_not_found() {
        let ast = vec![Expr::FuncDecl(
            "hello".to_string(),
            vec![],
            None,
            vec![],
            1,
            1,
        )];
        let mut defs = Vec::new();
        collect_definitions(&ast, "world", &mut defs);
        assert!(defs.is_empty());
    }

    #[test]
    fn test_collect_definitions_struct() {
        let ast = vec![Expr::StructDecl("Point".to_string(), vec![], vec![], 5, 3)];
        let mut defs = Vec::new();
        collect_definitions(&ast, "Point", &mut defs);
        assert_eq!(defs.len(), 1);
        assert_eq!(defs[0], (5, 3));
    }

    #[test]
    fn test_identifier_name_at_goto() {
        let source = "func add(a: int, b: int):\n    return a + b";
        // "func add(...)" — 'a' in "add" at col 6
        assert_eq!(identifier_name_at(source, 1, 6).as_deref(), Some("add"));
        // "    return a + b" — 'a' in "a + b" at col 12
        assert_eq!(identifier_name_at(source, 2, 12).as_deref(), Some("a"));
        // 'n' in "func" at col 3
        assert_eq!(identifier_name_at(source, 1, 3).as_deref(), Some("func"));
        // col 1 = space → None
        assert_eq!(identifier_name_at(source, 2, 1).as_deref(), None);
    }
}