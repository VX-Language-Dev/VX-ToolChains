package main

import (
	"flag"
	"fmt"
	"os"
	"path/filepath"
	"strings"

	"github.com/vx-lang/vxc/internal/translator"
)

const version = "vx-translator v2.0 — VXOBJ → LLVM IR → native executable"

func main() {
	outputFile := flag.String("o", "", "Output executable path (required)")
	targetTriple := flag.String("target", "", "Target triple (default: auto-detect from host)")
	llvmPath := flag.String("llvm-path", "", "Custom LLVM bin directory (containing llc, lld)")
	keepIR := flag.Bool("keep-ir", false, "Keep temporary .ll and .o files")
	noLink := flag.Bool("S", false, "Stop after .ll generation (like clang -S)")
	showVer := flag.Bool("v", false, "Print version")
	flag.Usage = func() {
		fmt.Fprintf(os.Stderr, "VX Translator — translate VXOBJ (pure code logic) to native executable\n")
		fmt.Fprintf(os.Stderr, "Pipeline: VXOBJ → LLVM IR → llc (compile) → lld (link)\n")
		fmt.Fprintf(os.Stderr, "VXOBJ contains only pure code logic — platform adaptation is explicit.\n")
		fmt.Fprintf(os.Stderr, "\nUsage: %s -o <output> <input.vxobj> [extra.o ...]\n", os.Args[0])
		fmt.Fprintf(os.Stderr, "\nOptions:\n")
		fmt.Fprintf(os.Stderr, "  -o <path>        Output executable path (required)\n")
		fmt.Fprintf(os.Stderr, "  -target <triple> Target triple (default: auto-detect from host)\n")
		fmt.Fprintf(os.Stderr, "                   Examples: x86_64-linux-gnu, arm64-apple-macosx\n")
		fmt.Fprintf(os.Stderr, "  -llvm-path <dir> Custom LLVM bin directory\n")
		fmt.Fprintf(os.Stderr, "  -keep-ir         Keep temporary .ll and .o files\n")
		fmt.Fprintf(os.Stderr, "  -S               Stop after LLVM IR generation (inspect .ll)\n")
		fmt.Fprintf(os.Stderr, "  -v               Print version\n")
		fmt.Fprintf(os.Stderr, "\nExtra files (.o, .dylib, .a) are passed to lld for linking\n")
	}
	flag.Parse()

	if *showVer {
		fmt.Println(version)
		os.Exit(0)
	}

	if *outputFile == "" || flag.NArg() == 0 {
		flag.Usage()
		os.Exit(1)
	}

	// Separate .vxobj files from extra linker inputs
	var vxobjFiles []string
	var extraInputs []string
	for _, arg := range flag.Args() {
		if strings.HasSuffix(arg, ".vxobj") {
			vxobjFiles = append(vxobjFiles, arg)
		} else {
			extraInputs = append(extraInputs, arg)
		}
	}

	if len(vxobjFiles) == 0 {
		fmt.Fprintf(os.Stderr, "[Error] No .vxobj input files\n")
		os.Exit(1)
	}

	// Create translator (auto-detect host or use explicit target)
	var trans *translator.Translator
	if *targetTriple != "" {
		target, err := translator.TargetFromTriple(*targetTriple)
		if err != nil {
			fmt.Fprintf(os.Stderr, "[Error] %v\n", err)
			os.Exit(1)
		}
		trans = translator.NewTranslator(target)
	} else {
		trans = translator.NewHostTranslator()
		fmt.Fprintf(os.Stderr, "[TRANSLATOR] Auto-detected host: %s (%s)\n",
			trans.Target.Triple, trans.Target.Arch.Name())
	}

	trans.KeepTemp = *keepIR
	trans.LLVMPath = *llvmPath
	trans.NoLink = *noLink

	// Ensure output path is absolute
	outputPath := *outputFile
	if !filepath.IsAbs(outputPath) {
		cwd, _ := os.Getwd()
		outputPath = filepath.Join(cwd, outputPath)
	}

	fmt.Fprintf(os.Stderr, "[TRANSLATOR] Translating %d VXOBJ file(s) → %s\n",
		len(vxobjFiles), outputPath)

	if err := trans.Translate(vxobjFiles, outputPath, extraInputs); err != nil {
		fmt.Fprintf(os.Stderr, "[TRANSLATOR] Failed: %v\n", err)
		os.Exit(1)
	}

	fmt.Fprintf(os.Stderr, "[TRANSLATOR] Done.\n")
}
