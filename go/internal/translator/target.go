// Package translator translates VXOBJ (pure code logic binary) into native
// executables via LLVM IR + llc + lld, with automatic platform adaptation.
//
// VXOBJ is a pure code-logic line binary representation: it contains only
// TypeIR instructions stripped of all high-level features (generics, traits,
// ownership annotations) and platform API calls. The translator's job is to
// resolve the abstract operations against the host/target platform and produce
// a native executable with no hidden behavior — following Zig's philosophy.
package translator

import (
	"fmt"
	"runtime"
	"strings"
)

// ==================== Output Format ====================

// OutputFormat is the native binary format of the output executable.
type OutputFormat int

const (
	Elf   OutputFormat = iota // Linux/ELF
	MachO                     // macOS/Mach-O
	Pe                        // Windows/PE
)

// ==================== Architecture ====================

// Architecture is the target CPU architecture.
type Architecture int

const (
	ArchX86_64  Architecture = iota // x86-64 (amd64)
	ArchAarch64                     // AArch64 (ARM64)
	ArchUnknown                     // Unsupported
)

// ==================== LLD Flavor ====================

// LldFlavor selects the LLD frontend to use.
type LldFlavor int

const (
	LldDefault LldFlavor = iota // ELF / PE (default lld)
	LldDarwin                   // Mach-O (ld64.lld)
)

// ==================== TargetProfile ====================

// TargetProfile holds all target-platform configuration.
type TargetProfile struct {
	Triple       string       // LLVM target triple
	Arch         Architecture // CPU architecture
	OutputFormat OutputFormat // binary format
	LldFlavor    LldFlavor    // LLD frontend
	LinkerFlags  []string     // extra linker flags
	EntrySymbol  string       // entry point symbol name
}

// LldBinary returns the LLD binary name for this target.
func (t *TargetProfile) LldBinary() string {
	switch t.LldFlavor {
	case LldDarwin:
		return "ld64.lld"
	default:
		return "lld"
	}
}

// LLCTripleFlags returns target-specific flags for llc.
func (t *TargetProfile) LLCTripleFlags() []string {
	// Always emit a binary object file (-filetype=obj) since our pipeline
	// expects .o input for lld. On macOS, llc defaults to assembly output
	// if -filetype is not specified.
	switch t.OutputFormat {
	case MachO:
		return []string{"-filetype", "obj", "-relocation-model", "pic"}
	case Pe:
		return []string{"-filetype", "obj"}
	default:
		return []string{"-filetype", "obj", "-relocation-model", "static"}
	}
}

// ==================== Auto-Detection ====================

// HostTarget auto-detects the current host platform and returns a TargetProfile.
func HostTarget() TargetProfile {
	goos := runtime.GOOS
	goarch := runtime.GOARCH

	var arch Architecture
	switch goarch {
	case "amd64":
		arch = ArchX86_64
	case "arm64":
		arch = ArchAarch64
	default:
		arch = ArchUnknown
	}

	var (
		triple      string
		outFmt      OutputFormat
		flavor      LldFlavor
		linkerFlags []string
		entrySymbol string
	)

	switch goos {
	case "darwin":
		outFmt = MachO
		flavor = LldDarwin
		entrySymbol = "_main"
		switch arch {
		case ArchAarch64:
			triple = "arm64-apple-macosx"
			linkerFlags = []string{"-lSystem", "-e", "_main", "-arch", "arm64", "-platform_version", "macos", "14.0.0", "14.0.0"}
		default:
			triple = "x86_64-apple-macosx"
			linkerFlags = []string{"-lSystem", "-e", "_main", "-arch", "x86_64", "-platform_version", "macos", "14.0.0", "14.0.0"}
		}

	case "linux":
		outFmt = Elf
		flavor = LldDefault
		linkerFlags = []string{"-lc", "-e", "_start"}
		entrySymbol = "main"
		switch arch {
		case ArchAarch64:
			triple = "aarch64-linux-gnu"
		default:
			triple = "x86_64-linux-gnu"
		}

	case "windows":
		outFmt = Pe
		flavor = LldDefault
		linkerFlags = []string{"-subsystem:console", "-entry:mainCRTStartup"}
		entrySymbol = "main"
		triple = "x86_64-windows-msvc"
	}

	return TargetProfile{
		Triple:       triple,
		Arch:         arch,
		OutputFormat: outFmt,
		LldFlavor:    flavor,
		LinkerFlags:  linkerFlags,
		EntrySymbol:  entrySymbol,
	}
}

// TargetFromTriple creates a TargetProfile from an explicit triple string.
// Useful for cross-compilation.
func TargetFromTriple(triple string) (TargetProfile, error) {
	arch := ArchUnknown
	parts := strings.Split(triple, "-")
	if len(parts) > 0 {
		switch {
		case strings.HasPrefix(parts[0], "x86_64"):
			arch = ArchX86_64
		case strings.HasPrefix(parts[0], "aarch64"), strings.HasPrefix(parts[0], "arm64"):
			arch = ArchAarch64
		}
	}

	if arch == ArchUnknown {
		return TargetProfile{}, fmt.Errorf("unsupported architecture in triple: %s", triple)
	}

	outFmt := Elf
	flavor := LldDefault
	linkerFlags := []string{"-e", "_start"}
	entrySymbol := "main"

	if strings.Contains(triple, "apple") || strings.Contains(triple, "darwin") || strings.Contains(triple, "macos") {
		outFmt = MachO
		flavor = LldDarwin
		entrySymbol = "_main"
		archFlag, osVersion := "x86_64", "14.0.0"
		if strings.HasPrefix(triple, "aarch64") || strings.HasPrefix(triple, "arm64") {
			archFlag = "arm64"
		}
		linkerFlags = []string{"-lSystem", "-e", "_main", "-arch", archFlag, "-platform_version", "macos", osVersion, osVersion}
	} else if strings.Contains(triple, "windows") || strings.Contains(triple, "mingw") || strings.Contains(triple, "msvc") {
		outFmt = Pe
		flavor = LldDefault
		linkerFlags = []string{"-subsystem:console", "-entry:mainCRTStartup"}
		entrySymbol = "main"
	}

	return TargetProfile{
		Triple:       triple,
		Arch:         arch,
		OutputFormat: outFmt,
		LldFlavor:    flavor,
		LinkerFlags:  linkerFlags,
		EntrySymbol:  entrySymbol,
	}, nil
}

// Name returns the human-readable name of the architecture.
func (a Architecture) Name() string {
	switch a {
	case ArchX86_64:
		return "x86_64"
	case ArchAarch64:
		return "aarch64"
	default:
		return "unknown"
	}
}

// IsStable returns true if the architecture is considered stable.
func (a Architecture) IsStable() bool {
	return a == ArchX86_64
}

// IsExperimental returns true if the architecture is experimental.
func (a Architecture) IsExperimental() bool {
	return a != ArchX86_64 && a != ArchUnknown
}
