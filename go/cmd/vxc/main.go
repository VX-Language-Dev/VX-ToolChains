package main

import (
	"flag"
	"fmt"
	"os"
	"path/filepath"

	"github.com/vx-lang/vxc/internal/bytecode"
	"github.com/vx-lang/vxc/internal/importer"
	"github.com/vx-lang/vxc/internal/lexer"
	"github.com/vx-lang/vxc/internal/ownership"
	"github.com/vx-lang/vxc/internal/parser"
	"github.com/vx-lang/vxc/internal/semantic"
	"github.com/vx-lang/vxc/internal/typeirgen"
)

const version = "vxc-go version 1.6.0"

func main() {
	var (
		outputFile       = flag.String("o", "out.vxobj", "输出文件名")
		target           = flag.String("target", "x86_64-linux-gnu", "目标 triple")
		showVer          = flag.Bool("v", false, "打印版本")
		noOwnershipCheck = flag.Bool("no-ownership", false, "跳过所有权检查")
	)
	flag.Usage = func() {
		fmt.Fprintf(os.Stderr, "用法: %s [选项] <源文件>\n", os.Args[0])
		flag.PrintDefaults()
	}
	flag.Parse()

	if *showVer {
		fmt.Println(version)
		os.Exit(0)
	}

	if flag.NArg() == 0 {
		flag.Usage()
		os.Exit(1)
	}

	sourceFile := flag.Arg(0)
	content := readFile(sourceFile)

	// 阶段 1: Lexing
	tokens := lexer.NewLexer(content).TokenizeAll()

	// 阶段 2: Parsing
	p := parser.NewParser(tokens)
	ast := p.ParseProgram()
	if len(p.Errors) > 0 {
		fmt.Fprintln(os.Stderr, "语法错误:")
		for _, err := range p.Errors {
			fmt.Fprintln(os.Stderr, err)
		}
		os.Exit(1)
	}

	// 阶段 2.5: 模块导入解析
	// 将 `import` 语句展开为被引用模块的全部顶层声明（递归、去重、循环检测）。
	merged, err := importer.Resolve(ast, importer.Options{
		SourceDir:   filepath.Dir(sourceFile),
		SearchRoots: stdSearchRoots(),
		Verbose:     true,
	})
	if err != nil {
		fmt.Fprintf(os.Stderr, "导入解析失败: %v\n", err)
		os.Exit(1)
	}

	// 阶段 3: Semantic Analysis
	st := semantic.AnalyzeProgram(merged)
	if len(st.Errors) > 0 {
		fmt.Fprintln(os.Stderr, "语义错误:")
		for _, err := range st.Errors {
			fmt.Fprintf(os.Stderr, "%s\n", err.Msg)
		}
		os.Exit(1)
	}

	// 阶段 4: Ownership Check (可跳过)
	if !*noOwnershipCheck {
		checker := ownership.CheckProgram(merged)
		if len(checker.Errors) > 0 {
			fmt.Fprintln(os.Stderr, "所有权错误:")
			for _, err := range checker.Errors {
				fmt.Fprintf(os.Stderr, "%s\n", err.Msg)
			}
			os.Exit(1)
		}
		for _, w := range checker.Warnings {
			fmt.Fprintf(os.Stderr, "警告: %s\n", w.Msg)
		}
	}

	// 阶段 5: TypeIR Generation
	moduleName := filepath.Base(sourceFile)
	module := typeirgen.GenerateIR(merged, moduleName)

	// 阶段 6: VXOBJ Serialization
	obj, err := bytecode.BuildVXOBJ(module, *target, nil)
	if err != nil {
		fmt.Fprintf(os.Stderr, "生成目标文件失败: %v\n", err)
		os.Exit(1)
	}

	writeFile(*outputFile, obj)
	fmt.Printf("编译成功: %s -> %s\n", sourceFile, *outputFile)
}

func readFile(path string) string {
	data, err := os.ReadFile(path)
	if err != nil {
		fmt.Fprintf(os.Stderr, "读取文件失败 %s: %v\n", path, err)
		os.Exit(1)
	}
	return string(data)
}

func writeFile(path string, data []byte) {
	if err := os.WriteFile(path, data, 0644); err != nil {
		fmt.Fprintf(os.Stderr, "写入文件失败 %s: %v\n", path, err)
		os.Exit(1)
	}
}

// stdSearchRoots returns candidate directories where the VX standard library
// (the `std` package tree) may live, so `import std.xxx` resolves regardless of
// the current working directory.
func stdSearchRoots() []string {
	roots := make([]string, 0, 4)
	if v := os.Getenv("VX_STD"); v != "" {
		roots = append(roots, v)
	}
	if exe, err := os.Executable(); err == nil {
		exeDir := filepath.Dir(exe)
		// go/bin/vxc -> repo root std
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
