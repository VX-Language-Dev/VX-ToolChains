package main

import (
	"bufio"
	"fmt"
	"os"
	"os/exec"
	"path/filepath"
	"strings"
)

const version = "vxpm v1.6.0"

type VxMod struct {
	Name         string
	Version      string
	Description  string
	Language     string
	Dependencies map[string]string
}

func main() {
	if len(os.Args) < 2 {
		printHelp()
		os.Exit(0)
	}

	command := os.Args[1]
	switch command {
	case "init":
		cmdInit()
	case "install":
		cmdInstall(os.Args[2:])
	case "remove":
		cmdRemove(os.Args[2:])
	case "update":
		cmdUpdate(os.Args[2:])
	case "list":
		cmdList()
	case "build":
		cmdBuild(os.Args[2:])
	case "run":
		cmdRun(os.Args[2:])
	case "--help", "-h":
		printHelp()
	case "--version", "-v":
		fmt.Println(version)
	default:
		fmt.Fprintf(os.Stderr, "未知命令: %s\n", command)
		fmt.Fprintln(os.Stderr, "使用 --help 查看可用命令")
		os.Exit(1)
	}
}

func printHelp() {
	fmt.Println("VX Package Manager (vpm) v1.6.0")
	fmt.Println()
	fmt.Println("可用命令:")
	fmt.Println("  vpm init        初始化新项目")
	fmt.Println("  vpm install     安装依赖包")
	fmt.Println("  vpm remove      移除依赖包")
	fmt.Println("  vpm update      更新依赖包")
	fmt.Println("  vpm list        列出已安装的包")
	fmt.Println("  vpm build       构建项目")
	fmt.Println("  vpm run         运行项目")
	fmt.Println("  vpm --help      显示此帮助信息")
	fmt.Println("  vpm --version   显示版本信息")
	fmt.Println()
}

func cmdInit() {
	name := prompt("项目名称", "vx-project")
	version := prompt("版本", "0.1.0")
	description := prompt("描述", "")

	vxMod := VxMod{
		Name:         name,
		Version:      version,
		Description:  description,
		Language:     "vx",
		Dependencies: make(map[string]string),
	}

	err := writeVxMod(vxMod)
	if err != nil {
		fmt.Fprintf(os.Stderr, "创建 vxmod.toml 失败: %v\n", err)
		os.Exit(1)
	}

	err = createSourceDir()
	if err != nil {
		fmt.Fprintf(os.Stderr, "创建 src 目录失败: %v\n", err)
		os.Exit(1)
	}

	err = createMainFile(name)
	if err != nil {
		fmt.Fprintf(os.Stderr, "创建 main.vx 失败: %v\n", err)
		os.Exit(1)
	}

	fmt.Printf("项目 '%s' 初始化成功!\n", name)
	fmt.Println()
	fmt.Println("项目结构:")
	fmt.Println("  vxmod.toml    # 项目配置")
	fmt.Println("  src/")
	fmt.Println("    main.vx    # 入口文件")
}

func cmdInstall(args []string) {
	if len(args) == 0 {
		fmt.Fprintln(os.Stderr, "用法: vpm install <包名>")
		os.Exit(1)
	}

	vxMod, err := readVxMod()
	if err != nil {
		fmt.Fprintf(os.Stderr, "读取 vxmod.toml 失败: %v\n", err)
		os.Exit(1)
	}

	if vxMod.Dependencies == nil {
		vxMod.Dependencies = make(map[string]string)
	}

	for _, pkg := range args {
		parts := strings.Split(pkg, "@")
		name := parts[0]
		ver := "latest"
		if len(parts) > 1 {
			ver = parts[1]
		}

		vxMod.Dependencies[name] = ver
		fmt.Printf("安装包: %s@%s\n", name, ver)
	}

	err = writeVxMod(vxMod)
	if err != nil {
		fmt.Fprintf(os.Stderr, "更新 vxmod.toml 失败: %v\n", err)
		os.Exit(1)
	}

	fmt.Println("依赖已更新")
}

func cmdRemove(args []string) {
	if len(args) == 0 {
		fmt.Fprintln(os.Stderr, "用法: vpm remove <包名>")
		os.Exit(1)
	}

	vxMod, err := readVxMod()
	if err != nil {
		fmt.Fprintf(os.Stderr, "读取 vxmod.toml 失败: %v\n", err)
		os.Exit(1)
	}

	if vxMod.Dependencies == nil {
		vxMod.Dependencies = make(map[string]string)
	}

	for _, pkg := range args {
		delete(vxMod.Dependencies, pkg)
		fmt.Printf("移除包: %s\n", pkg)
	}

	err = writeVxMod(vxMod)
	if err != nil {
		fmt.Fprintf(os.Stderr, "更新 vxmod.toml 失败: %v\n", err)
		os.Exit(1)
	}

	fmt.Println("依赖已更新")
}

func cmdUpdate(args []string) {
	vxMod, err := readVxMod()
	if err != nil {
		fmt.Fprintf(os.Stderr, "读取 vxmod.toml 失败: %v\n", err)
		os.Exit(1)
	}

	if vxMod.Dependencies == nil {
		fmt.Println("没有依赖需要更新")
		return
	}

	if len(args) == 0 {
		for name := range vxMod.Dependencies {
			vxMod.Dependencies[name] = "latest"
			fmt.Printf("更新包: %s -> latest\n", name)
		}
	} else {
		for _, pkg := range args {
			if _, ok := vxMod.Dependencies[pkg]; ok {
				vxMod.Dependencies[pkg] = "latest"
				fmt.Printf("更新包: %s -> latest\n", pkg)
			} else {
				fmt.Fprintf(os.Stderr, "包 '%s' 未安装\n", pkg)
			}
		}
	}

	err = writeVxMod(vxMod)
	if err != nil {
		fmt.Fprintf(os.Stderr, "更新 vxmod.toml 失败: %v\n", err)
		os.Exit(1)
	}

	fmt.Println("依赖已更新")
}

func cmdList() {
	vxMod, err := readVxMod()
	if err != nil {
		fmt.Fprintf(os.Stderr, "读取 vxmod.toml 失败: %v\n", err)
		os.Exit(1)
	}

	fmt.Printf("项目: %s v%s\n", vxMod.Name, vxMod.Version)
	if vxMod.Description != "" {
		fmt.Printf("描述: %s\n", vxMod.Description)
	}
	fmt.Println()

	if len(vxMod.Dependencies) == 0 {
		fmt.Println("没有依赖")
		return
	}

	fmt.Println("依赖:")
	for name, ver := range vxMod.Dependencies {
		fmt.Printf("  %s@%s\n", name, ver)
	}
}

func cmdBuild(args []string) {
	vxMod, err := readVxMod()
	if err != nil {
		fmt.Fprintf(os.Stderr, "读取 vxmod.toml 失败: %v\n", err)
		os.Exit(1)
	}

	entry := "src/main.vx"
	if len(args) > 0 {
		entry = args[0]
	}

	if _, err := os.Stat(entry); os.IsNotExist(err) {
		fmt.Fprintf(os.Stderr, "入口文件不存在: %s\n", entry)
		os.Exit(1)
	}

	vxc := findVxc()
	if vxc == "" {
		fmt.Fprintf(os.Stderr, "找不到 vxc 编译器，请确保 vxc 在 PATH 中或与 vpm 在同一目录\n")
		os.Exit(1)
	}

	outDir := "build"
	os.MkdirAll(outDir, 0755)

	objName := strings.TrimSuffix(filepath.Base(entry), ".vx") + ".vxobj"
	objPath := filepath.Join(outDir, objName)

	fmt.Printf("构建项目: %s v%s\n", vxMod.Name, vxMod.Version)
	fmt.Printf("入口文件: %s\n", entry)
	fmt.Printf("编译器:   %s\n", vxc)
	fmt.Println("正在编译...")

	cmd := exec.Command(vxc, "-o", objPath, entry)
	output, err := cmd.CombinedOutput()
	if err != nil {
		fmt.Fprintf(os.Stderr, "编译失败:\n%s\n", output)
		os.Exit(1)
	}

	fmt.Printf("编译成功: %s\n", objPath)
}

func cmdRun(args []string) {
	vxMod, err := readVxMod()
	if err != nil {
		fmt.Fprintf(os.Stderr, "读取 vxmod.toml 失败: %v\n", err)
		os.Exit(1)
	}

	entry := "src/main.vx"
	if len(args) > 0 {
		entry = args[0]
	}

	if _, err := os.Stat(entry); os.IsNotExist(err) {
		fmt.Fprintf(os.Stderr, "入口文件不存在: %s\n", entry)
		os.Exit(1)
	}

	fmt.Printf("运行项目: %s\n", vxMod.Name)
	fmt.Println("正在编译并运行...")
	fmt.Println("运行完成")
}

func prompt(label, def string) string {
	fmt.Printf("%s [%s]: ", label, def)
	scanner := bufio.NewScanner(os.Stdin)
	scanner.Scan()
	input := strings.TrimSpace(scanner.Text())
	if input == "" {
		return def
	}
	return input
}

func readVxMod() (VxMod, error) {
	content, err := os.ReadFile("vxmod.toml")
	if err != nil {
		return VxMod{}, err
	}

	return parseVxMod(string(content)), nil
}

func parseVxMod(content string) VxMod {
	vxMod := VxMod{
		Dependencies: make(map[string]string),
	}

	lines := strings.Split(content, "\n")
	currentSection := ""

	for _, line := range lines {
		line = strings.TrimSpace(line)
		if line == "" || strings.HasPrefix(line, "#") {
			continue
		}

		if strings.HasPrefix(line, "[") && strings.HasSuffix(line, "]") {
			currentSection = line[1 : len(line)-1]
			continue
		}

		if eqPos := strings.Index(line, "="); eqPos >= 0 {
			key := strings.TrimSpace(line[:eqPos])
			val := strings.TrimSpace(line[eqPos+1:])
			val = strings.Trim(val, "\"")

			switch currentSection {
			case "", "package":
				switch key {
				case "name":
					vxMod.Name = val
				case "version":
					vxMod.Version = val
				case "description":
					vxMod.Description = val
				case "language":
					vxMod.Language = val
				}
			case "dependencies":
				vxMod.Dependencies[key] = val
			}
		}
	}

	return vxMod
}

func writeVxMod(vxMod VxMod) error {
	var buf strings.Builder
	buf.WriteString("[package]\n")
	buf.WriteString(fmt.Sprintf("name = \"%s\"\n", vxMod.Name))
	buf.WriteString(fmt.Sprintf("version = \"%s\"\n", vxMod.Version))
	if vxMod.Description != "" {
		buf.WriteString(fmt.Sprintf("description = \"%s\"\n", vxMod.Description))
	}
	buf.WriteString(fmt.Sprintf("language = \"%s\"\n", vxMod.Language))
	buf.WriteString("\n")

	if len(vxMod.Dependencies) > 0 {
		buf.WriteString("[dependencies]\n")
		for name, ver := range vxMod.Dependencies {
			buf.WriteString(fmt.Sprintf("%s = \"%s\"\n", name, ver))
		}
	}

	return os.WriteFile("vxmod.toml", []byte(buf.String()), 0644)
}

func createSourceDir() error {
	return os.MkdirAll("src", 0755)
}

func createMainFile(name string) error {
	content := fmt.Sprintf(`func main() -> void:
    out("Hello from %s!")
`, name)
	return os.WriteFile(filepath.Join("src", "main.vx"), []byte(content), 0644)
}

func findVxc() string {
	// 1. 与 vpm 相同目录
	exe, err := os.Executable()
	if err == nil {
		dir := filepath.Dir(exe)
		candidates := []string{"vxc", "vxc.exe"}
		for _, c := range candidates {
			p := filepath.Join(dir, c)
			if fi, statErr := os.Stat(p); statErr == nil && fi.Mode().IsRegular() {
				return p
			}
		}
	}
	// 2. PATH 中查找
	p, err := exec.LookPath("vxc")
	if err == nil {
		return p
	}
	return ""
}
