// main.cpp — vmake CLI：解析 VMakeSet.vmake 并编排 vxc/vlnk。
// 工作目录（构建目录）必须通过 -B <dir> 显式指定；输出风格对齐 Cargo。
#include "build.hpp"
#include "config.hpp"
#include "process.hpp"
#include "toolchain.hpp"

#include "vx/util/fs.hpp"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include <unistd.h>

namespace fs = std::filesystem;

static constexpr std::string_view VERSION = "0.4.0";

enum class Command { Build, Clean, Run, Test };

struct CliOptions {
  fs::path build_dir;
  bool has_build_dir = false;
  Command command = Command::Build;
  bool command_explicit = false; // 是否显式给出 build/clean/run/test
  std::vector<std::string> targets;
  std::vector<std::string> include_dirs; // -I 头文件搜索目录（透传给 vxc）
  std::vector<std::string> lib_dirs;     // --lib-dir（覆盖所有目标的 lib_dirs）
  std::string target;                     // --target <triple>
  std::string color = "auto";
  std::string profile = "debug";          // --release / --debug
  bool verbose = false;
  int jobs = 0; // 0 = 自动（CPU 核数）
  bool help = false;
  bool version = false;
  std::vector<std::string> passthrough; // "--" 之后的参数（转发给 run/test 程序）
};

static void print_help() {
  std::fprintf(stdout,
               "VX Make %s\n"
               "Usage: vmake [build|run|test] -B <dir> [options] [target...]\n"
               "       vmake clean [-B <dir>]\n"
               "\n"
               "Reads VMakeSet.vmake from the current directory and orchestrates the toolchain in dependency order:\n"
               "  vxc <source> -o <obj>   （每个源文件）\n"
               "  vlnk <obj...> -o <out>  （合并链接；dylib 目标加 --shared）\n"
               "  ar rcs <out> <obj...>   （staticlib 目标归档）\n"
               "VMake itself compiles no code; it only arranges arguments and order.\n"
               "\n"
               "Commands:\n"
               "  build (default)  Compile and link selected targets\n"
               "  run              Build an exe target, then run it\n"
               "                   （无目标名时用清单中唯一的 exe；参数经 -- 转发）\n"
               "  test             Build and run test targets declared via tests=[...]\n"
               "                   （产物 <name>-test；参数经 -- 转发给每个测试）\n"
               "  clean            Remove the build directory\n"
               "\n"
               "Options:\n"
               "  -B <dir>        Build directory (required; auto-created if missing)\n"
               "  -I <dir>        Header search dir for @imp/@cppimp (repeatable,\n"
               "                  forwarded to every vxc call; also accepts -I<dir>)\n"
               "  --lib-dir <dir> Library search dir (repeatable, forwarded to vlnk\n"
               "                  as -L<dir>; merges with per-target lib_dirs)\n"
               "  --target <tpl>  Cross-compile target triple (e.g. x86_64-linux-gnu)\n"
               "                  forwarded to every vxc and vlnk call\n"
               "  --release       Release profile: link-stage clang gets -O2\n"
               "  --debug         Debug profile (default): link-stage clang gets -g\n"
               "  --color <when>  Diagnostic color: auto|always|never (default auto)\n"
               "  -j <n>          Parallel compile jobs (default: CPU cores; -j0 = auto)\n"
               "  -v, --verbose   Print executed commands and intermediate stages\n"
               "  --              Forward the rest to the run/test program\n"
               "  -V, --version   Print version\n"
               "  -h, --help      Show this help\n"
               "\n"
               "VMakeSet.vmake supports exe / dylib / staticlib targets:\n"
               "  exe(\"hello\", sources=[\"src/main.vx\", \"src/util.vx\"])\n"
               "  dylib(\"mylib\", sources=[\"src/lib.vx\"], output=\"libmylib.dylib\")\n"
               "  staticlib(\"util\", sources=[\"src/util.vx\"])\n"
               "  exe(\"app\", sources=[\"src/app.vx\"], deps=[\"mylib\", \"util\"])\n"
               "  exe(\"sys\", sources=[\"src/sys.vx\"], libs=[\"ssl\", \"/opt/lib/libfoo.a\"],\n"
               "       lib_dirs=[\"/usr/local/lib\"])\n"
               "  exe(\"app\", sources=[\"src/app.vx\"],\n"
               "       cflags=[\"-Wall\"], defines=[\"NDEBUG\"],\n"
               "       ldflags=[\"-Wl,-rpath,/opt/lib\"],\n"
               "       tests=[\"tests/app_test.vx\", \"tests/util_test.vx\"])\n"
               "  # output is optional, relative to the -B build directory;\n"
               "  # deps links other VMake targets (dylib or staticlib);\n"
               "  # libs/lib_dirs add external libraries (裸名 → -l, 路径 → --lib);\n"
               "  # cflags/defines/ldflags are extra clang args at link stage;\n"
               "  # tests build <name>-test, run by `vmake test`\n"
               "\n"
               "Environment: VX_VXC / VX_VLNK / VX_AR override toolchain paths; VX_HEADERS "
               "overrides the import library root.\n",
               VERSION.data());
}

static int parse_args(int argc, char **argv, CliOptions &opts) {
  for (int i = 1; i < argc; ++i) {
    const std::string_view a = argv[i];
    if (a == "-h" || a == "--help") {
      opts.help = true;
    } else if (a == "-V" || a == "--version") {
      opts.version = true;
    } else if (a == "-v" || a == "--verbose") {
      opts.verbose = true;
    } else if (a == "-j" || a.starts_with("-j")) {
      if (a == "-j") {
        if (i + 1 >= argc) {
          std::fprintf(stderr, "error: -j requires a number\n");
          return 2;
        }
        opts.jobs = std::stoi(argv[++i]);
      } else {
        opts.jobs = std::stoi(std::string(a.substr(2)));
      }
      if (opts.jobs < 0) opts.jobs = 0;
    } else if (a == "-B" || a.starts_with("-B")) {
      if (a == "-B") {
        if (i + 1 >= argc) {
          std::fprintf(stderr, "error: -B requires a directory argument\n");
          return 2;
        }
        opts.build_dir = argv[++i];
      } else {
        opts.build_dir = std::string(a.substr(2));
      }
      opts.has_build_dir = true;
    } else if (a == "-I" || (a.size() > 2 && a.starts_with("-I"))) {
      if (a == "-I") {
        if (i + 1 >= argc) {
          std::fprintf(stderr, "error: -I requires a directory argument\n");
          return 2;
        }
        opts.include_dirs.emplace_back(argv[++i]);
      } else {
        opts.include_dirs.emplace_back(std::string(a.substr(2)));
      }
    } else if (a == "--lib-dir") {
      if (i + 1 >= argc) {
        std::fprintf(stderr, "error: --lib-dir requires a directory argument\n");
        return 2;
      }
      opts.lib_dirs.emplace_back(argv[++i]);
    } else if (a.starts_with("--lib-dir=")) {
      opts.lib_dirs.emplace_back(std::string(a.substr(10)));
    } else if (a == "--target") {
      if (i + 1 >= argc) {
        std::fprintf(stderr, "error: --target requires a triple argument\n");
        return 2;
      }
      opts.target = argv[++i];
    } else if (a.starts_with("--target=")) {
      opts.target = std::string(a.substr(9));
    } else if (a == "--color" || a.starts_with("--color=")) {
      if (a == "--color") {
        if (i + 1 >= argc) {
          std::fprintf(stderr, "error: --color requires an argument\n");
          return 2;
        }
        opts.color = argv[++i];
      } else {
        opts.color = std::string(a.substr(8));
      }
      if (opts.color != "auto" && opts.color != "always" &&
          opts.color != "never") {
        std::fprintf(stderr, "error: invalid --color value: %s\n",
                     opts.color.c_str());
        return 2;
      }
    } else if (a == "--release") {
      opts.profile = "release";
    } else if (a == "--debug") {
      opts.profile = "debug";
    } else if (a == "--") {
      // 其余参数原样转发给 run/test 的目标程序
      for (++i; i < argc; ++i)
        opts.passthrough.emplace_back(argv[i]);
      break;
    } else if (a.starts_with("-")) {
      std::fprintf(stderr, "error: unknown option: %.*s (try --help)\n",
                   static_cast<int>(a.size()), a.data());
      return 2;
    } else {
      // 首个位置词为子命令关键字（build/clean/run/test）时设定命令，
      // 否则作为目标名；命令一经设定，后续同名字词按目标名处理。
      if (!opts.command_explicit &&
          (a == "build" || a == "clean" || a == "run" || a == "test")) {
        opts.command = (a == "build")   ? Command::Build
                       : (a == "clean") ? Command::Clean
                       : (a == "run")   ? Command::Run
                                        : Command::Test;
        opts.command_explicit = true;
      } else {
        opts.targets.emplace_back(a);
      }
    }
  }
  return 0;
}

static bool color_enabled(std::string_view mode) {
  if (mode == "always")
    return true;
  if (mode == "never")
    return false;
  if (const char *no_color = std::getenv("NO_COLOR"); no_color != nullptr) {
    return false;
  }
  if (const char *term = std::getenv("TERM");
      term != nullptr && std::string_view(term) == "dumb") {
    return false;
  }
  return ::isatty(STDERR_FILENO) != 0;
}

// 在可执行文件上级目录中寻找 headers/（覆盖 build/tools/vmake → build →
// 仓库根）
static std::optional<fs::path> headers_near(const fs::path &exe) {
  fs::path dir = exe.parent_path();
  if (dir.empty())
    return std::nullopt;
  for (int i = 0; i < 4; ++i) {
    std::error_code ec;
    const fs::path candidate = dir / "headers";
    if (fs::is_directory(candidate, ec) && !ec)
      return candidate;
    if (!dir.has_parent_path())
      break;
    const fs::path parent = dir.parent_path();
    if (parent == dir)
      break;
    dir = parent;
  }
  return std::nullopt;
}

// ---- run / test 子命令 ----

// vmake run：构建指定（或清单中唯一的）exe 目标后直接执行（继承 stdio，
// 保持交互性），返回目标程序的退出码。
static int run_command(const vmake::Manifest &manifest,
                       vmake::BuildOptions &opts, const CliOptions &cli,
                       const fs::path &build_dir) {
  const vmake::Target *t = nullptr;
  if (cli.targets.size() > 1) {
    std::fprintf(stderr, "error: run takes at most one target\n");
    return 2;
  }
  if (!cli.targets.empty()) {
    for (const vmake::Target &c : manifest.targets) {
      if (c.name == cli.targets[0]) {
        t = &c;
        break;
      }
    }
    if (t == nullptr) { // 目标名校验在 parse 后已做，此处防御
      std::fprintf(stderr, "error: target not found: '%s'\n",
                   cli.targets[0].c_str());
      return 2;
    }
    if (t->kind != vmake::TargetKind::Exe) {
      std::fprintf(stderr,
                   "error: target '%s' is a %s; run requires an exe target\n",
                   t->name.c_str(), vmake::kind_name(t->kind));
      return 2;
    }
  } else {
    std::vector<const vmake::Target *> exes;
    for (const vmake::Target &c : manifest.targets) {
      if (c.kind == vmake::TargetKind::Exe)
        exes.push_back(&c);
    }
    if (exes.empty()) {
      std::fprintf(stderr, "error: no exe target declared in VMakeSet.vmake\n");
      return 2;
    }
    if (exes.size() > 1) {
      std::fprintf(stderr,
                   "error: multiple exe targets; specify one:"
                   "\n  vmake run -B <dir> <target>  (available:");
      for (const vmake::Target *e : exes)
        std::fprintf(stderr, " %s", e->name.c_str());
      std::fprintf(stderr, ")\n");
      return 2;
    }
    t = exes[0];
  }

  opts.selected = {t->name};
  if (const int rc = vmake::build_manifest(manifest, opts); rc != 0)
    return rc;

  const fs::path artifact = vmake::artifact_path(build_dir, *t);
  std::error_code ec;
  if (!fs::is_regular_file(artifact, ec) || ec) {
    std::fprintf(stderr, "error: artifact missing after build: %s\n",
                 artifact.string().c_str());
    return 1;
  }
  std::fprintf(stderr, "vmake-runner: running %s\n",
               artifact.filename().c_str());
  return vmake::run_process_direct(artifact.string(), cli.passthrough,
                                   opts.child_env);
}

// vmake test：为每个声明 tests=[...] 的目标派生 <name>-test 可执行目标并
// 构建（依赖闭包自动纳入），随后逐个运行并汇总结果。
static int test_command(const vmake::Manifest &manifest,
                        vmake::BuildOptions &opts, const CliOptions &cli,
                        const fs::path &build_dir) {
  auto named = [&](const std::string &name) {
    for (const std::string &n : cli.targets) {
      if (n == name)
        return true;
    }
    return false;
  };

  std::vector<vmake::Target> test_targets;
  for (const vmake::Target &t : manifest.targets) {
    if (t.tests.empty()) {
      if (!cli.targets.empty() && named(t.name)) {
        std::fprintf(stderr, "error: target '%s' declares no tests\n",
                     t.name.c_str());
        return 2;
      }
      continue;
    }
    if (!cli.targets.empty() && !named(t.name))
      continue;
    std::optional<vmake::Target> tt = vmake::test_target_of(t);
    if (!tt)
      continue;
    for (const vmake::Target &other : manifest.targets) {
      if (other.name == tt->name) {
        std::fprintf(stderr,
                     "error: derived test target '%s' collides with an "
                     "existing target\n",
                     tt->name.c_str());
        return 2;
      }
    }
    test_targets.push_back(*std::move(tt));
  }

  if (test_targets.empty()) {
    if (!cli.targets.empty())
      return 2; // 显式点名但无 tests：错误已打印
    std::fprintf(stderr, "vmake-test: no tests declared in VMakeSet.vmake\n");
    return 0;
  }

  // 测试目标追加进清单副本；selected 只含测试目标，deps 闭包自动纳入依赖
  vmake::Manifest m2 = manifest;
  opts.selected.clear();
  for (const vmake::Target &tt : test_targets) {
    m2.targets.push_back(tt);
    opts.selected.push_back(tt.name);
  }
  if (const int rc = vmake::build_manifest(m2, opts); rc != 0)
    return rc;

  constexpr std::string_view RESET = "\033[0m";
  constexpr std::string_view GREEN = "\033[1;32m";
  constexpr std::string_view RED = "\033[1;31m";
  std::size_t passed = 0;
  std::size_t failed = 0;
  for (const vmake::Target &tt : test_targets) {
    const fs::path artifact = vmake::artifact_path(build_dir, tt);
    std::error_code ec;
    if (!fs::is_regular_file(artifact, ec) || ec) {
      std::fprintf(stderr, "error: test binary missing: %s\n",
                   artifact.string().c_str());
      return 1;
    }
    const vmake::ProcessResult r = vmake::run_process(
        artifact.string(), cli.passthrough, opts.child_env, cli.verbose);
    if (r.ok) {
      ++passed;
      if (opts.color)
        std::fprintf(stderr, "test %s ... %sok%s\n", tt.name.c_str(),
                     std::string(GREEN).c_str(), std::string(RESET).c_str());
      else
        std::fprintf(stderr, "test %s ... ok\n", tt.name.c_str());
      if (cli.verbose && !r.output.empty())
        std::fwrite(r.output.data(), 1, r.output.size(), stderr);
    } else {
      ++failed;
      if (opts.color)
        std::fprintf(stderr, "test %s ... %sFAILED%s (exit %d)\n",
                     tt.name.c_str(), std::string(RED).c_str(),
                     std::string(RESET).c_str(), r.exit_code);
      else
        std::fprintf(stderr, "test %s ... FAILED (exit %d)\n", tt.name.c_str(),
                     r.exit_code);
      if (!r.output.empty()) {
        std::fwrite(r.output.data(), 1, r.output.size(), stderr);
        if (r.output.back() != '\n')
          std::fputc('\n', stderr);
      }
      if (!r.error.empty())
        std::fprintf(stderr, "  %s\n", r.error.c_str());
    }
  }
  std::fprintf(stderr, "vmake-test: %zu passed, %zu failed\n", passed, failed);
  return failed == 0 ? 0 : 1;
}

int main(int argc, char **argv) {
  CliOptions cli;
  if (const int rc = parse_args(argc, argv, cli); rc != 0)
    return rc;
  if (cli.help) {
    print_help();
    return 0;
  }
  if (cli.version) {
    std::fprintf(stdout, "vmake %s\n", VERSION.data());
    return 0;
  }
  if (cli.command == Command::Clean) {
    // Determine build dir from -B or default
    fs::path bd = cli.has_build_dir ? cli.build_dir : fs::path("build-out");
    std::error_code ec;
    fs::remove_all(bd, ec);
    if (ec) {
      std::fprintf(stderr, "error: cannot remove %s: %s\n", bd.string().c_str(), ec.message().c_str());
      return 1;
    }
    std::fprintf(stdout, "Removed %s\n", bd.string().c_str());
    return 0;
  }

  if (!cli.has_build_dir || cli.build_dir.empty()) {
    std::fprintf(stderr,
                 "error: missing -B <dir> argument (build directory must be explicitly specified)\n\n");
    print_help();
    return 2;
  }

  std::error_code ec;
  const fs::path cwd = fs::current_path(ec);
  if (ec) {
    std::fprintf(stderr, "error: cannot get current directory: %s\n", ec.message().c_str());
    return 1;
  }
  const fs::path manifest_path = cwd / "VMakeSet.vmake";
  auto text = vx::util::read_file(manifest_path);
  if (!text) {
    std::fprintf(stderr,
                 "error: VMakeSet.vmake not found (run vmake from the project root)\n");
    return 1;
  }

  std::vector<vmake::ParseError> errors;
  auto manifest = vmake::parse_manifest(*text, errors);
  if (!manifest) {
    for (const vmake::ParseError &e : errors) {
      std::fprintf(stderr, "error: VMakeSet.vmake:%zu: %s\n", e.line,
                   e.message.c_str());
    }
    return 2;
  }

  // 目标名筛选校验
  for (const std::string &name : cli.targets) {
    bool found = false;
    for (const vmake::Target &t : manifest->targets) {
      if (t.name == name) {
        found = true;
        break;
      }
    }
    if (!found) {
      std::fprintf(stderr, "error: target not found: '%s' (available targets:", name.c_str());
      for (const vmake::Target &t : manifest->targets) {
        std::fprintf(stderr, " %s", t.name.c_str());
      }
      std::fprintf(stderr, "）\n");
      return 2;
    }
  }

  std::string tool_error;
  auto toolchain = vmake::find_toolchain(argv[0], tool_error);
  if (!toolchain) {
    std::fprintf(stderr, "error: %s\n", tool_error.c_str());
    return 1;
  }

  fs::path build_dir =
      cli.build_dir.is_absolute() ? cli.build_dir : cwd / cli.build_dir;
  fs::create_directories(build_dir, ec);
  if (ec) {
    std::fprintf(stderr, "error: cannot create build directory %s: %s\n",
                 build_dir.string().c_str(), ec.message().c_str());
    return 1;
  }

  // 传递导入库根目录：显式 VX_HEADERS > 项目 headers/ > 编译器旁的 headers/
  // 同时记录到 opts.headers_root，供增量签名的依赖树哈希使用。
  std::vector<std::pair<std::string, std::string>> child_env;
  std::optional<fs::path> headers_root;
  const char *inherited_headers = std::getenv("VX_HEADERS");
  if (inherited_headers != nullptr && inherited_headers[0] != '\0') {
    headers_root = fs::path(inherited_headers);
  } else {
    std::optional<fs::path> headers;
    if (fs::is_directory(manifest_path.parent_path() / "headers", ec) && !ec) {
      headers = manifest_path.parent_path() / "headers";
    } else {
      headers = headers_near(toolchain->vxc);
    }
    if (headers) {
      child_env.emplace_back("VX_HEADERS", fs::absolute(*headers).string());
      headers_root = *headers;
    }
  }

  vmake::BuildOptions opts;
  opts.build_dir = build_dir;
  opts.manifest_dir = manifest_path.parent_path();
  opts.manifest_path = manifest_path;
  if (headers_root) {
    const fs::path abs = fs::absolute(*headers_root, ec);
    opts.headers_root = ec ? *headers_root : abs;
    ec.clear();
  }
  opts.vxc = toolchain->vxc;
  opts.vlnk = toolchain->vlnk;
  opts.include_dirs = cli.include_dirs;
  opts.lib_dirs_cli = cli.lib_dirs;
  opts.target = cli.target;
  opts.profile = cli.profile;
  opts.selected = cli.targets;
  opts.child_env = std::move(child_env);
  opts.verbose = cli.verbose;
  opts.jobs = cli.jobs;
  opts.color = color_enabled(cli.color);
  switch (cli.command) {
  case Command::Build:
    return vmake::build_manifest(*manifest, opts);
  case Command::Run:
    return run_command(*manifest, opts, cli, build_dir);
  case Command::Test:
    return test_command(*manifest, opts, cli, build_dir);
  case Command::Clean:
    break; // clean 已在读取清单前处理
  }
  return 0;
}
