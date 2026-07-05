// ==================== VX 项目配置解析 (vxsetting.toml) ====================
// 替代已废弃的 vxmodel / vxmodel.toml 配置文件
//
// 语法结构 (见 vxset.toml语法模板.toml):
//   [bin]     编译为可执行文件 (sources 可多个, 仅一个 main)
//   [vxlib]   编译为 vx 库
//   [lib]     编译为库 (万能 C 调用接口)
//   [module]  声明模块 (可多个 [[module]])
//   [vxset]   构建器与编译器全局配置
//   [libraries]  (向后兼容) 库名→路径映射, 供 import 语句查询

use std::collections::HashMap;
use std::path::{Path, PathBuf};

// ==================== 默认值 ====================

const DEFAULT_VERSION: &str = "0.0.1";
const DEFAULT_OPT_LEVEL: u8 = 20;
const MAX_OPT_LEVEL: u8 = 20;
/// 默认构建产物目录: vpmfile/build
const DEFAULT_BUILD_DIR: &str = "vpmfile/build";
/// 缓存目录与文件: vpmfile/vpm/buildcache.toml
const CACHE_DIR: &str = "vpmfile/vpm";
const CACHE_FILE: &str = "buildcache.toml";

// ==================== 顶层配置结构 ====================

/// VX 项目配置结构体, 对应 `vxsetting.toml`
#[derive(Debug, Clone)]
pub struct VxSettings {
    /// 配置文件所在目录 (用于相对路径解析)
    pub source_dir: String,

    /// `[bin]` 可执行文件构建目标
    pub bin: Option<BuildTarget>,
    /// `[vxlib]` vx 库构建目标
    pub vxlib: Option<BuildTarget>,
    /// `[lib]` C 调用接口库构建目标
    pub lib: Option<BuildTarget>,
    /// `[[module]]` 模块声明列表 (TOML 数组表, 支持多个)
    pub modules: Vec<ModuleDecl>,
    /// `[vxset]` 构建器/编译器全局配置
    pub vxset: VxsetConfig,

    /// `[libraries]` 库名→路径映射 (向后兼容旧 vxmodel 查询语义)
    pub libraries: HashMap<String, String>,

    // === 预留新语法接口 (具体规则待定) ===
    pub compiler: Option<VxCompilerSettings>,
    pub linker: Option<VxLinkerSettings>,
    pub memory: Option<VxMemorySettings>,
}

// ==================== 构建目标 ====================

/// 单个构建目标 ([bin] / [vxlib] / [lib])
#[derive(Debug, Clone)]
pub struct BuildTarget {
    /// 源文件路径列表 (支持多文件, [bin] 要求仅一个 main)
    pub sources: Vec<String>,
    /// 版本号, 默认 "0.0.1"
    pub version: String,
    /// 输出路径; None 时使用默认 vpmfile/build/<目标名>
    pub output: Option<String>,
}

/// 模块声明 ([[module]])
#[derive(Debug, Clone)]
pub struct ModuleDecl {
    /// info.toml 元数据路径
    pub info: String,
    /// 模块在 VX import 语句中的调用名称
    pub name: String,
    /// 模块源文件路径列表
    pub sources: Vec<String>,
}

/// `[vxset]` 全局配置
#[derive(Debug, Clone)]
pub struct VxsetConfig {
    /// 允许死代码 (true 时编译器不报警告)
    pub deadcode: bool,
    /// 开启构建缓存 (vpmfile/vpm/buildcache.toml)
    pub cache: bool,
    /// 指定 shell; None 时使用 $SHELL
    pub shell: Option<String>,
    /// 优化等级 0-20, 默认 20
    pub optimization: u8,
}

impl Default for VxsetConfig {
    fn default() -> Self {
        Self {
            deadcode: true,
            cache: true,
            shell: None,
            optimization: DEFAULT_OPT_LEVEL,
        }
    }
}

// ==================== 预留子结构 ====================

/// 编译器配置子结构 (预留, 暂不实现)
#[derive(Debug, Clone, Default)]
pub struct VxCompilerSettings {
    // TODO: 待定 — 如目标三元组、警告级别等
}

/// 链接器配置子结构 (预留, 暂不实现)
#[derive(Debug, Clone, Default)]
pub struct VxLinkerSettings {
    // TODO: 待定 — 如链接模式、运行时 stub 路径等
}

/// 内存安全模型配置子结构 (预留, 暂不实现)
#[derive(Debug, Clone, Default)]
pub struct VxMemorySettings {
    // TODO: 待定 — 如默认所有权模式、GC 策略等
}

// ==================== 构建目标类型 ====================

/// 构建目标类型枚举, 用于区分产物形态
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum TargetKind {
    Bin,
    VxLib,
    Lib,
}

impl TargetKind {
    pub fn as_str(&self) -> &'static str {
        match self {
            TargetKind::Bin => "bin",
            TargetKind::VxLib => "vxlib",
            TargetKind::Lib => "lib",
        }
    }
}

// ==================== 解析实现 ====================

impl VxSettings {
    /// 从 `vxsetting.toml` 文件路径加载配置
    pub fn from_file(path: &str) -> Result<Self, String> {
        let abs = Path::new(path)
            .canonicalize()
            .unwrap_or_else(|_| PathBuf::from(path));
        let content = std::fs::read_to_string(&abs)
            .map_err(|e| format!("无法读取 vxsetting.toml: {}", e))?;

        let parsed: toml::Value = content
            .parse()
            .map_err(|e| format!("vxsetting.toml 解析失败: {}", e))?;

        let source_dir = abs
            .parent()
            .map(|p| p.to_string_lossy().to_string())
            .unwrap_or_default();

        Self::from_toml_value(&parsed, &source_dir)
    }

    /// 从已解析的 TOML Value 构建配置 (供测试与构建器复用)
    pub fn from_toml_value(parsed: &toml::Value, source_dir: &str) -> Result<Self, String> {
        let root = parsed
            .as_table()
            .ok_or_else(|| "vxsetting.toml 顶层必须是表结构".to_string())?;

        // [libraries] 向后兼容
        let mut libraries = HashMap::new();
        if let Some(toml::Value::Table(lib_table)) = root.get("libraries") {
            for (k, v) in lib_table {
                if let Some(s) = v.as_str() {
                    libraries.insert(k.clone(), s.to_string());
                } else {
                    return Err(format!(
                        "vxsetting.toml: libraries.{} 必须是字符串类型",
                        k
                    ));
                }
            }
        }

        let bin = parse_build_target(root.get("bin"), "bin")?;
        let vxlib = parse_build_target(root.get("vxlib"), "vxlib")?;
        let lib = parse_build_target(root.get("lib"), "lib")?;

        // [[module]] 数组表
        let mut modules = Vec::new();
        match root.get("module") {
            Some(toml::Value::Array(arr)) => {
                for (i, item) in arr.iter().enumerate() {
                    modules.push(parse_module_decl(item, i)?);
                }
            }
            Some(toml::Value::Table(t)) => {
                // 兼容单 [module] 写法
                modules.push(parse_module_decl(&toml::Value::Table(t.clone()), 0)?);
            }
            Some(other) => {
                return Err(format!(
                    "vxsetting.toml: [module] 必须是表或数组表, 实际为 {}",
                    other.type_str()
                ));
            }
            None => {}
        }

        let vxset = parse_vxset(root.get("vxset"))?;

        Ok(Self {
            source_dir: source_dir.to_string(),
            libraries,
            bin,
            vxlib,
            lib,
            modules,
            vxset,
            compiler: None,
            linker: None,
            memory: None,
        })
    }

    /// 获取指定库的路径 (向后兼容旧 vxmodel 查询语义)
    ///
    /// 支持点分模块路径：`std.io` 会先查找 `std.io`，没找到则拆成 `std` + `io`。
    pub fn library_path(&self, name: &str) -> Option<String> {
        // 先尝试完整名称匹配
        if let Some(path) = self.libraries.get(name) {
            return Some(path.clone());
        }
        // 点分路径：拆出根模块
        if let Some(dot_pos) = name.find('.') {
            let root = &name[..dot_pos];
            let sub = &name[dot_pos + 1..];
            if let Some(base) = self.libraries.get(root) {
                return Some(format!("{}/{}", base, sub));
            }
        }
        None
    }

    /// 判断是否为多文件构建项目
    ///
    /// 存在 [bin] / [vxlib] / [lib] / [[module]] 任一时视为多文件项目;
    /// 否则视为单文件项目, 构建器回退至 ipt (vxcompiler) 编译。
    pub fn is_multi_file_project(&self) -> bool {
        self.bin.is_some() || self.vxlib.is_some() || self.lib.is_some() || !self.modules.is_empty()
    }

    /// 返回默认构建产物目录 (相对于 source_dir): vpmfile/build
    pub fn default_build_dir(&self) -> PathBuf {
        Path::new(&self.source_dir).join(DEFAULT_BUILD_DIR)
    }

    /// 返回缓存文件路径 (相对于 source_dir): vpmfile/vpm/buildcache.toml
    pub fn cache_file_path(&self) -> PathBuf {
        Path::new(&self.source_dir).join(CACHE_DIR).join(CACHE_FILE)
    }

    /// 解析构建目标的输出路径: 显式 output 优先, 否则默认 vpmfile/build/<kind>
    pub fn resolve_output(&self, target: &BuildTarget, kind: TargetKind) -> PathBuf {
        if let Some(out) = &target.output {
            // 相对路径基于 source_dir 解析
            let p = Path::new(out);
            if p.is_absolute() {
                p.to_path_buf()
            } else {
                Path::new(&self.source_dir).join(p)
            }
        } else {
            self.default_build_dir().join(kind.as_str())
        }
    }
}

// ==================== 子解析函数 ====================

/// 解析 [bin] / [vxlib] / [lib] 构建目标
fn parse_build_target(val: Option<&toml::Value>, kind: &str) -> Result<Option<BuildTarget>, String> {
    let table = match val {
        None => return Ok(None),
        Some(toml::Value::Table(t)) => t,
        Some(other) => {
            return Err(format!(
                "vxsetting.toml: [{}] 必须是表结构, 实际为 {}",
                kind,
                other.type_str()
            ));
        }
    };

    // source: 支持字符串或字符串数组
    let sources = parse_sources(table.get("source"), kind)?;

    // version: 默认 "0.0.1"
    let version = match table.get("version") {
        None => DEFAULT_VERSION.to_string(),
        Some(toml::Value::String(s)) => s.clone(),
        Some(other) => {
            return Err(format!(
                "vxsetting.toml: [{}].version 必须是字符串, 实际为 {}",
                kind,
                other.type_str()
            ));
        }
    };

    // output: 可选字符串
    let output = match table.get("output") {
        None => None,
        Some(toml::Value::String(s)) => Some(s.clone()),
        Some(other) => {
            return Err(format!(
                "vxsetting.toml: [{}].output 必须是字符串, 实际为 {}",
                kind,
                other.type_str()
            ));
        }
    };

    if sources.is_empty() {
        return Err(format!(
            "vxsetting.toml: [{}] 缺少 source 字段 (至少一个源文件)",
            kind
        ));
    }

    Ok(Some(BuildTarget {
        sources,
        version,
        output,
    }))
}

/// 解析 [[module]] 模块声明
fn parse_module_decl(val: &toml::Value, idx: usize) -> Result<ModuleDecl, String> {
    let table = match val {
        toml::Value::Table(t) => t,
        other => {
            return Err(format!(
                "vxsetting.toml: [[module]]#{} 必须是表结构, 实际为 {}",
                idx,
                other.type_str()
            ));
        }
    };

    let info = match table.get("info") {
        Some(toml::Value::String(s)) => s.clone(),
        Some(other) => {
            return Err(format!(
                "vxsetting.toml: [[module]]#{}.info 必须是字符串, 实际为 {}",
                idx,
                other.type_str()
            ));
        }
        None => {
            return Err(format!(
                "vxsetting.toml: [[module]]#{} 缺少 info 字段 (info.toml 路径)",
                idx
            ));
        }
    };

    let name = match table.get("name") {
        Some(toml::Value::String(s)) => s.clone(),
        Some(other) => {
            return Err(format!(
                "vxsetting.toml: [[module]]#{}.name 必须是字符串, 实际为 {}",
                idx,
                other.type_str()
            ));
        }
        None => {
            return Err(format!(
                "vxsetting.toml: [[module]]#{} 缺少 name 字段 (import 调用名)",
                idx
            ));
        }
    };

    let sources = parse_sources(table.get("source"), &format!("module#{}", idx))?;
    if sources.is_empty() {
        return Err(format!(
            "vxsetting.toml: [[module]]#{} 缺少 source 字段 (至少一个源文件)",
            idx
        ));
    }

    Ok(ModuleDecl { info, name, sources })
}

/// 解析 [vxset] 全局配置
fn parse_vxset(val: Option<&toml::Value>) -> Result<VxsetConfig, String> {
    let mut cfg = VxsetConfig::default();
    let table = match val {
        None => return Ok(cfg),
        Some(toml::Value::Table(t)) => t,
        Some(other) => {
            return Err(format!(
                "vxsetting.toml: [vxset] 必须是表结构, 实际为 {}",
                other.type_str()
            ));
        }
    };

    if let Some(v) = table.get("deadcode") {
        cfg.deadcode = v.as_bool().ok_or_else(|| {
            "vxsetting.toml: [vxset].deadcode 必须是布尔值".to_string()
        })?;
    }

    if let Some(v) = table.get("cache") {
        cfg.cache = v.as_bool().ok_or_else(|| {
            "vxsetting.toml: [vxset].cache 必须是布尔值".to_string()
        })?;
    }

    if let Some(v) = table.get("shell") {
        cfg.shell = Some(v.as_str().ok_or_else(|| {
            "vxsetting.toml: [vxset].shell 必须是字符串".to_string()
        })?.to_string());
    }

    if let Some(v) = table.get("o") {
        let n = v.as_integer().ok_or_else(|| {
            "vxsetting.toml: [vxset].o 必须是整数 (0-20)".to_string()
        })?;
        if !(0..=MAX_OPT_LEVEL as i64).contains(&n) {
            return Err(format!(
                "vxsetting.toml: [vxset].o 取值范围为 0-{}, 实际为 {}",
                MAX_OPT_LEVEL, n
            ));
        }
        cfg.optimization = n as u8;
    }

    Ok(cfg)
}

/// 解析 source 字段: 兼容单字符串 `source = "a.vx"` 与数组 `source = ["a.vx","b.vx"]`
fn parse_sources(val: Option<&toml::Value>, ctx: &str) -> Result<Vec<String>, String> {
    match val {
        None => Ok(Vec::new()),
        Some(toml::Value::String(s)) => Ok(vec![s.clone()]),
        Some(toml::Value::Array(arr)) => {
            let mut out = Vec::with_capacity(arr.len());
            for (i, item) in arr.iter().enumerate() {
                match item.as_str() {
                    Some(s) => out.push(s.to_string()),
                    None => {
                        return Err(format!(
                            "vxsetting.toml: [{}].source[{}] 必须是字符串",
                            ctx, i
                        ));
                    }
                }
            }
            Ok(out)
        }
        Some(other) => Err(format!(
            "vxsetting.toml: [{}].source 必须是字符串或字符串数组, 实际为 {}",
            ctx,
            other.type_str()
        )),
    }
}

// ==================== 测试 ====================
#[cfg(test)]
mod tests {
    use super::*;
    use std::io::Write;
    use tempfile::NamedTempFile;

    fn write_temp(content: &str) -> NamedTempFile {
        let mut file = NamedTempFile::new().unwrap();
        write!(file, "{}", content).unwrap();
        file
    }

    #[test]
    fn test_parse_libraries_backward_compat() {
        let file = write_temp(
            r#"
[libraries]
stdlib = "/usr/local/lib/vx/stdlib"
math = "/usr/local/lib/vx/math"
"#,
        );
        let settings = VxSettings::from_file(file.path().to_str().unwrap()).unwrap();
        assert_eq!(
            settings.library_path("stdlib"),
            Some("/usr/local/lib/vx/stdlib".to_string())
        );
        assert_eq!(settings.library_path("missing"), None);
    }

    #[test]
    fn test_empty_file() {
        let file = write_temp("\n");
        let settings = VxSettings::from_file(file.path().to_str().unwrap()).unwrap();
        assert!(settings.libraries.is_empty());
        assert!(!settings.is_multi_file_project());
    }

    #[test]
    fn test_invalid_toml() {
        let file = write_temp("[libraries\n");
        assert!(VxSettings::from_file(file.path().to_str().unwrap()).is_err());
    }

    #[test]
    fn test_non_string_library_value() {
        let file = write_temp("[libraries]\nfoo = 42\n");
        assert!(VxSettings::from_file(file.path().to_str().unwrap()).is_err());
    }

    #[test]
    fn test_bin_target_single_source() {
        let file = write_temp(
            r#"
[bin]
source = "main.vx"
version = "1.2.3"
output = "dist/myapp"
"#,
        );
        let s = VxSettings::from_file(file.path().to_str().unwrap()).unwrap();
        assert!(s.is_multi_file_project());
        let bin = s.bin.expect("bin target");
        assert_eq!(bin.sources, vec!["main.vx".to_string()]);
        assert_eq!(bin.version, "1.2.3");
        assert_eq!(bin.output.as_deref(), Some("dist/myapp"));
    }

    #[test]
    fn test_bin_target_multi_source_array() {
        let file = write_temp(
            r#"
[bin]
source = ["main.vx", "util.vx", "net.vx"]
"#,
        );
        let s = VxSettings::from_file(file.path().to_str().unwrap()).unwrap();
        let bin = s.bin.unwrap();
        assert_eq!(bin.sources.len(), 3);
        assert_eq!(bin.version, DEFAULT_VERSION); // 默认值
        assert!(bin.output.is_none());
    }

    #[test]
    fn test_bin_missing_source_errors() {
        let file = write_temp("[bin]\nversion = \"1.0.0\"\n");
        assert!(VxSettings::from_file(file.path().to_str().unwrap()).is_err());
    }

    #[test]
    fn test_modules_array_of_tables() {
        let file = write_temp(
            r#"
[[module]]
info = "pkg/a/info.toml"
name = "a"
source = ["a/main.vx"]

[[module]]
info = "pkg/b/info.toml"
name = "b"
source = "b/lib.vx"
"#,
        );
        let s = VxSettings::from_file(file.path().to_str().unwrap()).unwrap();
        assert_eq!(s.modules.len(), 2);
        assert_eq!(s.modules[0].name, "a");
        assert_eq!(s.modules[0].sources, vec!["a/main.vx".to_string()]);
        assert_eq!(s.modules[1].name, "b");
        assert_eq!(s.modules[1].sources, vec!["b/lib.vx".to_string()]);
        assert!(s.is_multi_file_project());
    }

    #[test]
    fn test_module_missing_required_field() {
        let file = write_temp("[[module]]\nname = \"x\"\nsource = [\"x.vx\"]\n");
        assert!(VxSettings::from_file(file.path().to_str().unwrap()).is_err());
    }

    #[test]
    fn test_vxset_defaults() {
        let file = write_temp("[bin]\nsource = \"m.vx\"\n");
        let s = VxSettings::from_file(file.path().to_str().unwrap()).unwrap();
        assert!(s.vxset.deadcode);
        assert!(s.vxset.cache);
        assert_eq!(s.vxset.optimization, DEFAULT_OPT_LEVEL);
        assert!(s.vxset.shell.is_none());
    }

    #[test]
    fn test_vxset_custom() {
        let file = write_temp(
            r#"
[vxset]
deadcode = false
cache = false
shell = "/bin/zsh"
o = 10
"#,
        );
        let s = VxSettings::from_file(file.path().to_str().unwrap()).unwrap();
        assert!(!s.vxset.deadcode);
        assert!(!s.vxset.cache);
        assert_eq!(s.vxset.shell.as_deref(), Some("/bin/zsh"));
        assert_eq!(s.vxset.optimization, 10);
    }

    #[test]
    fn test_vxset_opt_out_of_range() {
        let file = write_temp("[vxset]\no = 25\n");
        assert!(VxSettings::from_file(file.path().to_str().unwrap()).is_err());
    }

    #[test]
    fn test_resolve_output_default() {
        let file = write_temp("[bin]\nsource = \"m.vx\"\n");
        let s = VxSettings::from_file(file.path().to_str().unwrap()).unwrap();
        let bin = s.bin.as_ref().unwrap();
        let out = s.resolve_output(bin, TargetKind::Bin);
        assert!(out.ends_with("vpmfile/build/bin"));
    }

   #[test]
fn test_resolve_output_explicit() {
    let file = write_temp("[vxlib]\nsource = \"lib.vx\"\noutput = \"out/vxlib\"\n");
    let s = VxSettings::from_file(file.path().to_str().unwrap()).unwrap();
    let target_ref = s.vxlib.as_ref().unwrap(); // 提前获取引用，避免部分移动
    let out = s.resolve_output(target_ref, TargetKind::VxLib);
    assert!(out.to_string_lossy().ends_with("out/vxlib"));
}

    #[test]
    fn test_single_file_detection() {
        // 仅有 [libraries], 视为单文件项目
        let file = write_temp("[libraries]\nfoo = \"bar\"\n");
        let s = VxSettings::from_file(file.path().to_str().unwrap()).unwrap();
        assert!(!s.is_multi_file_project());
    }
}
