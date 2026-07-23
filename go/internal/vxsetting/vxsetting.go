package vxsetting

import (
	"fmt"
	"os"
	"path/filepath"
	"strings"

	"github.com/BurntSushi/toml"
)

// VxSettings VX 编译器设置
type VxSettings struct {
	SourceDir string
	Bin       *BuildTarget
	Vxlib     *BuildTarget
	Lib       *BuildTarget
	Modules   []ModuleDecl
	Vxset     VxsetConfig
	Libraries map[string]string
	Compiler  *VxCompilerSettings
	Linker    *VxLinkerSettings
	Memory    *VxMemorySettings
}

// NewVxSettings 创建新的 VxSettings 实例
func NewVxSettings(sourceDir string) *VxSettings {
	return &VxSettings{
		SourceDir: sourceDir,
		Libraries: make(map[string]string),
	}
}

// IsMultiFileProject 检查是否为多文件项目
func (vs *VxSettings) IsMultiFileProject() bool {
	return vs.Bin != nil || vs.Vxlib != nil || vs.Lib != nil || len(vs.Modules) > 0
}

// LibraryPath 获取库路径
func (vs *VxSettings) LibraryPath(name string) (string, bool) {
	path, ok := vs.Libraries[name]
	return path, ok
}

// FromFile 从文件加载设置
func FromFile(path string) (*VxSettings, error) {
	content, err := os.ReadFile(path)
	if err != nil {
		return nil, err
	}

	abs, err := filepath.Abs(path)
	if err != nil {
		return nil, err
	}

	sourceDir := filepath.Dir(abs)
	if sourceDir == "" {
		sourceDir = "."
	}

	return ParseFromContent(string(content), sourceDir)
}

// rawBuildTarget 用于 TOML 解析的临时构建目标结构
type rawBuildTarget struct {
	Source       string `toml:"source"`
	Version      string `toml:"version"`
	Output       string `toml:"output"`
	Optimization uint8  `toml:"o"`
}

// rawVxsetConfig 用于 TOML 解析的临时 vxset 配置
type rawVxsetConfig struct {
	Deadcode bool   `toml:"deadcode"`
	Cache    bool   `toml:"cache"`
	Shell    string `toml:"shell"`
}

// ParseFromContent 从内容解析设置（使用 TOML）
func ParseFromContent(content string, sourceDir string) (*VxSettings, error) {
	var raw struct {
		Bin       *rawBuildTarget     `toml:"bin"`
		Vxlib     *rawBuildTarget     `toml:"vxlib"`
		Lib       *rawBuildTarget     `toml:"lib"`
		Vxset     rawVxsetConfig      `toml:"vxset"`
		Libraries map[string]string   `toml:"libraries"`
		Compiler  *VxCompilerSettings `toml:"compiler"`
		Linker    *VxLinkerSettings   `toml:"linker"`
		Memory    *VxMemorySettings   `toml:"memory"`
	}
	// 设置 Vxset 默认值（与 Zig 侧的默认值一致）
	raw.Vxset = rawVxsetConfig{Deadcode: true, Cache: true}

	if _, err := toml.Decode(content, &raw); err != nil {
		return nil, err
	}

	settings := NewVxSettings(sourceDir)

	if raw.Bin != nil {
		settings.Bin = buildTargetFromRaw(raw.Bin)
	}
	if raw.Vxlib != nil {
		settings.Vxlib = buildTargetFromRaw(raw.Vxlib)
	}
	if raw.Lib != nil {
		settings.Lib = buildTargetFromRaw(raw.Lib)
	}

	settings.Vxset = VxsetConfig{
		Deadcode: raw.Vxset.Deadcode,
		Cache:    raw.Vxset.Cache,
	}
	if raw.Vxset.Shell != "" {
		s := raw.Vxset.Shell
		settings.Vxset.Shell = &s
	}

	if raw.Libraries != nil {
		settings.Libraries = raw.Libraries
	}
	settings.Compiler = raw.Compiler
	settings.Linker = raw.Linker
	settings.Memory = raw.Memory

	return settings, nil
}

// buildTargetFromRaw 将临时解析结果转换为 BuildTarget
func buildTargetFromRaw(raw *rawBuildTarget) *BuildTarget {
	bt := &BuildTarget{
		Sources:      splitSources(raw.Source),
		Version:      "0.0.1",
		Optimization: raw.Optimization,
	}

	if raw.Version != "" {
		bt.Version = raw.Version
	}
	if raw.Output != "" {
		bt.Output = &raw.Output
	}
	if bt.Optimization < 1 {
		bt.Optimization = 1
	}
	if bt.Optimization > 10 {
		bt.Optimization = 10
	}

	return bt
}

// splitSources 将逗号分隔的源文件字符串拆分为切片
func splitSources(source string) []string {
	var sources []string
	for _, s := range strings.Split(source, ",") {
		trimmed := strings.Trim(s, " \"")
		if trimmed != "" {
			sources = append(sources, trimmed)
		}
	}
	return sources
}

// BuildTarget 构建目标
type BuildTarget struct {
	Sources      []string
	Version      string
	Output       *string
	Optimization uint8
}

// NewBuildTarget 创建默认构建目标
func NewBuildTarget() *BuildTarget {
	return &BuildTarget{
		Sources:      []string{},
		Version:      "0.0.1",
		Output:       nil,
		Optimization: 1,
	}
}

// OptGroup 返回优化级别分组
func (bt *BuildTarget) OptGroup() string {
	if bt.Optimization <= 4 {
		return "Debug"
	}
	if bt.Optimization <= 7 {
		return "Release"
	}
	return "Super"
}

// ModuleDecl 模块声明
type ModuleDecl struct {
	Info    string
	Name    string
	Sources []string
}

// VxsetConfig VX 设置配置
type VxsetConfig struct {
	Deadcode bool
	Cache    bool
	Shell    *string
}

// NewVxsetConfig 创建默认 VX 设置配置
func NewVxsetConfig() VxsetConfig {
	return VxsetConfig{
		Deadcode: true,
		Cache:    true,
		Shell:    nil,
	}
}

// VxCompilerSettings 编译器设置
type VxCompilerSettings struct {
	Target     *string `toml:"target"`
	StdlibPath *string `toml:"stdlib_path"`
}

// LinkerType 链接器类型
type LinkerType int

const (
	// LinkerBuiltin 内置链接器
	LinkerBuiltin LinkerType = iota
	// LinkerLLD LLD 链接器
	LinkerLLD
)

// UnmarshalText 实现 encoding.TextUnmarshaler，用于 TOML 解析
func (lt *LinkerType) UnmarshalText(text []byte) error {
	switch string(text) {
	case "builtin":
		*lt = LinkerBuiltin
	case "lld":
		*lt = LinkerLLD
	default:
		return fmt.Errorf("unknown linker type: %s", string(text))
	}
	return nil
}

// VxLinkerSettings 链接器设置
type VxLinkerSettings struct {
	Linker     LinkerType `toml:"linker"`
	EmbedVxobj bool       `toml:"embed_vxobj"`
}

// NewVxLinkerSettings 创建默认链接器设置
func NewVxLinkerSettings() *VxLinkerSettings {
	return &VxLinkerSettings{
		Linker:     LinkerBuiltin,
		EmbedVxobj: false,
	}
}

// VxMemorySettings 内存设置
type VxMemorySettings struct {
	StackSize uint64 `toml:"stack_size"`
	HeapSize  uint64 `toml:"heap_size"`
}

// NewVxMemorySettings 创建默认内存设置
func NewVxMemorySettings() *VxMemorySettings {
	return &VxMemorySettings{
		StackSize: 8 * 1024 * 1024,
		HeapSize:  64 * 1024 * 1024,
	}
}
