package importer

import (
	"os"
	"path/filepath"
	"testing"

	"github.com/vx-lang/vxc/internal/ast"
	"github.com/vx-lang/vxc/internal/lexer"
	"github.com/vx-lang/vxc/internal/parser"
)

func writeFile(t *testing.T, dir, name, content string) string {
	t.Helper()
	p := filepath.Join(dir, name)
	if err := os.WriteFile(p, []byte(content), 0644); err != nil {
		t.Fatalf("write %s: %v", p, err)
	}
	return p
}

func parse(t *testing.T, src string) []ast.Expr {
	t.Helper()
	tokens := lexer.NewLexer(src).TokenizeAll()
	p := parser.NewParser(tokens)
	prog := p.ParseProgram()
	if len(p.Errors) > 0 {
		t.Fatalf("parse: %v", p.Errors)
	}
	return prog
}

func countKind(prog []ast.Expr, name string) int {
	n := 0
	for _, e := range prog {
		if fd, ok := e.(*ast.FuncDecl); ok && fd.Name == name {
			n++
		}
	}
	return n
}

func TestResolveSingleImport(t *testing.T) {
	dir := t.TempDir()
	writeFile(t, dir, "lib.vx", "func helper(a: int) -> int:\n    return a\n")
	main := writeFile(t, dir, "main.vx", "import lib\n\nfunc main() -> i32:\n    return helper(1)\n")

	prog := parse(t, read(t, main))
	merged, err := Resolve(prog, Options{SourceDir: dir})
	if err != nil {
		t.Fatalf("Resolve: %v", err)
	}
	if countKind(merged, "helper") != 1 {
		t.Fatalf("expected helper merged once, got %d", countKind(merged, "helper"))
	}
	if countKind(merged, "main") != 1 {
		t.Fatalf("expected main preserved")
	}
	for _, e := range merged {
		if _, ok := e.(*ast.ImportStmt); ok {
			t.Fatalf("import statement was not consumed")
		}
	}
}

func TestResolveRecursiveAndDedup(t *testing.T) {
	dir := t.TempDir()
	writeFile(t, dir, "a.vx", "func afunc() -> int:\n    return 1\nimport b\n")
	writeFile(t, dir, "b.vx", "func bfunc() -> int:\n    return 2\n")
	main := writeFile(t, dir, "main.vx", "import a\nimport b\n\nfunc main() -> i32:\n    return 0\n")

	prog := parse(t, read(t, main))
	merged, err := Resolve(prog, Options{SourceDir: dir})
	if err != nil {
		t.Fatalf("Resolve: %v", err)
	}
	if countKind(merged, "afunc") != 1 {
		t.Fatalf("afunc expected once, got %d", countKind(merged, "afunc"))
	}
	if countKind(merged, "bfunc") != 1 {
		t.Fatalf("bfunc expected once (dedup), got %d", countKind(merged, "bfunc"))
	}
}

func TestResolveCycle(t *testing.T) {
	dir := t.TempDir()
	writeFile(t, dir, "x.vx", "func xfunc() -> int:\n    return 1\nimport y\n")
	writeFile(t, dir, "y.vx", "func yfunc() -> int:\n    return 2\nimport x\n")
	main := writeFile(t, dir, "main.vx", "import x\n\nfunc main() -> i32:\n    return 0\n")

	prog := parse(t, read(t, main))
	merged, err := Resolve(prog, Options{SourceDir: dir})
	if err != nil {
		t.Fatalf("Resolve: %v", err)
	}
	if countKind(merged, "xfunc") != 1 || countKind(merged, "yfunc") != 1 {
		t.Fatalf("cycle not terminated correctly: %v", merged)
	}
}

func TestResolveUnresolvableKeepsGoing(t *testing.T) {
	dir := t.TempDir()
	main := writeFile(t, dir, "main.vx", "import nonexistent.module\n\nfunc main() -> i32:\n    return 0\n")

	prog := parse(t, read(t, main))
	merged, err := Resolve(prog, Options{SourceDir: dir})
	if err != nil {
		t.Fatalf("Resolve should not fail on missing import: %v", err)
	}
	if countKind(merged, "main") != 1 {
		t.Fatalf("main must be preserved")
	}
}

func read(t *testing.T, path string) string {
	t.Helper()
	b, err := os.ReadFile(path)
	if err != nil {
		t.Fatalf("read %s: %v", path, err)
	}
	return string(b)
}
