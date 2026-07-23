package linker

import (
	"bytes"
	"encoding/binary"
	"os"
	"os/exec"
	"path/filepath"
	"strings"
)

// ==================== 输出格式 ====================

// OutputFormat 输出格式
type OutputFormat int

const (
	Elf OutputFormat = iota
	MachO
	Pe
)

// ==================== 架构 ====================

// Architecture 目标架构
type Architecture int

const (
	ArchX86_64 Architecture = iota
	ArchAarch64
	ArchArm32
	ArchRiscv64
	ArchRiscv32
	ArchUnknown
)

// ArchFromTriple 从 triple 字符串解析架构
func ArchFromTriple(triple string) Architecture {
	parts := strings.Split(triple, "-")
	if len(parts) == 0 {
		return ArchUnknown
	}
	arch := parts[0]
	switch {
	case strings.HasPrefix(arch, "x86_64"):
		return ArchX86_64
	case strings.HasPrefix(arch, "aarch64"):
		return ArchAarch64
	case strings.HasPrefix(arch, "arm"):
		return ArchArm32
	case strings.HasPrefix(arch, "riscv64"):
		return ArchRiscv64
	case strings.HasPrefix(arch, "riscv32"):
		return ArchRiscv32
	default:
		return ArchUnknown
	}
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
	case ArchRiscv64:
		return "riscv64"
	case ArchRiscv32:
		return "riscv32"
	default:
		return "unknown"
	}
}

// IsStable 检查架构是否稳定
func (a Architecture) IsStable() bool {
	return a == ArchX86_64
}

// IsExperimental 检查架构是否实验性
func (a Architecture) IsExperimental() bool {
	return a != ArchX86_64 && a != ArchUnknown
}

// ==================== LLD Flavor ====================

// LldFlavor LLD 链接器变体
type LldFlavor int

const (
	LldDefault LldFlavor = iota
	LldDarwin
)

// ==================== TargetProfile ====================

// TargetProfile 目标配置
type TargetProfile struct {
	Triple          string
	Arch            Architecture
	OutputFormat    OutputFormat
	LldFlavor       LldFlavor
	StaticLinkFlags []string
	SourceDir       string
}

// TargetFromTriple 从 triple 字符串创建目标配置
func TargetFromTriple(triple string) TargetProfile {
	arch := ArchFromTriple(triple)
	outFmt := Elf
	flavor := LldDefault

	if strings.Contains(triple, "apple") || strings.Contains(triple, "darwin") || strings.Contains(triple, "macos") {
		outFmt = MachO
		flavor = LldDarwin
	} else if strings.Contains(triple, "windows") || strings.Contains(triple, "mingw") || strings.Contains(triple, "msvc") {
		outFmt = Pe
	}

	flags := []string{}
	switch outFmt {
	case MachO:
		flags = []string{"-lSystem", "-syslibroot", "$(SDKROOT)"}
	case Pe:
		flags = []string{"-subsystem", "console"}
	}

	return TargetProfile{
		Triple:          triple,
		Arch:            arch,
		OutputFormat:    outFmt,
		LldFlavor:       flavor,
		StaticLinkFlags: flags,
	}
}

// IsArchSupported 检查架构是否受支持
func (t *TargetProfile) IsArchSupported() bool {
	return t.Arch != ArchUnknown
}

// ArchStatus 返回架构状态字符串
func (t *TargetProfile) ArchStatus() string {
	if t.Arch.IsStable() {
		return "stable"
	}
	if t.Arch.IsExperimental() {
		return "experimental"
	}
	return "unsupported"
}

// LldBinaryName 返回 LLD 二进制文件名
func (t *TargetProfile) LldBinaryName() string {
	switch t.LldFlavor {
	case LldDarwin:
		return "ld64.lld"
	default:
		return "lld"
	}
}

// ==================== 错误类型 ====================

// LinkerError 链接器错误
type LinkerError int

const (
	ErrLldNotFound LinkerError = iota
	ErrLinkFailed
	ErrIoError
	ErrObjectFileWriteFailed
	ErrInvalidArchitecture
	ErrOutputNotCreated
)

func (e LinkerError) Error() string {
	switch e {
	case ErrLldNotFound:
		return "lld not found"
	case ErrLinkFailed:
		return "link failed"
	case ErrIoError:
		return "IO error"
	case ErrObjectFileWriteFailed:
		return "object file write failed"
	case ErrInvalidArchitecture:
		return "invalid architecture"
	case ErrOutputNotCreated:
		return "output not created"
	default:
		return "unknown linker error"
	}
}

// ==================== LinkResult ====================

// LinkResult 链接结果
type LinkResult struct {
	OutputPath string
	OutputSize int64
}

// ==================== LinkerType ====================

// LinkerType 链接器类型
type LinkerType int

const (
	BuiltinLinkerType LinkerType = iota
	LldLinkerType
)

// ==================== Linker ====================

// Linker VX 链接器
type Linker struct {
	Type   LinkerType
	Target TargetProfile
}

// NewLinker 创建新的链接器
func NewLinker(linkerType LinkerType, target TargetProfile) *Linker {
	return &Linker{
		Type:   linkerType,
		Target: target,
	}
}

// Link 执行链接
func (l *Linker) Link(text, rodata, data []byte, bssSize, entryOffset uint64, outputPath string) (*LinkResult, error) {
	switch l.Type {
	case BuiltinLinkerType:
		return l.linkBuiltin(text, rodata, data, bssSize, entryOffset, outputPath)
	case LldLinkerType:
		return l.linkLld(text, outputPath)
	default:
		return nil, ErrInvalidArchitecture
	}
}

// linkBuiltin 内建链接器实现
func (l *Linker) linkBuiltin(text, rodata, data []byte, bssSize, entryOffset uint64, outputPath string) (*LinkResult, error) {
	_ = entryOffset
	_ = bssSize

	file, err := os.Create(outputPath)
	if err != nil {
		return nil, ErrObjectFileWriteFailed
	}
	defer file.Close()

	switch l.Target.OutputFormat {
	case Elf:
		if err := writeElfHeader(file); err != nil {
			return nil, err
		}
	case MachO:
		if err := writeMachOHeader(file); err != nil {
			return nil, err
		}
	case Pe:
		if err := writePeHeader(file); err != nil {
			return nil, err
		}
	}

	if _, err := file.Write(text); err != nil {
		return nil, ErrIoError
	}
	if len(rodata) > 0 {
		if _, err := file.Write(rodata); err != nil {
			return nil, ErrIoError
		}
	}
	if len(data) > 0 {
		if _, err := file.Write(data); err != nil {
			return nil, ErrIoError
		}
	}

	stat, err := file.Stat()
	if err != nil {
		return nil, ErrOutputNotCreated
	}

	return &LinkResult{
		OutputPath: outputPath,
		OutputSize: stat.Size(),
	}, nil
}

// linkLld 使用外部 lld 链接器
func (l *Linker) linkLld(objData []byte, outputPath string) (*LinkResult, error) {
	// 写入临时 .o 文件
	tmpDir, err := os.MkdirTemp("", "vx_lld_*")
	if err != nil {
		return nil, ErrIoError
	}
	defer os.RemoveAll(tmpDir)

	objPath := filepath.Join(tmpDir, "vx_lld_tmp.o")
	if err := os.WriteFile(objPath, objData, 0644); err != nil {
		return nil, ErrObjectFileWriteFailed
	}

	lldBinary := l.Target.LldBinaryName()
	args := []string{"-o", outputPath}
	args = append(args, l.Target.StaticLinkFlags...)
	args = append(args, objPath)

	cmd := exec.Command(lldBinary, args...)
	if err := cmd.Run(); err != nil {
		return nil, ErrLinkFailed
	}

	stat, err := os.Stat(outputPath)
	if err != nil {
		return nil, ErrOutputNotCreated
	}

	return &LinkResult{
		OutputPath: outputPath,
		OutputSize: stat.Size(),
	}, nil
}

// ==================== 头部写入 ====================

func writeElfHeader(f *os.File) error {
	var buf bytes.Buffer

	// e_ident
	eIdent := [16]byte{
		0x7f, 'E', 'L', 'F',
		2, 1, 1, 0,
		0, 0, 0, 0,
		0, 0, 0, 0,
	}
	buf.Write(eIdent[:])

	binary.Write(&buf, binary.LittleEndian, uint16(2))         // e_type: ET_EXEC
	binary.Write(&buf, binary.LittleEndian, uint16(0x3E))      // e_machine: x86_64
	binary.Write(&buf, binary.LittleEndian, uint32(1))         // e_version
	binary.Write(&buf, binary.LittleEndian, uint64(0x401000))  // e_entry
	binary.Write(&buf, binary.LittleEndian, uint64(64))        // e_phoff
	binary.Write(&buf, binary.LittleEndian, uint64(0))         // e_shoff
	binary.Write(&buf, binary.LittleEndian, uint32(0))         // e_flags
	binary.Write(&buf, binary.LittleEndian, uint16(64))        // e_ehsize
	binary.Write(&buf, binary.LittleEndian, uint16(56))        // e_phentsize
	binary.Write(&buf, binary.LittleEndian, uint16(1))         // e_phnum
	binary.Write(&buf, binary.LittleEndian, uint16(64))        // e_shentsize
	binary.Write(&buf, binary.LittleEndian, uint16(0))         // e_shnum
	binary.Write(&buf, binary.LittleEndian, uint16(0))         // e_shstrndx

	_, err := f.Write(buf.Bytes())
	return err
}

func writeMachOHeader(f *os.File) error {
	var buf bytes.Buffer

	binary.Write(&buf, binary.LittleEndian, uint32(0xFEEDFACF)) // magic
	binary.Write(&buf, binary.LittleEndian, uint32(0x0100000C)) // cputype: ARM64
	binary.Write(&buf, binary.LittleEndian, uint32(0x00000003)) // cpusubtype
	binary.Write(&buf, binary.LittleEndian, uint32(0x00000002)) // filetype: MH_EXECUTE
	binary.Write(&buf, binary.LittleEndian, uint32(0))          // ncmds
	binary.Write(&buf, binary.LittleEndian, uint32(0))          // sizeofcmds
	binary.Write(&buf, binary.LittleEndian, uint32(0))          // flags
	binary.Write(&buf, binary.LittleEndian, uint32(0))          // reserved

	_, err := f.Write(buf.Bytes())
	return err
}

func writePeHeader(f *os.File) error {
	// MZ header
	_, err := f.Write([]byte("MZ"))
	if err != nil {
		return err
	}

	// DOS stub (58 bytes of zeros)
	stub := make([]byte, 58)
	f.Write(stub)

	// PE offset
	var buf bytes.Buffer
	binary.Write(&buf, binary.LittleEndian, uint32(0x80))
	f.Write(buf.Bytes())

	// PE signature
	f.Write([]byte("PE\x00\x00"))

	// COFF header
	var coff bytes.Buffer
	binary.Write(&coff, binary.LittleEndian, uint16(0x8664)) // machine: AMD64
	binary.Write(&coff, binary.LittleEndian, uint16(1))      // number of sections
	_, err = f.Write(coff.Bytes())
	return err
}
