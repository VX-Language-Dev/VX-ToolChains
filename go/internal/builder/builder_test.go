package builder

import (
	"os"
	"path/filepath"
	"testing"
)

func TestParseVxmodEmpty(t *testing.T) {
	packages := ParseVxmod("")
	if len(packages) != 0 {
		t.Errorf("expected 0 packages, got %d", len(packages))
	}
}

func TestParseVxmodCommentsOnly(t *testing.T) {
	content := `# This is a comment
# Another comment
`
	packages := ParseVxmod(content)
	if len(packages) != 0 {
		t.Errorf("expected 0 packages, got %d", len(packages))
	}
}

func TestParseVxmodSinglePackage(t *testing.T) {
	content := `[mylib]
path = "/usr/local/vx/libs/mylib"
version = "1.0.0"
language = "vx"
`
	packages := ParseVxmod(content)
	if len(packages) != 1 {
		t.Fatalf("expected 1 package, got %d", len(packages))
	}
	if packages[0].Name != "mylib" {
		t.Errorf("expected name 'mylib', got %q", packages[0].Name)
	}
	if packages[0].Path != "/usr/local/vx/libs/mylib" {
		t.Errorf("expected path '/usr/local/vx/libs/mylib', got %q", packages[0].Path)
	}
	if packages[0].Version != "1.0.0" {
		t.Errorf("expected version '1.0.0', got %q", packages[0].Version)
	}
	if packages[0].Language != "vx" {
		t.Errorf("expected language 'vx', got %q", packages[0].Language)
	}
}

func TestParseVxmodMultiplePackages(t *testing.T) {
	content := `[stdlib]
path = "/usr/local/vx/stdlib"
version = "1.2.0"
language = "vx"

[mathlib]
path = "/usr/local/vx/math"
version = "0.9.0"
language = "vx"
`
	packages := ParseVxmod(content)
	if len(packages) != 2 {
		t.Fatalf("expected 2 packages, got %d", len(packages))
	}
	if packages[0].Name != "stdlib" {
		t.Errorf("expected first package name 'stdlib', got %q", packages[0].Name)
	}
	if packages[1].Name != "mathlib" {
		t.Errorf("expected second package name 'mathlib', got %q", packages[1].Name)
	}
}

func TestParseVxmodWithMinimalConfig(t *testing.T) {
	content := `[mypkg]
path = "/tmp/mypkg"
`
	packages := ParseVxmod(content)
	if len(packages) != 1 {
		t.Fatalf("expected 1 package, got %d", len(packages))
	}
	if packages[0].Name != "mypkg" {
		t.Errorf("expected name 'mypkg', got %q", packages[0].Name)
	}
	if packages[0].Path != "/tmp/mypkg" {
		t.Errorf("expected path '/tmp/mypkg', got %q", packages[0].Path)
	}
	if packages[0].Version != "" {
		t.Errorf("expected empty version, got %q", packages[0].Version)
	}
}

func TestVxSettingsIsMultiFileProject(t *testing.T) {
	tests := []struct {
		settings VxSettings
		multi    bool
	}{
		{VxSettings{}, false},
		{VxSettings{Modules: []string{"a"}}, false},
		{VxSettings{Modules: []string{"a", "b"}}, true},
		{VxSettings{Bin: &BinTarget{Sources: []string{"main.vx"}}}, false},
		{VxSettings{Bin: &BinTarget{Sources: []string{"main.vx", "lib.vx"}}}, true},
		{VxSettings{Modules: []string{"a", "b"}, Bin: &BinTarget{Sources: []string{"main.vx"}}}, true},
	}

	for i, tt := range tests {
		result := tt.settings.IsMultiFileProject()
		if result != tt.multi {
			t.Errorf("tests[%d]: expected IsMultiFileProject()=%v, got %v", i, tt.multi, result)
		}
	}
}

func TestChangeExtension(t *testing.T) {
	tests := []struct {
		path    string
		newExt  string
		expect  string
	}{
		{"main.vx", "vxobj", "main.vxobj"},
		{"foo/bar.vx", "ll", "foo/bar.ll"},
		{"noext", "txt", "noext.txt"},
		{"/path/to/file.vx", "o", "/path/to/file.o"},
	}

	for _, tt := range tests {
		result := changeExtension(tt.path, tt.newExt)
		if result != tt.expect {
			t.Errorf("changeExtension(%q, %q) = %q, want %q", tt.path, tt.newExt, result, tt.expect)
		}
	}
}

func TestResolveSourcePath(t *testing.T) {
	tmpDir := t.TempDir()
	b := &VxBuilder{
		Settings: VxSettings{SourceDir: tmpDir},
	}

	// Test absolute path
	absResult := b.resolveSourcePath("/absolute/path.vx")
	if absResult != "/absolute/path.vx" {
		t.Errorf("expected absolute path unchanged, got %q", absResult)
	}

	// Test relative path
	relResult := b.resolveSourcePath("file.vx")
	expected := filepath.Join(tmpDir, "file.vx")
	if relResult != expected {
		t.Errorf("expected %q, got %q", expected, relResult)
	}
}

func TestResolveModuleDependenciesNoVxmod(t *testing.T) {
	tmpDir := t.TempDir()
	b := &VxBuilder{
		Settings: VxSettings{
			SourceDir: tmpDir,
			Modules:   []string{"testmod"},
		},
	}

	err := b.resolveModuleDependencies()
	if err != nil {
		t.Errorf("expected no error when vxmod.toml doesn't exist, got %v", err)
	}
}

func TestResolveSingleEntryWithFile(t *testing.T) {
	tmpDir := t.TempDir()
	entryPath := filepath.Join(tmpDir, "main.vx")
	if err := os.WriteFile(entryPath, []byte("func main():\n    out(\"hello\")"), 0644); err != nil {
		t.Fatalf("failed to write file: %v", err)
	}

	b := &VxBuilder{
		Settings: VxSettings{SourceDir: tmpDir},
	}

	entry, err := b.resolveSingleEntry()
	if err != nil {
		t.Fatalf("resolveSingleEntry failed: %v", err)
	}
	if entry != entryPath {
		t.Errorf("expected entry %q, got %q", entryPath, entry)
	}
}

func TestResolveSingleEntryWithSingleEntry(t *testing.T) {
	b := &VxBuilder{
		Settings:    VxSettings{SourceDir: "/tmp"},
		SingleEntry: "/tmp/explicit.vx",
	}

	entry, err := b.resolveSingleEntry()
	if err != ErrInvalidSource {
		// Should get ErrInvalidSource since /tmp/explicit.vx doesn't exist
		if err == nil {
			t.Error("expected error for non-existent file")
		}
	}
	_ = entry
}

func TestResolveSingleEntryNoFiles(t *testing.T) {
	tmpDir := t.TempDir()
	b := &VxBuilder{
		Settings: VxSettings{SourceDir: tmpDir},
	}

	_, err := b.resolveSingleEntry()
	if err != ErrNoEntryPoint {
		t.Errorf("expected ErrNoEntryPoint, got %v", err)
	}
}

func TestNewVxBuilder(t *testing.T) {
	settings := VxSettings{SourceDir: "/test"}
	b := NewVxBuilder(settings)
	if b == nil {
		t.Fatal("expected non-nil builder")
	}
	if b.Settings.SourceDir != "/test" {
		t.Errorf("expected SourceDir '/test', got %q", b.Settings.SourceDir)
	}

	b2 := b.WithSingleEntry("main.vx")
	if b2 != b {
		t.Error("WithSingleEntry should return the same builder")
	}
	if b.SingleEntry != "main.vx" {
		t.Errorf("expected SingleEntry 'main.vx', got %q", b.SingleEntry)
	}

	b3 := b.WithForceRebuild(true)
	if b3 != b {
		t.Error("WithForceRebuild should return the same builder")
	}
	if !b.ForceRebuild {
		t.Error("expected ForceRebuild true")
	}

	b4 := b.WithNoCache(true)
	if b4 != b {
		t.Error("WithNoCache should return the same builder")
	}
	if !b.NoCache {
		t.Error("expected NoCache true")
	}
}
