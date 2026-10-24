// build.cpp — 构建编排实现：Cargo 风格状态行 + vxc/vlnk 调用顺序。
// 每个目标：所有源文件各自 vxc → 一份 .vxobj；随后 vlnk 合并全部 .vxobj 链接。
#include "build.hpp"

#include "process.hpp"
#include "vx/util/platform.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <string>
#include <string_view>
#include <system_error>
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

bool target_is_fresh(const fs::path &manifest, const fs::path &artifact,
                     const std::vector<fs::path> &sources) {
  std::error_code ec;
  if (!fs::is_regular_file(artifact, ec) || ec)
    return false;
  const auto artifact_time = fs::last_write_time(artifact, ec);
  if (ec)
    return false;
  auto newer = [&](const fs::path &p) {
    const auto t = fs::last_write_time(p, ec);
    return !ec && t > artifact_time;
  };
  if (newer(manifest))
    return false;
  for (const fs::path &src : sources) {
    if (newer(src))
      return false;
  }
  return true;
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
        error_line("目标 '" + t->name + "' 的源文件不存在: " + abs.string(),
                   opts.color);
        missing = true;
      }
    }
  }
  if (missing)
    return 1;

  const auto start = std::chrono::steady_clock::now();
  std::size_t compiled = 0;
  std::size_t fresh = 0;

  for (const Target *t : plan) {
    const fs::path artifact = artifact_path(opts.build_dir, *t);
    const fs::path obj_dir = opts.build_dir / "obj" / t->name;

    std::vector<fs::path> source_paths;
    source_paths.reserve(t->sources.size());
    for (const std::string &src : t->sources) {
      source_paths.push_back(opts.manifest_dir / src);
    }

    if (target_is_fresh(opts.manifest_path, artifact, source_paths)) {
      ++fresh;
      if (opts.verbose) {
        status_line("Fresh", CYAN, t->name + " (" + kind_name(t->kind) + ")",
                    opts.color);
      }
      continue;
    }

    ++compiled;
    status_line("Compiling", GREEN, t->name + " (" + kind_name(t->kind) + ")",
                opts.color);

    std::error_code ec;
    fs::create_directories(obj_dir, ec);
    if (ec) {
      error_line("无法创建中间目录 " + obj_dir.string() + ": " + ec.message(),
                 opts.color);
      return 1;
    }
    if (artifact.has_parent_path()) {
      fs::create_directories(artifact.parent_path(), ec);
      if (ec) {
        error_line("无法创建输出目录 " + artifact.parent_path().string() +
                       ": " + ec.message(),
                   opts.color);
        return 1;
      }
    }

    // 阶段 1：逐源文件 vxc → .vxobj
    std::vector<fs::path> objects;
    objects.reserve(t->sources.size());
    for (std::size_t i = 0; i < t->sources.size(); ++i) {
      const fs::path obj = object_path(opts.build_dir, *t, i, t->sources[i]);
      objects.push_back(obj);

      std::vector<std::string> args = {source_paths[i].string(), "-o",
                                       obj.string(), "--color",
                                       opts.color ? "always" : "never"};
      if (opts.verbose)
        args.push_back("-v");

      if (opts.verbose) {
        status_line("Running", CYAN, "`" + format_command(opts.vxc, args) + "`",
                    opts.color);
      }
      ProcessResult r = run_process(opts.vxc, args, opts.child_env);
      if (!r.ok) {
        print_child_output(r.output);
        if (!r.error.empty())
          std::fprintf(stderr, "  %s\n", r.error.c_str());
        error_line("could not compile `" + t->name + "` (" +
                       kind_name(t->kind) + ")",
                   opts.color);
        return 1;
      }
    }

    // 阶段 2：vlnk 合并全部 .vxobj 并链接为最终产物
    std::vector<std::string> args;
    for (const fs::path &obj : objects)
      args.push_back(obj.string());
    args.push_back("-o");
    args.push_back(artifact.string());
    if (t->kind == TargetKind::Dylib)
      args.push_back("--shared");
    if (opts.verbose)
      args.push_back("-v");

    if (opts.verbose) {
      status_line("Running", CYAN, "`" + format_command(opts.vlnk, args) + "`",
                  opts.color);
    }
    ProcessResult r = run_process(opts.vlnk, args, opts.child_env);
    if (!r.ok) {
      print_child_output(r.output);
      if (!r.error.empty())
        std::fprintf(stderr, "  %s\n", r.error.c_str());
      error_line("could not link `" + t->name + "` (" + kind_name(t->kind) +
                     ")",
                 opts.color);
      return 1;
    }
  }

  std::string summary =
      "build in " + format_duration(std::chrono::steady_clock::now() - start);
  if (opts.verbose) {
    summary += " (compiled " + std::to_string(compiled) + ", fresh " +
               std::to_string(fresh) + ")";
  }
  status_line("Finished", GREEN, summary, opts.color);
  return 0;
}

} // namespace vmake
