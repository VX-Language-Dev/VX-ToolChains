package semantic

import (
	"testing"

	"github.com/vx-lang/vxc/internal/lexer"
	"github.com/vx-lang/vxc/internal/parser"
)

func analyzeSource(t *testing.T, source string) (*SemanticResult, error) {
	t.Helper()
	tokens := lexer.NewLexer(source).TokenizeAll()
	p := parser.NewParser(tokens)
	program := p.ParseProgram()

	if len(p.Errors) > 0 {
		return nil, &ParseError{Errors: p.Errors}
	}

	return AnalyzeProgram(program), nil
}

func TestAnalyzeSimpleProgram(t *testing.T) {
	source := `func main():
    out("hello")
`
	result, err := analyzeSource(t, source)
	if err != nil {
		t.Fatalf("analysis failed: %v", err)
	}

	if len(result.Errors) > 0 {
		t.Fatalf("semantic errors: %v", result.Errors)
	}

	if len(result.Functions) != 1 {
		t.Fatalf("expected 1 function, got %d", len(result.Functions))
	}

	mainFunc := result.Functions[0]
	if mainFunc.Name != "main" {
		t.Errorf("expected function name 'main', got '%s'", mainFunc.Name)
	}
}

func TestAnalyzeVariableScope(t *testing.T) {
	source := `func test_scope():
    x: int = 10
    if true:
        y: int = x + 1
        out(y)
    out(x)
`
	result, err := analyzeSource(t, source)
	if err != nil {
		t.Fatalf("analysis failed: %v", err)
	}

	if len(result.Errors) > 0 {
		t.Fatalf("semantic errors: %v", result.Errors)
	}
}

func TestAnalyzeUndefinedVariable(t *testing.T) {
	source := `func undefined_var():
    out(undefined_name)
`
	result, _ := analyzeSource(t, source)

	foundError := false
	for _, err := range result.Errors {
		if contains(err.Msg, "undefined") || contains(err.Msg, "not found") || contains(err.Msg, "undeclared") {
			foundError = true
			break
		}
	}

	if !foundError {
		t.Error("expected undefined variable error")
	}
}

func TestAnalyzeTypeMismatch(t *testing.T) {
	source := `func type_mismatch():
    x: int = "string"
`
	result, _ := analyzeSource(t, source)

	if len(result.Errors) == 0 {
		t.Fatal("expected type mismatch error")
	}
}

func TestAnalyzeFunctionCall(t *testing.T) {
	source := `func add(a: int, b: int) -> int:
    return a + b

func call_func():
    result: int = add(1, 2)
    out(result)
`
	result, err := analyzeSource(t, source)
	if err != nil {
		t.Fatalf("analysis failed: %v", err)
	}

	if len(result.Errors) > 0 {
		t.Fatalf("unexpected semantic errors: %v", result.Errors)
	}

	if len(result.Functions) != 2 {
		t.Fatalf("expected 2 functions, got %d", len(result.Functions))
	}
}

func TestAnalyzeReturnTypes(t *testing.T) {
	source := `func returns_int() -> int:
    return 42

func returns_string() -> string:
    return "hello"

func wrong_return() -> int:
    return "wrong"
`
	result, _ := analyzeSource(t, source)

	foundTypeError := false
	for _, err := range result.Errors {
		if contains(err.Msg, "type") && contains(err.Msg, "return") {
			foundTypeError = true
			break
		}
	}

	if !foundTypeError {
		t.Error("expected return type mismatch error")
	}
}

func TestAnalyzeStructUsage(t *testing.T) {
	source := `struct Point:
    x: float
    y: float

func use_struct():
    p: Point = new Point()
    p.x = 1.0
    p.y = 2.0
    out(p.x)
`
	result, err := analyzeSource(t, source)
	if err != nil {
		t.Fatalf("analysis failed: %v", err)
	}

	if len(result.Errors) > 0 {
		t.Fatalf("unexpected semantic errors: %v", result.Errors)
	}

	if len(result.Structs) != 1 {
		t.Fatalf("expected 1 struct, got %d", len(result.Structs))
	}

	pointStruct := result.Structs[0]
	if pointStruct.Name != "Point" {
		t.Errorf("expected struct name 'Point', got '%s'", pointStruct.Name)
	}
}

func TestAnalyzeClassMethods(t *testing.T) {
	source := `class Counter:
    count: int

    func new() -> Counter:
        var c: Counter = new Counter()
        c.count = 0
        return c
    
    func increment():
        self.count = self.count + 1
    
    func get_count() -> int:
        return self.count

func use_class():
    c: Counter = Counter.new()
    c.increment()
    val: int = c.get_count()
`
	result, err := analyzeSource(t, source)
	if err != nil {
		t.Fatalf("analysis failed: %v", err)
	}

	if len(result.Classes) != 1 {
		t.Fatalf("expected 1 class, got %d", len(result.Classes))
	}

	counterClass := result.Classes[0]
	if counterClass.Name != "Counter" {
		t.Errorf("expected class name 'Counter', got '%s'", counterClass.Name)
	}

	if len(counterClass.Methods) < 2 {
		t.Errorf("expected at least 2 methods, got %d", len(counterClass.Methods))
	}
}

func TestAnalyzeImportResolution(t *testing.T) {
	source := `import std.io
import std.math as math

func use_imports():
    out("test")
    val: float = math.sqrt(16.0)
`
	result, err := analyzeSource(t, source)
	if err != nil {
		t.Fatalf("analysis failed: %v", err)
	}

	if len(result.Imports) != 2 {
		t.Fatalf("expected 2 imports, got %d", len(result.Imports))
	}
}

func TestAnalyzeControlFlow(t *testing.T) {
	source := `func control_flow(x: int):
    if x > 0:
        out("positive")
    elif x < 0:
        out("negative")
    else:
        out("zero")
    
    for i in range(10):
        out(i)
    
    while x > 0:
        x = x - 1
`
	result, err := analyzeSource(t, source)
	if err != nil {
		t.Fatalf("analysis failed: %v", err)
	}

	if len(result.Errors) > 0 {
		t.Fatalf("unexpected semantic errors: %v", result.Errors)
	}
}

func TestAnalyzeRecursiveFunction(t *testing.T) {
	source := `func factorial(n: int) -> int:
    if n <= 1:
        return 1
    return n * factorial(n - 1)

func use_recursive():
    result: int = factorial(5)
    out(result)
`
	result, err := analyzeSource(t, source)
	if err != nil {
		t.Fatalf("analysis failed: %v", err)
	}

	if len(result.Errors) > 0 {
		t.Fatalf("unexpected semantic errors: %v", result.Errors)
	}
}

func TestAnalyzeMultipleFiles(t *testing.T) {
	libSource := `func add(a: int, b: int) -> int:
    return a + b

func multiply(a: int, b: int) -> int:
    return a * b
`

	mainSource := `extern add(a: int, b: int) -> int
extern multiply(a: int, b: int) -> int

func main() -> int:
    sum: int = add(10, 20)
    product: int = multiply(5, 6)
    return 0
`

	libResult, err := analyzeSource(t, libSource)
	if err != nil {
		t.Fatalf("lib analysis failed: %v", err)
	}

	mainResult, err := analyzeSource(t, mainSource)
	if err != nil {
		t.Fatalf("main analysis failed: %v", err)
	}

	if len(libResult.Functions) != 2 {
		t.Fatalf("lib expected 2 functions, got %d", len(libResult.Functions))
	}

	if len(mainResult.ExternDecls) != 2 {
		t.Fatalf("main expected 2 extern declarations, got %d", len(mainResult.ExternDecls))
	}
}

func contains(s string, substr string) bool {
	return len(s) >= len(substr) && (s == substr || len(s) > 0 && containsHelper(s, substr))
}

func containsHelper(s string, substr string) bool {
	for i := 0; i <= len(s)-len(substr); i++ {
		if s[i:i+len(substr)] == substr {
			return true
		}
	}
	return false
}
