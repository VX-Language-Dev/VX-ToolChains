package typeirgen

import (
	"testing"

	"github.com/vx-lang/vxc/internal/ast"
	"github.com/vx-lang/vxc/internal/lexer"
	"github.com/vx-lang/vxc/internal/parser"
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

func TestGenerateIRWithImport(t *testing.T) {
	input := `import std.io

func main():
    out("hello")
`
	program := parseProgram(t, input)
	module := GenerateIR(program, "test")

	if module == nil {
		t.Fatal("expected non-nil module")
	}
	if module.Name != "test" {
		t.Errorf("expected module name 'test', got %q", module.Name)
	}
	if len(module.Functions) != 1 {
		t.Fatalf("expected 1 function, got %d", len(module.Functions))
	}
	if module.Functions[0].Name != "main" {
		t.Errorf("expected function name 'main', got %q", module.Functions[0].Name)
	}
}

func TestGenerateIRWithMultipleImports(t *testing.T) {
	input := `import std.io
import std.math
import std.string

func main():
    out("hello")
`
	program := parseProgram(t, input)
	module := GenerateIR(program, "test")

	if module == nil {
		t.Fatal("expected non-nil module")
	}
	if len(module.Functions) != 1 {
		t.Fatalf("expected 1 function, got %d", len(module.Functions))
	}
}

func TestGenerateIRImportOnly(t *testing.T) {
	input := `import std.io`
	program := parseProgram(t, input)
	module := GenerateIR(program, "test")

	if module == nil {
		t.Fatal("expected non-nil module")
	}
	if len(module.Functions) != 0 {
		t.Errorf("expected 0 functions for import-only module, got %d", len(module.Functions))
	}
}

func TestGenerateIRWithImportAndAlias(t *testing.T) {
	input := `import std.io as myio

func main():
    out("hello")
`
	program := parseProgram(t, input)
	module := GenerateIR(program, "test")

	if module == nil {
		t.Fatal("expected non-nil module")
	}
	if len(module.Functions) != 1 {
		t.Fatalf("expected 1 function, got %d", len(module.Functions))
	}
}
