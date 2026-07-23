package main

import (
	"flag"
	"fmt"
	"os"
	"path/filepath"
	"strings"

	"github.com/vx-lang/vxc/internal/lexer"
	"github.com/vx-lang/vxc/internal/parser"
	"github.com/vx-lang/vxc/internal/vfmt"
)

const version = "vfmt-go version 1.0.0"

func main() {
	var (
		outputFile  = flag.String("o", "", "输出文件 (默认: stdout)")
		configFile  = flag.String("config", "", "TOML 规则配置文件路径")
		builtin     = flag.Bool("builtin", false, "使用内置迁移规则集")
		listRules   = flag.Bool("list-rules", false, "列出所有可用规则")
		showVer     = flag.Bool("v", false, "打印版本")
		check       = flag.Bool("check", false, "仅检查差异，不修改文件")
		dryRun      = flag.Bool("dry-run", false, "预览更改但不写入文件")
		inplace     = flag.Bool("i", false, "原地修改源文件")
		recursive   = flag.Bool("r", false, "递归处理目录")
		quiet       = flag.Bool("q", false, "静默模式 (不输出解析警告)")
		addImport   = flag.String("add-import", "", "添加缺失的 import 路径")
	)
	flag.Usage = func() {
		fmt.Fprintf(os.Stderr, "用法: %s [选项] <源文件...>\n", os.Args[0])
		fmt.Fprintf(os.Stderr, "\nVX 代码格式化与迁移工具 - 基于 AST 的遗留代码重构\n")
		fmt.Fprintf(os.Stderr, "\n选项:\n")
		flag.PrintDefaults()
		fmt.Fprintf(os.Stderr, "\n示例:\n")
		fmt.Fprintf(os.Stderr, "  %s file.vx                    # 转换并输出到 stdout\n", os.Args[0])
		fmt.Fprintf(os.Stderr, "  %s -i file.vx                 # 原地修改\n", os.Args[0])
		fmt.Fprintf(os.Stderr, "  %s -i -r src/                 # 递归处理目录\n", os.Args[0])
		fmt.Fprintf(os.Stderr, "  %s -builtin -i file.vx        # 使用内置规则原地修改\n", os.Args[0])
		fmt.Fprintf(os.Stderr, "  %s -config rules.toml -i file.vx # 使用自定义规则\n", os.Args[0])
		fmt.Fprintf(os.Stderr, "  %s -check file.vx             # 仅检查 diff\n", os.Args[0])
		fmt.Fprintf(os.Stderr, "  %s -list-rules                # 列出内置规则\n", os.Args[0])
	}
	flag.Parse()

	if *showVer {
		fmt.Println(version)
		os.Exit(0)
	}

	if *listRules {
		showBuiltinRules()
		os.Exit(0)
	}

	args := flag.Args()
	if len(args) == 0 {
		flag.Usage()
		os.Exit(1)
	}

	// Load config
	config := vfmt.LoadDefaultRules()
	if *builtin {
		config = vfmt.LoadBuiltinRules()
	}
	if *configFile != "" {
		data, err := os.ReadFile(*configFile)
		if err != nil {
			fmt.Fprintf(os.Stderr, "读取配置文件失败 %s: %v\n", *configFile, err)
			os.Exit(1)
		}
		parsedCfg, err := vfmt.ParseConfig(string(data))
		if err != nil {
			fmt.Fprintf(os.Stderr, "解析配置文件失败: %v\n", err)
			os.Exit(1)
		}
		if *builtin {
			parsedCfg.Rules = append(config.Rules, parsedCfg.Rules...)
		}
		config = parsedCfg

		if errs := vfmt.ValidateConfig(config); len(errs) > 0 {
			for _, err := range errs {
				fmt.Fprintf(os.Stderr, "配置错误: %s\n", err)
			}
			if !*builtin {
				os.Exit(1)
			}
		}
	}

	// Collect source files
	var files []string
	for _, arg := range args {
		info, err := os.Stat(arg)
		if err != nil {
			fmt.Fprintf(os.Stderr, "无法访问 %s: %v\n", arg, err)
			os.Exit(1)
		}
		if info.IsDir() {
			if !*recursive {
				fmt.Fprintf(os.Stderr, "跳过目录 %s (使用 -r 递归处理)\n", arg)
				continue
			}
			vxFiles, err := findVXFiles(arg)
			if err != nil {
				fmt.Fprintf(os.Stderr, "扫描目录 %s 失败: %v\n", arg, err)
				os.Exit(1)
			}
			files = append(files, vxFiles...)
		} else {
			if !strings.HasSuffix(arg, ".vx") {
				fmt.Fprintf(os.Stderr, "警告: %s 不是 .vx 文件，跳过\n", arg)
				continue
			}
			files = append(files, arg)
		}
	}

	if len(files) == 0 {
		fmt.Fprintln(os.Stderr, "没有找到要处理的 .vx 文件")
		os.Exit(1)
	}

	// Process files
	transformer := vfmt.NewTransformer(config)
	changed := 0
	skipped := 0
	errCount := 0

	for _, file := range files {
		result, err := processFile(file, transformer, *addImport)
		if err != nil {
			fmt.Fprintf(os.Stderr, "处理 %s 失败: %v\n", file, err)
			errCount++
			continue
		}

		// Show parser warnings (only in non-quiet mode)
		if !*quiet && len(result.parseErrors) > 0 {
			for _, parseErr := range result.parseErrors {
				fmt.Fprintf(os.Stderr, "解析警告 (%s): %s\n", file, parseErr)
			}
		}

		if !result.changed {
			skipped++
			if *check && !*quiet {
				fmt.Printf("无需变更: %s\n", file)
			}
			continue
		}

		changed++

		switch {
		case *check:
			fmt.Printf("[需要变更] %s\n", file)

		case *dryRun:
			fmt.Printf("=== %s ===\n", file)
			fmt.Println(result.output)

		case *inplace:
			if err := os.WriteFile(file, []byte(result.output), 0644); err != nil {
				fmt.Fprintf(os.Stderr, "写入 %s 失败: %v\n", file, err)
				errCount++
				continue
			}
			fmt.Printf("已更新: %s\n", file)

		case *outputFile != "":
			if err := os.WriteFile(*outputFile, []byte(result.output), 0644); err != nil {
				fmt.Fprintf(os.Stderr, "写入 %s 失败: %v\n", *outputFile, err)
				errCount++
				continue
			}
			fmt.Printf("已写入: %s -> %s\n", file, *outputFile)

		default:
			// Default: stdout
			fmt.Print(result.output)
		}
	}

	// Summary
	total := len(files)
	if *check {
		fmt.Fprintf(os.Stderr, "\n检查完成: %d 文件, %d 需要变更, %d 无需修改, %d 错误\n",
			total, changed, skipped, errCount)
	} else if !*quiet && !*dryRun && !*inplace && *outputFile == "" {
		// Only show summary when not outputting to stdout
	} else if !*quiet {
		fmt.Fprintf(os.Stderr, "\n处理完成: %d 文件, %d 已转换, %d 无需修改, %d 错误\n",
			total, changed, skipped, errCount)
	}

	if errCount > 0 {
		os.Exit(1)
	}
}

// ==================== File Processing ====================

type fileResult struct {
	original    string
	output      string
	changed     bool
	parseErrors []string
}

func processFile(path string, transformer *vfmt.Transformer, addImport string) (fileResult, error) {
	content, err := os.ReadFile(path)
	if err != nil {
		return fileResult{}, fmt.Errorf("读取失败: %w", err)
	}
	original := string(content)

	// Strip comments before lexing (the Go lexer treats # as macro hash token).
	cleanSource, comments := extractComments(original)

	tokens := lexer.NewLexer(cleanSource).TokenizeAll()
	p := parser.NewParser(tokens)
	program := p.ParseProgram()

	transformed := transformer.Transform(program, original)

	if addImport != "" {
		transformed = vfmt.FindAndAddImport(transformed, addImport)
	}

	printer := vfmt.NewPrinter()
	output := printer.Print(transformed)
	output = insertComments(output, comments)

	return fileResult{
		original:    original,
		output:      output,
		changed:     output != original,
		parseErrors: p.Errors,
	}, nil
}

// ==================== Helpers ====================

// commentInfo stores a comment line and its position in the original source.
type commentInfo struct {
	text   string
	lineNo int // 0-based line number in original source
}

// extractComments removes comment lines (# ...) from VX source,
// replacing them with blank lines to preserve line numbering.
func extractComments(source string) (string, []commentInfo) {
	lines := strings.Split(source, "\n")
	var result []string
	var comments []commentInfo

	for i, line := range lines {
		trimmed := strings.TrimSpace(line)
		if strings.HasPrefix(trimmed, "#") {
			comments = append(comments, commentInfo{text: line, lineNo: i})
			result = append(result, "")
		} else {
			result = append(result, line)
		}
	}

	return strings.Join(result, "\n"), comments
}

// insertComments merges comment lines back into generated code at positions
// approximated from their original line numbers.
func insertComments(generated string, comments []commentInfo) string {
	if len(comments) == 0 {
		return generated
	}

	genLines := strings.Split(generated, "\n")
	totalGen := len(genLines)
	estimatedTotal := totalGen + len(comments)

	var result []string
	ci := 0

	for gi := 0; gi < totalGen; gi++ {
		for ci < len(comments) {
			pos := comments[ci].lineNo * totalGen / max(estimatedTotal, 1)
			if pos < 0 {
				pos = 0
			}
			if pos <= gi {
				result = append(result, comments[ci].text)
				ci++
			} else {
				break
			}
		}
		result = append(result, genLines[gi])
	}

	for ci < len(comments) {
		result = append(result, comments[ci].text)
		ci++
	}

	return strings.Join(result, "\n")
}

func max(a, b int) int {
	if a > b {
		return a
	}
	return b
}

func showDiff(filename, original, transformed string) {
	origLines := strings.Split(original, "\n")
	newLines := strings.Split(transformed, "\n")

	maxLen := len(origLines)
	if len(newLines) > maxLen {
		maxLen = len(newLines)
	}

	for i := 0; i < maxLen; i++ {
		var oldLine, newLine string
		if i < len(origLines) {
			oldLine = origLines[i]
		}
		if i < len(newLines) {
			newLine = newLines[i]
		}
		if oldLine != newLine {
			if oldLine != "" {
				fmt.Printf("  - |%s\n    %s\n", filename, oldLine)
			}
			if newLine != "" {
				fmt.Printf("  + |%s\n    %s\n", filename, newLine)
			}
		}
	}
}

func findVXFiles(dir string) ([]string, error) {
	var files []string
	err := filepath.Walk(dir, func(path string, info os.FileInfo, err error) error {
		if err != nil {
			return err
		}
		if info.IsDir() {
			if strings.HasPrefix(info.Name(), ".") && info.Name() != "." {
				return filepath.SkipDir
			}
			return nil
		}
		if strings.HasSuffix(path, ".vx") {
			files = append(files, path)
		}
		return nil
	})
	return files, err
}

func showBuiltinRules() {
	builtinCfg := vfmt.LoadBuiltinRules()
	fmt.Println("内置迁移规则:")
	fmt.Println(strings.Repeat("=", 72))
	for i, rule := range builtinCfg.Rules {
		fmt.Printf("%2d. [%s] %s\n", i+1, rule.Kind, rule.Description)
		fmt.Printf("    匹配: %s", rule.OldName)
		if rule.OldModule != "" {
			fmt.Printf(" (模块: %s)", rule.OldModule)
		}
		fmt.Printf("\n    替换: ")
		if rule.NewModule != "" {
			fmt.Printf("%s.", rule.NewModule)
		}
		fmt.Printf("%s\n", rule.NewName)
		if i < len(builtinCfg.Rules)-1 {
			fmt.Println()
		}
	}
}
