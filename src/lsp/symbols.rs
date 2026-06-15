// VX Language LSP - 符号导航引擎
// 文档符号：列出当前文档中所有声明的符号（函数、类型、变量等）
// 工作区符号：在所有打开的文档中搜索符号

use tower_lsp::lsp_types::{
    DocumentSymbol, Position, Range, SymbolInformation, SymbolKind,
    Url, WorkspaceSymbolParams,
};
use vx_vm::parser::Expr;
use vx_vm::parser::Stmt;

/// 从 AST 提取文档符号（支持层级结构）
pub fn document_symbols(ast: &[Stmt]) -> Vec<DocumentSymbol> {
    let mut symbols = Vec::new();
    for stmt in ast {
        if let Some(sym) = build_document_symbol(stmt) {
            symbols.push(sym);
        }
    }
    symbols
}

fn build_document_symbol(stmt: &Stmt) -> Option<DocumentSymbol> {
    match stmt {
        Expr::FuncDecl(name, params, ret, body, line, col) => {
            let _detail = format_func_signature(name, params, ret);
            let mut children = Vec::new();
            for s in body {
                if let Some(child) = build_document_symbol(s) {
                    children.push(child);
                }
            }
            let line0 = (*line as u32).saturating_sub(1);
            let col0 = (*col as u32).saturating_sub(1);
            Some(DocumentSymbol {
                name: name.clone(),
                detail: Some(_detail),
                kind: SymbolKind::FUNCTION,
                tags: None,
                #[allow(deprecated)]
                deprecated: None,
                range: Range {
                    start: Position {
                        line: line0,
                        character: col0,
                    },
                    end: Position {
                        line: line0,
                        character: col0 + name.chars().count() as u32,
                    },
                },
                selection_range: Range {
                    start: Position {
                        line: line0,
                        character: col0,
                    },
                    end: Position {
                        line: line0,
                        character: col0 + name.chars().count() as u32,
                    },
                },
                children: if children.is_empty() {
                    None
                } else {
                    Some(children)
                },
            })
        }
        Expr::StructDecl(name, fields, methods, line, col) => {
            let mut children = Vec::new();
            for (fname, ftype) in fields {
                children.push(make_var_symbol(fname, Some(ftype), *line, *col));
            }
            for m in methods {
                if let Some(s) = build_document_symbol(m) {
                    children.push(s);
                }
            }
            let line0 = (*line as u32).saturating_sub(1);
            let col0 = (*col as u32).saturating_sub(1);
            Some(DocumentSymbol {
                name: name.clone(),
                detail: Some(format!("struct {}", name)),
                kind: SymbolKind::STRUCT,
                tags: None,
                #[allow(deprecated)]
                deprecated: None,
                range: Range {
                    start: Position {
                        line: line0,
                        character: col0,
                    },
                    end: Position {
                        line: line0,
                        character: col0 + name.chars().count() as u32,
                    },
                },
                selection_range: Range {
                    start: Position {
                        line: line0,
                        character: col0,
                    },
                    end: Position {
                        line: line0,
                        character: col0 + name.chars().count() as u32,
                    },
                },
                children: if children.is_empty() {
                    None
                } else {
                    Some(children)
                },
            })
        }
        Expr::ClassDecl(name, fields, methods, _parent, _interfaces, line, col) => {
            let mut children = Vec::new();
            for (fname, ftype, _vis) in fields {
                children.push(make_var_symbol(fname, Some(ftype), *line, *col));
            }
            for m in methods {
                if let Some(s) = build_document_symbol(m) {
                    children.push(s);
                }
            }
            let line0 = (*line as u32).saturating_sub(1);
            let col0 = (*col as u32).saturating_sub(1);
            Some(DocumentSymbol {
                name: name.clone(),
                detail: Some(format!("class {}", name)),
                kind: SymbolKind::CLASS,
                tags: None,
                #[allow(deprecated)]
                deprecated: None,
                range: Range {
                    start: Position {
                        line: line0,
                        character: col0,
                    },
                    end: Position {
                        line: line0,
                        character: col0 + name.chars().count() as u32,
                    },
                },
                selection_range: Range {
                    start: Position {
                        line: line0,
                        character: col0,
                    },
                    end: Position {
                        line: line0,
                        character: col0 + name.chars().count() as u32,
                    },
                },
                children: if children.is_empty() {
                    None
                } else {
                    Some(children)
                },
            })
        }
        Expr::EnumDecl(name, variants, line, col) => {
            let mut children = Vec::new();
            for (vname, val) in variants {
                let line0 = (*line as u32).saturating_sub(1);
                let col0 = (*col as u32).saturating_sub(1);
                children.push(DocumentSymbol {
                    name: vname.clone(),
                    detail: Some(format!("= {}", val)),
                    kind: SymbolKind::ENUM_MEMBER,
                    tags: None,
                    #[allow(deprecated)]
                    deprecated: None,
                    range: Range {
                        start: Position {
                            line: line0,
                            character: col0,
                        },
                        end: Position {
                            line: line0,
                            character: col0 + vname.chars().count() as u32,
                        },
                    },
                    selection_range: Range {
                        start: Position {
                            line: line0,
                            character: col0,
                        },
                        end: Position {
                            line: line0,
                            character: col0 + vname.chars().count() as u32,
                        },
                    },
                    children: None,
                });
            }
            let line0 = (*line as u32).saturating_sub(1);
            let col0 = (*col as u32).saturating_sub(1);
            Some(DocumentSymbol {
                name: name.clone(),
                detail: Some(format!("enum {}", name)),
                kind: SymbolKind::ENUM,
                tags: None,
                #[allow(deprecated)]
                deprecated: None,
                range: Range {
                    start: Position {
                        line: line0,
                        character: col0,
                    },
                    end: Position {
                        line: line0,
                        character: col0 + name.chars().count() as u32,
                    },
                },
                selection_range: Range {
                    start: Position {
                        line: line0,
                        character: col0,
                    },
                    end: Position {
                        line: line0,
                        character: col0 + name.chars().count() as u32,
                    },
                },
                children: if children.is_empty() {
                    None
                } else {
                    Some(children)
                },
            })
        }
        Expr::UnionDecl(name, fields, line, col) => {
            let mut children = Vec::new();
            for (fname, ftype) in fields {
                children.push(make_var_symbol(fname, Some(ftype), *line, *col));
            }
            let line0 = (*line as u32).saturating_sub(1);
            let col0 = (*col as u32).saturating_sub(1);
            Some(DocumentSymbol {
                name: name.clone(),
                detail: Some(format!("union {}", name)),
                kind: SymbolKind::ENUM,
                tags: None,
                #[allow(deprecated)]
                deprecated: None,
                range: Range {
                    start: Position {
                        line: line0,
                        character: col0,
                    },
                    end: Position {
                        line: line0,
                        character: col0 + name.chars().count() as u32,
                    },
                },
                selection_range: Range {
                    start: Position {
                        line: line0,
                        character: col0,
                    },
                    end: Position {
                        line: line0,
                        character: col0 + name.chars().count() as u32,
                    },
                },
                children: if children.is_empty() {
                    None
                } else {
                    Some(children)
                },
            })
        }
        Expr::VarDecl(name, _type_ann, _value, _is_mut, line, col) => {
            Some(make_var_symbol(name, None, *line, *col))
        }
        Expr::ForStmt(var, _iter, _body, line, col) => {
            Some(make_var_symbol(var, Some("loop var"), *line, *col))
        }
        _ => None,
    }
}

fn make_var_symbol(name: &str, type_ann: Option<&str>, line: usize, col: usize) -> DocumentSymbol {
    let line0 = (line as u32).saturating_sub(1);
    let col0 = (col as u32).saturating_sub(1);
    DocumentSymbol {
        name: name.to_string(),
        detail: type_ann.map(|t| t.to_string()),
        kind: SymbolKind::VARIABLE,
        tags: None,
        #[allow(deprecated)]
        deprecated: None,
        range: Range {
            start: Position {
                line: line0,
                character: col0,
            },
            end: Position {
                line: line0,
                character: col0 + name.chars().count() as u32,
            },
        }, selection_range: Range {
            start: Position {
                line: line0,
                character: col0,
            },
            end: Position {
                line: line0,
                character: col0 + name.chars().count() as u32,
            },
        }, children: None, }
}

fn format_func_signature(name: &str, params: &[(String, String)], ret: &Option<String>) -> String {
    let param_str = params
        .iter()
        .map(|(n, t)| format!("{}: {}", n, t))
        .collect::<Vec<_>>()
        .join(", ");
    let ret_str = ret
        .as_ref()
        .map(|r| format!(" -> {}", r))
        .unwrap_or_default();
    format!("func {}({}){}", name, param_str, ret_str)
}

/// 从单个 AST 提取工作区符号列表
fn extract_workspace_symbols(ast: &[Stmt], uri: &Url) -> Vec<SymbolInformation> {
    let mut out = Vec::new();
    for stmt in ast {
        collect_symbol_info(stmt, uri, None, &mut out, 0);
    }
    out
}

fn collect_symbol_info(
    stmt: &Stmt,
    uri: &Url,
    container: Option<String>,
    out: &mut Vec<SymbolInformation>,
    depth: usize,
) {
    if depth > 16 {
        return;
    }
    match stmt {
        Expr::FuncDecl(name, params, ret, body, line, col) => {
            let _detail = format_func_signature(name, params, ret);
            let line0 = (*line as u32).saturating_sub(1);
            let col0 = (*col as u32).saturating_sub(1);
            out.push(SymbolInformation {
                name: name.clone(),
                kind: SymbolKind::FUNCTION,
                tags: None,
                #[allow(deprecated)]
                deprecated: None,
                location: tower_lsp::lsp_types::Location {
                    uri: uri.clone(),
                    range: Range {
                        start: Position {
                            line: line0,
                            character: col0,
                        },
                        end: Position {
                            line: line0,
                            character: col0 + name.chars().count() as u32,
                        },
                    },
                },
                container_name: container.clone(),
            });
            let nested_container = Some(name.clone());
            for s in body {
                collect_symbol_info(s, uri, nested_container.clone(), out, depth + 1);
            }
        }
        Expr::StructDecl(name, _fields, methods, line, col) => {
            let line0 = (*line as u32).saturating_sub(1);
            let col0 = (*col as u32).saturating_sub(1);
            out.push(SymbolInformation {
                name: name.clone(),
                kind: SymbolKind::STRUCT,
                tags: None,
                #[allow(deprecated)]
                deprecated: None,
                location: tower_lsp::lsp_types::Location {
                    uri: uri.clone(),
                    range: Range {
                        start: Position {
                            line: line0,
                            character: col0,
                        },
                        end: Position {
                            line: line0,
                            character: col0 + name.chars().count() as u32,
                        },
                    },
                },
                container_name: container.clone(),
            });
            let nested_container = Some(name.clone());
            for m in methods {
                collect_symbol_info(m, uri, nested_container.clone(), out, depth + 1);
            }
        }
        Expr::ClassDecl(name, _fields, methods, _parent, _interfaces, line, col) => {
            let line0 = (*line as u32).saturating_sub(1);
            let col0 = (*col as u32).saturating_sub(1);
            out.push(SymbolInformation {
                name: name.clone(),
                kind: SymbolKind::CLASS,
                tags: None,
                #[allow(deprecated)]
                deprecated: None,
                location: tower_lsp::lsp_types::Location {
                    uri: uri.clone(),
                    range: Range {
                        start: Position {
                            line: line0,
                            character: col0,
                        },
                        end: Position {
                            line: line0,
                            character: col0 + name.chars().count() as u32,
                        },
                    },
                },
                container_name: container.clone(),
            });
            let nested_container = Some(name.clone());
            for m in methods {
                collect_symbol_info(m, uri, nested_container.clone(), out, depth + 1);
            }
        }
        Expr::EnumDecl(name, _variants, line, col) => {
            let line0 = (*line as u32).saturating_sub(1);
            let col0 = (*col as u32).saturating_sub(1);
            out.push(SymbolInformation {
                name: name.clone(),
                kind: SymbolKind::ENUM,
                tags: None,
                #[allow(deprecated)]
                deprecated: None,
                location: tower_lsp::lsp_types::Location {
                    uri: uri.clone(),
                    range: Range {
                        start: Position {
                            line: line0,
                            character: col0,
                        },
                        end: Position {
                            line: line0,
                            character: col0 + name.chars().count() as u32,
                        },
                    },
                },
                container_name: container.clone(),
            });
        }
        Expr::UnionDecl(name, _fields, line, col) => {
            let line0 = (*line as u32).saturating_sub(1);
            let col0 = (*col as u32).saturating_sub(1);
            out.push(SymbolInformation {
                name: name.clone(),
                kind: SymbolKind::ENUM,
                tags: None,
                #[allow(deprecated)]
                deprecated: None,
                location: tower_lsp::lsp_types::Location {
                    uri: uri.clone(),
                    range: Range {
                        start: Position {
                            line: line0,
                            character: col0,
                        },
                        end: Position {
                            line: line0,
                            character: col0 + name.chars().count() as u32,
                        },
                    },
                },
                container_name: container.clone(),
            });
        }
        Expr::VarDecl(name, _, _, _, line, col) => {
            let line0 = (*line as u32).saturating_sub(1);
            let col0 = (*col as u32).saturating_sub(1);
            out.push(SymbolInformation {
                name: name.clone(),
                kind: SymbolKind::VARIABLE,
                tags: None,
                #[allow(deprecated)]
                deprecated: None,
                location: tower_lsp::lsp_types::Location {
                    uri: uri.clone(),
                    range: Range {
                        start: Position {
                            line: line0,
                            character: col0,
                        },
                        end: Position {
                            line: line0,
                            character: col0 + name.chars().count() as u32,
                        },
                    },
                },
                container_name: container.clone(),
            });
        }
        Expr::ForStmt(var, _, body, line, col) => {
            let line0 = (*line as u32).saturating_sub(1);
            let col0 = (*col as u32).saturating_sub(1);
            out.push(SymbolInformation {
                name: var.clone(),
                kind: SymbolKind::VARIABLE,
                tags: None,
                #[allow(deprecated)]
                deprecated: None,
                location: tower_lsp::lsp_types::Location {
                    uri: uri.clone(),
                    range: Range {
                        start: Position {
                            line: line0,
                            character: col0,
                        },
                        end: Position {
                            line: line0,
                            character: col0 + var.chars().count() as u32,
                        },
                    },
                },
                container_name: container.clone(),
            });
            for s in body {
                collect_symbol_info(s, uri, container.clone(), out, depth + 1);
            }
        }
        Expr::IfStmt(_, body, elifs, else_body, _, _) => {
            for s in body {
                collect_symbol_info(s, uri, container.clone(), out, depth + 1);
            }
            for (_, b) in elifs {
                for s in b {
                    collect_symbol_info(s, uri, container.clone(), out, depth + 1);
                }
            }
            if let Some(b) = else_body {
                for s in b {
                    collect_symbol_info(s, uri, container.clone(), out, depth + 1);
                }
            }
        }
        Expr::WhileStmt(_, body, _, _) => {
            for s in body {
                collect_symbol_info(s, uri, container.clone(), out, depth + 1);
            }
        }
        _ => {}
    }
}

/// 在所有打开的文档中搜索匹配查询字符串的符号（子串匹配，忽略大小写）
pub fn workspace_symbols(
    documents: &dashmap::DashMap<Url, super::state::DocumentState>,
    params: &WorkspaceSymbolParams,
) -> Vec<SymbolInformation> {
    let query = params.query.to_lowercase();
    let mut all = Vec::new();

    for entry in documents.iter() {
        let uri = entry.key();
        let doc = entry.value();
        let symbols = extract_workspace_symbols(&doc.ast, uri);
        for s in symbols {
            if query.is_empty() || s.name.to_lowercase().contains(&query) {
                all.push(s);
            }
        }
    }

    all.sort_by(|a, b| a.name.cmp(&b.name));
    all
}

#[cfg(test)]
mod tests {
    use super::*;
    use vx_vm::parser::Expr;

    #[test]
    fn test_document_symbols_empty() {
        let ast: Vec<Stmt> = vec![];
        let syms = document_symbols(&ast);
        assert!(syms.is_empty());
    }

    #[test]
    fn test_document_symbols_func() {
        let ast = vec![Expr::FuncDecl(
            "test_func".to_string(),
            vec![],
            None,
            vec![],
            1,
            1,
        )];
        let syms = document_symbols(&ast);
        assert_eq!(syms.len(), 1);
        assert_eq!(syms[0].name, "test_func");
        assert_eq!(syms[0].kind, SymbolKind::FUNCTION);
    }

    #[test]
    fn test_document_symbols_struct() {
        let ast = vec![Expr::StructDecl(
            "Point".to_string(),
            vec![("x".into(), "int".into()), ("y".into(), "int".into())],
            vec![],
            1,
            1,
        )];
        let syms = document_symbols(&ast);
        assert_eq!(syms.len(), 1);
        assert_eq!(syms[0].name, "Point");
        assert_eq!(syms[0].kind, SymbolKind::STRUCT);
        assert!(syms[0].children.is_some());
    }

    #[test]
    fn test_format_func_signature() {
        let params = vec![("a".into(), "int".into()), ("b".into(), "string".into())];
        let sig = format_func_signature("foo", &params, &Some("bool".into()));
        assert!(sig.contains("foo"));
        assert!(sig.contains("a: int"));
        assert!(sig.contains("b: string"));
        assert!(sig.contains("-> bool"));
    }

    #[test]
    fn test_extract_workspace_symbols() {
        let ast = vec![
            Expr::FuncDecl("hello".to_string(), vec![], None, vec![], 2, 3),
            Expr::EnumDecl("Color".to_string(), vec![], 5, 1),
        ];
        let uri = Url::parse("file:///test.vx").unwrap();
        let syms = extract_workspace_symbols(&ast, &uri);
        assert_eq!(syms.len(), 2);
        assert_eq!(syms[0].name, "hello");
        assert_eq!(syms[1].name, "Color");
    }
}
