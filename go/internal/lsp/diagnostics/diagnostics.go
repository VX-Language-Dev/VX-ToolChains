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

	diagnosticsList := make([]types.Diagnostic, 0, 8)

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
