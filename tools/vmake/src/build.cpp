// build.cpp — 构建编排实现：Cargo 风格状态行 + vxc/vlnk 调用顺序。
// 每个目标：所有源文件各自 vxc → 一份 .vxobj；随后 vlnk 合并全部 .vxobj 链接。
//
// 增量判定采用内容哈希签名，而非 mtime：
//   - 对象签名 = 工具身份 + 导入依赖签名 + 源文件内容哈希 + 源路径
//   - 链接签名 = vlnk 身份 + 导入依赖签名 + 目标描述 + 全部对象内容哈希
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
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <vector>

namespace vmake {
namespace {

namespace fs = std::filesystem;

constexpr std::string_view RESET = "\033[0m";
constexpr std::string_view GREEN = "\033[1;32m";
constexpr std::string_view RED = "\033[1;31m";
constexpr std::string_view CYAN = "\033[1;36m";

// Cargo 风格状态行：右对齐 12 列标签 + 消息（写 stderr）
void status_line(std::string_view label, std::string_view color,
                 const std::string &message, bool use_color) {
  const std::size_t pad = label.size() < 12 ? 12 - label.size() : 0;
  std::fprintf(stderr, "%*s", static_cast<int>(pad), "");
  if (use_color) {
    std::fprintf(stderr, "%s%.*s%s", std::string(color).c_str(),
                 static_cast<int>(label.size()), label.data(),
                 std::string(RESET).c_str());
  } else {
    std::fprintf(stderr, "%.*s", static_cast<int>(label.size()), label.data());
  }
  std::fprintf(stderr, " %s\n", message.c_str());
}

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

// 导入依赖签名：headers 根目录全部文件 + 项目内 .vxh（排除 build 目录）
std::string dependency_signature(const BuildOptions &opts) {
  std::vector<fs::path> files;
  collect_files(opts.headers_root, {}, false, files);
  collect_files(opts.manifest_dir, opts.build_dir, true, files);
  std::sort(files.begin(), files.end());
  files.erase(std::unique(files.begin(), files.end()), files.end());

  std::string sig;
  add_field(sig, "vmake-deps-v1");
  add_field(sig, opts.headers_root.string());
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
                             std::string_view source, std::string_view digest) {
  std::string sig;
  add_field(sig, "vmake-obj-v1");
  add_field(sig, vxc_id);
  add_field(sig, deps_id);
  add_field(sig, source);
  add_field(sig, digest);
  return sig_digest(sig);
}

std::string link_signature(std::string_view vlnk_id, std::string_view deps_id,
                           const Target &target,
                           const std::vector<std::string> &object_digests) {
  std::string sig;
  add_field(sig, "vmake-link-v1");
  add_field(sig, vlnk_id);
  add_field(sig, deps_id);
  add_field(sig, target.name);
  add_field(sig, kind_name(target.kind));
  add_field(sig, target.output ? *target.output : "");
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
  if (host_os == "macosx")
    return "lib" + std::string(name) + ".dylib";
  if (host_os == "linux")
    return "lib" + std::string(name) + ".so";
  if (host_os == "windows")
    return std::string(name) + ".dll";
  return "lib" + std::string(name) + ".so";
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
  auto wanted = [&](const Target &t) {
    return selected.empty() || std::find(selected.begin(), selected.end(),
                                         t.name) != selected.end();
  };
  std::vector<const Target *> plan;
  // 动态库先构建：可执行文件可能通过导入/链接依赖它们。
  for (const Target &t : manifest.targets) {
    if (t.kind == TargetKind::Dylib && wanted(t))
      plan.push_back(&t);
  }
  for (const Target &t : manifest.targets) {
    if (t.kind == TargetKind::Exe && wanted(t))
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
          object_signature(vxc_id, deps_id, t->sources[i], source_digest);

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
      if (opts.verbose)
        job.args.push_back("-v");
      jobs.push_back(std::move(job));
    }

    if (any_stale) {
      tp.link_needed = true;
    } else {
      const std::string lsig =
          link_signature(vlnk_id, deps_id, *t, tp.object_digests);
      tp.link_needed = !up_to_date(tp.artifact, lsig);
    }
    plans.push_back(std::move(tp));
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

  // 阶段 2：按计划顺序链接（对象已就绪，签名按最终对象内容重算）
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
    const std::string lsig = link_signature(vlnk_id, deps_id, t, digests);

    std::vector<std::string> args;
    for (const fs::path &obj : tp.objects)
      args.push_back(obj.string());
    args.push_back("-o");
    args.push_back(tp.artifact.string());
    if (t.kind == TargetKind::Dylib)
      args.push_back("--shared");
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
