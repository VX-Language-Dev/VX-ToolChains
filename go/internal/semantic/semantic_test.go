package semantic

import (
	"testing"

	"github.com/vx-lang/vxc/internal/ast"
	"github.com/vx-lang/vxc/internal/lexer"
	"github.com/vx-lang/vxc/internal/parser"
	"github.com/vx-lang/vxc/internal/typeir"
)

func parseProgram(t *testing.T, input string) []ast.Expr {
	t.Helper()
	l := lexer.NewLexer(input)
	tokens := l.TokenizeAll()
	p := parser.NewParser(tokens)
	program := p.ParseProgram()
	if len(p.Errors) > 0 {
		for _, err := range p.Errors {
			t.Logf("parse error: %s", err)
		}
		t.Fatalf("parser produced %d errors", len(p.Errors))
	}
	return program
}

func TestAnalyzeProgramWithImport(t *testing.T) {
	input := `import std.io

func main() -> int:
    out("hello")
    return 0
`
	program := parseProgram(t, input)
	st := AnalyzeProgram(program)

	if len(st.Errors) > 0 {
		for _, err := range st.Errors {
			t.Errorf("semantic error: %s at %d:%d", err.Msg, err.Pos.Line, err.Pos.Col)
		}
		t.Fatalf("semantic analysis produced %d errors", len(st.Errors))
	}

	fn, ok := st.Functions["main"]
	if !ok {
		t.Fatalf("expected function 'main' to be registered")
	}
	if typeir.TypToString(fn.ReturnType) != "int" {
		t.Errorf("expected 'main' return type 'int', got %s", typeir.TypToString(fn.ReturnType))
	}
}

func TestAnalyzeProgramWithMultipleImports(t *testing.T) {
	input := `import std.io
import std.math
import std.string

func main():
    out("hello")
`
	program := parseProgram(t, input)
	st := AnalyzeProgram(program)

	if len(st.Errors) > 0 {
		for _, err := range st.Errors {
			t.Errorf("semantic error: %s at %d:%d", err.Msg, err.Pos.Line, err.Pos.Col)
		}
		t.Fatalf("semantic analysis produced %d errors", len(st.Errors))
	}

	if _, ok := st.Functions["main"]; !ok {
		t.Fatalf("expected function 'main' to be registered")
	}
}

func TestAnalyzeProgramWithImportAndAlias(t *testing.T) {
	input := `import std.io as myio

func main():
    out("hello")
`
	program := parseProgram(t, input)
	st := AnalyzeProgram(program)

	if len(st.Errors) > 0 {
		for _, err := range st.Errors {
			t.Errorf("semantic error: %s at %d:%d", err.Msg, err.Pos.Line, err.Pos.Col)
		}
		t.Fatalf("semantic analysis produced %d errors", len(st.Errors))
	}

	if _, ok := st.Functions["main"]; !ok {
		t.Fatalf("expected function 'main' to be registered")
	}
}

func TestAnalyzeProgramImportOnly(t *testing.T) {
	input := `import std.io`
	program := parseProgram(t, input)
	st := AnalyzeProgram(program)

	if len(st.Errors) > 0 {
		for _, err := range st.Errors {
			t.Errorf("semantic error: %s at %d:%d", err.Msg, err.Pos.Line, err.Pos.Col)
		}
		t.Fatalf("semantic analysis produced %d errors", len(st.Errors))
	}
}
