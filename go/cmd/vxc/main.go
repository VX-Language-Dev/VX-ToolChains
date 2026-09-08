package main

import (
	"flag"
	"fmt"
	"os"
	"path/filepath"
	"time"

	"github.com/vx-lang/vxc/internal/bytecode"
	"github.com/vx-lang/vxc/internal/importer"
	"github.com/vx-lang/vxc/internal/lexer"
	"github.com/vx-lang/vxc/internal/ownership"
	"github.com/vx-lang/vxc/internal/parser"
	"github.com/vx-lang/vxc/internal/semantic"
	"github.com/vx-lang/vxc/internal/typeirgen"
	"github.com/vx-lang/vxc/internal/vxcout"
)

const version = "vxc-go version 1.6.0"

func main() {
	var (
		outputFile       = flag.String("o", "out.vxobj", "Output VXOBJ file")
		target           = flag.String("target", "x86_64-linux-gnu", "Target triple")
		showVer          = flag.Bool("version", false, "Print version")
		verbose          = flag.Bool("v", false, "Verbose output")
		noOwnershipCheck = flag.Bool("no-ownership", false, "Skip ownership check")
	)
	flag.Usage = func() {
		vxcout.Get().PrintHelp()
	}
	flag.Parse()

	out := vxcout.NewVXCOut(os.Stdout, *verbose)
	vxcout.SetGlobal(out)

	if *showVer {
		out.PrintVersion()
		os.Exit(0)
	}

	if flag.NArg() == 0 {
		flag.Usage()
		os.Exit(0)
	}

	sourceFile := flag.Arg(0)
	start := time.Now()

	out.PrintCompiling(sourceFile)

	content := readFile(sourceFile)

	out.PrintStep(1, 6, "lexer")
	tokens := lexer.NewLexer(content).TokenizeAll()

	out.PrintStep(2, 6, "parser")
	p := parser.NewParser(tokens)
	ast := p.ParseProgram()
	if len(p.Errors) > 0 {
		out.PrintError("syntax errors found")
		for _, err := range p.Errors {
			out.PrintError(err)
		}
		os.Exit(1)
	}

	out.PrintStep(3, 6, "import resolution")
	merged, err := importer.Resolve(ast, importer.Options{
		SourceDir:   filepath.Dir(sourceFile),
		SearchRoots: stdSearchRoots(),
		Verbose:     *verbose,
	})
	if err != nil {
		out.PrintError(fmt.Sprintf("import resolution failed: %v", err))
		os.Exit(1)
	}

	out.PrintStep(4, 6, "semantic analysis")
	st := semantic.AnalyzeProgram(merged)
	if len(st.Errors) > 0 {
		out.PrintError("semantic errors found")
		for _, err := range st.Errors {
			out.PrintError(err.Msg)
		}
		os.Exit(1)
	}

	out.PrintStep(5, 6, "ownership check")
	if !*noOwnershipCheck {
		checker := ownership.CheckProgram(merged)
		if len(checker.Errors) > 0 {
			out.PrintError("ownership errors found")
			for _, err := range checker.Errors {
				out.PrintError(err.Msg)
			}
			os.Exit(1)
		}
		for _, w := range checker.Warnings {
			out.PrintWarning(w.Msg)
		}
	}

	out.PrintStep(6, 6, "typeir generation")
	moduleName := filepath.Base(sourceFile)
	module := typeirgen.GenerateIR(merged, moduleName)

	out.PrintStage("bytecode", "serializing VXOBJ")
	obj, err := bytecode.BuildVXOBJ(module, *target, nil)
	if err != nil {
		out.PrintError(fmt.Sprintf("VXOBJ generation failed: %v", err))
		os.Exit(1)
	}

	writeFile(*outputFile, obj)
	out.PrintCompiled(sourceFile, *outputFile, len(obj))
	out.PrintElapsed(start)
}

func readFile(path string) string {
	data, err := os.ReadFile(path)
	if err != nil {
		fmt.Fprintf(os.Stderr, "failed to read %s: %v\n", path, err)
		os.Exit(1)
	}
	return string(data)
}

func writeFile(path string, data []byte) {
	dir := filepath.Dir(path)
	if dir != "." && dir != "" {
		if err := os.MkdirAll(dir, 0755); err != nil {
			fmt.Fprintf(os.Stderr, "failed to create directory %s: %v\n", dir, err)
			os.Exit(1)
		}
	}
	if err := os.WriteFile(path, data, 0644); err != nil {
		fmt.Fprintf(os.Stderr, "failed to write %s: %v\n", path, err)
		os.Exit(1)
	}
}

func stdSearchRoots() []string {
	roots := make([]string, 0, 4)
	if v := os.Getenv("VX_STD"); v != "" {
		roots = append(roots, v)
	}
	if exe, err := os.Executable(); err == nil {
		exeDir := filepath.Dir(exe)
		if d := filepath.Join(exeDir, "..", "std"); dirExists(d) {
			roots = append(roots, d)
		}
		if d := filepath.Join(exeDir, "std"); dirExists(d) {
			roots = append(roots, d)
		}
	}
	return roots
}

func dirExists(p string) bool {
	info, err := os.Stat(p)
	return err == nil && info.IsDir()
}
