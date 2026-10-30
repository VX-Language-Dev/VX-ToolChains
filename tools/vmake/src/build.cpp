// build.cpp — 构建编排实现：Cargo 风格状态行 + vxc/vlnk 调用顺序。
// 每个目标：所有源文件各自 vxc → 一份 .vxobj；随后 vlnk 合并全部 .vxobj 链接。
//
// 增量判定采用内容哈希签名，而非 mtime：
//   - 对象签名 = 工具身份 + 导入依赖签名 + 源文件内容哈希 + 源路径
//   - 链接签名 = vlnk 身份 + 导入依赖签名 + 目标描述 + 全部对象内容哈希
//     + profile + cflags/defines/ldflags（链接期 clang 参数变化 → 仅重链）
// 签名写入产物旁的 <产物>.sig 旁路文件。内容未变的 touch 不会重编，也不会重链。
// 导入依赖签名覆盖 headers 根目录（@imp/@cppimp 的 .vxh/.c/.a 等）以及项目内
// 所有 .vxh，修复了头文件变更不触发重建的漏编问题。
#include "build.hpp"

#include "process.hpp"
#include "vx/util/platform.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <vector>

namespace vmake {
namespace {

namespace fs = std::filesystem;

constexpr std::string_view RESET = "\033[0m";
constexpr std::string_view RED = "\033[1;31m";

void error_line(const std::string &message, bool use_color) {
  if (use_color) {
    std::fprintf(stderr, "%serror%s: %s\n", std::string(RED).c_str(),
                 std::string(RESET).c_str(), message.c_str());
  } else {
    std::fprintf(stderr, "error: %s\n", message.c_str());
  }
}

void print_child_output(const std::string &output) {
  if (output.empty())
    return;
  std::fwrite(output.data(), 1, output.size(), stderr);
  if (output.back() != '\n')
    std::fputc('\n', stderr);
}

std::string format_duration(std::chrono::steady_clock::duration d) {
  const double seconds = std::chrono::duration<double>(d).count();
  char buf[32];
  std::snprintf(buf, sizeof(buf), "%.2fs", seconds);
  return buf;
}

// ---- 内容签名（FNV-1a 64） ----

constexpr std::uint64_t FNV_OFFSET = 1469598103934665603ULL;
constexpr std::uint64_t FNV_PRIME = 1099511628211ULL;

std::uint64_t fnv1a(std::string_view data, std::uint64_t h = FNV_OFFSET) {
  for (unsigned char c : data) {
    h ^= c;
    h *= FNV_PRIME;
  }
  return h;
}

std::string hex64(std::uint64_t v) {
  char buf[32];
  std::snprintf(buf, sizeof(buf), "%016llx",
                static_cast<unsigned long long>(v));
  return buf;
}

// 文件内容摘要：内容字节 + 总长度（读入失败返回 false）
bool file_digest(const fs::path &path, std::string &out) {
  std::ifstream in(path, std::ios::binary);
  if (!in)
    return false;
  std::uint64_t h = FNV_OFFSET;
  std::uint64_t total = 0;
  char buf[65536];
  while (in) {
    in.read(buf, sizeof(buf));
    const std::streamsize n = in.gcount();
    if (n > 0) {
      h = fnv1a(std::string_view(buf, static_cast<std::size_t>(n)), h);
      total += static_cast<std::uint64_t>(n);
    }
  }
  if (in.bad())
    return false;
  out = hex64(h ^ (total * FNV_PRIME));
  return true;
}

// 签名字段：长度前缀 + 内容，避免字段拼接歧义
void add_field(std::string &sig, std::string_view value) {
  sig += std::to_string(value.size());
  sig += ':';
  sig.append(value);
  sig += '\n';
}

std::string sig_digest(const std::string &text) { return hex64(fnv1a(text)); }

// 工具身份：路径 + 大小 + mtime（vxc/vlnk 重建后哈希变化，令旧签名失效）
std::string tool_identity(const std::string &program) {
  std::string sig;
  add_field(sig, "vmake-tool-v1");
  add_field(sig, program);
  std::error_code ec;
  const fs::path p(program);
  const auto size = fs::file_size(p, ec);
  if (!ec)
    add_field(sig, std::to_string(size));
  ec.clear();
  const auto mtime = fs::last_write_time(p, ec);
  if (!ec)
    add_field(sig,
              std::to_string(static_cast<long long>(
                  mtime.time_since_epoch().count())));
  return sig_digest(sig);
}

bool path_is_under(const fs::path &child, const fs::path &parent) {
  if (parent.empty())
    return false;
  const std::string c = child.lexically_normal().string();
  const std::string p = parent.lexically_normal().string();
  if (p.empty() || c.size() < p.size())
    return false;
  if (c.compare(0, p.size(), p) != 0)
    return false;
  return c.size() == p.size() || p.back() == fs::path::preferred_separator ||
         c[p.size()] == fs::path::preferred_separator;
}

// 递归收集普通文件；skip 子树不进入；only_vxh 时仅收 .vxh
void collect_files(const fs::path &root, const fs::path &skip, bool only_vxh,
                   std::vector<fs::path> &out) {
  std::error_code ec;
  if (root.empty() || !fs::is_directory(root, ec) || ec)
    return;
  fs::recursive_directory_iterator it(
      root, fs::directory_options::skip_permission_denied, ec);
  if (ec)
    return;
  const fs::recursive_directory_iterator end;
  for (; it != end; it.increment(ec)) {
    if (ec) {
      ec.clear();
      continue;
    }
    const fs::path p = it->path();
    if (!skip.empty() && path_is_under(p, skip)) {
      it.disable_recursion_pending();
      continue;
    }
    std::error_code fec;
    if (!it->is_regular_file(fec) || fec)
      continue;
    if (only_vxh && p.extension() != ".vxh")
      continue;
    out.push_back(p);
  }
}

// 导入依赖签名：headers 根目录全部文件 + -I 目录全部文件 +
// 项目内 .vxh（排除 build 目录）
std::string dependency_signature(const BuildOptions &opts) {
  std::vector<fs::path> files;
  collect_files(opts.headers_root, {}, false, files);
  for (const std::string &inc : opts.include_dirs)
    collect_files(fs::path(inc), {}, false, files);
  collect_files(opts.manifest_dir, opts.build_dir, true, files);
  std::sort(files.begin(), files.end());
  files.erase(std::unique(files.begin(), files.end()), files.end());

  std::string sig;
  add_field(sig, "vmake-deps-v1");
  add_field(sig, opts.headers_root.string());
  for (const std::string &inc : opts.include_dirs)
    add_field(sig, inc);
  for (const fs::path &f : files) {
    std::string d;
    if (!file_digest(f, d))
      d = "unreadable";
    add_field(sig, f.string());
    add_field(sig, d);
  }
  return sig_digest(sig);
}

std::string object_signature(std::string_view vxc_id, std::string_view deps_id,
                             std::string_view target,
                             std::string_view source, std::string_view digest) {
  std::string sig;
  add_field(sig, "vmake-obj-v1");
  add_field(sig, vxc_id);
  add_field(sig, deps_id);
  add_field(sig, target);
  add_field(sig, source);
  add_field(sig, digest);
  return sig_digest(sig);
}

// profile 默认 clang 参数：链接期单次 clang 调用同时编译 .ll/.c/.cpp，
// debug 补 -g，release 补 -O2（vxc 前端不受 profile 影响，对象不重编）。
std::vector<std::string> profile_cflags(std::string_view profile) {
  if (profile == "release")
    return {"-O2"};
  return {"-g"};
}

// 目标的最终链接参数：profile 默认值 + 目标 cflags + defines（→ -D...），
// ldflags 单独成列（置于 clang 命令末尾）。参与链接签名。
struct LinkFlags {
  std::vector<std::string> cflags;
  std::vector<std::string> ldflags;
};

LinkFlags target_link_flags(const Target &t, const BuildOptions &opts) {
  LinkFlags f;
  f.cflags = profile_cflags(opts.profile);
  f.cflags.insert(f.cflags.end(), t.cflags.begin(), t.cflags.end());
  for (const std::string &d : t.defines)
    f.cflags.push_back(d.starts_with("-D") ? d : "-D" + d);
  f.ldflags = t.ldflags;
  return f;
}

std::string link_signature(std::string_view vlnk_id, std::string_view deps_id,
                           std::string_view target,
                           const Target &target_struct,
                           const std::vector<std::string> &object_digests,
                           const std::vector<std::pair<std::string, std::string>> &dep_libs,
                           std::string_view profile,
                           const std::vector<std::string> &cflags,
                           const std::vector<std::string> &ldflags) {
  std::string sig;
  add_field(sig, "vmake-link-v2");
  add_field(sig, vlnk_id);
  add_field(sig, deps_id);
  add_field(sig, target);
  add_field(sig, profile);
  add_field(sig, target_struct.name);
  add_field(sig, kind_name(target_struct.kind));
  add_field(sig, target_struct.output ? *target_struct.output : "");
  for (const std::string &d : object_digests)
    add_field(sig, d);
  for (const auto &[path, digest] : dep_libs) {
    add_field(sig, path);
    add_field(sig, digest);
  }
  for (const std::string &d : target_struct.lib_dirs)
    add_field(sig, d);
  for (const std::string &l : target_struct.libs)
    add_field(sig, l);
  for (const std::string &f : cflags)
    add_field(sig, f);
  for (const std::string &f : ldflags)
    add_field(sig, f);
  return sig_digest(sig);
}

std::string staticlib_signature(std::string_view vxc_id, std::string_view deps_id,
                                std::string_view target,
                                const Target &target_struct,
                                const std::vector<std::string> &object_digests) {
  std::string sig;
  add_field(sig, "vmake-ar-v1");
  add_field(sig, vxc_id);
  add_field(sig, deps_id);
  add_field(sig, target);
  add_field(sig, target_struct.name);
  add_field(sig, kind_name(target_struct.kind));
  for (const std::string &d : object_digests)
    add_field(sig, d);
  return sig_digest(sig);
}

fs::path sidecar_path(const fs::path &artifact) {
  fs::path p = artifact;
  p += ".sig";
  return p;
}

bool read_sig(const fs::path &artifact, std::string &out) {
  std::ifstream in(sidecar_path(artifact), std::ios::binary);
  if (!in)
    return false;
  std::getline(in, out);
  return !out.empty();
}

bool write_sig(const fs::path &artifact, std::string_view sig) {
  const fs::path path = sidecar_path(artifact);
  const fs::path tmp = path.string() + ".tmp";
  {
    std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
    if (!out)
      return false;
    out << sig << '\n';
    if (!out.good())
      return false;
  }
  std::error_code ec;
  fs::rename(tmp, path, ec);
  if (ec) {
    fs::remove(tmp, ec);
    return false;
  }
  return true;
}

// 产物存在且签名旁路文件内容一致 → 视为最新
bool up_to_date(const fs::path &artifact, std::string_view sig) {
  std::error_code ec;
  if (!fs::is_regular_file(artifact, ec) || ec)
    return false;
  std::string got;
  return read_sig(artifact, got) && got == sig;
}

std::size_t worker_count(const BuildOptions &opts, std::size_t job_count) {
  std::size_t n = opts.jobs > 0
                      ? static_cast<std::size_t>(opts.jobs)
                      : static_cast<std::size_t>(
                            std::thread::hardware_concurrency());
  if (n == 0)
    n = 4;
  return std::min(n, job_count);
}

// 判断库条目是否是路径型（含路径分隔符或常见后缀）→ --lib；否则裸名 → -l
bool looks_like_lib_path(std::string_view lib) {
  if (lib.find('/') != std::string_view::npos) return true;
#ifdef _WIN32
  if (lib.find('\\') != std::string_view::npos) return true;
#endif
  const char *exts[] = {".a", ".dylib", ".so", ".lib", ".dll"};
  for (const char *e : exts) {
    std::size_t elen = std::strlen(e);
    if (lib.size() >= elen &&
        lib.compare(lib.size() - elen, elen, e) == 0) return true;
  }
  return false;
}

// 查找 ar 工具：VX_AR 环境变量 > PATH 中的 ar
std::string find_ar() {
  if (const char *env = std::getenv("VX_AR"); env != nullptr && env[0] != '\0')
    return env;
  // Windows 上可能是 lib.exe
  const char *candidates[] = {"ar", "llvm-ar", "lib.exe"};
  for (const char *c : candidates) {
    // 用 which 查找
    std::error_code ec;
    fs::path found;
    // 先看 PATH
    if (const char *path = std::getenv("PATH"); path != nullptr) {
      std::string p = path;
      std::size_t start = 0;
      while (start <= p.size()) {
        std::size_t end = p.find(':', start);
        if (end == std::string::npos) end = p.size();
        fs::path cand = fs::path(std::string(p.substr(start, end - start))) / c;
        if (fs::is_regular_file(cand, ec) && !ec)
          return cand.string();
        start = end + 1;
      }
    }
  }
  return "ar"; // 兜底：让子进程自己报 "command not found"
}

struct TargetPlan {
  const Target *target = nullptr;
  fs::path artifact;
  std::vector<fs::path> sources;
  std::vector<fs::path> objects;
  std::vector<std::string> object_digests;
  bool link_needed = false;
};

struct CompileJob {
  std::size_t plan_index = 0;
  std::size_t source_index = 0;
  fs::path source;
  fs::path object;
  std::string sig;
  std::vector<std::string> args;
};

// 线程池执行编译任务：固定 worker 数量 + 原子任务队列（无批屏障、无每任务线程）。
// 首个失败（按任务序号）在全部 worker 退出后统一打印。
int run_compile_jobs(const std::vector<CompileJob> &jobs,
                     const std::vector<TargetPlan> &plans,
                     const BuildOptions &opts) {
  if (jobs.empty())
    return 0;
  std::atomic<std::size_t> next{0};
  std::atomic<bool> stop{false};
  std::mutex failure_mutex;
  bool failed = false;
  std::size_t failed_index = 0;
  ProcessResult failed_result;

  auto worker = [&]() {
    while (!stop.load(std::memory_order_relaxed)) {
      const std::size_t k = next.fetch_add(1);
      if (k >= jobs.size())
        return;
      ProcessResult r = run_process(opts.vxc, jobs[k].args, opts.child_env);
      if (!r.ok) {
        std::lock_guard<std::mutex> lk(failure_mutex);
        if (!failed || k < failed_index) {
          failed = true;
          failed_index = k;
          failed_result = std::move(r);
        }
        stop.store(true, std::memory_order_relaxed);
        return;
      }
      write_sig(jobs[k].object, jobs[k].sig);
    }
  };

  const std::size_t workers = worker_count(opts, jobs.size());
  std::vector<std::thread> threads;
  threads.reserve(workers);
  for (std::size_t i = 0; i < workers; ++i)
    threads.emplace_back(worker);
  for (std::thread &th : threads)
    th.join();

  if (!failed)
    return 0;
  print_child_output(failed_result.output);
  if (!failed_result.error.empty())
    std::fprintf(stderr, "  %s\n", failed_result.error.c_str());
  const Target &t = *plans[jobs[failed_index].plan_index].target;
  error_line("could not compile `" + t.name + "` (" + kind_name(t.kind) + ")",
             opts.color);
  return 1;
}

} // namespace

std::string default_output_name(TargetKind kind, std::string_view name,
                                std::string_view host_os) {
  if (kind == TargetKind::Exe) {
    return host_os == "windows" ? std::string(name) + ".exe"
                                : std::string(name);
  }
  if (kind == TargetKind::StaticLib) {
    if (host_os == "windows") return std::string(name) + ".lib";
    return "lib" + std::string(name) + ".a";
  }
  // Dylib
  if (host_os == "macosx")
    return "lib" + std::string(name) + ".dylib";
  if (host_os == "linux")
    return "lib" + std::string(name) + ".so";
  if (host_os == "windows")
    return std::string(name) + ".dll";
  return "lib" + std::string(name) + ".so";
}

// 由原目标派生测试可执行目标（vmake test 构建与运行的单元）。
// 源组成规则（vlnk 合并期同一模块内函数名不得重复，且每程序仅一个入口）：
//   - staticlib/dylib：无入口 → 测试二进制并入目标自身源文件，测试文件提供 main
//   - exe：源文件含 main，与测试入口冲突 → 仅用测试源，经 deps/libs 引用被测代码
std::optional<Target> test_target_of(const Target &target) {
  if (target.tests.empty())
    return std::nullopt;
  Target t;
  t.name = target.name + "-test";
  t.kind = TargetKind::Exe;
  if (target.kind != TargetKind::Exe)
    t.sources = target.sources;
  t.sources.insert(t.sources.end(), target.tests.begin(), target.tests.end());
  t.deps = target.deps;
  t.libs = target.libs;
  t.lib_dirs = target.lib_dirs;
  t.cflags = target.cflags;
  t.defines = target.defines;
  t.ldflags = target.ldflags;
  t.line = target.line;
  return t;
}

fs::path artifact_path(const fs::path &build_dir, const Target &target) {
  if (target.output)
    return build_dir / *target.output;
  return build_dir /
         default_output_name(target.kind, target.name, vx::util::host_os());
}

fs::path object_path(const fs::path &build_dir, const Target &target,
                     std::size_t source_index, std::string_view source) {
  std::string stem = fs::path(source).stem().string();
  if (stem.empty())
    stem = "src";
  return build_dir / "obj" / target.name /
         (std::to_string(source_index) + "_" + stem + ".vxobj");
}

std::vector<const Target *>
plan_order(const Manifest &manifest, const std::vector<std::string> &selected) {
  // 目标索引：name → target
  std::map<std::string, const Target *> by_name;
  for (const Target &t : manifest.targets) by_name.emplace(t.name, &t);

  // 计算纳入计划的目标集合：selected 非空时做依赖传递闭包展开
  //（deps 均为 dylib，先于被依赖方构建；此处保证依赖目标也被构建）。
  std::set<std::string> wanted;
  if (selected.empty()) {
    for (const Target &t : manifest.targets) wanted.insert(t.name);
  } else {
    std::vector<const Target *> queue;
    for (const std::string &name : selected) {
      const auto it = by_name.find(name);
      if (it != by_name.end()) queue.push_back(it->second);
    }
    while (!queue.empty()) {
      const Target *t = queue.back();
      queue.pop_back();
      if (!wanted.insert(t->name).second) continue;
      for (const std::string &dep : t->deps) {
        const auto it = by_name.find(dep);
        if (it != by_name.end()) queue.push_back(it->second);
      }
    }
  }

  auto is_wanted = [&](const Target &t) { return wanted.count(t.name) > 0; };
  std::vector<const Target *> plan;
  // StaticLib 先构建 → Dylib → Exe：静态库是对象归档，dylib/exe 可依赖；
  // dylib 先于 exe：exe 可依赖 dylib。组内保持声明顺序。
  for (const Target &t : manifest.targets) {
    if (t.kind == TargetKind::StaticLib && is_wanted(t))
      plan.push_back(&t);
  }
  for (const Target &t : manifest.targets) {
    if (t.kind == TargetKind::Dylib && is_wanted(t))
      plan.push_back(&t);
  }
  for (const Target &t : manifest.targets) {
    if (t.kind == TargetKind::Exe && is_wanted(t))
      plan.push_back(&t);
  }
  return plan;
}

int build_manifest(const Manifest &manifest, const BuildOptions &opts) {
  const std::vector<const Target *> plan = plan_order(manifest, opts.selected);

  // 预检：源文件必须存在（一次报告全部缺失项，避免半途失败）
  bool missing = false;
  for (const Target *t : plan) {
    for (const std::string &src : t->sources) {
      const fs::path abs = opts.manifest_dir / src;
      std::error_code ec;
      if (!fs::is_regular_file(abs, ec) || ec) {
        error_line("target '" + t->name + "' source file does not exist: " + abs.string(),
                   opts.color);
        missing = true;
      }
    }
  }
  if (missing)
    return 1;

  const auto start = std::chrono::steady_clock::now();

  // ---- 计划阶段：计算签名，决定哪些对象需要重编、哪些产物需要重链 ----
  const std::string vxc_id = tool_identity(opts.vxc);
  const std::string vlnk_id = tool_identity(opts.vlnk);
  const std::string deps_id = dependency_signature(opts);

  std::vector<TargetPlan> plans;
  plans.reserve(plan.size());
  std::vector<CompileJob> jobs;

  // 依赖产物索引：目标名 → 产物路径（artifact_path 为纯函数，对全 manifest 计算）
  std::map<std::string, fs::path> dep_artifact;
  for (const Target &t : manifest.targets)
    dep_artifact.emplace(t.name, artifact_path(opts.build_dir, t));

  // ---- 第一遍：对象级判定（编译任务收集 + any_stale）----
  for (const Target *t : plan) {
    TargetPlan tp;
    tp.target = t;
    tp.artifact = artifact_path(opts.build_dir, *t);
    tp.sources.reserve(t->sources.size());
    tp.objects.reserve(t->sources.size());
    tp.object_digests.reserve(t->sources.size());

    bool any_stale = false;
    for (std::size_t i = 0; i < t->sources.size(); ++i) {
      const fs::path source = opts.manifest_dir / t->sources[i];
      const fs::path object = object_path(opts.build_dir, *t, i, t->sources[i]);
      tp.sources.push_back(source);
      tp.objects.push_back(object);

      std::string source_digest;
      if (!file_digest(source, source_digest)) {
        error_line("cannot read source file: " + source.string(), opts.color);
        return 1;
      }
      const std::string osig =
          object_signature(vxc_id, deps_id, opts.target, t->sources[i], source_digest);

      std::string object_digest;
      if (up_to_date(object, osig) && file_digest(object, object_digest)) {
        tp.object_digests.push_back(std::move(object_digest));
        continue;
      }

      // 该对象需重编；对象内容必然变化 → 目标必须重链
      any_stale = true;
      tp.object_digests.emplace_back();
      CompileJob job;
      job.plan_index = plans.size();
      job.source_index = i;
      job.source = source;
      job.object = object;
      job.sig = osig;
      job.args = {source.string(), "-o", object.string(), "--color",
                  opts.color ? "always" : "never"};
      for (const std::string &inc : opts.include_dirs) {
        job.args.push_back("-I");
        job.args.push_back(inc);
      }
      if (!opts.target.empty()) {
        job.args.push_back("--target");
        job.args.push_back(opts.target);
      }
      if (opts.verbose)
        job.args.push_back("-v");
      jobs.push_back(std::move(job));
    }

    tp.link_needed = any_stale;
    plans.push_back(std::move(tp));
  }

  // ---- 第二遍：链接/归档级判定 ----
  // deps 均为 dylib 或 staticlib，且计划顺序保证被依赖方在前，因此处理某
  // 目标时可确定其依赖是否将重链/重建；dep 将重链 → 内容必变 → 哨兵摘要
  // 强制本目标重链。staticlib 用 staticlib_signature 判定（无依赖、无链接）。
  {
    std::map<std::string, TargetPlan *> by_name;
    for (TargetPlan &tp : plans) by_name.emplace(tp.target->name, &tp);
    for (TargetPlan &tp : plans) {
      if (tp.link_needed) continue;
      const Target &t = *tp.target;
      if (t.kind == TargetKind::StaticLib) {
        const std::string ssig =
            staticlib_signature(vxc_id, deps_id, opts.target, t, tp.object_digests);
        tp.link_needed = !up_to_date(tp.artifact, ssig);
        continue;
      }
      if (t.deps.empty()) {
        const LinkFlags flags = target_link_flags(t, opts);
        const std::string lsig =
            link_signature(vlnk_id, deps_id, opts.target, t, tp.object_digests,
                           {}, opts.profile, flags.cflags, flags.ldflags);
        tp.link_needed = !up_to_date(tp.artifact, lsig);
        continue;
      }
      std::vector<std::pair<std::string, std::string>> dep_libs;
      for (const std::string &dep : t.deps) {
        const auto da = dep_artifact.find(dep);
        const auto dp = by_name.find(dep);
        std::string digest = "unreadable";
        if (dp != by_name.end() && dp->second->link_needed) {
          digest = "relinking";
        } else if (da != dep_artifact.end()) {
          file_digest(da->second, digest);
        }
        if (da != dep_artifact.end())
          dep_libs.emplace_back(da->second.string(), digest);
      }
      const LinkFlags flags = target_link_flags(t, opts);
      const std::string lsig =
          link_signature(vlnk_id, deps_id, opts.target, t, tp.object_digests,
                         dep_libs, opts.profile, flags.cflags, flags.ldflags);
      tp.link_needed = !up_to_date(tp.artifact, lsig);
    }
  }

  std::size_t total_steps = jobs.size();
  for (const TargetPlan &tp : plans) {
    if (tp.link_needed)
      ++total_steps;
  }

  // 中间目录：有编译任务的目标需要 obj 目录
  for (const TargetPlan &tp : plans) {
    if (tp.link_needed) {
      std::error_code ec;
      fs::create_directories(opts.build_dir / "obj" / tp.target->name, ec);
      if (ec) {
        error_line("cannot create intermediate directory " +
                       (opts.build_dir / "obj" / tp.target->name).string() + ": " +
                       ec.message(),
                   opts.color);
        return 1;
      }
    }
  }
  for (const TargetPlan &tp : plans) {
    if (tp.link_needed && tp.artifact.has_parent_path()) {
      std::error_code ec;
      fs::create_directories(tp.artifact.parent_path(), ec);
      if (ec) {
        error_line("cannot create output directory " +
                       tp.artifact.parent_path().string() + ": " + ec.message(),
                   opts.color);
        return 1;
      }
    }
  }

  std::size_t step = 0;

  // 启动行一次性串行打印（避免交错）
  for (const CompileJob &job : jobs) {
    ++step;
    const Target &t = *plans[job.plan_index].target;
    std::fprintf(stderr, "[ %zu/%zu] %s %s\n", step, total_steps,
                 opts.color ? "\033[1;32mvxc\033[0m" : "vxc",
                 t.sources[job.source_index].c_str());
    if (opts.verbose) {
      std::fprintf(stderr, "  %s\n",
                   format_command(opts.vxc, job.args).c_str());
    }
  }

  // 阶段 1：并行编译（线程池，尊重 -j）
  if (run_compile_jobs(jobs, plans, opts) != 0)
    return 1;

  std::size_t compiled = 0;
  std::size_t fresh = 0;

  // 阶段 2：按计划顺序链接/归档（对象已就绪，签名按最终对象内容重算）
  for (TargetPlan &tp : plans) {
    const Target &t = *tp.target;
    if (!tp.link_needed) {
      ++fresh;
      if (opts.verbose) {
        std::fprintf(stderr, "vmake-builder: no work to do for '%s'\n",
                     t.name.c_str());
      }
      continue;
    }
    ++compiled;

    std::vector<std::string> digests;
    digests.reserve(tp.objects.size());
    for (const fs::path &obj : tp.objects) {
      std::string d;
      if (!file_digest(obj, d)) {
        error_line("cannot read object file: " + obj.string(), opts.color);
        return 1;
      }
      digests.push_back(std::move(d));
    }

    // ---- staticlib：ar 归档 ----
    if (t.kind == TargetKind::StaticLib) {
      const std::string ssig =
          staticlib_signature(vxc_id, deps_id, opts.target, t, digests);
      // macOS ranlib 可选：ar rcs 后 ranlib 索引（无 ranlib 也能工作，但链接时警告）
      std::string ar_path = find_ar();
      std::vector<std::string> args = {"rcs", tp.artifact.string()};
      for (const fs::path &obj : tp.objects)
        args.push_back(obj.string());

      ++step;
      std::fprintf(stderr, "[ %zu/%zu] %s %s\n", step, total_steps,
                   opts.color ? "\033[1;32mar\033[0m" : "ar",
                   tp.artifact.filename().c_str());

      if (opts.verbose) {
        std::fprintf(stderr, "  %s\n", format_command(ar_path, args).c_str());
      }
      ProcessResult r = run_process(ar_path, args, opts.child_env);
      if (!r.ok) {
        print_child_output(r.output);
        if (!r.error.empty())
          std::fprintf(stderr, "  %s\n", r.error.c_str());
        error_line("could not archive `" + t.name + "` (staticlib)",
                   opts.color);
        return 1;
      }
      if (!write_sig(tp.artifact, ssig) && opts.verbose) {
        std::fprintf(stderr,
                     "vmake-builder: warning: cannot write signature for %s\n",
                     tp.artifact.string().c_str());
      }
      continue;
    }

    // ---- dylib / exe：vlnk 链接 ----
    // deps（dylib 或 staticlib）：此时被依赖方（计划顺序在前）已链接/归档完毕
    std::vector<std::pair<std::string, std::string>> dep_libs;
    std::vector<std::string> lib_args;
    for (const std::string &dep : t.deps) {
      const auto da = dep_artifact.find(dep);
      if (da == dep_artifact.end())
        continue;
      std::error_code ec;
      if (!fs::is_regular_file(da->second, ec) || ec) {
        error_line("target '" + t.name + "' dependency artifact is missing: " +
                       da->second.string(),
                   opts.color);
        return 1;
      }
      std::string d;
      if (!file_digest(da->second, d))
        d = "unreadable";
      dep_libs.emplace_back(da->second.string(), d);
      lib_args.push_back(da->second.string());
    }
    // 外部 libs/lib_dirs 与链接参数（profile/cflags/defines/ldflags）：纳入链接签名
    const LinkFlags flags = target_link_flags(t, opts);
    const std::string lsig =
        link_signature(vlnk_id, deps_id, opts.target, t, digests, dep_libs,
                       opts.profile, flags.cflags, flags.ldflags);

    std::vector<std::string> args;
    for (const fs::path &obj : tp.objects)
      args.push_back(obj.string());
    args.push_back("-o");
    args.push_back(tp.artifact.string());
    if (t.kind == TargetKind::Dylib)
      args.push_back("--shared");
    if (!opts.target.empty()) {
      args.push_back("--target");
      args.push_back(opts.target);
    }
    // 额外 clang 参数（cflags + defines → -D...）：透传 vlnk --cflag
    for (const std::string &f : flags.cflags) {
      args.push_back("--cflag");
      args.push_back(f);
    }
    // lib_dirs：CLI --lib-dir 合并到目标配置；CLI 优先（放在前面）
    for (const std::string &ld : opts.lib_dirs_cli) {
      args.push_back("-L" + ld);
    }
    for (const std::string &ld : t.lib_dirs) {
      args.push_back("-L" + ld);
    }
    // deps 产物（dylib/staticlib）→ --lib <path>
    for (const std::string &lib : lib_args) {
      args.push_back("--lib");
      args.push_back(lib);
    }
    // 外部 libs：路径型 → --lib；裸名 → -l<name>
    for (const std::string &l : t.libs) {
      if (looks_like_lib_path(l)) {
        args.push_back("--lib");
        args.push_back(l);
      } else {
        args.push_back("-l" + l);
      }
    }
    // 链接器参数：透传 vlnk --ldflag（置于 clang 命令末尾）
    for (const std::string &f : flags.ldflags) {
      args.push_back("--ldflag");
      args.push_back(f);
    }
    if (opts.verbose)
      args.push_back("-v");

    ++step;
    std::fprintf(stderr, "[ %zu/%zu] %s %s\n", step, total_steps,
                 opts.color ? "\033[1;32mvlnk\033[0m" : "vlnk",
                 tp.artifact.filename().c_str());

    if (opts.verbose) {
      std::fprintf(stderr, "  %s\n", format_command(opts.vlnk, args).c_str());
    }
    ProcessResult r = run_process(opts.vlnk, args, opts.child_env);
    if (!r.ok) {
      print_child_output(r.output);
      if (!r.error.empty())
        std::fprintf(stderr, "  %s\n", r.error.c_str());
      error_line("could not link `" + t.name + "` (" + kind_name(t.kind) + ")",
                 opts.color);
      return 1;
    }
    if (!write_sig(tp.artifact, lsig) && opts.verbose) {
      std::fprintf(stderr,
                   "vmake-builder: warning: cannot write signature for %s\n",
                   tp.artifact.string().c_str());
    }
  }

  std::string summary =
      "build in " + format_duration(std::chrono::steady_clock::now() - start);
  if (opts.verbose) {
    summary += " (compiled " + std::to_string(compiled) + ", fresh " +
               std::to_string(fresh) + ")";
  }
  std::fprintf(stderr, "vmake-builder: %s\n", summary.c_str());
  return 0;
}

} // namespace vmake
