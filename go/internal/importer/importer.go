// Package importer implements VX module import resolution.
//
// A VX source file may declare dependencies on other modules with top-level
// `import` statements, e.g.:
//
//	import std.error
//	import std.collections.vec
//
// The path is a dotted module name that maps to a file on disk by replacing
// the dots with path separators and appending the ".vx" extension
// (e.g. `std.collections.vec` -> `std/collections/vec.vx`).
//
// Resolve merges every imported module's top-level declarations (functions,
// externs, globals, structs, classes, enums, unions) into the importing
// program so that the rest of the pipeline (semantic analysis, typeirgen,
// ...) sees a single, unified compilation unit. Imports are resolved
// recursively and cycle detection prevents infinite loops.
package importer

import (
	"fmt"
	"os"
	"path/filepath"
	"strings"

	"github.com/vx-lang/vxc/internal/ast"
	"github.com/vx-lang/vxc/internal/lexer"
	"github.com/vx-lang/vxc/internal/parser"
)

// Options configures import resolution.
type Options struct {
	// SourceDir is the directory of the file that triggered the import. It is
	// used as the first candidate location for resolving relative imports.
	SourceDir string
	// SearchRoots are additional directories searched (in order) when a path
	// cannot be resolved relative to SourceDir or the working directory. The
	// conventional std library root ("std") is always appended automatically.
	SearchRoots []string
	// Verbose enables diagnostic messages on stderr.
	Verbose bool
}

// Resolve rewrites program so that every top-level ImportStmt is expanded into
// the declarations of the referenced module. Imported modules are loaded and
// parsed recursively. The returned slice is ready to be handed to the semantic
// analyzer / typeirgen.
//
// Resolution failures are non-fatal: an unresolvable import simply produces a
// warning so the compiler can still attempt to compile what it has (the
// translator will later surface any genuinely missing symbols).
func Resolve(program []ast.Expr, opts Options) ([]ast.Expr, error) {
	r := &resolver{
		opts:    opts,
		visited: make(map[string]bool),
		seen:    make(map[string]bool),
	}
	return r.merge(program)
}

type resolver struct {
	opts    Options
	visited map[string]bool // canonical import paths already being expanded
	seen    map[string]bool // declaration keys already merged (dedup)
	out     []ast.Expr
}

// merge flattens a program: imports are expanded in place and every non-import
// declaration is appended (deduplicated by a stable key).
func (r *resolver) merge(program []ast.Expr) ([]ast.Expr, error) {
	for _, e := range program {
		imp, ok := e.(*ast.ImportStmt)
		if !ok {
			r.append(e)
			continue
		}
		if err := r.expandImport(imp); err != nil {
			return nil, err
		}
	}
	return r.out, nil
}

// expandImport loads a single module and appends its merged declarations.
func (r *resolver) expandImport(imp *ast.ImportStmt) error {
	path := imp.Path
	if path == "" {
		return nil
	}
	if r.visited[path] {
		return nil
	}
	r.visited[path] = true

	resolved := r.resolveImportPath(path)
	if resolved == "" {
		if r.opts.Verbose {
			fmt.Fprintf(os.Stderr, "[import] warning: cannot resolve module %q (searched std library and relative paths); treating as external.\n", path)
		}
		return nil
	}

	src, err := os.ReadFile(resolved)
	if err != nil {
		if r.opts.Verbose {
			fmt.Fprintf(os.Stderr, "[import] warning: cannot read %q: %v\n", resolved, err)
		}
		return nil
	}

	tokens := lexer.NewLexer(string(src)).TokenizeAll()
	p := parser.NewParser(tokens)
	sub := p.ParseProgram()
	if len(p.Errors) > 0 {
		if r.opts.Verbose {
			fmt.Fprintf(os.Stderr, "[import] warning: parse error in %q: %v\n", resolved, p.Errors)
		}
		return nil
	}

	// Recursively merge the imported module's declarations. Transitive imports
	// are fully expanded before this module's own decls are appended.
	for _, e := range sub {
		if subImp, ok := e.(*ast.ImportStmt); ok {
			if err := r.expandImport(subImp); err != nil {
				return err
			}
			continue
		}
		r.append(e)
	}
	return nil
}

// append appends e unless an identical declaration key was already seen.
func (r *resolver) append(e ast.Expr) {
	if k := DeclKey(e); k != "" {
		if r.seen[k] {
			return
		}
		r.seen[k] = true
	}
	r.out = append(r.out, e)
}

// resolveImportPath maps a dotted module name to a file path, returning "" if
// no candidate exists.
func (r *resolver) resolveImportPath(importPath string) string {
	rel := strings.ReplaceAll(importPath, ".", string(filepath.Separator)) + ".vx"

	candidates := make([]string, 0, 8)
	if r.opts.SourceDir != "" {
		candidates = append(candidates, filepath.Join(r.opts.SourceDir, rel))
	}
	candidates = append(candidates, rel)
	// When a search root already points at the std library directory, the
	// import path's leading "std/" must be stripped to avoid double prefixing.
	for _, root := range r.opts.SearchRoots {
		if filepath.Base(root) == "std" && strings.HasPrefix(rel, "std"+string(filepath.Separator)) {
			candidates = append(candidates, filepath.Join(root, strings.TrimPrefix(rel, "std"+string(filepath.Separator))))
		} else {
			candidates = append(candidates, filepath.Join(root, rel))
		}
	}
	candidates = append(candidates, filepath.Join("std", rel))
	candidates = append(candidates, filepath.Join("..", rel))
	if dir := filepath.Dir(rel); dir != "." && dir != "" {
		candidates = append(candidates, filepath.Join("std", dir, "mod.vx"))
		candidates = append(candidates, filepath.Join(dir, "mod.vx"))
		if r.opts.SourceDir != "" {
			candidates = append(candidates, filepath.Join(r.opts.SourceDir, dir, "mod.vx"))
		}
	}

	for _, c := range candidates {
		if _, err := os.Stat(c); err == nil {
			return c
		}
	}
	return ""
}

// DeclKey returns a stable dedup key for a top-level declaration.
func DeclKey(e ast.Expr) string {
	switch d := e.(type) {
	case *ast.FuncDecl:
		return "func:" + d.Name
	case *ast.ExternDecl:
		return "extern:" + d.Name
	case *ast.VarDecl:
		return "var:" + d.Name
	case *ast.StructDecl:
		return "struct:" + d.Name
	case *ast.ClassDecl:
		return "class:" + d.Name
	case *ast.EnumDecl:
		return "enum:" + d.Name
	case *ast.UnionDecl:
		return "union:" + d.Name
	case *ast.ImportStmt:
		return "import:" + d.Path
	}
	return ""
}
