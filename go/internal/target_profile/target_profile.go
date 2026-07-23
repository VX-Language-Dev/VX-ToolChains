package targetprofile

import (
	"runtime"
	"strings"
)

// LldFlavor LLD 链接器风格
type LldFlavor int

const (
	LldGnu    LldFlavor = iota // GNU 风格
	LldDarwin                  // Darwin/macOS 风格
	LldCoff                    // COFF 风格
)

// OutputFormat 输出文件格式
type OutputFormat int

const (
	FormatElf  OutputFormat = iota // ELF 格式
	FormatMachO                    // Mach-O 格式
	FormatPe                       // PE 格式
)

// Architecture 目标架构
type Architecture int

const (
	ArchX86_64  Architecture = iota // x86_64 架构
	ArchAarch64                     // AArch64 架构
	ArchArm32                       // ARM32 架构
	ArchRiscv32                     // RISC-V 32 位架构
	ArchRiscv64                     // RISC-V 64 位架构
	ArchUnknown                     // 未知架构
)

// ArchitectureFromTriple 从三元组字符串解析架构
func ArchitectureFromTriple(triple string) Architecture {
	if strings.Contains(triple, "x86_64") || strings.Contains(triple, "amd64") {
		return ArchX86_64
	}
	if strings.Contains(triple, "aarch64") || strings.Contains(triple, "arm64") {
		return ArchAarch64
	}
	if strings.Contains(triple, "arm") && !strings.Contains(triple, "aarch64") {
		return ArchArm32
	}
	if strings.Contains(triple, "riscv32") {
		return ArchRiscv32
	}
	if strings.Contains(triple, "riscv64") {
		return ArchRiscv64
	}
	return ArchUnknown
}

// Name 返回架构名称
func (a Architecture) Name() string {
	switch a {
	case ArchX86_64:
		return "x86_64"
	case ArchAarch64:
		return "aarch64"
	case ArchArm32:
		return "arm32"
	case ArchRiscv32:
		return "riscv32"
	case ArchRiscv64:
		return "riscv64"
	default:
		return "unknown"
	}
}

// IsStable 检查架构是否稳定
func (a Architecture) IsStable() bool {
	return a == ArchX86_64
}

// IsExperimental 检查架构是否为实验性
func (a Architecture) IsExperimental() bool {
	return a == ArchAarch64 || a == ArchArm32 || a == ArchRiscv32 || a == ArchRiscv64
}

// TargetProfile 目标平台配置（简化版，无需外部 target-lexicon crate）
type TargetProfile struct {
	// 目标三元组字符串（如 "x86_64-linux-gnu"）
	Triple string
	// 目标架构
	Arch Architecture
	// LLD 链接器风格
	LldFlavor LldFlavor
	// 输出文件格式
	OutputFormat OutputFormat
	// 入口符号
	EntrySymbol string
	// 默认输出扩展名
	DefaultOutputExtension string
	// 静态链接标志
	StaticLinkFlags []string
	// 库文件前缀
	LibPrefix string
	// 库文件扩展名
	LibExtension string
}

// Host 构建为宿主平台
func Host() TargetProfile {
	goos := runtime.GOOS
	goarch := runtime.GOARCH

	var triple string
	switch goos {
	case "darwin":
		triple = goarch + "-apple-darwin"
	case "linux":
		triple = goarch + "-linux-gnu"
	case "windows":
		triple = goarch + "-windows-msvc"
	default:
		triple = goarch + "-unknown-unknown"
	}

	return FromTriple(triple)
}

// FromTriple 从三元组字符串构建
func FromTriple(triple string) TargetProfile {
	arch := ArchitectureFromTriple(triple)

	isMacos := strings.Contains(triple, "darwin") ||
		strings.Contains(triple, "macos") ||
		strings.Contains(triple, "apple")
	isWindows := strings.Contains(triple, "windows") ||
		strings.Contains(triple, "win32") ||
		strings.Contains(triple, "coff")

	if isMacos {
		return TargetProfile{
			Triple:                 triple,
			Arch:                   arch,
			LldFlavor:              LldDarwin,
			OutputFormat:           FormatMachO,
			EntrySymbol:            "_main",
			DefaultOutputExtension: "out",
			StaticLinkFlags:        []string{"-static", "-no_pie"},
			LibPrefix:              "lib",
			LibExtension:           ".dylib",
		}
	}

	if isWindows {
		return TargetProfile{
			Triple:                 triple,
			Arch:                   arch,
			LldFlavor:              LldCoff,
			OutputFormat:           FormatPe,
			EntrySymbol:            "mainCRTStartup",
			DefaultOutputExtension: "exe",
			StaticLinkFlags:        []string{"/NODEFAULTLIB", "/SUBSYSTEM:CONSOLE"},
			LibPrefix:              "",
			LibExtension:           ".dll",
		}
	}

	// 默认 Linux/ELF
	return TargetProfile{
		Triple:                 triple,
		Arch:                   arch,
		LldFlavor:              LldGnu,
		OutputFormat:           FormatElf,
		EntrySymbol:            "_start",
		DefaultOutputExtension: "out",
		StaticLinkFlags:        []string{"-static"},
		LibPrefix:              "lib",
		LibExtension:           ".so",
	}
}

// LldBinaryName 返回 LLD 二进制名称
func (tp *TargetProfile) LldBinaryName() string {
	switch tp.LldFlavor {
	case LldGnu:
		return "ld.lld"
	case LldDarwin:
		return "ld64.lld"
	case LldCoff:
		return "lld-link"
	default:
		return "ld.lld"
	}
}

// IsArchSupported 检查架构是否支持
func (tp *TargetProfile) IsArchSupported() bool {
	switch tp.Arch {
	case ArchX86_64:
		return true // 稳定
	case ArchAarch64, ArchArm32, ArchRiscv32, ArchRiscv64:
		return true // 实验性
	default:
		return false
	}
}

// ArchStatus 获取架构稳定性描述
func (tp *TargetProfile) ArchStatus() string {
	switch tp.Arch {
	case ArchX86_64:
		return "stable"
	case ArchAarch64, ArchArm32, ArchRiscv32, ArchRiscv64:
		return "experimental"
	default:
		return "unsupported"
	}
}
