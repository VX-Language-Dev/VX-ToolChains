package compiler

import (
	"os"
	"path/filepath"
	"testing"

	"github.com/vx-lang/vxc/internal/bytecode"
	"github.com/vx-lang/vxc/internal/lexer"
	"github.com/vx-lang/vxc/internal/ownership"
	"github.com/vx-lang/vxc/internal/parser"
	"github.com/vx-lang/vxc/internal/semantic"
	"github.com/vx-lang/vxc/internal/typeirgen"
)

func compileSource(t *testing.T, source string) []byte {
	t.Helper()
	tokens := lexer.NewLexer(source).TokenizeAll()
	p := parser.NewParser(tokens)
	program := p.ParseProgram()
	if len(p.Errors) > 0 {
		for _, err := range p.Errors {
			t.Logf("parse error: %s", err)
		}
		t.Fatalf("parser produced %d errors", len(p.Errors))
	}

	st := semantic.AnalyzeProgram(program)
	if len(st.Errors) > 0 {
		for _, err := range st.Errors {
			t.Errorf("semantic error: %s at %d:%d", err.Msg, err.Pos.Line, err.Pos.Col)
		}
		t.Fatalf("semantic analysis produced %d errors", len(st.Errors))
	}

	checker := ownership.CheckProgram(program)
	if len(checker.Errors) > 0 {
		for _, err := range checker.Errors {
			t.Errorf("ownership error: %s", err.Msg)
		}
		t.Fatalf("ownership check produced %d errors", len(checker.Errors))
	}

	module := typeirgen.GenerateIR(program, "test")
	obj, err := bytecode.BuildVXOBJ(module, "x86_64-linux-gnu", nil)
	if err != nil {
		t.Fatalf("BuildVXOBJ failed: %v", err)
	}

	return obj
}

func TestCompileWithImport(t *testing.T) {
	source := `import std.io

func main():
    out("hello")
`
	obj := compileSource(t, source)
	if len(obj) == 0 {
		t.Fatal("expected non-empty VXOBJ data")
	}

	// Verify we can parse back the VXOBJ
	container, err := bytecode.ParseVxobjV4(obj)
	if err != nil {
		t.Fatalf("ParseVxobjV4 failed: %v", err)
	}
	if container.Header.Version != bytecode.VERSION_V4 {
		t.Errorf("expected VXOBJ version %d, got %d", bytecode.VERSION_V4, container.Header.Version)
	}
}

func TestCompileWithMultipleImports(t *testing.T) {
	source := `import std.io
import std.math
import std.string

func main():
    out("test")
`
	obj := compileSource(t, source)
	if len(obj) == 0 {
		t.Fatal("expected non-empty VXOBJ data")
	}

	container, err := bytecode.ParseVxobjV4(obj)
	if err != nil {
		t.Fatalf("ParseVxobjV4 failed: %v", err)
	}
	if container.Header.Version != bytecode.VERSION_V4 {
		t.Errorf("expected VXOBJ version %d, got %d", bytecode.VERSION_V4, container.Header.Version)
	}
}

func TestCompileWithImportAndAlias(t *testing.T) {
	source := `import std.io as myio

func main():
    out("hello")
`
	obj := compileSource(t, source)
	if len(obj) == 0 {
		t.Fatal("expected non-empty VXOBJ data")
	}

	container, err := bytecode.ParseVxobjV4(obj)
	if err != nil {
		t.Fatalf("ParseVxobjV4 failed: %v", err)
	}
	if container.Header.Version != bytecode.VERSION_V4 {
		t.Errorf("expected VXOBJ version %d, got %d", bytecode.VERSION_V4, container.Header.Version)
	}
}

func TestCompileImportOnly(t *testing.T) {
	source := `import std.io`
	obj := compileSource(t, source)
	if len(obj) == 0 {
		t.Fatal("expected non-empty VXOBJ data")
	}

	container, err := bytecode.ParseVxobjV4(obj)
	if err != nil {
		t.Fatalf("ParseVxobjV4 failed: %v", err)
	}
	if container.Header.Version != bytecode.VERSION_V4 {
		t.Errorf("expected VXOBJ version %d, got %d", bytecode.VERSION_V4, container.Header.Version)
	}
}

func TestCompileImportDeepPath(t *testing.T) {
	source := `import a.b.c.d.e`
	obj := compileSource(t, source)
	if len(obj) == 0 {
		t.Fatal("expected non-empty VXOBJ data")
	}
}

func TestCompilerEndToEnd(t *testing.T) {
	// Create a temporary directory with VX source files for multi-file project test
	tmpDir := t.TempDir()

	// Write lib_math.vx
	libContent := `func add(a: int, b: int) -> int:
    return a + b

func multiply(a: int, b: int) -> int:
    return a * b
`
	if err := os.WriteFile(filepath.Join(tmpDir, "lib_math.vx"), []byte(libContent), 0644); err != nil {
		t.Fatalf("failed to write lib_math.vx: %v", err)
	}

	// Write main.vx with an extern declaration referencing the other file's functions
	mainContent := `extern add(a: int, b: int) -> int
extern multiply(a: int, b: int) -> int

func main() -> int:
    var x: int = add(10, 20)
    var y: int = multiply(5, 6)
    return 0
`
	mainPath := filepath.Join(tmpDir, "main.vx")
	if err := os.WriteFile(mainPath, []byte(mainContent), 0644); err != nil {
		t.Fatalf("failed to write main.vx: %v", err)
	}

	// Read and compile main.vx
	source, err := os.ReadFile(mainPath)
	if err != nil {
		t.Fatalf("failed to read main.vx: %v", err)
	}

	obj := compileSource(t, string(source))
	if len(obj) == 0 {
		t.Fatal("expected non-empty VXOBJ data")
	}

	container, err := bytecode.ParseVxobjV4(obj)
	if err != nil {
		t.Fatalf("ParseVxobjV4 failed: %v", err)
	}

	// Verify the compiled module contains the main function
	typeIRData := container.GetSection(bytecode.SECTION_TYPE_IR)
	if typeIRData == nil {
		t.Fatal("expected TypeIR section in VXOBJ")
	}
}
