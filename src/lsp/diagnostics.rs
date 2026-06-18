// VX Language LSP - 诊断引擎
// 将词法/语法/所有权检查错误转换为 LSP Diagnostic

use tower_lsp::lsp_types::{Diagnostic, DiagnosticSeverity, Position, Range, Url};
use vx_vm::compiler_ownership::OwnershipChecker;
use vx_vm::parser::Parser;
use vx_vm::token::{Lexer, Token, VXError};

/// 文档解析结果：包含 AST、Token 列表和所有诊断
pub struct DiagnosticResult {
    pub tokens: Vec<Token>,
    pub ast: Vec<vx_vm::parser::Stmt>,
    pub diagnostics: Vec<Diagnostic>,
}

/// 运行完整的诊断流程：词法分析 → 语法分析 → 所有权检查
pub fn run_diagnostics(_uri: &Url, source: &str) -> DiagnosticResult {
    let mut diagnostics = Vec::new();
    let mut tokens = Vec::new();
    let mut ast = Vec::new();

    // 阶段一：词法分析
    let lexer = Lexer::new(source);
    match lexer.tokenize() {
        Ok(t) => {
            tokens = t;
        }
        Err(err) => {
            diagnostics.push(vx_error_to_diagnostic(&err));
            return DiagnosticResult {
                tokens,
                ast,
                diagnostics,
            };
        }
    }

    // 阶段二：语法分析
    let mut parser = Parser::new(tokens.clone(), source);
    match parser.parse() {
        Ok(a) => {
            ast = a;
        }
        Err(err) => {
            diagnostics.push(vx_error_to_diagnostic(&err));
            return DiagnosticResult {
                tokens,
                ast,
                diagnostics,
            };
        }
    }

    // 阶段三：所有权检查
    let mut checker = OwnershipChecker::new(source);
    checker.check_ast(&ast);
    for err in &checker.errors {
        diagnostics.extend(ownership_error_to_diagnostic(err, DiagnosticSeverity::ERROR));
    }
    for warn in &checker.warnings {
        diagnostics.extend(ownership_error_to_diagnostic(warn, DiagnosticSeverity::WARNING));
    }

    DiagnosticResult {
        tokens,
        ast,
        diagnostics,
    }
}

/// 转换 VXError 为 LSP Diagnostic
fn vx_error_to_diagnostic(err: &VXError) -> Diagnostic {
    Diagnostic {
        range: Range {
            start: Position {
                line: err.line.saturating_sub(1) as u32,
                character: err.col.saturating_sub(1) as u32,
            },
            end: Position {
                line: err.line.saturating_sub(1) as u32,
                character: err.col as u32,
            },
        },
        severity: Some(DiagnosticSeverity::ERROR),
        message: err.msg.clone(),
        source: Some("vx".to_string()),
        ..Default::default()
    }
}

/// 从所有权检查错误字符串中提取行号
/// 所有权错误格式："描述文字\n LINE | SOURCE_CODE"
fn extract_line_from_ownership_error(err: &str) -> Option<usize> {
    // 跳过第一行（描述文字），从第二行开始查找 " LINE |" 模式
    for line in err.lines().skip(1) {
        let trimmed = line.trim();
        if let Some(rest) = trimmed.find('|') {
            let before_pipe = trimmed[..rest].trim();
            if let Ok(line_num) = before_pipe.parse::<usize>() {
                return Some(line_num);
            }
        }
    }
    None
}

/// 转换所有权错误字符串为 LSP Diagnostic（可能含有多行）
fn ownership_error_to_diagnostic(err: &str, severity: DiagnosticSeverity) -> Vec<Diagnostic> {
    let mut diagnostics = Vec::new();
    let message = err.to_string();

    let line = extract_line_from_ownership_error(&message);

    let clean_msg = message
        .lines()
        .next()
        .unwrap_or(&message)
        .to_string();

    diagnostics.push(Diagnostic {
        range: Range {
            start: Position {
                line: line.unwrap_or(0).saturating_sub(1) as u32,
                character: 0,
            },
            end: Position {
                line: line.unwrap_or(0).saturating_sub(1) as u32,
                character: u32::MAX,
            },
        },
        severity: Some(severity),
        message: clean_msg,
        source: Some("vx-ownership".to_string()),
        ..Default::default()
    });

    diagnostics
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_vx_error_to_diagnostic() {
        let err = VXError {
            msg: "测试错误".to_string(),
            line: 2,
            col: 5,
            source: None,
        };
        let diag = vx_error_to_diagnostic(&err);
        assert_eq!(diag.message, "测试错误");
        assert_eq!(diag.range.start.line, 1);
        assert_eq!(diag.range.start.character, 4);
        assert_eq!(diag.source, Some("vx".to_string()));
    }

    #[test]
    fn test_extract_line_from_ownership_error() {
        let err = "变量 'x' 已被释放（use-after-free/悬垂指针）\n 4 | out(x)";
        assert_eq!(extract_line_from_ownership_error(err), Some(4));

        let err_no_line = "变量 'x' 未定义";
        assert_eq!(extract_line_from_ownership_error(err_no_line), None);
    }

    #[test]
    fn test_run_diagnostics_valid() {
        let uri = Url::parse("file:///test.vx").unwrap();
        let source = "func main():\n    out(1)\n".to_string();
        let result = run_diagnostics(&uri, &source);
        assert!(result.diagnostics.is_empty(), "expected no errors, got: {:?}", result.diagnostics);
        assert!(!result.tokens.is_empty());
        assert!(!result.ast.is_empty());
    }

    #[test]
    fn test_run_diagnostics_syntax_error() {
        let uri = Url::parse("file:///test.vx").unwrap();
        let source = "func main():\n    x = $bad\n".to_string();
        let result = run_diagnostics(&uri, &source);
        assert!(!result.diagnostics.is_empty(), "expected at least 1 diagnostic");
    }
}
