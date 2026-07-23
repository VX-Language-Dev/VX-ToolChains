package parser

import (
	"testing"

	"github.com/vx-lang/vxc/internal/ast"
	"github.com/vx-lang/vxc/internal/lexer"
)

func TestHelloWorld(t *testing.T) {
	input := "func main():\n    out(\"Hello World\")"
	l := lexer.NewLexer(input)
	tokens := l.TokenizeAll()
	p := NewParser(tokens)
	program := p.ParseProgram()

	if len(p.Errors) > 0 {
		for _, err := range p.Errors {
			t.Logf("parse error: %s", err)
		}
		t.Fatalf("parser produced %d errors", len(p.Errors))
	}

	if len(program) != 1 {
		t.Fatalf("expected program length 1, got %d", len(program))
	}

	fn, ok := program[0].(*ast.FuncDecl)
	if !ok {
		t.Fatalf("expected *ast.FuncDecl, got %T", program[0])
	}
	if fn.Name != "main" {
		t.Errorf("expected function name 'main', got %q", fn.Name)
	}
	if len(fn.Body) != 1 {
		t.Fatalf("expected body length 1, got %d", len(fn.Body))
	}

	exprStmt, ok := fn.Body[0].(*ast.ExprStmt)
	if !ok {
		t.Fatalf("expected *ast.ExprStmt, got %T", fn.Body[0])
	}

	call, ok := exprStmt.Expr.(*ast.CallExpr)
	if !ok {
		t.Fatalf("expected *ast.CallExpr, got %T", exprStmt.Expr)
	}

	callee, ok := call.Callee.(*ast.Identifier)
	if !ok {
		t.Fatalf("expected *ast.Identifier callee, got %T", call.Callee)
	}
	if callee.Name != "out" {
		t.Errorf("expected callee name 'out', got %q", callee.Name)
	}

	if len(call.Args) != 1 {
		t.Fatalf("expected 1 argument, got %d", len(call.Args))
	}
	strLit, ok := call.Args[0].(*ast.StringLiteral)
	if !ok {
		t.Fatalf("expected *ast.StringLiteral argument, got %T", call.Args[0])
	}
	if strLit.Value != "Hello World" {
		t.Errorf("expected string value 'Hello World', got %q", strLit.Value)
	}
}

func TestFunctionWithReturnType(t *testing.T) {
	input := "func add(a: int, b: int) -> int:\n    return a + b"
	l := lexer.NewLexer(input)
	tokens := l.TokenizeAll()
	p := NewParser(tokens)
	program := p.ParseProgram()

	if len(p.Errors) > 0 {
		for _, err := range p.Errors {
			t.Logf("parse error: %s", err)
		}
		t.Fatalf("parser produced %d errors", len(p.Errors))
	}

	if len(program) != 1 {
		t.Fatalf("expected program length 1, got %d", len(program))
	}

	fn, ok := program[0].(*ast.FuncDecl)
	if !ok {
		t.Fatalf("expected *ast.FuncDecl, got %T", program[0])
	}
	if fn.Name != "add" {
		t.Errorf("expected function name 'add', got %q", fn.Name)
	}
	if len(fn.Params) != 2 {
		t.Fatalf("expected 2 params, got %d", len(fn.Params))
	}
	if fn.Params[0].Name != "a" {
		t.Errorf("expected first param name 'a', got %q", fn.Params[0].Name)
	}
	if fn.Params[0].ParamType.Name != "int" {
		t.Errorf("expected first param type 'int', got %q", fn.Params[0].ParamType.Name)
	}
	if fn.ReturnType == nil {
		t.Fatalf("expected return type, got nil")
	}
	if fn.ReturnType.Name != "int" {
		t.Errorf("expected return type 'int', got %q", fn.ReturnType.Name)
	}
}

func TestVariableDeclaration(t *testing.T) {
	input := "var x: int = 42"
	l := lexer.NewLexer(input)
	tokens := l.TokenizeAll()
	p := NewParser(tokens)
	program := p.ParseProgram()

	if len(p.Errors) > 0 {
		for _, err := range p.Errors {
			t.Logf("parse error: %s", err)
		}
		t.Fatalf("parser produced %d errors", len(p.Errors))
	}

	if len(program) != 1 {
		t.Fatalf("expected program length 1, got %d", len(program))
	}

	vd, ok := program[0].(*ast.VarDecl)
	if !ok {
		t.Fatalf("expected *ast.VarDecl, got %T", program[0])
	}
	if vd.Name != "x" {
		t.Errorf("expected variable name 'x', got %q", vd.Name)
	}
	if vd.Type == nil {
		t.Fatalf("expected type, got nil")
	}
	if vd.Type.Name != "int" {
		t.Errorf("expected type 'int', got %q", vd.Type.Name)
	}

	init, ok := vd.Init.(*ast.IntLiteral)
	if !ok {
		t.Fatalf("expected *ast.IntLiteral init, got %T", vd.Init)
	}
	if init.Value != 42 {
		t.Errorf("expected init value 42, got %d", init.Value)
	}

	if !vd.IsMut {
		t.Errorf("expected IsMut true for var declaration")
	}
}

func TestIfStatement(t *testing.T) {
	input := "if x > 0:\n    out(\"positive\")"
	l := lexer.NewLexer(input)
	tokens := l.TokenizeAll()
	p := NewParser(tokens)
	program := p.ParseProgram()

	if len(p.Errors) > 0 {
		for _, err := range p.Errors {
			t.Logf("parse error: %s", err)
		}
		t.Fatalf("parser produced %d errors", len(p.Errors))
	}

	if len(program) != 1 {
		t.Fatalf("expected program length 1, got %d", len(program))
	}

	ifStmt, ok := program[0].(*ast.IfStmt)
	if !ok {
		t.Fatalf("expected *ast.IfStmt, got %T", program[0])
	}

	binOp, ok := ifStmt.Condition.(*ast.BinaryOp)
	if !ok {
		t.Fatalf("expected *ast.BinaryOp condition, got %T", ifStmt.Condition)
	}
	if binOp.Op != ">" {
		t.Errorf("expected operator '>', got %q", binOp.Op)
	}

	if len(ifStmt.ThenBranch) != 1 {
		t.Fatalf("expected then branch length 1, got %d", len(ifStmt.ThenBranch))
	}
	if len(ifStmt.ElseBranch) != 0 {
		t.Errorf("expected else branch length 0, got %d", len(ifStmt.ElseBranch))
	}
}

func TestImportStmt(t *testing.T) {
	input := "import std.io"
	l := lexer.NewLexer(input)
	tokens := l.TokenizeAll()
	p := NewParser(tokens)
	program := p.ParseProgram()

	if len(p.Errors) > 0 {
		for _, err := range p.Errors {
			t.Logf("parse error: %s", err)
		}
		t.Fatalf("parser produced %d errors", len(p.Errors))
	}

	if len(program) != 1 {
		t.Fatalf("expected program length 1, got %d", len(program))
	}

	imp, ok := program[0].(*ast.ImportStmt)
	if !ok {
		t.Fatalf("expected *ast.ImportStmt, got %T", program[0])
	}
	if imp.Path != "std.io" {
		t.Errorf("expected import path 'std.io', got %q", imp.Path)
	}
	if imp.Alias != "" {
		t.Errorf("expected no alias, got %q", imp.Alias)
	}
}

func TestImportStmtWithAlias(t *testing.T) {
	input := "import std.io as myio"
	l := lexer.NewLexer(input)
	tokens := l.TokenizeAll()
	p := NewParser(tokens)
	program := p.ParseProgram()

	if len(p.Errors) > 0 {
		for _, err := range p.Errors {
			t.Logf("parse error: %s", err)
		}
		t.Fatalf("parser produced %d errors", len(p.Errors))
	}

	if len(program) != 1 {
		t.Fatalf("expected program length 1, got %d", len(program))
	}

	imp, ok := program[0].(*ast.ImportStmt)
	if !ok {
		t.Fatalf("expected *ast.ImportStmt, got %T", program[0])
	}
	if imp.Path != "std.io" {
		t.Errorf("expected import path 'std.io', got %q", imp.Path)
	}
	if imp.Alias != "myio" {
		t.Errorf("expected alias 'myio', got %q", imp.Alias)
	}
}

func TestImportStmtSimplePath(t *testing.T) {
	input := "import io"
	l := lexer.NewLexer(input)
	tokens := l.TokenizeAll()
	p := NewParser(tokens)
	program := p.ParseProgram()

	if len(p.Errors) > 0 {
		for _, err := range p.Errors {
			t.Logf("parse error: %s", err)
		}
		t.Fatalf("parser produced %d errors", len(p.Errors))
	}

	if len(program) != 1 {
		t.Fatalf("expected program length 1, got %d", len(program))
	}

	imp, ok := program[0].(*ast.ImportStmt)
	if !ok {
		t.Fatalf("expected *ast.ImportStmt, got %T", program[0])
	}
	if imp.Path != "io" {
		t.Errorf("expected import path 'io', got %q", imp.Path)
	}
}

func TestMultipleImports(t *testing.T) {
	input := "import std.io\nimport std.math\nimport std.string.util"
	l := lexer.NewLexer(input)
	tokens := l.TokenizeAll()
	p := NewParser(tokens)
	program := p.ParseProgram()

	if len(p.Errors) > 0 {
		for _, err := range p.Errors {
			t.Logf("parse error: %s", err)
		}
		t.Fatalf("parser produced %d errors", len(p.Errors))
	}

	if len(program) != 3 {
		t.Fatalf("expected program length 3, got %d", len(program))
	}

	expectedPaths := []string{"std.io", "std.math", "std.string.util"}
	for i, expectedPath := range expectedPaths {
		imp, ok := program[i].(*ast.ImportStmt)
		if !ok {
			t.Fatalf("program[%d]: expected *ast.ImportStmt, got %T", i, program[i])
		}
		if imp.Path != expectedPath {
			t.Errorf("program[%d]: expected import path %q, got %q", i, expectedPath, imp.Path)
		}
	}
}

func TestImportThenFunction(t *testing.T) {
	input := "import std.io\n\nfunc main():\n    out(\"hello\")"
	l := lexer.NewLexer(input)
	tokens := l.TokenizeAll()
	p := NewParser(tokens)
	program := p.ParseProgram()

	if len(p.Errors) > 0 {
		for _, err := range p.Errors {
			t.Logf("parse error: %s", err)
		}
		t.Fatalf("parser produced %d errors", len(p.Errors))
	}

	if len(program) != 2 {
		t.Fatalf("expected program length 2, got %d", len(program))
	}

	imp, ok := program[0].(*ast.ImportStmt)
	if !ok {
		t.Fatalf("program[0]: expected *ast.ImportStmt, got %T", program[0])
	}
	if imp.Path != "std.io" {
		t.Errorf("expected import path 'std.io', got %q", imp.Path)
	}

	fn, ok := program[1].(*ast.FuncDecl)
	if !ok {
		t.Fatalf("program[1]: expected *ast.FuncDecl, got %T", program[1])
	}
	if fn.Name != "main" {
		t.Errorf("expected function name 'main', got %q", fn.Name)
	}
}

func TestImportDeepPath(t *testing.T) {
	input := "import a.b.c.d.e"
	l := lexer.NewLexer(input)
	tokens := l.TokenizeAll()
	p := NewParser(tokens)
	program := p.ParseProgram()

	if len(p.Errors) > 0 {
		for _, err := range p.Errors {
			t.Logf("parse error: %s", err)
		}
		t.Fatalf("parser produced %d errors", len(p.Errors))
	}

	if len(program) != 1 {
		t.Fatalf("expected program length 1, got %d", len(program))
	}

	imp, ok := program[0].(*ast.ImportStmt)
	if !ok {
		t.Fatalf("expected *ast.ImportStmt, got %T", program[0])
	}
	if imp.Path != "a.b.c.d.e" {
		t.Errorf("expected import path 'a.b.c.d.e', got %q", imp.Path)
	}
}
