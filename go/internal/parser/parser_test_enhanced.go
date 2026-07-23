package parser

import (
	"testing"

	"github.com/vx-lang/vxc/internal/ast"
	"github.com/vx-lang/vxc/internal/lexer"
)

func parseSource(t *testing.T, source string) *Parser {
	t.Helper()
	tokens := lexer.NewLexer(source).TokenizeAll()
	return NewParser(tokens)
}

func TestParseVariableDeclaration(t *testing.T) {
	source := `x: int = 42`
	p := parseSource(t, source)
	stmts := p.ParseProgram()

	if len(p.Errors) > 0 {
		t.Fatalf("parser errors: %v", p.Errors)
	}

	if len(stmts) != 1 {
		t.Fatalf("expected 1 statement, got %d", len(stmts))
	}

	varDecl, ok := stmts[0].(*ast.VarDecl)
	if !ok {
		t.Fatalf("expected VarDecl, got %T", stmts[0])
	}

	if varDecl.Name != "x" {
		t.Errorf("expected variable name 'x', got '%s'", varDecl.Name)
	}
}

func TestParseFunctionDefinition(t *testing.T) {
	source := `func add(a: int, b: int) -> int:
    return a + b
`
	p := parseSource(t, source)
	stmts := p.ParseProgram()

	if len(p.Errors) > 0 {
		t.Fatalf("parser errors: %v", p.Errors)
	}

	if len(stmts) != 1 {
		t.Fatalf("expected 1 statement, got %d", len(stmts))
	}

	funcDecl, ok := stmts[0].(*ast.FuncDecl)
	if !ok {
		t.Fatalf("expected FuncDecl, got %T", stmts[0])
	}

	if funcDecl.Name != "add" {
		t.Errorf("expected function name 'add', got '%s'", funcDecl.Name)
	}

	if len(funcDecl.Params) != 2 {
		t.Fatalf("expected 2 parameters, got %d", len(funcDecl.Params))
	}
}

func TestParseIfStatement(t *testing.T) {
	source := `if x > 0:
    out("positive")
else:
    out("non-positive")
`
	p := parseSource(t, source)
	stmts := p.ParseProgram()

	if len(p.Errors) > 0 {
		t.Fatalf("parser errors: %v", p.Errors)
	}

	if len(stmts) != 1 {
		t.Fatalf("expected 1 statement, got %d", len(stmts))
	}

	ifStmt, ok := stmts[0].(*ast.IfStmt)
	if !ok {
		t.Fatalf("expected IfStmt, got %T", stmts[0])
	}

	if ifStmt.Else == nil {
		t.Error("expected else branch")
	}
}

func TestParseForLoop(t *testing.T) {
	source := `for i in range(10):
    out(i)
`
	p := parseSource(t, source)
	stmts := p.ParseProgram()

	if len(p.Errors) > 0 {
		t.Fatalf("parser errors: %v", p.Errors)
	}

	if len(stmts) != 1 {
		t.Fatalf("expected 1 statement, got %d", len(stmts))
	}

	forLoop, ok := stmts[0].(*ast.ForInLoop)
	if !ok {
		t.Fatalf("expected ForInLoop, got %T", stmts[0])
	}

	if forLoop.Variable != "i" {
		t.Errorf("expected loop variable 'i', got '%s'", forLoop.Variable)
	}
}

func TestParseWhileLoop(t *testing.T) {
	source := `while x < 10:
    x = x + 1
`
	p := parseSource(t, source)
	stmts := p.ParseProgram()

	if len(p.Errors) > 0 {
		t.Fatalf("parser errors: %v", p.Errors)
	}

	if len(stmts) != 1 {
		t.Fatalf("expected 1 statement, got %d", len(stmts))
	}

	whileLoop, ok := stmts[0].(*ast.WhileLoop)
	if !ok {
		t.Fatalf("expected WhileLoop, got %T", stmts[0])
	}
}

func TestParseStructDefinition(t *testing.T) {
	source := `struct Point:
    x: float
    y: float
`
	p := parseSource(t, source)
	stmts := p.ParseProgram()

	if len(p.Errors) > 0 {
		t.Fatalf("parser errors: %v", p.Errors)
	}

	if len(stmts) != 1 {
		t.Fatalf("expected 1 statement, got %d", len(stmts))
	}

	structDecl, ok := stmts[0].(*ast.StructDecl)
	if !ok {
		t.Fatalf("expected StructDecl, got %T", stmts[0])
	}

	if structDecl.Name != "Point" {
		t.Errorf("expected struct name 'Point', got '%s'", structDecl.Name)
	}

	if len(structDecl.Fields) != 2 {
		t.Fatalf("expected 2 fields, got %d", len(structDecl.Fields))
	}
}

func TestParseClassDefinition(t *testing.T) {
	source := `class Counter:
    count: int

    func new() -> Counter:
        return new Counter()
    
    func increment():
        self.count = self.count + 1
`
	p := parseSource(t, source)
	stmts := p.ParseProgram()

	if len(p.Errors) > 0 {
		t.Fatalf("parser errors: %v", p.Errors)
	}

	if len(stmts) != 1 {
		t.Fatalf("expected 1 statement, got %d", len(stmts))
	}

	classDecl, ok := stmts[0].(*ast.ClassDecl)
	if !ok {
		t.Fatalf("expected ClassDecl, got %T", stmts[0])
	}

	if classDecl.Name != "Counter" {
		t.Errorf("expected class name 'Counter', got '%s'", classDecl.Name)
	}
}

func TestParseImportStatement(t *testing.T) {
	source := `import std.io
import std.math as math
import a.b.c.d
`
	p := parseSource(t, source)
	stmts := p.ParseProgram()

	if len(p.Errors) > 0 {
		t.Fatalf("parser errors: %v", p.Errors)
	}

	if len(stmts) != 3 {
		t.Fatalf("expected 3 statements, got %d", len(stmts))
	}

	for i, stmt := range stmts {
		importStmt, ok := stmt.(*ast.ImportStmt)
		if !ok {
			t.Fatalf("statement[%d] expected ImportStmt, got %T", i, stmt)
		}

		switch i {
		case 0:
			if importStmt.Path != "std.io" {
				t.Errorf("import[%d] expected path 'std.io', got '%s'", i, importStmt.Path)
			}
		case 1:
			if importStmt.Path != "std.math" {
				t.Errorf("import[%d] expected path 'std.math', got '%s'", i, importStmt.Path)
			}
			if importStmt.Alias != "math" {
				t.Errorf("import[%d] expected alias 'math', got '%s'", i, importStmt.Alias)
			}
		case 2:
			if importStmt.Path != "a.b.c.d" {
				t.Errorf("import[%d] expected path 'a.b.c.d', got '%s'", i, importStmt.Path)
			}
		}
	}
}

func TestParseBinaryExpression(t *testing.T) {
	tests := []struct {
		name   string
		source string
		check  func(*ast.BinaryExpr) bool
	}{
		{
			name:   "simple addition",
			source: `x = a + b`,
			check: func(expr *ast.BinaryExpr) bool {
				return expr.Op == ast.Plus && expr.Left.(*ast.Ident).Name == "a" && expr.Right.(*ast.Ident).Name == "b"
			},
		},
		{
			name:   "complex expression",
			source: `x = (a + b) * c / d`,
			check: func(expr *ast.BinaryExpr) bool {
				return expr.Op == ast.Slash
			},
		},
	}

	for _, tt := range tests {
		t.Run(tt.name, func(t *testing.T) {
			p := parseSource(t, tt.source)
			stmts := p.ParseProgram()

			if len(p.Errors) > 0 {
				t.Fatalf("parser errors: %v", p.Errors)
			}

			if len(stmts) != 1 {
				t.Fatalf("expected 1 statement, got %d", len(stmts))
			}

			varDecl, ok := stmts[0].(*ast.VarDecl)
			if !ok {
				t.Fatalf("expected VarDecl, got %T", stmts[0])
			}

			binExpr, ok := varDecl.Value.(*ast.BinaryExpr)
			if !ok {
				t.Fatalf("expected BinaryExpr, got %T", varDecl.Value)
			}

			if !tt.check(binExpr) {
				t.Error("expression check failed")
			}
		})
	}
}

func TestParseErrorRecovery(t *testing.T) {
	source := `x = 
y = 42
z = "hello"
`
	p := parseSource(t, source)
	stmts := p.ParseProgram()

	if len(p.Errors) == 0 {
		t.Fatal("expected parser errors")
	}

	if len(stmts) < 2 {
		t.Fatalf("expected at least 2 statements after error recovery, got %d", len(stmts))
	}
}
