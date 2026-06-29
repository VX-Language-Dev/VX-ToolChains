// ==================== VX 构建缓存 ====================
//
// 实现 `vpmfile/vpm/buildcache.toml` 持久化增量构建缓存,
// 对应 `vxsetting.toml` 中 `[vxset].cache` 开关。
//
// 缓存策略:
//   - 快速路径: 源文件 mtime + size 指纹比对 (零 IO 开销)
//   - 精确路径: mtime/size 变化时计算内容哈希 (FNV-1a 64-bit) 兜底,
//               内容未变则仍视为新鲜 (应对 mtime 回拨/触碰)
//   - 全局失效: vxsetting.toml / vxmod.toml 内容哈希变化 → 全部失效
//   - 优化等级变化 → 该目标失效 (不同 opt 产物不可复用)
//   - 损坏降级: 解析失败时备份 .bak 并返回空缓存, 不阻断构建
//   - 原子写入: tempfile + rename, 避免并发写冲突

use std::collections::HashMap;
use std::fs;
use std::path::{Path, PathBuf};
use std::time::{SystemTime, UNIX_EPOCH};

use serde::{Deserialize, Serialize};

/// 工具链版本 (取自 Cargo.toml, 用于跨版本缓存失效)
pub const TOOLCHAIN_VERSION: &str = env!("CARGO_PKG_VERSION");

// ==================== 缓存文件结构 (TOML) ====================

/// `buildcache.toml` 顶层结构
#[derive(Debug, Clone, Serialize, Deserialize, Default)]
pub struct CacheFile {
    pub meta: CacheMeta,
    /// 目标键 → 缓存条目 (键: "bin"/"vxlib"/"lib"/"module:<name>")
    #[serde(default)]
    pub targets: HashMap<String, CacheEntry>,
}

/// 全局元数据 (任一字段变化 → 全部目标失效)
#[derive(Debug, Clone, Serialize, Deserialize, Default)]
pub struct CacheMeta {
    /// vxsetting.toml 内容哈希
    pub vxsetting_hash: String,
    /// vxmod.toml 内容哈希 (无 vxmod.toml 时为 None)
    #[serde(default)]
    pub vxmod_hash: Option<String>,
    /// 最后构建时间 (Unix 秒)
    pub timestamp: u64,
    /// 工具链版本
    pub toolchain_version: String,
}


/// 单个构建目标的缓存条目
#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct CacheEntry {
    /// 源文件路径列表 (绝对路径)
    pub sources: Vec<String>,
    /// 快速指纹: "mtime_secs;size" (与 sources 一一对应)
    pub sources_fingerprint: Vec<String>,
    /// 精确内容哈希: FNV-1a 64-bit hex (与 sources 一一对应)
    #[serde(default)]
    pub sources_hash: Vec<String>,
    /// 主产物 .vxobj 路径
    pub obj_path: String,
    /// 最终输出路径
    pub output: String,
    /// 输出产物是否存在
    pub output_exists: bool,
    /// 最后构建时间 (Unix 秒)
    pub last_build: u64,
    /// 构建时使用的优化等级
    pub opt_level: u8,
}

// ==================== 构建缓存 ====================

/// 构建缓存: 负责加载/判定/更新/持久化 `buildcache.toml`
pub struct BuildCache {
    file: CacheFile,
    path: PathBuf,
    /// 是否从磁盘成功加载 (false = 新建或损坏降级)
    loaded: bool,
}

impl BuildCache {
    /// 从磁盘加载缓存; 文件不存在或损坏时返回空缓存 (损坏文件备份为 `.bak`)
    pub fn load(cache_path: &Path) -> Self {
        if !cache_path.exists() {
            return Self::empty(cache_path);
        }
        let content = match fs::read_to_string(cache_path) {
            Ok(c) => c,
            Err(_) => return Self::empty(cache_path),
        };
        match toml::from_str::<CacheFile>(&content) {
            Ok(file) => Self {
                file,
                path: cache_path.to_path_buf(),
                loaded: true,
            },
            Err(e) => {
                // 损坏: 备份原文件后降级为空缓存
                let bak = cache_path.with_extension("toml.bak");
                if let Err(write_err) = fs::write(&bak, &content) {
                    eprintln!(
                        "[VXBUILD 警告] 缓存文件损坏, 但备份至 {} 失败: {}; 降级为全量构建 ({})",
                        bak.display(),
                        write_err,
                        e
                    );
                } else {
                    eprintln!(
                        "[VXBUILD 警告] 缓存文件损坏, 已备份至 {} 并降级为全量构建 ({})",
                        bak.display(),
                        e
                    );
                }
                Self::empty(cache_path)
            }
        }
    }

    /// 新建空缓存
    pub fn empty(cache_path: &Path) -> Self {
        Self {
            file: CacheFile::default(),
            path: cache_path.to_path_buf(),
            loaded: false,
        }
    }

    /// 原子写入缓存文件 (tempfile + rename)
    pub fn save(&self) -> Result<(), String> {
        if let Some(parent) = self.path.parent() {
            fs::create_dir_all(parent).map_err(|e| format!("创建缓存目录失败: {}", e))?;
        }
        let content =
            toml::to_string_pretty(&self.file).map_err(|e| format!("序列化缓存失败: {}", e))?;
        let tmp = self.path.with_extension("toml.tmp");
        fs::write(&tmp, &content).map_err(|e| format!("写入缓存临时文件失败: {}", e))?;
        fs::rename(&tmp, &self.path).map_err(|e| format!("缓存原子替换失败: {}", e))?;
        Ok(())
    }

    /// 全局有效性: vxsetting/vxmod 哈希与工具链版本是否一致
    pub fn is_globally_valid(
        &self,
        vxsetting_hash: &str,
        vxmod_hash: Option<&str>,
        toolchain_version: &str,
    ) -> bool {
        if !self.loaded {
            return false;
        }
        self.file.meta.vxsetting_hash == vxsetting_hash
            && self.file.meta.vxmod_hash.as_deref() == vxmod_hash
            && (self.file.meta.toolchain_version.is_empty()
                || self.file.meta.toolchain_version == toolchain_version)
    }

    /// 设置全局元数据 (构建开始时调用, 记录当前配置指纹)
    pub fn set_meta(
        &mut self,
        vxsetting_hash: String,
        vxmod_hash: Option<String>,
        toolchain_version: String,
    ) {
        self.file.meta.vxsetting_hash = vxsetting_hash;
        self.file.meta.vxmod_hash = vxmod_hash;
        self.file.meta.toolchain_version = toolchain_version;
        self.file.meta.timestamp = now_unix();
        self.loaded = true;
    }

    /// 判断目标是否新鲜 (可跳过构建)
    ///
    /// 判定顺序: 条目存在 → opt 等级一致 → 源数量一致 → 产物存在 →
    ///           逐文件快速指纹; 不一致则退回内容哈希精确比对
    pub fn is_target_fresh(&self, key: &str, sources: &[PathBuf], opt_level: u8) -> bool {
        let entry = match self.file.targets.get(key) {
            Some(e) => e,
            None => return false,
        };
        if entry.opt_level != opt_level {
            return false;
        }
        if entry.sources.len() != sources.len() {
            return false;
        }
        // 防御: update_entry 中 filter_map 可能在文件元数据读取瞬时失败时
        // 产生短于 sources 的指纹/哈希向量, 此处缺长度校验会导致越界 panic。
        if entry.sources_fingerprint.len() != sources.len()
            || entry.sources_hash.len() != sources.len()
        {
            return false;
        }
        if !entry.output_exists || !Path::new(&entry.output).exists() {
            return false;
        }
        for (i, src) in sources.iter().enumerate() {
            let cur_fp = match file_fingerprint(src) {
                Some(f) => f,
                None => return false,
            };
            if cur_fp == entry.sources_fingerprint[i] {
                continue; // 快速路径命中
            }
            // mtime/size 变化: 退回精确内容哈希比对
            let cur_hash = match file_content_hash(src) {
                Some(h) => h,
                None => return false,
            };
            if entry.sources_hash.get(i) != Some(&cur_hash) {
                return false;
            }
        }
        true
    }

    /// 更新/插入目标缓存条目 (成功构建后调用)
    pub fn update_entry(
        &mut self,
        key: String,
        sources: &[PathBuf],
        obj_path: &str,
        output: &str,
        opt_level: u8,
    ) {
        // 闭包显式解引用 PathBuf → Path, 避免 fn 指针签名 (&Path) 与迭代项 (&PathBuf) 不匹配
        let fingerprints: Vec<String> = sources
            .iter()
            .filter_map(|p| file_fingerprint(p.as_path()))
            .collect();
        let hashes: Vec<String> = sources
            .iter()
            .filter_map(|p| file_content_hash(p.as_path()))
            .collect();
        self.file.targets.insert(
            key,
            CacheEntry {
                sources: sources
                    .iter()
                    .map(|p| p.to_string_lossy().to_string())
                    .collect(),
                sources_fingerprint: fingerprints,
                sources_hash: hashes,
                obj_path: obj_path.to_string(),
                output: output.to_string(),
                output_exists: Path::new(output).exists(),
                last_build: now_unix(),
                opt_level,
            },
        );
    }

    /// 失效单个目标
    pub fn invalidate(&mut self, key: &str) {
        self.file.targets.remove(key);
    }

    /// 失效全部目标 (保留 meta)
    pub fn invalidate_all(&mut self) {
        self.file.targets.clear();
    }

    /// 当前缓存的目标条目数
    pub fn target_count(&self) -> usize {
        self.file.targets.len()
    }

    /// 是否已从磁盘加载
    pub fn is_loaded(&self) -> bool {
        self.loaded
    }
}

// ==================== 哈希与指纹工具 ====================

/// FNV-1a 64-bit 哈希 (稳定, 跨进程一致, 无外部依赖)
///
/// 用于缓存内容指纹; 非加密强度, 仅作变更检测。
fn fnv1a_64(data: &[u8]) -> u64 {
    let mut hash: u64 = 0xcbf29ce484222325;
    for &b in data {
        hash ^= b as u64;
        hash = hash.wrapping_mul(0x100000001b3);
    }
    hash
}

/// 计算文件内容 FNV-1a 哈希, 返回 16 位十六进制字符串
fn file_content_hash(path: &Path) -> Option<String> {
    let data = fs::read(path).ok()?;
    Some(format!("{:016x}", fnv1a_64(&data)))
}

/// 文件指纹: "mtime_secs;size"
fn file_fingerprint(path: &Path) -> Option<String> {
    let meta = fs::metadata(path).ok()?;
    let size = meta.len();
    let mtime = meta.modified().ok()?;
    let secs = mtime
        .duration_since(UNIX_EPOCH)
        .map(|d| d.as_secs())
        .unwrap_or(0);
    Some(format!("{};{}", secs, size))
}

/// 计算 TOML 配置文件内容哈希 (用于全局失效判定)
pub fn config_hash(path: &Path) -> Option<String> {
    let data = fs::read(path).ok()?;
    Some(format!("{:016x}", fnv1a_64(&data)))
}

fn now_unix() -> u64 {
    SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .map(|d| d.as_secs())
        .unwrap_or(0)
}

// ==================== 测试 ====================
#[cfg(test)]
mod tests {
    use super::*;
    use std::fs::File;
    use std::io::Write;
    use std::time::Duration;
    use tempfile::TempDir;

    fn write_file(dir: &Path, name: &str, content: &str) -> PathBuf {
        let p = dir.join(name);
        let mut f = File::create(&p).unwrap();
        f.write_all(content.as_bytes()).unwrap();
        p
    }

    #[test]
    fn fnv1a_is_stable_and_distinguishes_content() {
        let a = fnv1a_64(b"hello");
        let b = fnv1a_64(b"hello");
        let c = fnv1a_64(b"world");
        assert_eq!(a, b, "相同内容哈希应一致");
        assert_ne!(a, c, "不同内容哈希应不同");
        // 已知向量 (FNV-1a 64 of "hello")
        assert_eq!(format!("{:016x}", a), "a430d84680aabd0b");
    }

    #[test]
    fn t1_load_missing_returns_empty() {
        let dir = TempDir::new().unwrap();
        let path = dir.path().join("buildcache.toml");
        let cache = BuildCache::load(&path);
        assert!(!cache.is_loaded());
        assert_eq!(cache.target_count(), 0);
        // 空缓存: 任何目标都不新鲜
        assert!(!cache.is_target_fresh("bin", &[], 20));
    }

    #[test]
    fn t2_save_load_roundtrip_and_fresh() {
        let dir = TempDir::new().unwrap();
        let src = write_file(dir.path(), "main.vx", "fn main() {}");
        let output = write_file(dir.path(), "app.bin", "BIN");
        let path = dir.path().join("buildcache.toml");

        let mut cache = BuildCache::empty(&path);
        cache.set_meta("h1".to_string(), None, TOOLCHAIN_VERSION.to_string());
        cache.update_entry(
            "bin".to_string(),
            std::slice::from_ref(&src),
            &src.to_string_lossy(),
            &output.to_string_lossy(),
            20,
        );
        cache.save().unwrap();

        // 重新加载: 应新鲜
        let reloaded = BuildCache::load(&path);
        assert!(reloaded.is_loaded());
        assert!(reloaded.is_globally_valid("h1", None, TOOLCHAIN_VERSION));
        assert!(reloaded.is_target_fresh("bin", &[src], 20));
    }

    #[test]
    fn t3_source_mtime_change_invalidates() {
        let dir = TempDir::new().unwrap();
        let src = write_file(dir.path(), "main.vx", "fn main() {}");
        let output = write_file(dir.path(), "app.bin", "BIN");
        let path = dir.path().join("buildcache.toml");

        let mut cache = BuildCache::empty(&path);
        cache.set_meta("h1".into(), None, TOOLCHAIN_VERSION.into());
        cache.update_entry("bin".into(), std::slice::from_ref(&src), "obj", &output.to_string_lossy(), 20);

        // 等待并修改源文件内容 (mtime + 内容均变)
        std::thread::sleep(Duration::from_millis(1100));
        write_file(dir.path(), "main.vx", "fn main() { changed }");

        assert!(!cache.is_target_fresh("bin", std::slice::from_ref(&src), 20));
    }

    #[test]
    fn t9_mtime_touch_same_content_stays_fresh() {
        let dir = TempDir::new().unwrap();
        let src = write_file(dir.path(), "main.vx", "fn main() {}");
        let output = write_file(dir.path(), "app.bin", "BIN");
        let path = dir.path().join("buildcache.toml");

        let mut cache = BuildCache::empty(&path);
        cache.set_meta("h1".into(), None, TOOLCHAIN_VERSION.into());
        cache.update_entry("bin".into(), std::slice::from_ref(&src), "obj", &output.to_string_lossy(), 20);

        // mtime 变化但内容相同: 精确哈希兜底 → 仍新鲜
        std::thread::sleep(Duration::from_millis(1100));
        write_file(dir.path(), "main.vx", "fn main() {}"); // 同内容重写

        assert!(
            cache.is_target_fresh("bin", &[src], 20),
            "mtime 变化但内容相同时应通过精确哈希判定为新鲜"
        );
    }

    #[test]
    fn t4_global_invalidation_on_config_hash_change() {
        let dir = TempDir::new().unwrap();
        let path = dir.path().join("buildcache.toml");
        let mut cache = BuildCache::empty(&path);
        cache.set_meta("hash-A".into(), None, TOOLCHAIN_VERSION.into());
        assert!(cache.is_globally_valid("hash-A", None, TOOLCHAIN_VERSION));
        assert!(!cache.is_globally_valid("hash-B", None, TOOLCHAIN_VERSION));
        assert!(!cache.is_globally_valid("hash-A", Some("modhash"), TOOLCHAIN_VERSION));
    }

    #[test]
    fn t5_toolchain_version_mismatch_invalidates() {
        let path = PathBuf::from("/nonexistent/buildcache.toml");
        let mut cache = BuildCache::empty(&path);
        cache.set_meta("h".into(), None, "1.0.0".into());
        assert!(cache.is_globally_valid("h", None, "1.0.0"));
        assert!(!cache.is_globally_valid("h", None, "2.0.0"));
    }

    #[test]
    fn t6_opt_level_change_invalidates() {
        let dir = TempDir::new().unwrap();
        let src = write_file(dir.path(), "main.vx", "fn main() {}");
        let output = write_file(dir.path(), "app.bin", "BIN");

        let mut cache = BuildCache::empty(Path::new("/tmp/x.toml"));
        cache.set_meta("h".into(), None, TOOLCHAIN_VERSION.into());
        cache.update_entry("bin".into(), std::slice::from_ref(&src), "obj", &output.to_string_lossy(), 10);

        assert!(cache.is_target_fresh("bin", std::slice::from_ref(&src), 10));
        assert!(!cache.is_target_fresh("bin", &[src], 20), "优化等级变化应失效");
    }

    #[test]
    fn t7_corrupt_file_backed_up_and_degraded() {
        let dir = TempDir::new().unwrap();
        let path = dir.path().join("buildcache.toml");
        fs::write(&path, "this is not === valid toml {{{").unwrap();

        let cache = BuildCache::load(&path);
        assert!(!cache.is_loaded(), "损坏文件应降级为空缓存");
        assert_eq!(cache.target_count(), 0);
        // 原文件应已备份
        let bak = dir.path().join("buildcache.toml.bak");
        assert!(bak.exists(), "损坏缓存应备份为 .bak");
    }

    #[test]
    fn t8_missing_target_not_fresh() {
        let cache = BuildCache::empty(Path::new("/tmp/none.toml"));
        assert!(!cache.is_target_fresh("bin", &[], 20));
        assert!(!cache.is_target_fresh("module:foo", &[], 0));
    }

    #[test]
    fn t10_invalidate_single_and_all() {
        let dir = TempDir::new().unwrap();
        let src = write_file(dir.path(), "main.vx", "x");
        let out = write_file(dir.path(), "o", "y");
        let mut cache = BuildCache::empty(Path::new("/tmp/x.toml"));
        cache.update_entry("bin".into(), std::slice::from_ref(&src), "obj", &out.to_string_lossy(), 20);
        cache.update_entry("vxlib".into(), std::slice::from_ref(&src), "obj", &out.to_string_lossy(), 20);
        assert_eq!(cache.target_count(), 2);

        cache.invalidate("bin");
        assert_eq!(cache.target_count(), 1);
        assert!(!cache.is_target_fresh("bin", std::slice::from_ref(&src), 20));
        assert!(cache.is_target_fresh("vxlib", &[src], 20));

        cache.invalidate_all();
        assert_eq!(cache.target_count(), 0);
    }

    #[test]
    fn t11_missing_output_invalidates() {
        let dir = TempDir::new().unwrap();
        let src = write_file(dir.path(), "main.vx", "x");
        // output 路径不存在
        let ghost_output = dir.path().join("ghost.bin");
        let mut cache = BuildCache::empty(Path::new("/tmp/x.toml"));
        cache.update_entry("bin".into(), std::slice::from_ref(&src), "obj", &ghost_output.to_string_lossy(), 20);
        // update_entry 会检测 output_exists = false
        assert!(!cache.is_target_fresh("bin", std::slice::from_ref(&src), 20));
    }

    #[test]
    fn config_hash_stable_for_same_content() {
        let dir = TempDir::new().unwrap();
        let p = write_file(dir.path(), "vxsetting.toml", "[bin]\nsource=\"m.vx\"\n");
        let h1 = config_hash(&p).unwrap();
        let h2 = config_hash(&p).unwrap();
        assert_eq!(h1, h2);
        write_file(dir.path(), "vxsetting.toml", "[bin]\nsource=\"other.vx\"\n");
        let h3 = config_hash(&p).unwrap();
        assert_ne!(h1, h3);
    }

    #[test]
    fn save_creates_parent_dirs() {
        let dir = TempDir::new().unwrap();
        let nested = dir.path().join("vpmfile").join("vpm").join("buildcache.toml");
        let cache = BuildCache::empty(&nested);
        cache.save().unwrap();
        assert!(nested.exists());
    }
}
