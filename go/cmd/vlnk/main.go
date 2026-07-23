package main

import (
	"flag"
	"fmt"
	"os"
	"os/exec"
	"path/filepath"
	"strings"

	"github.com/vx-lang/vxc/internal/bytecode"
	"github.com/vx-lang/vxc/internal/llvmir"
	"github.com/vx-lang/vxc/internal/typeir"
)

func main() {
	outputFile := flag.String("o", "", "Output executable path")
	keepIR := flag.Bool("keep-ir", false, "Keep temporary .ll and .o files")
	flag.Usage = func() {
		fmt.Fprintf(os.Stderr, "VX Linker v1.0 - Link VXOBJ into native executable via LLVM IR + LLD\n")
		fmt.Fprintf(os.Stderr, "Usage: %s -o <output> <input.vxobj> [extra.o ...]\n", os.Args[0])
		fmt.Fprintf(os.Stderr, "\nOptions:\n")
		fmt.Fprintf(os.Stderr, "  -o <path>       Output executable path (required)\n")
		fmt.Fprintf(os.Stderr, "  --keep-ir       Keep temporary .ll and .o files\n")
		fmt.Fprintf(os.Stderr, "Extra files (.o, .dylib, .c, .a) are passed to clang for linking\n")
	}
	flag.Parse()

	if *outputFile == "" || flag.NArg() == 0 {
		flag.Usage()
		os.Exit(1)
	}

	// Separate .vxobj files from extra linker inputs
	var vxobjFiles []string
	var extraLinkInputs []string
	for _, arg := range flag.Args() {
		if strings.HasSuffix(arg, ".vxobj") {
			vxobjFiles = append(vxobjFiles, arg)
		} else {
			extraLinkInputs = append(extraLinkInputs, arg)
		}
	}

	if len(vxobjFiles) == 0 {
		fmt.Fprintf(os.Stderr, "[Error] No .vxobj input files\n")
		os.Exit(1)
	}

	// Read and parse all VXOBJ files
	var modules []*typeir.TypeModule
	var targetTriple string

	for i, input := range vxobjFiles {
		raw, err := os.ReadFile(input)
		if err != nil {
			fmt.Fprintf(os.Stderr, "[Error] Failed to read '%s': %v\n", input, err)
			os.Exit(1)
		}

		container, err := bytecode.ParseVxobjV4(raw)
		if err != nil {
			fmt.Fprintf(os.Stderr, "[Error] Failed to parse VXOBJ: %v\n", err)
			os.Exit(1)
		}

		if i == 0 {
			targetTriple = container.Header.TargetTriple
			fmt.Fprintf(os.Stderr, "Target: %s\n", targetTriple)
		}

		typeIRData := container.GetSection(bytecode.SECTION_TYPE_IR)
		if typeIRData == nil {
			fmt.Fprintf(os.Stderr, "[Error] No TypeIR section found in '%s'\n", input)
			os.Exit(1)
		}

		module, err := typeir.DeserializeTypeModule(typeIRData)
		if err != nil {
			fmt.Fprintf(os.Stderr, "[Error] Failed to deserialize TypeIR from '%s': %v\n", input, err)
			os.Exit(1)
		}

		fmt.Fprintf(os.Stderr, "  [%d] %s: %d functions\n", i, input, len(module.Functions))
		modules = append(modules, module)
	}

	// Merge all modules into one
	module := mergeModules(modules)
	fmt.Fprintf(os.Stderr, "Total merged: %d functions\n", len(module.Functions))

	// Generate LLVM IR
	llvmIR, err := llvmir.Generate(module, targetTriple)
	if err != nil {
		fmt.Fprintf(os.Stderr, "[Error] LLVM IR generation failed: %v\n", err)
		os.Exit(1)
	}

	// Create temp directory
	tmpDir, err := os.MkdirTemp("", "vxlnk_*")
	if err != nil {
		fmt.Fprintf(os.Stderr, "[Error] Failed to create temp dir: %v\n", err)
		os.Exit(1)
	}
	if !*keepIR {
		defer os.RemoveAll(tmpDir)
	} else {
		fmt.Fprintf(os.Stderr, "Temp files: %s\n", tmpDir)
	}

	base := filepath.Join(tmpDir, "output")

	// Write .ll file
	llPath := base + ".ll"
	if err := os.WriteFile(llPath, []byte(llvmIR), 0644); err != nil {
		fmt.Fprintf(os.Stderr, "[Error] Failed to write .ll: %v\n", err)
		os.Exit(1)
	}

	// Call clang to compile .ll → executable
	// clang handles both LLVM IR compilation and linking in one step
	fmt.Fprintf(os.Stderr, "Compiling with clang...\n")
	if err := runClang(llPath, *outputFile, targetTriple, extraLinkInputs); err != nil {
		fmt.Fprintf(os.Stderr, "[Error] clang failed: %v\n", err)
		os.Exit(1)
	}

	// Make executable
	os.Chmod(*outputFile, 0755)

	stat, _ := os.Stat(*outputFile)
	fmt.Fprintf(os.Stderr, "Output: %s (%d bytes)\n", *outputFile, stat.Size())
}

func runClang(llPath, outputPath, targetTriple string, extraInputs []string) error {
	binary := findBinary("clang", "clang-19", "clang-18", "clang-17")
	if binary == "" {
		return fmt.Errorf("clang not found in PATH (install Clang/LLVM)")
	}

	arch := archFromTriple(targetTriple)
	var args []string

	if strings.Contains(targetTriple, "apple") || strings.Contains(targetTriple, "darwin") || strings.Contains(targetTriple, "macos") {
		// macOS: use -Wl to handle SDK path issues
		args = []string{
			llPath,
			"-o", outputPath,
			"-O2",
		}
		// Try to find macOS SDK
		if sdk, err := findMacOSSDK(); err == nil {
			args = append(args, "-isysroot", sdk)
		}
	} else if strings.Contains(targetTriple, "windows") || strings.Contains(targetTriple, "mingw") || strings.Contains(targetTriple, "msvc") {
		args = []string{
			llPath,
			"-o", outputPath,
			"-O2",
		}
	} else {
		// Linux/ELF: add minimal startup
		args = []string{
			llPath,
			"-o", outputPath,
			"-O2",
			"-nostartfiles", // don't link crt by default
			"-Wl,-e,main",   // use main as entry point
		}
		if arch == "aarch64" || arch == "arm64" {
			args = append(args, "-target", "aarch64-linux-gnu")
		} else {
			args = append(args, "-target", "x86_64-linux-gnu")
		}
	}

	// Append extra linker inputs (.o, .dylib, .c, .a, etc.)
	args = append(args, extraInputs...)

	cmd := exec.Command(binary, args...)
	cmd.Stderr = os.Stderr
	cmd.Stdout = os.Stdout
	return cmd.Run()
}

func findBinary(names ...string) string {
	for _, name := range names {
		path, err := exec.LookPath(name)
		if err == nil {
			return path
		}
	}
	return ""
}

func archFromTriple(triple string) string {
	parts := strings.Split(triple, "-")
	if len(parts) == 0 {
		return ""
	}
	return parts[0]
}

func findMacOSSDK() (string, error) {
	// Try common Xcode SDK paths
	paths := []string{
		"/Applications/Xcode.app/Contents/Developer/Platforms/MacOSX.platform/Developer/SDKs/MacOSX.sdk",
		"/Library/Developer/CommandLineTools/SDKs/MacOSX.sdk",
	}
	for _, p := range paths {
		if _, err := os.Stat(p); err == nil {
			return p, nil
		}
	}
	return "", fmt.Errorf("no macOS SDK found")
}

func exists(path string) bool {
	_, err := os.Stat(path)
	return err == nil
}

// mergeModules combines multiple TypeIR modules into a single module.
func mergeModules(modules []*typeir.TypeModule) *typeir.TypeModule {
	if len(modules) == 0 {
		return &typeir.TypeModule{}
	}
	if len(modules) == 1 {
		return modules[0]
	}

	merged := &typeir.TypeModule{
		Name:        modules[0].Name,
		Functions:   make([]typeir.FunctionIR, 0),
		Globals:     make([]typeir.GlobalIR, 0),
		StructTypes: make([]typeir.StructTypeDef, 0),
		Linkage:     make(map[uint32]typeir.LinkageEntry),
	}

	usedNames := make(map[string]bool)
	for _, m := range modules {
		// Merge functions (skip duplicates by name)
		for _, fn := range m.Functions {
			if !usedNames[fn.Name] {
				usedNames[fn.Name] = true
				merged.Functions = append(merged.Functions, fn)
			}
		}
		// Merge globals
		merged.Globals = append(merged.Globals, m.Globals...)
		// Merge struct types (skip duplicates by name)
		structNames := make(map[string]bool)
		for _, st := range merged.StructTypes {
			structNames[st.Name] = true
		}
		for _, st := range m.StructTypes {
			if !structNames[st.Name] {
				structNames[st.Name] = true
				merged.StructTypes = append(merged.StructTypes, st)
			}
		}
		// Merge linkage
		for k, v := range m.Linkage {
			merged.Linkage[k] = v
		}
	}
	return merged
}
