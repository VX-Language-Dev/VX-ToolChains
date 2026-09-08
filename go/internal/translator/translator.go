package translator

import (
	"bytes"
	"fmt"
	"os"
	"os/exec"
	"path/filepath"
	"strings"

	"github.com/vx-lang/vxc/internal/bytecode"
	"github.com/vx-lang/vxc/internal/llvmir"
	"github.com/vx-lang/vxc/internal/typeir"
)

// well-known macOS SDK paths to try when -syslibroot is needed.
var macOSSdkPaths = []string{
	"/Library/Developer/CommandLineTools/SDKs/MacOSX.sdk",
	"/Applications/Xcode.app/Contents/Developer/Platforms/MacOSX.platform/Developer/SDKs/MacOSX.sdk",
	"/Applications/Xcode-beta.app/Contents/Developer/Platforms/MacOSX.platform/Developer/SDKs/MacOSX.sdk",
}

// ==================== Translator ====================

// Translator translates VXOBJ (pure code logic binary) into a native
// executable. The pipeline is:
//
//	VXOBJ → TypeIR deserialize → LLVM IR generation (with platform adaptation)
//	→ llc (compile .ll → .o) → lld (link .o → executable)
//
// VXOBJ contains only pure code logic — all high-level features and
// platform API calls have been stripped at compile time. The translator
// re-adds platform bindings explicitly, ensuring zero hidden behavior.
type Translator struct {
	Target   TargetProfile
	KeepTemp bool   // keep temporary files (for debugging)
	LLVMPath string // optional: custom LLVM bin directory
	NoLink   bool   // stop after .ll generation (for inspection)

	tempDir string
}

// NewTranslator creates a new Translator for the given target.
func NewTranslator(target TargetProfile) *Translator {
	return &Translator{
		Target: target,
	}
}

// NewHostTranslator creates a Translator auto-detecting the host platform.
func NewHostTranslator() *Translator {
	return &Translator{
		Target: HostTarget(),
	}
}

// ==================== Translate ====================

// Translate reads one or more VXOBJ files, translates them into a native
// executable targeting the configured platform.
func (t *Translator) Translate(vxobjFiles []string, outputPath string, extraInputs []string) error {
	if len(vxobjFiles) == 0 {
		return ErrInvalidInput
	}

	// 1. Read and deserialize all VXOBJ files
	module, err := t.loadModules(vxobjFiles)
	if err != nil {
		return err
	}

	// 2. Generate complete LLVM IR with platform adaptation
	llvmIR, err := t.GenerateLLVMIR(module)
	if err != nil {
		return err
	}

	// 3. Create temp directory
	tmpDir, err := os.MkdirTemp("", "vxtrans_*")
	if err != nil {
		return fmt.Errorf("failed to create temp dir: %w", err)
	}
	t.tempDir = tmpDir
	if !t.KeepTemp {
		defer os.RemoveAll(tmpDir)
	}

	base := filepath.Join(tmpDir, "output")

	// 4. Write .ll file
	llPath := base + ".ll"
	if err := os.WriteFile(llPath, []byte(llvmIR), 0644); err != nil {
		return fmt.Errorf("failed to write .ll: %w", err)
	}

	if t.NoLink {
		// Copy IR to the output path so the caller can inspect it.
		irData, readErr := os.ReadFile(llPath)
		if readErr == nil {
			_ = os.WriteFile(outputPath, irData, 0644)
			fmt.Fprintf(os.Stderr, "[TRANSLATOR] LLVM IR written to %s (--no-link)\n", outputPath)
		} else {
			fmt.Fprintf(os.Stderr, "[TRANSLATOR] LLVM IR written to %s (--no-link)\n", llPath)
		}
		return nil
	}

	// 5. Compile .ll → .o via llc
	objPath, err := t.compileWithLLC(llPath, base)
	if err != nil {
		return err
	}

	// 6. Link .o → executable via lld
	execPath, err := t.linkWithLLD(objPath, outputPath, extraInputs)
	if err != nil {
		return err
	}

	// 7. Make executable
	if err := os.Chmod(execPath, 0755); err != nil {
		return fmt.Errorf("failed to chmod '%s': %w", execPath, err)
	}

	stat, err := os.Stat(execPath)
	if err != nil {
		return fmt.Errorf("failed to stat '%s': %w", execPath, err)
	}
	fmt.Fprintf(os.Stderr, "[TRANSLATOR] Output: %s (%d bytes)\n", execPath, stat.Size())

	return nil
}

// ==================== Module Loading ====================

func (t *Translator) loadModules(vxobjFiles []string) (*typeir.TypeModule, error) {
	modules := make([]*typeir.TypeModule, 0, len(vxobjFiles))

	for i, input := range vxobjFiles {
		raw, err := os.ReadFile(input)
		if err != nil {
			return nil, fmt.Errorf("failed to read '%s': %w", input, err)
		}

		container, err := bytecode.ParseVxobjV4(raw)
		if err != nil {
			return nil, fmt.Errorf("failed to parse VXOBJ '%s': %w", input, err)
		}

		if i == 0 {
			triple := container.Header.TargetTriple
			if triple != "" && t.Target.Triple != triple {
				fmt.Fprintf(os.Stderr, "[TRANSLATOR] Note: VXOBJ target '%s' overridden by translator target '%s'\n",
					triple, t.Target.Triple)
			}
		}

		typeIRData := container.GetSection(bytecode.SECTION_TYPE_IR)
		if typeIRData == nil {
			return nil, fmt.Errorf("no TypeIR section in '%s'", input)
		}

		mod, err := typeir.DeserializeTypeModule(typeIRData)
		if err != nil {
			return nil, fmt.Errorf("failed to deserialize TypeIR from '%s': %w", input, err)
		}

		fmt.Fprintf(os.Stderr, "[TRANSLATOR] Loaded: %s (%d functions)\n", input, len(mod.Functions))
		modules = append(modules, mod)
	}

	return mergeModules(modules), nil
}

// mergeModules combines multiple TypeIR modules into one.
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
		StructTypes: make([]typeir.StructTypeDef, 0),
		Linkage:     make(map[uint32]typeir.LinkageEntry),
	}

	usedNames := make(map[string]bool)
	for _, m := range modules {
		for _, fn := range m.Functions {
			if !usedNames[fn.Name] {
				usedNames[fn.Name] = true
				merged.Functions = append(merged.Functions, fn)
			}
		}
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
		for k, v := range m.Linkage {
			// Prefer non-extern (definition) linkage over extern (declaration).
			// This ensures user-defined functions from one module are not
			// overridden by extern declarations in another module.
			if existing, ok := merged.Linkage[k]; ok && existing.IsExternal && !v.IsExternal {
				merged.Linkage[k] = v
			} else if !ok {
				merged.Linkage[k] = v
			}
		}
	}
	return merged
}

// ==================== LLVM IR Generation ====================

// GenerateLLVMIR produces the complete LLVM IR text for the given module,
// including platform-specific API bindings and entry point code.
//
// The output structure is:
//
//  1. Target triple and data layout (from platform detection)
//  2. Platform API declarations (resolved from VXOBJ abstract ops)
//  3. User program functions (translated from TypeIR)
//  4. Minimal entry point (no hidden runtime)
func (t *Translator) GenerateLLVMIR(module *typeir.TypeModule) (string, error) {
	var sb strings.Builder

	// 1. Module header
	fmt.Fprintf(&sb, "; VXOBJ translated to LLVM IR by vx-translator\n")
	fmt.Fprintf(&sb, "; Target: %s\n", t.Target.Triple)
	fmt.Fprintf(&sb, "; No hidden behavior — all platform bindings are explicit\n")
	fmt.Fprintf(&sb, "target triple = \"%s\"\n", t.Target.Triple)
	fmt.Fprintf(&sb, "target datalayout = \"%s\"\n\n", t.TargetDataLayout())

	// 2. Apply platform-specific symbol renaming (e.g., main → _main on MachO)
	t.applySymbolRenames(module)

	// 3. Platform API declarations (explicit, no hidden magic)
	platformDecls := t.PlatformDeclarations(module)
	sb.WriteString(platformDecls)
	if platformDecls != "" {
		sb.WriteString("\n")
	}

	// 4. User program functions
	userIR, err := llvmir.Generate(module, t.Target.Triple)
	if err != nil {
		return "", fmt.Errorf("LLVM IR generation failed: %w", err)
	}

	// Strip the module header from llvmir.Generate output since we
	// already wrote our own (with the correct target triple).
	userIR = stripModuleHeader(userIR)
	sb.WriteString(userIR)
	sb.WriteString("\n")

	// 4. Platform runtime wrappers (explicit platform API resolution)
	runtimeIR := t.PlatformRuntime(module)
	sb.WriteString(runtimeIR)
	if runtimeIR != "" {
		sb.WriteString("\n")
	}

	// 5. Entry point (minimal, explicit)
	entry := t.EntryPointCode()
	if entry != "" {
		sb.WriteString(entry)
		sb.WriteString("\n")
	}

	return sb.String(), nil
}

// applySymbolRenames renames function symbols to match platform ABI conventions.
//
// On MachO (macOS), LLVM's backend automatically prepends a leading underscore
// to all C symbols. So `@main` in LLVM IR becomes `_main` in the object file.
// No manual renaming is needed — the LLVM backend handles this.
//
// ELF and PE platforms also need no renaming.
func (t *Translator) applySymbolRenames(module *typeir.TypeModule) {
	// No renaming needed. LLVM backends handle platform ABI naming:
	//   - MachO auto-prepends "_" to C symbols
	//   - ELF/PE use names as-is
	_ = module // unused
}

// stripModuleHeader removes the leading comment and target triple/datalayout
// lines from llvmir.Generate's output, since the translator provides its own.
func stripModuleHeader(ir string) string {
	lines := strings.Split(ir, "\n")
	start := 0
	for i, line := range lines {
		trimmed := strings.TrimSpace(line)
		if trimmed == "" || strings.HasPrefix(trimmed, ";") ||
			strings.HasPrefix(trimmed, "target ") {
			start = i + 1
			continue
		}
		break
	}
	return strings.Join(lines[start:], "\n")
}

// ==================== LLC (LLVM → Object) ====================

func (t *Translator) compileWithLLC(llPath, base string) (string, error) {
	objPath := base + ".o"

	binary := t.findTool("llc")
	if binary == "" {
		return "", ErrToolNotFound
	}

	args := []string{
		llPath,
		"-o", objPath,
		"-O2",
	}
	args = append(args, t.Target.LLCTripleFlags()...)

	cmd := exec.Command(binary, args...)
	var stderr bytes.Buffer
	cmd.Stderr = &stderr
	cmd.Stdout = &stderr

	fmt.Fprintf(os.Stderr, "[TRANSLATOR] Compiling .ll → .o: %s\n", binary)
	if err := cmd.Run(); err != nil {
		fmt.Fprintf(os.Stderr, "[TRANSLATOR] llc failed:\n%s\n", stderr.String())
		return "", ErrCompileFailed
	}

	if _, err := os.Stat(objPath); err != nil {
		return "", ErrCompileFailed
	}
	return objPath, nil
}

// ==================== LLD (Object → Executable) ====================

func (t *Translator) linkWithLLD(objPath, outputPath string, extraInputs []string) (string, error) {
	lldBinary := t.findTool(t.Target.LldBinary())
	if lldBinary == "" {
		// Fall back to just "lld"
		lldBinary = t.findTool("lld")
		if lldBinary == "" {
			return "", ErrToolNotFound
		}
	}

	args := []string{}
	args = append(args, t.Target.LinkerFlags...)

	// Auto-detect syslibroot for MachO targets (macOS SDK path).
	if t.Target.OutputFormat == MachO {
		syslibroot := t.findMacOSSyslibroot()
		if syslibroot != "" {
			args = append(args, "-syslibroot", syslibroot)
		}
	}

	args = append(args, "-o", outputPath)
	args = append(args, objPath)
	args = append(args, extraInputs...)

	cmd := exec.Command(lldBinary, args...)
	var stderr bytes.Buffer
	cmd.Stderr = &stderr
	cmd.Stdout = &stderr

	fmt.Fprintf(os.Stderr, "[TRANSLATOR] Linking .o → executable: %s\n", lldBinary)
	if err := cmd.Run(); err != nil {
		fmt.Fprintf(os.Stderr, "[TRANSLATOR] lld failed:\n%s\n", stderr.String())
		return "", ErrLinkFailed
	}

	if _, err := os.Stat(outputPath); err != nil {
		return "", ErrOutputNotCreated
	}
	return outputPath, nil
}

// findMacOSSyslibroot attempts to locate a valid macOS SDK path for -syslibroot.
// It tries xcrun first, then falls back to well-known SDK directories.
func (t *Translator) findMacOSSyslibroot() string {
	// Try xcrun (most reliable when CLT or Xcode is installed).
	if path, err := exec.LookPath("xcrun"); err == nil {
		cmd := exec.Command(path, "--sdk", "macosx", "--show-sdk-path")
		out, err := cmd.Output()
		if err == nil {
			sdkPath := strings.TrimSpace(string(out))
			if sdkPath != "" {
				if _, err := os.Stat(sdkPath); err == nil {
					return sdkPath
				}
			}
		}
	}
	// Fallback: check well-known SDK directories.
	for _, p := range macOSSdkPaths {
		if _, err := os.Stat(p); err == nil {
			return p
		}
	}
	return ""
}

// ==================== Tool Discovery ====================

func (t *Translator) findTool(name string) string {
	// Check custom path first
	if t.LLVMPath != "" {
		p := filepath.Join(t.LLVMPath, name)
		if _, err := os.Stat(p); err == nil {
			return p
		}
	}

	// Search PATH
	if path, err := exec.LookPath(name); err == nil {
		return path
	}

	// Common suffixed versions
	suffixes := []string{"-19", "-18", "-17", "-16", "-15"}
	for _, s := range suffixes {
		if path, err := exec.LookPath(name + s); err == nil {
			return path
		}
	}

	return ""
}
