// VX Package Manager (vpm) v1.0
// 管理 VX 语言的第三方包（.vack 格式，即重命名的 7z 压缩包）
// 支持的实现语言：Python, TS, JS, Java, Rust, Go, C, C++, CXX
//
// v1.1 新增: `vpm build` 构建器命令
//   基于 vxsetting.toml 配置, 实现 VX 多文件项目自动化构建流程,
//   单文件项目无缝回退至 ipt (vxcompiler) 编译。

use std::collections::HashMap;
use std::env;
use std::fs;
use std::io;
use std::path::{Path, PathBuf};
use std::process::{self, Command};
use std::sync::OnceLock;

use vx_vm::builder::VxBuilder;
use vx_vm::VxSettings;

// ==================== 常量 ====================
const REPO_URL: &str = "https://gitee.com/vx-language-dev/vx_packages/";
const VXMOD_FILE: &str = "vxmod.toml";
const PACKAGE_DIR: &str = "package";
const INFO_FILE: &str = "info.toml";
const VXSETTING_FILE: &str = "vxsetting.toml";

const SUPPORTED_LANGUAGES: &[&str] = &[
    "python", "ts", "js", "java", "rust", "go", "c", "cpp",
];

// ==================== 错误处理 ====================
#[derive(Debug)]
enum VpmError {
    Io(io::Error),
    MissingArg(String),
    FileNotFound(String),
    InvalidVack(String),
    PackageExists(String),
    PackageNotFound(String),
    SevenZipNotFound,
    ExtractionFailed(String),
    UnsupportedLanguage(String),
    ToolchainMismatch { expected: String, got: String },
}

impl std::fmt::Display for VpmError {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match self {
            VpmError::Io(e) => write!(f, "IO 错误: {}", e),
            VpmError::MissingArg(msg) => write!(f, "参数缺失: {}", msg),
            VpmError::FileNotFound(path) => write!(f, "找不到文件: {}", path),
            VpmError::InvalidVack(reason) => write!(f, "无效的 .vack 包文件: {}", reason),
            VpmError::PackageExists(name) => write!(f, "包 '{}' 已安装，请先使用 `vpm rm {}` 卸载", name, name),
            VpmError::PackageNotFound(name) => write!(f, "包 '{}' 未安装", name),
            VpmError::SevenZipNotFound => write!(
                f,
                "未找到 7z 解压工具，请安装 p7zip 或 7-Zip 后重试"
            ),
            VpmError::ExtractionFailed(reason) => write!(f, "解压失败: {}", reason),
            VpmError::UnsupportedLanguage(lang) => write!(
                f,
                "不支持的语言: '{}'，支持的语言: {}",
                lang,
                SUPPORTED_LANGUAGES.join(", ")
            ),
            VpmError::ToolchainMismatch { expected, got } => write!(
                f,
                "工具链版本不匹配: 要求 {}，但包要求 {}",
                expected, got
            ),
        }
    }
}

// 手动实现 Clone：std::io::Error 没有实现 Clone，因此不能在 VpmError 上派生。
impl Clone for VpmError {
    fn clone(&self) -> Self {
        match self {
            VpmError::Io(e) => VpmError::Io(io::Error::new(e.kind(), e.to_string())),
            VpmError::MissingArg(s) => VpmError::MissingArg(s.clone()),
            VpmError::FileNotFound(s) => VpmError::FileNotFound(s.clone()),
            VpmError::InvalidVack(s) => VpmError::InvalidVack(s.clone()),
            VpmError::PackageExists(s) => VpmError::PackageExists(s.clone()),
            VpmError::PackageNotFound(s) => VpmError::PackageNotFound(s.clone()),
            VpmError::SevenZipNotFound => VpmError::SevenZipNotFound,
            VpmError::ExtractionFailed(s) => VpmError::ExtractionFailed(s.clone()),
            VpmError::UnsupportedLanguage(s) => VpmError::UnsupportedLanguage(s.clone()),
            VpmError::ToolchainMismatch { expected, got } => VpmError::ToolchainMismatch {
                expected: expected.clone(),
                got: got.clone(),
            },
        }
    }
}

impl From<io::Error> for VpmError {
    fn from(e: io::Error) -> Self {
        VpmError::Io(e)
    }
}

// ==================== 辅助函数 ====================

/// 缓存 7z 探测结果：第一次执行子进程探测，之后直接返回缓存值。
static SEVEN_Z_CHECK: OnceLock<Result<(), VpmError>> = OnceLock::new();
/// 缓存检测到的 7z 可执行文件名（"7zz" 或 "7z"）。
static SEVEN_Z_CMD: OnceLock<String> = OnceLock::new();

/// 检查 7z 是否可用（仅在第一次调用时执行子进程探测，后续直接返回缓存结果）
fn check_7z() -> Result<(), VpmError> {
    let cached = SEVEN_Z_CHECK.get_or_init(|| match Command::new("7z").arg("--help").output() {
        Ok(_) => Ok(()),
        Err(_) => match Command::new("7zz").arg("--help").output() {
            Ok(_) => Ok(()),
            Err(_) => Err(VpmError::SevenZipNotFound),
        },
    });
    cached.clone()
}

/// 获取 7z 可执行文件名（仅在第一次调用时执行子进程探测，后续直接返回缓存结果）
fn get_7z_cmd() -> &'static str {
    SEVEN_Z_CMD
        .get_or_init(|| {
            // 先检测 7zz（新版），再回退到 7z
            if Command::new("7zz").arg("--help").output().is_ok() {
                "7zz".to_string()
            } else {
                "7z".to_string()
            }
        })
        .as_str()
}

/// 规范化语言名称（统一小写别名）
fn normalize_language(lang: &str) -> String {
    match lang.to_lowercase().as_str() {
        "javascript" | "js" => "js".to_string(),
        "typescript" | "ts" => "ts".to_string(),
        "python" | "py" => "python".to_string(),
        "java" => "java".to_string(),
        "rust" | "rs" => "rust".to_string(),
        "go" | "golang" => "go".to_string(),
        "c" => "c".to_string(),
        "cpp" | "c++" | "cxx" => "cpp".to_string(),
        other => other.to_string(),
    }
}

/// 判断语言是否被支持 (先规范化别名再查表，避免 py/js/typescript 等大小写不一致)
fn is_language_supported(lang: &str) -> bool {
    let normalized = normalize_language(lang);
    SUPPORTED_LANGUAGES.contains(&normalized.as_str())
}

/// 解析简单 TOML 格式的 info 文件为键值对
/// 支持: key = "value" 和 key = value 两种格式
fn parse_info_toml(content: &str) -> HashMap<String, String> {
    parse_simple_kv(content, '=')
}

/// 解析简单的 key 分隔的配置文件，跳过空行、`#` 注释和 `[section]`
fn parse_simple_kv(content: &str, delimiter: char) -> HashMap<String, String> {
    let mut map = HashMap::new();
    for line in content.lines() {
        let line = line.trim();
        if line.is_empty() || line.starts_with('#') || line.starts_with('[') {
            continue;
        }
        if let Some((k, v)) = line.split_once(delimiter) {
            map.insert(k.trim().to_string(), v.trim().trim_matches('"').to_string());
        }
    }
    map
}

/// 获取当前工具链版本号
fn get_toolchain_version() -> String {
    env!("CARGO_PKG_VERSION").to_string()
}

/// 查找工作区根目录（向上搜索 vxmod.toml 或 Cargo.toml）
fn find_workspace_root() -> Result<PathBuf, VpmError> {
    let mut dir = env::current_dir().map_err(VpmError::Io)?;
    loop {
        if dir.join(VXMOD_FILE).exists() || dir.join("Cargo.toml").exists() {
            return Ok(dir);
        }
        if !dir.pop() {
            return Err(VpmError::Io(io::Error::new(
                io::ErrorKind::NotFound,
                "无法找到工作区根目录（未找到 vxmod.toml 或 Cargo.toml）",
            )));
        }
    }
}

// ==================== 命令实现 ====================

/// 输出使用帮助
fn cmd_help() {
    let ver = get_toolchain_version();
    println!("VX Package Manager (vpm) v{}", ver);
    println!();
    println!("用法:");
    println!("  vpm help                  显示此帮助信息");
    println!("  vpm install <包文件.vack>  从本地 .vack 文件安装包");
    println!("  vpm rm <包名>              卸载指定包");
    println!("  vpm build [入口.vx]        基于 vxsetting.toml 构建项目");
    println!();
    println!("官方包仓库: {}", REPO_URL);
    println!();
    println!("说明:");
    println!("  .vack 文件是重命名的 7z 压缩包，安装前请确保系统已安装 p7zip/7-Zip。");
    println!("  安装包将解压到当前工作区的 package/<包名>/ 目录下。");
    println!("  支持的语言: {}", SUPPORTED_LANGUAGES.join(", "));
    println!();
    println!("vpm build 构建路径:");
    println!("  多文件项目 (vxsetting.toml 含 [bin]/[vxlib]/[lib]/[[module]]):");
    println!("    构建器解析 module 依赖 → 调用 vxcompiler 编译各源 → 调用 vxlinker 链接");
    println!("  单文件项目 (仅 [libraries]/[vxset]):");
    println!("    无缝回退至 vxcompiler (ipt) 直接编译入口源文件");
    println!();
    println!("示例:");
    println!("  vpm install ./my-lib.vack");
    println!("  vpm rm my-lib");
    println!("  vpm build                 # 自动判断多/单文件路径");
    println!("  vpm build src/main.vx     # 单文件模式指定入口");
}

/// 安装包：从 .vack 文件解压到 package/<包名>/ 并更新 vxmod.tmol
fn cmd_install(vack_path: &str) -> Result<(), VpmError> {
    // 1. 校验 .vack 文件存在
    let vack = Path::new(vack_path);
    if !vack.exists() {
        return Err(VpmError::FileNotFound(vack_path.to_string()));
    }
    if !vack.is_file() {
        return Err(VpmError::InvalidVack(format!(
            "'{}' 不是一个有效的文件",
            vack_path
        )));
    }
    if vack.extension().map(|e| e.to_str()) != Some(Some("vack")) {
        eprintln!("[VPM 警告] 文件扩展名不是 .vack，但仍将尝试解压...");
    }

    // 2. 检查 7z 可用
    check_7z()?;

    // 3. 创建临时解压目录
    let temp_dir = PathBuf::from(format!(
        ".vpm_temp_{}",
        process::id()
    ));
    if temp_dir.exists() {
        fs::remove_dir_all(&temp_dir)?;
    }
    fs::create_dir_all(&temp_dir)?;

    // 4. 使用 7z 解压
    let vack_abs = fs::canonicalize(vack_path)?;
    let seven_z = get_7z_cmd();
    let output = Command::new(seven_z)
        .args([
            "x",
            "-y", // 自动确认
            &vack_abs.to_string_lossy(),
            &format!("-o{}", temp_dir.display()),
        ])
        .output()
        .map_err(|_| VpmError::SevenZipNotFound)?;

    if !output.status.success() {
        let stderr = String::from_utf8_lossy(&output.stderr);
        // 清理临时目录
        let _ = fs::remove_dir_all(&temp_dir);
        return Err(VpmError::ExtractionFailed(stderr.to_string()));
    }

    println!("[VPM] 解压完成，正在验证包结构...");

    // 4.5 安全性：拒绝路径穿越条目（绝对路径、..、隐藏目录）
    if let Err(e) = validate_no_path_traversal(&temp_dir) {
        let _ = fs::remove_dir_all(&temp_dir);
        return Err(e);
    }

    // 5. 查找 info.toml（可能在根目录或子目录中）
    let info_path = find_info_toml(&temp_dir)?;
    let info_content = fs::read_to_string(&info_path).map_err(|_| {
        VpmError::InvalidVack("缺少 info.toml 元数据文件".to_string())
    })?;

    let info = parse_info_toml(&info_content);

    // 6. 校验必填字段
    let pkg_name = info.get("name").ok_or_else(|| {
        VpmError::InvalidVack("info.toml 中缺少 'name' 字段".to_string())
    })?;
    let pkg_version = info.get("version").ok_or_else(|| {
        VpmError::InvalidVack("info.toml 中缺少 'version' 字段".to_string())
    })?;
    let pkg_author = info.get("author").ok_or_else(|| {
        VpmError::InvalidVack("info.toml 中缺少 'author' 字段".to_string())
    })?;
    let pkg_desc = info.get("description").unwrap_or(&String::new()).clone();
    let pkg_toolchain = info.get("toolchain").ok_or_else(|| {
        VpmError::InvalidVack("info.toml 中缺少 'toolchain' 字段".to_string())
    })?;
    let pkg_lang = info
        .get("language")
        .map(|l| l.as_str())
        .unwrap_or("unknown");

    // 7. 校验语言支持
    if !is_language_supported(pkg_lang) {
        return Err(VpmError::UnsupportedLanguage(pkg_lang.to_string()));
    }

    // 8. 校验工具链版本（宽松匹配：主版本号一致即可）
    let current_ver = get_toolchain_version();
    let current_major = current_ver.split('.').next().unwrap_or("0");
    let pkg_major = pkg_toolchain.split('.').next().unwrap_or("0");
    if current_major != pkg_major {
        return Err(VpmError::ToolchainMismatch {
            expected: current_ver,
            got: pkg_toolchain.clone(),
        });
    }

    // 9. 确定工作区根目录并检查目标目录是否已存在
    let workspace_root = find_workspace_root()?;
    let target_dir = workspace_root.join(PACKAGE_DIR).join(pkg_name);
    if target_dir.exists() {
        let _ = fs::remove_dir_all(&temp_dir);
        return Err(VpmError::PackageExists(pkg_name.clone()));
    }

    // 10. 复制包文件到目标目录
    fs::create_dir_all(&target_dir)?;

    let pkg_source_dir = match info_path.parent() {
        Some(p) if p != temp_dir.as_path() => p.to_path_buf(),
        _ => temp_dir.clone(),
    };

    copy_dir_all(&pkg_source_dir, &target_dir)?;

    println!(
        "[VPM] 包 '{}' v{} 安装成功 -> {}",
        pkg_name,
        pkg_version,
        target_dir.display()
    );
    println!("[VPM] 作者: {}", pkg_author);
    if !pkg_desc.is_empty() {
        println!("[VPM] 描述: {}", pkg_desc);
    }
    println!(
        "[VPM] 语言: {} (标准化为: {})",
        pkg_lang,
        normalize_language(pkg_lang)
    );

    // 11. 更新 vxmod.tmol
    append_to_vxmod(&workspace_root, pkg_name, pkg_version, pkg_lang)?;
    println!("[VPM] 已更新 {} 配置文件", VXMOD_FILE);

    // 12. 清理临时目录
    let _ = fs::remove_dir_all(&temp_dir);

    Ok(())
}

/// 递归查找 info.toml（在临时目录和一级子目录中搜索）
fn find_info_toml(base: &Path) -> Result<PathBuf, VpmError> {
    // 先检查根目录
    let root_info = base.join(INFO_FILE);
    if root_info.exists() {
        return Ok(root_info);
    }

    // 再检查一级子目录
    if let Ok(entries) = fs::read_dir(base) {
        for entry in entries.flatten() {
            let candidate = entry.path().join(INFO_FILE);
            if candidate.exists() {
                return Ok(candidate);
            }
        }
    }

    Err(VpmError::InvalidVack(format!(
        "在解压内容中找不到 {} 文件。\n包结构要求: .vack 内必须包含 info.toml（含 name/version/author/toolchain 字段）",
        INFO_FILE
    )))
}

/// 校验解压后的所有路径都位于 `base` 之内。
///
/// 7z / tar 等工具在解压恶意压缩包时可能写入 `..`、`/`、绝对路径
/// 等条目，绕过临时目录。本函数递归遍历 `base`，对每个条目计算
/// `canonicalize` 后的真实路径，并断言其以 `base` 起始且不等于
/// `base` 本身（仅对条目本身检查）。
fn validate_no_path_traversal(base: &Path) -> Result<(), VpmError> {
    let base_canon = fs::canonicalize(base).map_err(|e| {
        VpmError::InvalidVack(format!("无法规范化临时目录: {}", e))
    })?;
    validate_no_path_traversal_inner(&base_canon, &base_canon)
}

fn validate_no_path_traversal_inner(
    base_canon: &Path,
    dir: &Path,
) -> Result<(), VpmError> {
    for entry in fs::read_dir(dir).map_err(|e| {
        VpmError::InvalidVack(format!("读取目录失败: {}", e))
    })? {
        let entry = match entry {
            Ok(e) => e,
            Err(_) => continue,
        };
        let file_name = entry.file_name();
        let name_str = file_name.to_string_lossy();
        // 显式拒绝以 `.` 开头的条目（隐藏文件 / 父目录引用）
        if name_str == "." || name_str == ".." || name_str.starts_with('/')
            || (cfg!(windows) && (name_str.contains(':') || name_str.starts_with('\\')))
        {
            return Err(VpmError::InvalidVack(format!(
                "包内含有非法路径条目: {:?}",
                file_name
            )));
        }
        let entry_path = entry.path();
        let canon = match fs::canonicalize(&entry_path) {
            Ok(p) => p,
            Err(_) => continue, // 已损坏，跳过
        };
        if !canon.starts_with(base_canon) {
            return Err(VpmError::InvalidVack(format!(
                "检测到路径穿越: {:?} 试图逃出临时目录",
                file_name
            )));
        }
        let ft = match entry.file_type() {
            Ok(t) => t,
            Err(_) => continue,
        };
        if ft.is_dir() {
            validate_no_path_traversal_inner(base_canon, &canon)?;
        }
    }
    Ok(())
}

/// 递归复制目录内容（防御性版本：拒绝目标逃出 `dst`）
fn copy_dir_all(src: &Path, dst: &Path) -> Result<(), io::Error> {
    if !dst.exists() {
        fs::create_dir_all(dst)?;
    }
    let dst_canon = fs::canonicalize(dst).unwrap_or_else(|_| dst.to_path_buf());
    for entry in fs::read_dir(src)? {
        let entry = entry?;
        let file_name = entry.file_name();
        let name_str = file_name.to_string_lossy();
        if name_str == "." || name_str == ".." {
            continue;
        }
        let dest = dst.join(&file_name);
        let ty = entry.file_type()?;
        if ty.is_dir() {
            copy_dir_all(&entry.path(), &dest)?;
        } else if ty.is_file() {
            // 再次规范化以拦截源条目本身的逃逸
            if let Ok(src_canon) = fs::canonicalize(entry.path()) {
                if !src_canon.starts_with(fs::canonicalize(src).unwrap_or_else(|_| src.to_path_buf())) {
                    return Err(io::Error::new(
                        io::ErrorKind::InvalidInput,
                        format!("拒绝复制逃出源目录的文件: {:?}", file_name),
                    ));
                }
            }
            // 防御性：目标必须在 dst 之内
            if let Some(parent) = dest.parent() {
                if let Ok(p) = fs::canonicalize(parent) {
                    if !p.starts_with(&dst_canon) {
                        return Err(io::Error::new(
                            io::ErrorKind::InvalidInput,
                            format!("拒绝复制逃出目标目录的文件: {:?}", file_name),
                        ));
                    }
                }
            }
            fs::copy(entry.path(), &dest)?;
        }
    }
    Ok(())
}

/// 向 vxmod.tmol 追加包配置
fn append_to_vxmod(
    workspace_root: &Path,
    name: &str,
    version: &str,
    language: &str,
) -> Result<(), VpmError> {
    let vxmod_path = workspace_root.join(VXMOD_FILE);
    let mut content = if vxmod_path.exists() {
        fs::read_to_string(&vxmod_path)?
    } else {
        String::from("# VX Module Configuration\n# 由 VPM 自动管理\n\n")
    };

    // 检查是否已有此包配置，避免重复
    let section_header = format!("[{}]", name);
    if content.contains(&section_header) {
        // 已存在，跳过
        return Ok(());
    }

    // 追加新配置块
    let normalized_lang = normalize_language(language);
    content.push_str(&format!(
        "\n{}\npath = \"{}/{}\"\nversion = \"{}\"\nlanguage = \"{}\"\n",
        section_header, PACKAGE_DIR, name, version, normalized_lang
    ));

    fs::write(&vxmod_path, content)?;
    Ok(())
}

/// 卸载包：删除 package/<包名>/ 并从 vxmod.tmol 移除配置
fn cmd_rm(package_name: &str) -> Result<(), VpmError> {
    if package_name.is_empty() {
        return Err(VpmError::MissingArg("请指定要卸载的包名".to_string()));
    }

    let workspace_root = find_workspace_root()?;
    let target_dir = workspace_root.join(PACKAGE_DIR).join(package_name);
    if !target_dir.exists() {
        return Err(VpmError::PackageNotFound(package_name.to_string()));
    }

    // 删除包目录
    fs::remove_dir_all(&target_dir)?;
    println!("[VPM] 已删除包目录: {}", target_dir.display());

    // 从 vxmod.tmol 移除配置
    remove_from_vxmod(&workspace_root, package_name)?;
    println!("[VPM] 已从 {} 移除 '{}' 的配置", VXMOD_FILE, package_name);

    // 如果 package 目录为空，也一并清理
    let pkg_root = workspace_root.join(PACKAGE_DIR);
    if pkg_root.exists() {
        if let Ok(entries) = fs::read_dir(&pkg_root) {
            if entries.count() == 0 {
                fs::remove_dir(&pkg_root)?;
                println!("[VPM] 已清理空的 {} 目录", PACKAGE_DIR);
            }
        }
    }

    Ok(())
}

/// 从 vxmod.tmol 中移除指定包的配置块
fn remove_from_vxmod(workspace_root: &Path, package_name: &str) -> Result<(), VpmError> {
    let vxmod_path = workspace_root.join(VXMOD_FILE);
    if !vxmod_path.exists() {
        return Ok(());
    }

    let content = fs::read_to_string(&vxmod_path)?;
    let lines: Vec<&str> = content.lines().collect();

    let section_header = format!("[{}]", package_name);
    let mut new_lines = Vec::new();
    let mut in_section = false;

    for line in lines.iter() {
        if line.trim() == section_header {
            in_section = true;
            // 向前回溯删除可能的前置空行
            while new_lines.last().map(|l: &&str| l.trim().is_empty()).unwrap_or(false) {
                new_lines.pop();
            }
            continue;
        }
        if in_section {
            // 遇到下一个 section 或非空非键值行时结束
            let trimmed = line.trim();
            if trimmed.is_empty() {
                continue;
            }
            if trimmed.starts_with('[') {
                // 新的 section，结束当前 section
                in_section = false;
                new_lines.push(line);
                continue;
            }
            if trimmed.contains('=') {
                // 当前 section 的键值对，跳过
                continue;
            }
            // 不是键值对，结束 section
            in_section = false;
            new_lines.push(line);
        } else {
            new_lines.push(line);
        }
    }

    let mut result = new_lines.join("\n");
    // 清理末尾多余空行
    while result.ends_with("\n\n") {
        result.pop();
    }
    if !result.ends_with('\n') {
        result.push('\n');
    }

    fs::write(&vxmod_path, result)?;
    Ok(())
}

// ==================== 主入口 ====================

/// `vpm build` 构建器命令: 基于 vxsetting.toml 自动化构建
///
/// 路径区分:
///   - 多文件项目 (vxsetting.toml 含 [bin]/[vxlib]/[lib]/[[module]]):
///       构建器读取配置 → 调用 vpm 包查询解析 module 依赖 →
///       调用 vxcompiler 编译各源 → 调用 vxlinker 链接产物
///   - 单文件项目 (仅 [libraries]/[vxset]):
///       无缝回退至 ipt (vxcompiler) 直接编译入口源文件
fn cmd_build(args: &[String]) -> Result<(), VpmError> {
    // 入口文件 (可选): vpm build [entry.vx]
    let single_entry = if args.is_empty() {
        None
    } else {
        Some(args[0].clone())
    };

    // 查找 vxsetting.toml (当前目录或指定路径)
    let setting_path = if let Some(ref entry) = single_entry {
        // 若指定入口, 在其所在目录查找 vxsetting.toml
        let p = Path::new(entry);
        let dir = p.parent().unwrap_or(Path::new("."));
        dir.join(VXSETTING_FILE)
    } else {
        // 默认当前目录
        PathBuf::from(VXSETTING_FILE)
    };

    // 废弃文件检测
    let dir = setting_path.parent().unwrap_or(Path::new("."));
    let vxmodel_path = dir.join("vxmodel");
    let vxmodel_toml_path = dir.join("vxmodel.toml");
    if vxmodel_path.exists() || vxmodel_toml_path.exists() {
        eprintln!("[VX 废弃警告] 检测到旧版配置文件 vxmodel / vxmodel.toml。");
        eprintln!("  该配置格式已废弃，请迁移至 vxsetting.toml。");
        return Err(VpmError::MissingArg(
            "请迁移 vxmodel → vxsetting.toml 后重试".to_string(),
        ));
    }

    if !setting_path.exists() {
        return Err(VpmError::FileNotFound(format!(
            "缺少 {} 文件: {}",
            VXSETTING_FILE,
            setting_path.display()
        )));
    }

    println!("[VPM] 加载配置: {}", setting_path.display());
    let settings = VxSettings::from_file(setting_path.to_str().unwrap())
        .map_err(|e| VpmError::InvalidVack(format!("解析 vxsetting.toml 失败: {}", e)))?;

    let builder = VxBuilder::new(settings).with_single_entry(single_entry.clone());
    builder.build().map_err(|e| {
        VpmError::InvalidVack(format!("构建失败: {}", e))
    })?;

    println!("[VPM] 构建完成");
    Ok(())
}

fn main() {
    let args: Vec<String> = env::args().collect();

    if args.len() < 2 {
        let prog = args.first().map(|s| s.as_str()).unwrap_or("vpm");
        eprintln!("VX Package Manager (vpm)");
        eprintln!("用法: {} <命令> [参数]", prog);
        eprintln!("使用 '{} help' 查看详细帮助。", prog);
        process::exit(1);
    }

    let cmd = args[1].as_str();
    let result = match cmd {
        "help" | "-h" | "--help" => {
            cmd_help();
            Ok(())
        }
        "install" | "i" => {
            if args.len() < 3 {
                Err(VpmError::MissingArg(
                    "用法: vpm install <包文件.vack>".to_string(),
                ))
            } else {
                cmd_install(&args[2])
            }
        }
        "rm" | "remove" | "uninstall" => {
            if args.len() < 3 {
                Err(VpmError::MissingArg(
                    "用法: vpm rm <包名>".to_string(),
                ))
            } else {
                cmd_rm(&args[2])
            }
        }
        "build" | "b" => {
            // vpm build [entry.vx]
            let build_args: Vec<String> = if args.len() > 2 {
                args[2..].to_vec()
            } else {
                Vec::new()
            };
            cmd_build(&build_args)
        }
        _ => {
            eprintln!("[VPM 错误] 未知命令: '{}'", cmd);
            eprintln!("可用命令: help, install, rm, build");
            process::exit(1);
        }
    };

    match result {
        Ok(_) => {}
        Err(e) => {
            eprintln!("[VPM 错误] {}", e);
            process::exit(1);
        }
    }
}

// ==================== 测试 ====================
#[cfg(test)]
mod tests {
    use super::*;
    use tempfile::TempDir;

    #[test]
    fn test_normalize_language() {
        assert_eq!(normalize_language("JavaScript"), "js");
        assert_eq!(normalize_language("TypeScript"), "ts");
        assert_eq!(normalize_language("PYTHON"), "python");
        assert_eq!(normalize_language("Rust"), "rust");
        assert_eq!(normalize_language("rs"), "rust");
        assert_eq!(normalize_language("Go"), "go");
        assert_eq!(normalize_language("C"), "c");
        assert_eq!(normalize_language("c++"), "cpp");
        assert_eq!(normalize_language("cxx"), "cpp");
        assert_eq!(normalize_language("unknown"), "unknown");
    }

    #[test]
    fn test_is_language_supported() {
        assert!(is_language_supported("Python"));
        assert!(is_language_supported("js"));
        assert!(is_language_supported("rust"));
        assert!(!is_language_supported("lua"));
        assert!(!is_language_supported("ruby"));
    }

    #[test]
    fn test_parse_info_toml() {
        let content = r#"
name = "my-lib"
version = "1.2.3"
author = "Test Author"
description = "A test library"
toolchain = "1.0.0"
language = "Rust"
"#;
        let info = parse_info_toml(content);
        assert_eq!(info.get("name").unwrap(), "my-lib");
        assert_eq!(info.get("version").unwrap(), "1.2.3");
        assert_eq!(info.get("author").unwrap(), "Test Author");
        assert_eq!(info.get("description").unwrap(), "A test library");
        assert_eq!(info.get("toolchain").unwrap(), "1.0.0");
        assert_eq!(info.get("language").unwrap(), "Rust");
    }

    #[test]
    fn test_parse_info_toml_with_comments() {
        let content = r#"
# 包名
name = "test-pkg"
# 版本
version = "0.1.0"
author = "dev"
toolchain = "1.0.0"
language = "python"
"#;
        let info = parse_info_toml(content);
        assert_eq!(info.get("name").unwrap(), "test-pkg");
        assert_eq!(info.get("version").unwrap(), "0.1.0");
    }

    #[test]
    fn test_find_info_toml_root() {
        let dir = TempDir::new().unwrap();
        let info_path = dir.path().join("info.toml");
        fs::write(&info_path, "name = \"test\"").unwrap();
        let found = find_info_toml(dir.path());
        assert!(found.is_ok());
        assert_eq!(found.unwrap(), info_path);
    }

    #[test]
    fn test_find_info_toml_subdir() {
        let dir = TempDir::new().unwrap();
        let sub = dir.path().join("mypkg");
        fs::create_dir(&sub).unwrap();
        let info_path = sub.join("info.toml");
        fs::write(&info_path, "name = \"test\"").unwrap();
        let found = find_info_toml(dir.path());
        assert!(found.is_ok());
        assert_eq!(found.unwrap(), info_path);
    }

    #[test]
    fn test_find_info_toml_not_found() {
        let dir = TempDir::new().unwrap();
        let found = find_info_toml(dir.path());
        assert!(found.is_err());
    }

    #[test]
    fn test_append_to_vxmod() {
        let dir = TempDir::new().unwrap();
        // 显式传入 workspace_root 避免修改全局 cwd (线程安全)
        fs::write(dir.path().join(VXMOD_FILE), "# test workspace\n").unwrap();

        append_to_vxmod(dir.path(), "test-pkg", "1.0.0", "Rust").unwrap();
        let content = fs::read_to_string(dir.path().join(VXMOD_FILE)).unwrap();
        assert!(content.contains("[test-pkg]"));
        assert!(content.contains("path = \"package/test-pkg\""));
        assert!(content.contains("version = \"1.0.0\""));
        assert!(content.contains("language = \"rust\""));
    }

    #[test]
    fn test_remove_from_vxmod() {
        let dir = TempDir::new().unwrap();
        // 显式传入 workspace_root 避免修改全局 cwd (线程安全)
        let content = r#"# VX Module Configuration

[pkg-a]
path = "package/pkg-a"
version = "1.0.0"
language = "rust"

[pkg-b]
path = "package/pkg-b"
version = "2.0.0"
language = "python"

[pkg-c]
path = "package/pkg-c"
version = "3.0.0"
language = "go"
"#;
        fs::write(dir.path().join(VXMOD_FILE), content).unwrap();

        // 删除 pkg-b
        remove_from_vxmod(dir.path(), "pkg-b").unwrap();
        let updated = fs::read_to_string(dir.path().join(VXMOD_FILE)).unwrap();
        assert!(updated.contains("[pkg-a]"));
        assert!(updated.contains("[pkg-c]"));
        assert!(!updated.contains("[pkg-b]"));
        assert!(!updated.contains("pkg-b"));
    }

    #[test]
    fn test_vpm_error_display() {
        let err = VpmError::PackageNotFound("test-pkg".to_string());
        assert_eq!(format!("{}", err), "包 'test-pkg' 未安装");

        let err = VpmError::UnsupportedLanguage("lua".to_string());
        assert!(format!("{}", err).contains("不支持的语言"));

        let err = VpmError::ToolchainMismatch {
            expected: "1.0.0".to_string(),
            got: "2.0.0".to_string(),
        };
        assert!(format!("{}", err).contains("工具链版本不匹配"));
    }
}
