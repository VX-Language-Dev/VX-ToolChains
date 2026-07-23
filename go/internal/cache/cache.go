package cache

import (
	"crypto/sha256"
	"encoding/json"
	"fmt"
	"os"
	"path/filepath"
	"time"
)

// SourceInfo 源文件信息
type SourceInfo struct {
	Path        string `json:"path"`
	Fingerprint string `json:"fingerprint"`
	ContentHash string `json:"content_hash"`
}

// CacheEntry 缓存条目
type CacheEntry struct {
	Key       string       `json:"key"`
	Sources   []SourceInfo `json:"sources"`
	ObjPath   string       `json:"obj_path"`
	Output    string       `json:"output"`
	OptLevel  uint8        `json:"opt_level"`
	LastBuild int64        `json:"last_build"`
	IsValid   bool         `json:"is_valid"`
}

// CacheFile 缓存文件结构
type CacheFile struct {
	Version          string                `json:"version"`
	VxsettingHash    string                `json:"vxsetting_hash"`
	VxmodHash        *string               `json:"vxmod_hash,omitempty"`
	ToolchainVersion string                `json:"toolchain_version"`
	Targets          map[string]CacheEntry `json:"targets"`
}

// BuildCache 构建缓存
type BuildCache struct {
	CacheDir string
	File     CacheFile
	Enabled  bool
}

const (
	// CacheVersion 缓存版本
	CacheVersion = "2"
	// ToolchainVersion 工具链版本
	ToolchainVersion = "1.6.0"
)

// NewBuildCache 创建新的 BuildCache 实例
func NewBuildCache(cacheDir string) *BuildCache {
	return &BuildCache{
		CacheDir: cacheDir,
		File: CacheFile{
			Version:          CacheVersion,
			VxsettingHash:    "",
			VxmodHash:        nil,
			ToolchainVersion: ToolchainVersion,
			Targets:          make(map[string]CacheEntry),
		},
		Enabled: true,
	}
}

// Disable 禁用缓存
func (bc *BuildCache) Disable() {
	bc.Enabled = false
}

// IsEnabled 检查缓存是否启用
func (bc *BuildCache) IsEnabled() bool {
	return bc.Enabled
}

// SetMeta 设置元数据
func (bc *BuildCache) SetMeta(vxsettingHash string, vxmodHash *string, toolchainVersion string) {
	bc.File.VxsettingHash = vxsettingHash
	bc.File.VxmodHash = vxmodHash
	bc.File.ToolchainVersion = toolchainVersion
}

// IsGloballyValid 检查全局缓存是否有效
func (bc *BuildCache) IsGloballyValid(vxsettingHash string, vxmodHash *string, toolchainVersion string) bool {
	if bc.File.VxsettingHash != vxsettingHash {
		return false
	}
	if vxmodHash != nil {
		if bc.File.VxmodHash == nil {
			return false
		}
		if *bc.File.VxmodHash != *vxmodHash {
			return false
		}
	}
	if bc.File.ToolchainVersion != toolchainVersion {
		return false
	}
	return true
}

// InvalidateAll 使所有缓存条目失效
func (bc *BuildCache) InvalidateAll() {
	bc.File.Targets = make(map[string]CacheEntry)
	bc.File.VxsettingHash = ""
	bc.File.VxmodHash = nil
}

// IsTargetFresh 检查目标是否仍然新鲜
func (bc *BuildCache) IsTargetFresh(key string, sources []string, optLevel uint8) bool {
	entry, ok := bc.File.Targets[key]
	if !ok {
		return false
	}
	if entry.OptLevel != optLevel {
		return false
	}
	if !entry.IsValid {
		return false
	}
	if len(entry.Sources) != len(sources) {
		return false
	}

	for i, src := range entry.Sources {
		if src.Path != sources[i] {
			return false
		}

		currentFP := fileFingerprint(sources[i])
		if currentFP == "" {
			return false
		}

		fpMatch := src.Fingerprint == currentFP

		if !fpMatch {
			// 快速指纹不匹配，回退到内容哈希比对
			currentHash := fileContentHash(sources[i])
			if currentHash == "" {
				return false
			}
			if src.ContentHash != currentHash {
				return false
			}
		}
	}

	return true
}

// IsUpToDate 检查目标是否仍然新鲜（IsTargetFresh 的别名）
func (bc *BuildCache) IsUpToDate(key string, sources []string, optLevel uint8) bool {
	return bc.IsTargetFresh(key, sources, optLevel)
}

// UpdateEntry 更新缓存条目
func (bc *BuildCache) UpdateEntry(key string, sources []string, objPath string, output string, optLevel uint8) error {
	// 释放旧 entry（如果存在）
	delete(bc.File.Targets, key)

	sourceInfos := make([]SourceInfo, 0, len(sources))
	for _, src := range sources {
		fp := fileFingerprint(src)
		ch := fileContentHash(src)
		sourceInfos = append(sourceInfos, SourceInfo{
			Path:        src,
			Fingerprint: fp,
			ContentHash: ch,
		})
	}

	bc.File.Targets[key] = CacheEntry{
		Key:       key,
		Sources:   sourceInfos,
		ObjPath:   objPath,
		Output:    output,
		OptLevel:  optLevel,
		LastBuild: time.Now().UnixMicro(),
		IsValid:   true,
	}
	return nil
}

// Find 查找指定 key 的缓存条目
func (bc *BuildCache) Find(key string) (CacheEntry, bool) {
	entry, ok := bc.File.Targets[key]
	return entry, ok
}

// LoadFromDisk 从磁盘加载缓存（JSON 格式）
func (bc *BuildCache) LoadFromDisk() error {
	cachePath := filepath.Join(bc.CacheDir, ".vxbuild_cache")

	content, err := os.ReadFile(cachePath)
	if err != nil {
		if os.IsNotExist(err) {
			return nil // 文件不存在时静默返回
		}
		return err
	}

	var file CacheFile
	if err := json.Unmarshal(content, &file); err != nil {
		return err
	}

	if file.Targets == nil {
		file.Targets = make(map[string]CacheEntry)
	}
	bc.File = file
	return nil
}

// SaveToDisk 保存缓存到磁盘（JSON 格式）
func (bc *BuildCache) SaveToDisk() error {
	cachePath := filepath.Join(bc.CacheDir, ".vxbuild_cache")

	// 确保缓存目录存在
	if err := os.MkdirAll(bc.CacheDir, 0755); err != nil {
		return err
	}

	data, err := json.MarshalIndent(bc.File, "", "  ")
	if err != nil {
		return err
	}

	return os.WriteFile(cachePath, data, 0644)
}

// ComputeFingerprint 计算指定文件的快速指纹
func ComputeFingerprint(path string) string {
	return fileFingerprint(path)
}

// fileFingerprint 使用 mtime 作为快速指纹
func fileFingerprint(path string) string {
	info, err := os.Stat(path)
	if err != nil {
		return ""
	}
	return fmt.Sprintf("%d", info.ModTime().UnixNano())
}

// fileContentHash 计算文件内容的 SHA-256 哈希
func fileContentHash(path string) string {
	data, err := os.ReadFile(path)
	if err != nil {
		return ""
	}
	h := sha256.Sum256(data)
	return fmt.Sprintf("%x", h)
}
