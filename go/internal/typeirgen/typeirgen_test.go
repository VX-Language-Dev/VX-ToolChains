package typeirgen

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

// callsWithCallee returns Call instructions whose first arg is ";callee".
func callsWithCallee(t *testing.T, body []typeir.TypedInstruction, callee string) []typeir.TypedInstruction {
	t.Helper()
	var out []typeir.TypedInstruction
	for _, inst := range body {
		if inst.Op == typeir.Call && len(inst.Args) > 0 && inst.Args[0] == ";"+callee {
			out = append(out, inst)
		}
	}
	return out
}

func TestOutWithIntLiteralEmitsItoa(t *testing.T) {
	input := `func main():
    out(42)
`
	program := parseProgram(t, input)
	module := GenerateIR(program, "test")
	body := module.Functions[0].Instructions

	itoaCalls := callsWithCallee(t, body, "itoa")
	if len(itoaCalls) != 1 {
		t.Fatalf("expected exactly 1 itoa call for out(42), got %d", len(itoaCalls))
	}
	if len(itoaCalls[0].Args) != 2 || itoaCalls[0].Args[1] != "int" {
		t.Errorf("expected itoa call with int arg type, got %v", itoaCalls[0].Args)
	}
	outCalls := callsWithCallee(t, body, "out")
	if len(outCalls) != 1 {
		t.Fatalf("expected 1 out call, got %d", len(outCalls))
	}
	if len(outCalls[0].Args) != 2 || outCalls[0].Args[1] != "string" {
		t.Errorf("expected out call with string arg type after conversion, got %v", outCalls[0].Args)
	}
}

func TestOutWithIntVarEmitsItoa(t *testing.T) {
	input := `func main():
    var x = 7
    out(x)
`
	program := parseProgram(t, input)
	module := GenerateIR(program, "test")
	body := module.Functions[0].Instructions

	itoaCalls := callsWithCallee(t, body, "itoa")
	if len(itoaCalls) != 1 {
		t.Fatalf("expected exactly 1 itoa call for out(x), got %d", len(itoaCalls))
	}
	outCalls := callsWithCallee(t, body, "out")
	if len(outCalls) != 1 || len(outCalls[0].Args) != 2 || outCalls[0].Args[1] != "string" {
		t.Fatalf("expected out call with string arg type, got %v", outCalls)
	}
}

func TestOutWithStringDoesNotEmitItoa(t *testing.T) {
	input := `func main():
    out("hello")
`
	program := parseProgram(t, input)
	module := GenerateIR(program, "test")
	body := module.Functions[0].Instructions

	if itoa := callsWithCallee(t, body, "itoa"); len(itoa) != 0 {
		t.Errorf("expected no itoa call for out(string), got %v", itoa)
	}
}

func TestStringConcatWithIntEmitsItoa(t *testing.T) {
	input := `func main():
    out("n=" + 42)
`
	program := parseProgram(t, input)
	module := GenerateIR(program, "test")
	body := module.Functions[0].Instructions

	itoaCalls := callsWithCallee(t, body, "itoa")
	if len(itoaCalls) != 1 {
		t.Fatalf("expected exactly 1 itoa call for \"n=\" + 42, got %d", len(itoaCalls))
	}
	var concatFound bool
	for _, inst := range body {
		if inst.Op == typeir.StrConcat {
			concatFound = true
		}
	}
	if !concatFound {
		t.Error("expected a StrConcat instruction")
	}
}

func TestIntPlusStringEmitsItoaWithTemp(t *testing.T) {
	input := `func main():
    out(42 + "x")
`
	program := parseProgram(t, input)
	module := GenerateIR(program, "test")
	body := module.Functions[0].Instructions

	itoaCalls := callsWithCallee(t, body, "itoa")
	if len(itoaCalls) != 1 {
		t.Fatalf("expected exactly 1 itoa call for 42 + \"x\", got %d", len(itoaCalls))
	}
	var concatFound bool
	for _, inst := range body {
		if inst.Op == typeir.StrConcat {
			concatFound = true
		}
	}
	if !concatFound {
		t.Error("expected a StrConcat instruction")
	}
}
