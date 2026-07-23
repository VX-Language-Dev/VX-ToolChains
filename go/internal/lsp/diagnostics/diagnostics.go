package diagnostics

import (
	"github.com/vx-lang/vxc/internal/ast"
	"github.com/vx-lang/vxc/internal/lsp/types"
	"github.com/vx-lang/vxc/internal/parser"
	"github.com/vx-lang/vxc/internal/token"
)

type DiagnosticResult struct {
	Tokens      []token.Token
	AstNodes    []ast.Expr
	Diagnostics []types.Diagnostic
}

func RunDiagnostics(uri, source string, memorySafetyAsWarning bool) (*DiagnosticResult, error) {
	_ = uri
	_ = memorySafetyAsWarning

	var diagnosticsList []types.Diagnostic

	lexer := token.NewLexer(source)
	tokens, err := lexer.Tokenize()
	if err != nil {
		diagnosticsList = append(diagnosticsList, types.Diagnostic{
			Range: types.Range{
				Start: types.Position{Line: 0, Character: 0},
				End:   types.Position{Line: 0, Character: 0},
			},
			Severity: types.SeverityError,
			Code:     nil,
			Source:   "vx",
			Message:  err.Error(),
		})
		return &DiagnosticResult{
			Tokens:      nil,
			AstNodes:    nil,
			Diagnostics: diagnosticsList,
		}, nil
	}

	p := parser.NewParser(tokens)
	astNodes := p.ParseProgram()

	for _, errMsg := range p.Errors {
		diagnosticsList = append(diagnosticsList, parseErrorToDiagnostic(errMsg))
	}

	return &DiagnosticResult{
		Tokens:      tokens,
		AstNodes:    astNodes,
		Diagnostics: diagnosticsList,
	}, nil
}

func vxErrorToDiagnostic(err *token.VXError) types.Diagnostic {
	startLine := uint32(err.Line - 1)
	if startLine > 0 && err.Line == 0 {
		startLine = 0
	}
	startCol := uint32(err.Col - 1)
	if startCol > 0 && err.Col == 0 {
		startCol = 0
	}
	endLine := uint32(err.EndLine - 1)
	if endLine > 0 && err.EndLine == 0 {
		endLine = 0
	}
	endCol := uint32(err.EndCol)

	return types.Diagnostic{
		Range: types.Range{
			Start: types.Position{Line: startLine, Character: startCol},
			End:   types.Position{Line: endLine, Character: endCol},
		},
		Severity: vxSeverityToLsp(err.Severity),
		Code:     &err.Code,
		Source:   "vx",
		Message:  err.Msg,
	}
}

func vxSeverityToLsp(s token.Severity) types.Severity {
	switch s {
	case token.SeverityError:
		return types.SeverityError
	case token.SeverityWarning:
		return types.SeverityWarning
	case token.SeverityNote:
		return types.SeverityInformation
	case token.SeverityHelp:
		return types.SeverityHint
	default:
		return types.SeverityError
	}
}

func parseErrorToDiagnostic(errMsg string) types.Diagnostic {
	return types.Diagnostic{
		Range: types.Range{
			Start: types.Position{Line: 0, Character: 0},
			End:   types.Position{Line: 0, Character: 0},
		},
		Severity: types.SeverityError,
		Code:     nil,
		Source:   "vx",
		Message:  errMsg,
	}
}