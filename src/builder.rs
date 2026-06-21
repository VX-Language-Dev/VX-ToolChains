// ==================== VX 项目构建器 ====================
//
// 基于 vxsetting.toml 的构建配置, 实现 VX 多文件项目自动化构建流程,
// 同时保留单文件项目由 ipt (vxcompiler) 编译的现有机制。
//
// 构建路径区分:
//   - 多文件项目 (存在 [bin]/[vxlib]/[lib]/[[module]]):
//       构建器读取配置 → 通过 vpm 包查询解析 module 依赖 →
//       调用 vxcompiler 编译各源文件 → 调用 vxlinker 链接产物
//   - 单文件项目 (仅 [libraries]/[vxset]):
//       无缝回退至 ipt (vxcompiler) 直接编译入口源文件

use std::collections::HashMap;
use std::fs;
use std::path::{Path, PathBuf};
use std::process::{Command, Stdio};

use crate::cache::{config_hash, BuildCache, TOOLCHAIN_VERSION};
use crate::vxsetting::{BuildTarget, ModuleDecl, TargetKind, VxSettings};

// ==================== 错误类型 ====================

#[derive(Debug)]
pub enum BuildError {
    Config(String),
    Io(std::io::Error),
    ToolNotFound(String),
    CompileFailed { tool: String, stderr: String },
    LinkFailed { stderr: String },
    NoEntryPoint,
    InvalidSource(String),
}

impl From<std::io::Error> for BuildError {
    fn from(e: std::io::Error) -> Self {
        BuildError::Io(e)
    }
}

impl std::fmt::Display for BuildError {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match self {
            BuildError::Config(m) => write!(f, "配置错误: {}", m),
            BuildError::Io(e) => write!(f, "IO 错误: {}", e),
            BuildError::ToolNotFound(t) => write!(f, "未找到工具 '{}', 请确认已编译并位于 PATH", t),
            BuildError::CompileFailed { tool, stderr } => {
                write!(f, "{} 编译失败: {}", tool, stderr.trim())
            }
            BuildError::LinkFailed { stderr } => write!(f, "链接失败: {}", stderr.trim()),
            BuildError::NoEntryPoint => write!(f, "单文件构建未指定入口源文件"),
            BuildError::InvalidSource(m) => write!(f, "无效的源文件: {}", m),
        }
    }
}

impl std::error::Error for BuildError {}

// ==================== 构建器 ====================

pub struct VxBuilder {
    settings: VxSettings,
    /// 单文件回退时的入口源文件 (多文件模式为 None)
    single_entry: Option<String>,
    /// 强制全量重建 (忽略缓存新鲜度, 仍写回缓存)
    force_rebuild: bool,
    /// 本次构建不读写缓存
    no_cache: bool,
    /// 优化等级覆盖 (None 时取自 vxsetting.toml [vxset].o)
    ///
    /// 该值会在调用 vxcompiler 子进程时:
    ///   1) 通过 `--opt-level <n>` CLI 参数透传
    ///   2) 同时通过 `VX_OPT_LEVEL` 环境变量同步注入, 供 ipt 在
    ///      `Compiler::with_options` 阶段使用, 保证编译核心字段与 CLI 一致。
    opt_level: Option<u8>,
    /// 死代码警告覆盖 (None 时根据 vxsetting.toml [vxset].deadcode 推导)
    warn_dead_code: Option<bool>,
    /// 死代码错误覆盖 (None 时根据 opt_level >= 10 推导)
    error_dead_code: Option<bool>,
}

/// vxmod.toml 中记录的已安装包条目
#[derive(Debug, Clone)]
struct InstalledPackage {
    name: String,
    path: String,
    version: String,
    language: String,
}

impl VxBuilder {
    /// 从 VxSettings 创建构建器
    pub fn new(settings: VxSettings) -> Self {
        Self {
            settings,
            single_entry: None,
            force_rebuild: false,
            no_cache: false,
            opt_level: None,
            warn_dead_code: None,
            error_dead_code: None,
        }
    }

    /// 指定单文件回退入口 (vpm build <entry.vx>)
    pub fn with_single_entry(mut self, entry: Option<String>) -> Self {
        self.single_entry = entry;
        self
    }

    /// 强制全量重建 (vpm build --force)
    pub fn with_force_rebuild(mut self, force: bool) -> Self {
        self.force_rebuild = force;
        self
    }

    /// 本次构建禁用缓存读写 (vpm build --no-cache)
    pub fn with_no_cache(mut self, no_cache: bool) -> Self {
        self.no_cache = no_cache;
        self
    }

    /// 显式覆盖优化等级 (None 时取自 vxsetting.toml [vxset].o)
    pub fn with_opt_level(mut self, opt_level: Option<u8>) -> Self {
        self.opt_level = opt_level;
        self
    }

    /// 显式覆盖死代码警告开关
    pub fn with_warn_dead_code(mut self, warn: Option<bool>) -> Self {
        self.warn_dead_code = warn;
        self
    }

    /// 显式覆盖死代码错误开关
    pub fn with_error_dead_code(mut self, err: Option<bool>) -> Self {
        self.error_dead_code = err;
        self
    }

    /// 解析最终生效的优化等级: 字段覆盖 > vxsetting.toml [vxset].o
    fn effective_opt_level(&self) -> u8 {
        self.opt_level
            .unwrap_or(self.settings.vxset.optimization)
    }

    /// 解析最终生效的死代码警告开关: 字段覆盖 > 推导 (deadcode=false 时为 true)
    fn effective_warn_dead_code(&self) -> bool {
        self.warn_dead_code
            .unwrap_or(!self.settings.vxset.deadcode)
    }

    /// 解析最终生效的死代码错误开关: 字段覆盖 > 推导 (opt_level >= 10 时为 true)
    fn effective_error_dead_code(&self) -> bool {
        self.error_dead_code
            .unwrap_or(self.effective_opt_level() >= 10)
    }

    /// 构建主入口: 自动判断多文件 / 单文件路径
    pub fn build(&self) -> Result<(), BuildError> {
        println!(
            "[VXBUILD] 配置目录: {} | 多文件项目: {}",
            self.settings.source_dir,
            self.settings.is_multi_file_project()
        );
        println!(
            "[VXBUILD] 优化等级: {} | 允许死代码: {} | 缓存: {}",
            self.settings.vxset.optimization,
            self.settings.vxset.deadcode,
            self.settings.vxset.cache
        );

        if self.settings.is_multi_file_project() {
            self.build_multi_file()
        } else {
            self.build_single_file()
        }
    }

    // ==================== 单文件路径 (回退 ipt) ====================

    /// 单文件构建: 无缝回退至 ipt (vxcompiler) 直接编译
    fn build_single_file(&self) -> Result<(), BuildError> {
        let entry = self.resolve_single_entry()?;
        println!("[VXBUILD] 单文件模式: 回退至 vxcompiler 编译 {}", entry);

        // 调用 vxcompiler <entry> (ipt 读取同目录 vxsetting.toml 处理 [libraries])
        let output = self.run_vxcompiler(&entry, None)?;
        println!("[VXBUILD] 单文件编译完成: {}", output);
        Ok(())
    }

    /// 解析单文件入口: 优先命令行参数, 其次扫描 source_dir 唯一 .vx 文件
    fn resolve_single_entry(&self) -> Result<String, BuildError> {
        if let Some(e) = &self.single_entry {
            let p = self.resolve_source_path(e);
            if !p.exists() {
                return Err(BuildError::InvalidSource(format!(
                    "入口文件不存在: {}",
                    p.display()
                )));
            }
            return Ok(p.to_string_lossy().to_string());
        }

        // 扫描 source_dir 下的 .vx 文件
        let mut vx_files = Vec::new();
        if let Ok(entries) = fs::read_dir(&self.settings.source_dir) {
            for entry in entries.flatten() {
                let p = entry.path();
                if p.extension().and_then(|e| e.to_str()) == Some("vx") {
                    vx_files.push(p);
                }
            }
        }
        match vx_files.len() {
            0 => Err(BuildError::NoEntryPoint),
            1 => Ok(vx_files[0].to_string_lossy().to_string()),
            _ => {
                eprintln!("[VXBUILD 警告] 发现多个 .vx 文件, 请通过 `vpm build <entry.vx>` 指定入口:");
                for f in &vx_files {
                    eprintln!("  - {}", f.display());
                }
                Err(BuildError::NoEntryPoint)
            }
        }
    }

    // ==================== 多文件路径 ====================

    /// 多文件构建: 解析 module 依赖 → 编译各源 → 链接目标
    ///
    /// 集成缓存机制: 按 [bin]/[vxlib]/[lib]/[[module]] 为最小粒度判断新鲜度,
    /// 新鲜则跳过编译与链接, 否则构建并更新缓存条目。
    fn build_multi_file(&self) -> Result<(), BuildError> {
        // 1. 解析 [[module]] 依赖 (通过 vpm 包查询 vxmod.toml)
        let module_libs = self.resolve_module_dependencies()?;
        if !module_libs.is_empty() {
            println!("[VXBUILD] 解析到 {} 个模块依赖:", module_libs.len());
            for (name, path) in &module_libs {
                println!("    {} -> {}", name, path);
            }
        }

        let opt = self.settings.vxset.optimization;
        let cache_enabled = self.settings.vxset.cache && !self.no_cache;
        let cache_path = self.settings.cache_file_path();
        let read_cache = cache_enabled && !self.force_rebuild;

        let mut cache = BuildCache::load(&cache_path);

        // 2. 全局有效性校验 (vxsetting.toml / vxmod.toml / 工具链版本)
        let vxsetting_path = Path::new(&self.settings.source_dir).join("vxsetting.toml");
        let vxmod_path = Path::new(&self.settings.source_dir).join("vxmod.toml");
        let vxsetting_hash = config_hash(&vxsetting_path).unwrap_or_default();
        let vxmod_hash = config_hash(&vxmod_path);

        if read_cache {
            if !cache.is_globally_valid(&vxsetting_hash, vxmod_hash.as_deref(), TOOLCHAIN_VERSION) {
                println!("[VXBUILD] 配置或工具链版本变更 → 失效全部缓存");
                cache.invalidate_all();
            }
        } else if self.force_rebuild {
            println!("[VXBUILD] --force: 忽略缓存新鲜度, 强制全量构建");
        }

        // 记录当前配置指纹 (供下次构建校验)
        if cache_enabled {
            cache.set_meta(
                vxsetting_hash,
                vxmod_hash,
                TOOLCHAIN_VERSION.to_string(),
            );
        }

        // 3. 构建各目标 (bin / vxlib / lib), 每目标独立判定缓存
        if let Some(bin) = &self.settings.bin {
            let sources_abs: Vec<PathBuf> = bin
                .sources
                .iter()
                .map(|s| self.resolve_source_path(s))
                .collect();
            if read_cache && cache.is_target_fresh("bin", &sources_abs, opt) {
                println!("[VXBUILD] [bin] cached (跳过编译与链接)");
            } else {
                let (obj, output) = self.build_target(bin, TargetKind::Bin, &module_libs)?;
                if cache_enabled {
                    cache.update_entry(
                        "bin".to_string(),
                        &sources_abs,
                        &obj,
                        &output.to_string_lossy(),
                        opt,
                    );
                }
            }
        }
        if let Some(vxlib) = &self.settings.vxlib {
            let sources_abs: Vec<PathBuf> = vxlib
                .sources
                .iter()
                .map(|s| self.resolve_source_path(s))
                .collect();
            if read_cache && cache.is_target_fresh("vxlib", &sources_abs, opt) {
                println!("[VXBUILD] [vxlib] cached (跳过编译与链接)");
            } else {
                let (obj, output) =
                    self.build_target(vxlib, TargetKind::VxLib, &module_libs)?;
                if cache_enabled {
                    cache.update_entry(
                        "vxlib".to_string(),
                        &sources_abs,
                        &obj,
                        &output.to_string_lossy(),
                        opt,
                    );
                }
            }
        }
        if let Some(lib) = &self.settings.lib {
            let sources_abs: Vec<PathBuf> = lib
                .sources
                .iter()
                .map(|s| self.resolve_source_path(s))
                .collect();
            if read_cache && cache.is_target_fresh("lib", &sources_abs, opt) {
                println!("[VXBUILD] [lib] cached (跳过编译与链接)");
            } else {
                let (obj, output) = self.build_target(lib, TargetKind::Lib, &module_libs)?;
                if cache_enabled {
                    cache.update_entry(
                        "lib".to_string(),
                        &sources_abs,
                        &obj,
                        &output.to_string_lossy(),
                        opt,
                    );
                }
            }
        }

        // 4. 编译 [[module]] 声明的模块源 (产出 vx 库供其他项目 import)
        for m in &self.settings.modules {
            let key = format!("module:{}", m.name);
            let sources_abs: Vec<PathBuf> = m
                .sources
                .iter()
                .map(|s| self.resolve_source_path(s))
                .collect();
            if read_cache && cache.is_target_fresh(&key, &sources_abs, opt) {
                println!("[VXBUILD] 模块 '{}' cached (跳过编译)", m.name);
            } else {
                let obj = self.build_module(m, &module_libs)?;
                if cache_enabled {
                    // 模块产物即 .vxobj; 用其同时作为 obj_path 与 output,
                    // 使缓存新鲜度校验能检测产物是否被外部删除 (原先误用源文件
                    // 路径作为 output, 导致产物缺失仍误判为新鲜)。
                    cache.update_entry(key, &sources_abs, &obj, &obj, opt);
                }
            }
        }

        // 5. 持久化缓存 (原子写入)
        if cache_enabled {
            match cache.save() {
                Ok(()) => println!(
                    "[VXBUILD] 缓存已写入 {} ({} 个目标)",
                    cache_path.display(),
                    cache.target_count()
                ),
                Err(e) => eprintln!("[VXBUILD 警告] 保存构建缓存失败: {}", e),
            }
        }

        Ok(())
    }

    /// 构建单个 [bin]/[vxlib]/[lib] 目标
    ///
    /// 返回 `(主产物 .vxobj 路径, 输出路径)`, 供缓存层记录。
    ///
    /// 多源文件语义: `sources[0]` 为入口, 其余源文件由入口通过 `import` 引入,
    /// vxcompiler 在编译入口时解析这些依赖并合并至同一 .vxobj。
    /// 由于 vxlinker 仅接受单个 .vxobj 输入, 本函数只编译入口; 完整 `sources`
    /// 列表由调用方用于缓存指纹判定 (任一源文件变更即触发目标重建)。
    fn build_target(
        &self,
        target: &BuildTarget,
        kind: TargetKind,
        module_libs: &HashMap<String, String>,
    ) -> Result<(String, PathBuf), BuildError> {
        println!(
            "[VXBUILD] 构建 [{}] 目标 ({} 个源文件, 版本 {})",
            kind.as_str(),
            target.sources.len(),
            target.version
        );

        if target.sources.is_empty() {
            return Err(BuildError::Config(format!(
                "[{}] sources 为空",
                kind.as_str()
            )));
        }

        // 入口源: sources[0]; 其余源由入口 import 引入, 不单独编译
        // (vxlinker 仅链接单个 .vxobj, 单独编译额外源会产生无法链接的孤儿产物)
        let entry = &target.sources[0];
        let entry_abs = self.resolve_source_path(entry);

        // module_libs 通过环境变量 VX_EXTRA_LIBS 注入, 供 vxcompiler 解析 import
        let extra_libs_env = self.encode_libs_env(module_libs);
        let vxobj_path = self.run_vxcompiler(
            &entry_abs.to_string_lossy(),
            Some(&extra_libs_env),
        )?;

        // 链接: 调用 vxlinker 将 .vxobj + stub 链接为可执行文件
        let output = self.settings.resolve_output(target, kind);
        if let Some(parent) = output.parent() {
            fs::create_dir_all(parent)?;
        }
        self.run_vxlinker(&vxobj_path, &output.to_string_lossy())?;

        println!(
            "[VXBUILD] [{}] 构建完成 -> {} (入口 {}, 共 {} 个源文件)",
            kind.as_str(),
            output.display(),
            entry,
            target.sources.len()
        );
        Ok((vxobj_path, output))
    }

    /// 编译 [[module]] 声明: 产出 vx 库, 注册到包目录供 import
    ///
    /// 返回主产物 .vxobj 路径, 供缓存层记录。
    fn build_module(
        &self,
        m: &ModuleDecl,
        module_libs: &HashMap<String, String>,
    ) -> Result<String, BuildError> {
        println!(
            "[VXBUILD] 构建模块 '{}' (import 名: {}, {} 个源文件)",
            m.name,
            m.name,
            m.sources.len()
        );
        // 入口源: sources[0]; 其余源由入口 import 引入, 不单独编译 (避免孤儿 .vxobj)
        let entry = &m.sources[0];
        let entry_abs = self.resolve_source_path(entry);
        let extra_libs_env = self.encode_libs_env(module_libs);
        let obj = self.run_vxcompiler(
            &entry_abs.to_string_lossy(),
            Some(&extra_libs_env),
        )?;
        println!("[VXBUILD] 模块 '{}' 编译完成 (入口 {})", m.name, entry);
        Ok(obj)
    }

    // ==================== vpm 包查询 (module 依赖解析) ====================

    /// 解析 [[module]] 依赖: 读取 vxmod.toml 中已安装包, 映射 name → package/<name> 路径
    ///
    /// 复用 vpm (pm.rs) 的包安装结构: 工作区根目录下 vxmod.toml + package/<name>/
    fn resolve_module_dependencies(&self) -> Result<HashMap<String, String>, BuildError> {
        let mut libs = HashMap::new();
        if self.settings.modules.is_empty() {
            return Ok(libs);
        }

        let vxmod_path = Path::new(&self.settings.source_dir).join("vxmod.toml");
        if !vxmod_path.exists() {
            // 无 vxmod.toml: 提示用户通过 vpm install 安装模块
            eprintln!("[VXBUILD 警告] 未找到 vxmod.toml, 无法解析模块依赖");
            eprintln!("  若项目依赖外部模块, 请先使用 `vpm install <pkg.vack>` 安装");
            return Ok(libs);
        }

        let content = fs::read_to_string(&vxmod_path)?;
        let packages = parse_vxmod(&content);

        for m in &self.settings.modules {
            match packages.iter().find(|p| p.name == m.name) {
                Some(pkg) => {
                    let pkg_path = if Path::new(&pkg.path).is_absolute() {
                        PathBuf::from(&pkg.path)
                    } else {
                        Path::new(&self.settings.source_dir).join(&pkg.path)
                    };
                    libs.insert(m.name.clone(), pkg_path.to_string_lossy().to_string());
                }
                None => {
                    eprintln!(
                        "[VXBUILD 警告] 模块 '{}' 未在 vxmod.toml 中找到, 请使用 `vpm install` 安装",
                        m.name
                    );
                }
            }
        }
        Ok(libs)
    }

    // ==================== 子进程调用 ====================

    /// 调用 vxcompiler (ipt) 编译源文件, 返回产物 .vxobj 路径
    ///
    /// extra_libs_env: 编码后的额外库映射, 通过环境变量 VX_EXTRA_LIBS 传递
    ///
    /// 优化/死代码策略通过两条链路注入:
    ///
    /// 1. `--opt-level` / `--warn-dead-code` / `--error-dead-code` CLI 参数
    ///
    /// 2. `VX_OPT_LEVEL` / `VX_WARN_DEAD_CODE` / `VX_ERROR_DEAD_CODE` 环境变量
    ///    后者供 vxcompiler 内部的 `Compiler::with_options` 使用, 保证编译核心
    ///    实例字段与 CLI 透传值一致 (避免 vxcompiler 内部默认值覆盖)。
    fn run_vxcompiler(
        &self,
        source: &str,
        extra_libs_env: Option<&str>,
    ) -> Result<String, BuildError> {
        let tool = "vxcompiler";
        // 用 with_extension 替换文件扩展名, 避免 replacen(".vx", ...) 在路径中
        // 含 ".vx" 的目录名 (如 ~/.vx-cache/main.vx) 时误伤目录段。
        let output_obj = Path::new(source)
            .with_extension("vxobj")
            .to_string_lossy()
            .to_string();

        let opt_level = self.effective_opt_level();
        let warn_dc = self.effective_warn_dead_code();
        let err_dc = self.effective_error_dead_code();

        let mut cmd = Command::new(tool);
        cmd.arg(source).arg("-o").arg(&output_obj);
        // 优化等级透传 (vxsetting.toml [vxset].o 或 VxBuilder 覆盖)
        cmd.arg("--opt-level").arg(opt_level.to_string());
        // 死代码诊断联动: warn_dc 触发警告, err_dc 升级为错误
        if warn_dc {
            cmd.arg("--warn-dead-code");
            if err_dc {
                cmd.arg("--error-dead-code");
            }
        }
        // 环境变量同步注入 — 供 ipt 内 Compiler::with_options 读取
        cmd.env("VX_OPT_LEVEL", opt_level.to_string());
        cmd.env(
            "VX_WARN_DEAD_CODE",
            if warn_dc { "1" } else { "0" },
        );
        cmd.env(
            "VX_ERROR_DEAD_CODE",
            if err_dc { "1" } else { "0" },
        );
        if let Some(env) = extra_libs_env {
            cmd.env("VX_EXTRA_LIBS", env);
        }
        cmd.stdin(Stdio::null())
            .stdout(Stdio::piped())
            .stderr(Stdio::piped());

        let output = cmd
            .output()
            .map_err(|e| {
                if e.kind() == std::io::ErrorKind::NotFound {
                    BuildError::ToolNotFound(tool.to_string())
                } else {
                    BuildError::Io(e)
                }
            })?;

        if !output.status.success() {
            let stderr = String::from_utf8_lossy(&output.stderr).to_string();
            return Err(BuildError::CompileFailed {
                tool: tool.to_string(),
                stderr,
            });
        }
        Ok(output_obj)
    }

    /// 调用 vxlinker 链接 .vxobj 为可执行文件
    fn run_vxlinker(&self, vxobj: &str, output: &str) -> Result<(), BuildError> {
        let tool = "vxlinker";
        let mut cmd = Command::new(tool);
        cmd.arg(vxobj)
            .arg("-o")
            .arg(output)
            .arg("--mode")
            .arg("interpret")
            // 优化等级透传 (链接器侧记录, 供后续优化通路)
            // 与编译器一致使用 effective_opt_level, 避免 CLI 覆盖时链接器仍取 toml 值
            .arg("--opt-level")
            .arg(self.effective_opt_level().to_string());
        cmd.stdin(Stdio::null())
            .stdout(Stdio::inherit())
            .stderr(Stdio::piped());

        let output_res = cmd
            .output()
            .map_err(|e| {
                if e.kind() == std::io::ErrorKind::NotFound {
                    BuildError::ToolNotFound(tool.to_string())
                } else {
                    BuildError::Io(e)
                }
            })?;

        if !output_res.status.success() {
            let stderr = String::from_utf8_lossy(&output_res.stderr).to_string();
            return Err(BuildError::LinkFailed { stderr });
        }
        Ok(())
    }

    // ==================== 辅助方法 ====================

    /// 解析源文件路径 (相对路径基于 source_dir)
    fn resolve_source_path(&self, src: &str) -> PathBuf {
        let p = Path::new(src);
        if p.is_absolute() {
            p.to_path_buf()
        } else {
            Path::new(&self.settings.source_dir).join(p)
        }
    }

    /// 将库映射编码为环境变量字符串 "name1=path1;name2=path2"
    fn encode_libs_env(&self, libs: &HashMap<String, String>) -> String {
        libs.iter()
            .map(|(k, v)| format!("{}={}", k, v))
            .collect::<Vec<_>>()
            .join(";")
    }
}

// ==================== vxmod.toml 解析 ====================

/// 解析 vxmod.toml 为已安装包列表
///
/// 格式 (由 vpm install 自动写入):
///   [pkg-name]
///   path = "package/pkg-name"
///   version = "1.0.0"
///   language = "rust"
fn parse_vxmod(content: &str) -> Vec<InstalledPackage> {
    let mut packages = Vec::new();
    let mut current: Option<InstalledPackage> = None;

    for line in content.lines() {
        let line = line.trim();
        if line.is_empty() || line.starts_with('#') {
            continue;
        }
        // 新 section: [pkg-name]
        if line.starts_with('[') && line.ends_with(']') {
            if let Some(pkg) = current.take() {
                packages.push(pkg);
            }
            let name = line
                .trim_start_matches('[')
                .trim_end_matches(']')
                .trim()
                .to_string();
            current = Some(InstalledPackage {
                name,
                path: String::new(),
                version: String::new(),
                language: String::new(),
            });
            continue;
        }
        // key = "value"
        if let Some((k, v)) = line.split_once('=') {
            let key = k.trim();
            let val = v.trim().trim_matches('"').to_string();
            if let Some(pkg) = current.as_mut() {
                match key {
                    "path" => pkg.path = val,
                    "version" => pkg.version = val,
                    "language" => pkg.language = val,
                    _ => {}
                }
            }
        }
    }
    if let Some(pkg) = current.take() {
        packages.push(pkg);
    }
    packages
}

// ==================== 测试 ====================
#[cfg(test)]
mod tests {
    use super::*;
    use crate::vxsetting::VxSettings;

    #[test]
    fn test_parse_vxmod_single_pkg() {
        let content = r#"
# VX Module Configuration
[mypkg]
path = "package/mypkg"
version = "1.0.0"
language = "rust"
"#;
        let pkgs = parse_vxmod(content);
        assert_eq!(pkgs.len(), 1);
        assert_eq!(pkgs[0].name, "mypkg");
        assert_eq!(pkgs[0].path, "package/mypkg");
        assert_eq!(pkgs[0].version, "1.0.0");
        assert_eq!(pkgs[0].language, "rust");
    }

    #[test]
    fn test_parse_vxmod_multiple() {
        let content = r#"
[a]
path = "package/a"
version = "1.0.0"
language = "rust"

[b]
path = "package/b"
version = "2.0.0"
language = "python"
"#;
        let pkgs = parse_vxmod(content);
        assert_eq!(pkgs.len(), 2);
        assert_eq!(pkgs[0].name, "a");
        assert_eq!(pkgs[1].name, "b");
    }

    #[test]
    fn test_encode_libs_env() {
        let settings = VxSettings {
            source_dir: ".".to_string(),
            bin: None,
            vxlib: None,
            lib: None,
            modules: vec![],
            vxset: Default::default(),
            libraries: HashMap::new(),
            compiler: None,
            linker: None,
            memory: None,
        };
        let builder = VxBuilder::new(settings);
        let mut libs = HashMap::new();
        libs.insert("a".to_string(), "/p/a".to_string());
        libs.insert("b".to_string(), "/p/b".to_string());
        let env = builder.encode_libs_env(&libs);
        // 顺序不固定, 检查两项都存在
        assert!(env.contains("a=/p/a"));
        assert!(env.contains("b=/p/b"));
    }

    #[test]
    fn test_resolve_source_path_relative() {
        let settings = VxSettings {
            source_dir: "/proj".to_string(),
            bin: None,
            vxlib: None,
            lib: None,
            modules: vec![],
            vxset: Default::default(),
            libraries: HashMap::new(),
            compiler: None,
            linker: None,
            memory: None,
        };
        let builder = VxBuilder::new(settings);
        assert_eq!(
            builder.resolve_source_path("src/main.vx"),
            PathBuf::from("/proj/src/main.vx")
        );
    }

    #[test]
    fn test_resolve_source_path_absolute() {
        let settings = VxSettings {
            source_dir: "/proj".to_string(),
            bin: None,
            vxlib: None,
            lib: None,
            modules: vec![],
            vxset: Default::default(),
            libraries: HashMap::new(),
            compiler: None,
            linker: None,
            memory: None,
        };
        let builder = VxBuilder::new(settings);
        assert_eq!(
            builder.resolve_source_path("/abs/main.vx"),
            PathBuf::from("/abs/main.vx")
        );
    }
}
