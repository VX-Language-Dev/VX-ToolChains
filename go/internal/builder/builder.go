package builder

import (
	"fmt"
	"os"
	"os/exec"
	"path/filepath"
	"strings"
)

// ==================== VxSettings ====================

// VxSettings VX 项目设置
type VxSettings struct {
	SourceDir string
	Modules   []string
	Bin       *BinTarget
	Vxset     VxSet
	Cache     bool
}

// VxSet VX 设置子结构
type VxSet struct {
	Deadcode bool
}

// BinTarget 二进制构建目标
type BinTarget struct {
	Sources      []string
	Optimization uint8
}

// IsMultiFileProject 检查是否为多文件项目
func (s *VxSettings) IsMultiFileProject() bool {
	return len(s.Modules) > 1 || (s.Bin != nil && len(s.Bin.Sources) > 1)
}

// CacheFilePath 返回缓存文件路径
func (s *VxSettings) CacheFilePath() string {
	return filepath.Join(s.SourceDir, ".vxbuild_cache")
}

// ==================== BuildError ====================

// BuildError 构建错误
type BuildError int

const (
	ErrConfig BuildError = iota
	ErrToolNotFound
	ErrCompileFailed
	ErrTranslateFailed
	ErrNoEntryPoint
	ErrInvalidSource
	ErrIoError
	ErrObjectFileWriteFailed
)

func (e BuildError) Error() string {
	switch e {
	case ErrConfig:
		return "configuration error"
	case ErrToolNotFound:
		return "tool not found"
	case ErrCompileFailed:
		return "compile failed"
	case ErrTranslateFailed:
		return "translation failed (VXOBJ → native)"
	case ErrNoEntryPoint:
		return "no entry point"
	case ErrInvalidSource:
		return "invalid source"
	case ErrIoError:
		return "IO error"
	case ErrObjectFileWriteFailed:
		return "object file write failed"
	default:
		return "unknown build error"
	}
}

// ==================== VxBuilder ====================

// VxBuilder VX 构建器
type VxBuilder struct {
	Settings      VxSettings
	SingleEntry   string
	ForceRebuild  bool
	NoCache       bool
	WarnDeadCode  *bool
	ErrorDeadCode *bool
}

// NewVxBuilder 创建新的构建器
func NewVxBuilder(settings VxSettings) *VxBuilder {
	return &VxBuilder{
		Settings: settings,
	}
}

// WithSingleEntry 设置单入口文件
func (b *VxBuilder) WithSingleEntry(entry string) *VxBuilder {
	b.SingleEntry = entry
	return b
}

// WithForceRebuild 设置强制重建
func (b *VxBuilder) WithForceRebuild(force bool) *VxBuilder {
	b.ForceRebuild = force
	return b
}

// WithNoCache 设置禁用缓存
func (b *VxBuilder) WithNoCache(noCache bool) *VxBuilder {
	b.NoCache = noCache
	return b
}

// WithWarnDeadCode 设置死代码警告
func (b *VxBuilder) WithWarnDeadCode(warn *bool) *VxBuilder {
	b.WarnDeadCode = warn
	return b
}

// WithErrorDeadCode 设置死代码错误
func (b *VxBuilder) WithErrorDeadCode(err *bool) *VxBuilder {
	b.ErrorDeadCode = err
	return b
}

func (b *VxBuilder) effectiveWarnDeadCode() bool {
	if b.WarnDeadCode != nil {
		return *b.WarnDeadCode
	}
	return !b.Settings.Vxset.Deadcode
}

func (b *VxBuilder) effectiveErrorDeadCodeFor(optLevel uint8) bool {
	if b.ErrorDeadCode != nil {
		return *b.ErrorDeadCode
	}
	return optLevel >= 8
}

// Build 执行构建
func (b *VxBuilder) Build() error {
	if b.Settings.IsMultiFileProject() {
		return b.buildMultiFile()
	}
	return b.buildSingleFile()
}

func (b *VxBuilder) buildSingleFile() error {
	entry, err := b.resolveSingleEntry()
	if err != nil {
		return err
	}
	_, err = b.runVxcompiler(entry, "", 1)
	return err
}

func (b *VxBuilder) resolveSingleEntry() (string, error) {
	if b.SingleEntry != "" {
		p := b.resolveSourcePath(b.SingleEntry)
		if _, err := os.Stat(p); err == nil {
			return b.SingleEntry, nil
		}
		return "", ErrInvalidSource
	}

	// 在 source_dir 中查找 .vx 文件
	entries, err := os.ReadDir(b.Settings.SourceDir)
	if err != nil {
		return "", ErrNoEntryPoint
	}

	var vxCandidates []string
	for _, e := range entries {
		if strings.HasSuffix(e.Name(), ".vx") {
			vxCandidates = append(vxCandidates, filepath.Join(b.Settings.SourceDir, e.Name()))
		}
	}

	switch len(vxCandidates) {
	case 0:
		return "", ErrNoEntryPoint
	case 1:
		return vxCandidates[0], nil
	default:
		fmt.Fprintf(os.Stderr, "[VXBUILD 警告] 发现多个 .vx 文件, 请通过 `vpm build <entry.vx>` 指定入口:\n")
		for _, f := range vxCandidates {
			fmt.Fprintf(os.Stderr, "  - %s\n", f)
		}
		return "", ErrNoEntryPoint
	}
}

func (b *VxBuilder) buildMultiFile() error {
	moduleLibs := make(map[string]string)

	if err := func() error {
		var _ map[string]string = moduleLibs
		return b.resolveModuleDependencies()
	}(); err != nil {
		return err
	}

	// 构建各目标
	if b.Settings.Bin != nil {
		bin := b.Settings.Bin
		if len(bin.Sources) > 0 {
			entry := b.resolveSourcePath(bin.Sources[0])
			obj := changeExtension(entry, "vxobj")
			output := b.resolveTargetOutput(bin, "bin")
			if _, err := b.runVxcompiler(entry, "", bin.Optimization); err != nil {
				return err
			}
			if err := b.runVxTranslator(obj, output); err != nil {
				return err
			}
		}
	}

	return nil
}

func (b *VxBuilder) resolveSourcePath(src string) string {
	if filepath.IsAbs(src) {
		return src
	}
	return filepath.Join(b.Settings.SourceDir, src)
}

func (b *VxBuilder) resolveTargetOutput(bin *BinTarget, kind string) string {
	_ = kind
	if len(bin.Sources) > 0 {
		base := filepath.Base(bin.Sources[0])
		name := strings.TrimSuffix(base, ".vx")
		return filepath.Join(b.Settings.SourceDir, name)
	}
	return filepath.Join(b.Settings.SourceDir, "output")
}

func (b *VxBuilder) runVxcompiler(source, extraLibsEnv string, optLevel uint8) (string, error) {
	tool := "vxcompiler"
	ext := filepath.Ext(source)
	outputObj := strings.TrimSuffix(source, ext) + ".vxobj"

	warnDC := b.effectiveWarnDeadCode()
	errDC := b.effectiveErrorDeadCodeFor(optLevel)

	args := []string{source, "-o", outputObj, "--opt-level", fmt.Sprintf("%d", optLevel)}

	if warnDC {
		args = append(args, "--warn-dead-code")
		if errDC {
			args = append(args, "--error-dead-code")
		}
	}

	cmd := exec.Command(tool, args...)

	// 设置环境变量
	cmd.Env = os.Environ()
	cmd.Env = append(cmd.Env, fmt.Sprintf("VX_OPT_LEVEL=%d", optLevel))
	if warnDC {
		cmd.Env = append(cmd.Env, "VX_WARN_DEAD_CODE=1")
	} else {
		cmd.Env = append(cmd.Env, "VX_WARN_DEAD_CODE=0")
	}
	if errDC {
		cmd.Env = append(cmd.Env, "VX_ERROR_DEAD_CODE=1")
	} else {
		cmd.Env = append(cmd.Env, "VX_ERROR_DEAD_CODE=0")
	}
	if extraLibsEnv != "" {
		cmd.Env = append(cmd.Env, "VX_EXTRA_LIBS="+extraLibsEnv)
	}

	cmd.Stderr = os.Stderr

	if err := cmd.Run(); err != nil {
		if os.IsNotExist(err) {
			return "", ErrToolNotFound
		}
		return "", ErrCompileFailed
	}

	return outputObj, nil
}

func (b *VxBuilder) runVxTranslator(objPath, outputPath string) error {
	tool := "vlnk"
	args := []string{"-o", outputPath, objPath}

	cmd := exec.Command(tool, args...)
	cmd.Stderr = os.Stderr

	if err := cmd.Run(); err != nil {
		if os.IsNotExist(err) {
			return ErrToolNotFound
		}
		return ErrTranslateFailed
	}

	return nil
}

func (b *VxBuilder) resolveModuleDependencies() error {
	if len(b.Settings.Modules) == 0 {
		return nil
	}

	vxmodPath := filepath.Join(b.Settings.SourceDir, "vxmod.toml")
	content, err := os.ReadFile(vxmodPath)
	if err != nil {
		if os.IsNotExist(err) {
			fmt.Fprintf(os.Stderr, "[VXBUILD 警告] 未找到 vxmod.toml, 无法解析模块依赖\n")
			return nil
		}
		return ErrIoError
	}

	packages := ParseVxmod(string(content))
	if len(packages) == 0 {
		fmt.Fprintf(os.Stderr, "[VXBUILD 警告] vxmod.toml 中未定义任何包\n")
		return nil
	}

	for _, pkg := range packages {
		fmt.Fprintf(os.Stderr, "[VXBUILD] 已加载模块: %s (version: %s, path: %s)\n", pkg.Name, pkg.Version, pkg.Path)
	}

	return nil
}

func changeExtension(path, newExt string) string {
	ext := filepath.Ext(path)
	return strings.TrimSuffix(path, ext) + "." + newExt
}

// ==================== InstalledPackage ====================

// InstalledPackage 已安装的包
type InstalledPackage struct {
	Name     string
	Path     string
	Version  string
	Language string
}

// ParseVxmod 解析 vxmod.toml 文件
func ParseVxmod(content string) []InstalledPackage {
	var packages []InstalledPackage
	var current *InstalledPackage

	lines := strings.Split(content, "\n")
	for _, line := range lines {
		trimmed := strings.TrimSpace(line)
		if trimmed == "" || strings.HasPrefix(trimmed, "#") {
			continue
		}

		if strings.HasPrefix(trimmed, "[") && strings.HasSuffix(trimmed, "]") {
			if current != nil {
				packages = append(packages, *current)
			}
			name := trimmed[1 : len(trimmed)-1]
			current = &InstalledPackage{
				Name: name,
			}
			continue
		}

		if eqPos := strings.Index(trimmed, "="); eqPos >= 0 {
			key := strings.TrimSpace(trimmed[:eqPos])
			val := strings.TrimSpace(trimmed[eqPos+1:])
			val = strings.Trim(val, "\"")

			if current != nil {
				switch key {
				case "path":
					current.Path = val
				case "version":
					current.Version = val
				case "language":
					current.Language = val
				}
			}
		}
	}

	if current != nil {
		packages = append(packages, *current)
	}

	return packages
}
