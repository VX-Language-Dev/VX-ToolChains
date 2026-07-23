package vfmt

import (
	"strings"
	"testing"

	"github.com/vx-lang/vxc/internal/lexer"
	"github.com/vx-lang/vxc/internal/parser"
)

// Test helpers
func normalize(s string) string {
	return strings.TrimSpace(s)
}

func TestTransformRenameFunc(t *testing.T) {
	input := `import std.io

func main():
    out(42)
`
	expected := `import std.io

func main():
    sys.print(42)
`

	config := Config{
		Rules: []Rule{
			{
				Kind:      "rename_func",
				OldName:   "out",
				NewName:   "print",
				NewModule: "sys",
			},
		},
	}

	result := transformSource(input, config)
	if normalize(result) != normalize(expected) {
		t.Errorf("expected:\n%s\ngot:\n%s", expected, result)
	}
}

func TestTransformRenameMethod(t *testing.T) {
	input := `func test():
    v.push(42)
`
	expected := `func test():
    v.append(42)
`

	config := Config{
		Rules: []Rule{
			{
				Kind:    "rename_method",
				OldName: "push",
				NewName: "append",
			},
		},
	}

	result := transformSource(input, config)
	if normalize(result) != normalize(expected) {
		t.Errorf("expected:\n%s\ngot:\n%s", expected, result)
	}
}

func TestTransformImportMigration(t *testing.T) {
	input := `import std.old_module

func main():
    old_module.do_something()
`
	expected := `import std.new_module

func main():
    old_module.do_something()
`

	config := Config{
		Rules: []Rule{
			{
				Kind:    "migrate_import",
				OldName: "std.old_module",
				NewName: "std.new_module",
			},
		},
	}

	result := transformSource(input, config)
	if normalize(result) != normalize(expected) {
		t.Errorf("expected:\n%s\ngot:\n%s", expected, result)
	}
}

func TestTransformTypeMigration(t *testing.T) {
	input := `func process(s: String) -> String:
    return s
`
	expected := `func process(s: str) -> str:
    return s
`

	config := Config{
		Rules: []Rule{
			{
				Kind:    "migrate_type",
				OldName: "String",
				NewName: "str",
			},
		},
	}

	result := transformSource(input, config)
	if normalize(result) != normalize(expected) {
		t.Errorf("expected:\n%s\ngot:\n%s", expected, result)
	}
}

func TestTransformPropertyAccess(t *testing.T) {
	input := `func main():
    var x = obj.old_prop
`
	expected := `func main():
    var x = obj.new_prop
`

	config := Config{
		Rules: []Rule{
			{
				Kind:    "migrate_property",
				OldName: "old_prop",
				NewName: "new_prop",
			},
		},
	}

	result := transformSource(input, config)
	if normalize(result) != normalize(expected) {
		t.Errorf("expected:\n%s\ngot:\n%s", expected, result)
	}
}

func TestTransformModuleFuncCall(t *testing.T) {
	input := `import std.vec

func main():
    var v = new Vec()
    push(v, 42)
`
	expected := `import std.vec

func main():
    var v = new Vec()
    Vec.append(v, 42)
`

	config := Config{
		Rules: []Rule{
			{
				Kind:      "rename_func",
				OldName:   "push",
				NewName:   "append",
				NewModule: "Vec",
			},
		},
	}

	result := transformSource(input, config)
	if normalize(result) != normalize(expected) {
		t.Errorf("expected:\n%s\ngot:\n%s", expected, result)
	}
}

func TestNoChange(t *testing.T) {
	input := `func main():
    return 42
`
	config := Config{}
	result := transformSource(input, config)
	if normalize(result) != normalize(input) {
		t.Errorf("expected no change:\n%s\ngot:\n%s", input, result)
	}
}

func TestMultipleTransformations(t *testing.T) {
	input := `import std.io

func main():
    out("hello")
    out("world")
`

	config := Config{
		Rules: []Rule{
			{
				Kind:      "rename_func",
				OldName:   "out",
				NewName:   "print",
				NewModule: "sys",
			},
		},
	}

	result := transformSource(input, config)
	if !strings.Contains(result, "sys.print") {
		t.Errorf("expected sys.print calls, got:\n%s", result)
	}
}

func TestParserConfig(t *testing.T) {
	tomlData := `[[rule]]
description = "Test rule"
kind = "rename_func"
old_name = "oldFunc"
new_name = "newFunc"
new_module = "newMod"
`
	cfg, err := ParseConfig(tomlData)
	if err != nil {
		t.Fatalf("ParseConfig failed: %v", err)
	}
	if len(cfg.Rules) != 1 {
		t.Fatalf("expected 1 rule, got %d", len(cfg.Rules))
	}
	rule := cfg.Rules[0]
	if rule.Kind != "rename_func" {
		t.Errorf("expected kind rename_func, got %s", rule.Kind)
	}
	if rule.OldName != "oldFunc" {
		t.Errorf("expected old_name oldFunc, got %s", rule.OldName)
	}
	if rule.NewName != "newFunc" {
		t.Errorf("expected new_name newFunc, got %s", rule.NewName)
	}
	if rule.NewModule != "newMod" {
		t.Errorf("expected new_module newMod, got %s", rule.NewModule)
	}
}

func TestFuncDeclBodyTransformation(t *testing.T) {
	input := `func calculate():
    out("start")
    var x = out(42)
    out("end")
`
	expected := `func calculate():
    sys.print("start")
    var x = sys.print(42)
    sys.print("end")
`

	config := Config{
		Rules: []Rule{
			{
				Kind:      "rename_func",
				OldName:   "out",
				NewName:   "print",
				NewModule: "sys",
			},
		},
	}

	result := transformSource(input, config)
	if normalize(result) != normalize(expected) {
		t.Errorf("expected:\n%s\ngot:\n%s", expected, result)
	}
}

func TestBinaryOpExprTransformation(t *testing.T) {
	input := `func add():
    var x = out(1) + out(2)
`
	expected := `func add():
    var x = (sys.print(1) + sys.print(2))
`

	config := Config{
		Rules: []Rule{
			{
				Kind:      "rename_func",
				OldName:   "out",
				NewName:   "print",
				NewModule: "sys",
			},
		},
	}

	result := transformSource(input, config)
	if normalize(result) != normalize(expected) {
		t.Errorf("expected:\n%s\ngot:\n%s", expected, result)
	}
}

// Helper: parse, transform, and print source
func transformSource(source string, config Config) string {
	tokens := lexer.NewLexer(source).TokenizeAll()
	p := parser.NewParser(tokens)
	program := p.ParseProgram()

	transformer := NewTransformer(config)
	transformed := transformer.Transform(program, source)

	printer := NewPrinter()
	return printer.Print(transformed)
}
