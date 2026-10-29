// main.cpp — vmake CLI：解析 VMakeSet.vmake 并编排 vxc/vlnk。
// 工作目录（构建目录）必须通过 -B <dir> 显式指定；输出风格对齐 Cargo。
#include "build.hpp"
#include "config.hpp"
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

static constexpr std::string_view VERSION = "0.2.0";

struct CliOptions {
  fs::path build_dir;
  bool has_build_dir = false;
  std::vector<std::string> targets;
  std::vector<std::string> include_dirs; // -I 头文件搜索目录（透传给 vxc）
  std::string color = "auto";
  bool verbose = false;
  int jobs = 0; // 0 = 自动（CPU 核数）
  bool help = false;
  bool version = false;
};

static void print_help() {
  std::fprintf(stdout,
               "VX Make %s\n"
               "Usage: vmake [build] -B <dir> [options] [target...]\n"
               "\n"
               "Reads VMakeSet.vmake from the current directory and orchestrates the toolchain in dependency order:\n"
               "  vxc <source> -o <obj>   （每个源文件）\n"
               "  vlnk <obj...> -o <out>  （合并链接；dylib 目标加 --shared）\n"
               "VMake itself compiles no code; it only arranges arguments and order.\n"
               "\n"
               "Options:\n"
               "  -B <dir>        Build directory (required; auto-created if missing)\n"
               "  -I <dir>        Header search dir for @imp/@cppimp (repeatable,\n"
               "                  forwarded to every vxc call; also accepts -I<dir>)\n"
               "  --color <when>  Diagnostic color: auto|always|never (default auto)\n"
               "  -j <n>          Parallel compile jobs (default: CPU cores; -j0 = auto)\n"
               "  -v, --verbose   Print executed commands and intermediate stages\n"
               "  -V, --version   Print version\n"
               "  -h, --help      Show this help\n"
               "\n"
               "VMakeSet.vmake (currently supports exe / dylib targets only):\n"
               "  exe(\"hello\", sources=[\"src/main.vx\", \"src/util.vx\"])\n"
               "  dylib(\"mylib\", sources=[\"src/lib.vx\"], output=\"libmylib.dylib\")\n"
               "  # output is optional, relative to the -B build directory\n"
               "\n"
               "Environment: VX_VXC / VX_VLNK override toolchain paths; VX_HEADERS "
               "overrides the import library root.\n",
               VERSION.data());
}

static int parse_args(int argc, char **argv, CliOptions &opts) {
  bool saw_command = false;
  for (int i = 1; i < argc; ++i) {
    const std::string_view a = argv[i];
    if (!saw_command && a == "build") {
      saw_command = true;
      continue;
    }
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
    } else if (a.starts_with("-")) {
      std::fprintf(stderr, "error: unknown option: %.*s (try --help)\n",
                   static_cast<int>(a.size()), a.data());
      return 2;
    } else {
      opts.targets.emplace_back(a);
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
  // Handle 'clean' subcommand
  bool is_clean = false;
  for (int i = 1; i < argc; ++i) {
    if (std::string(argv[i]) == "clean") { is_clean = true; break; }
  }
  if (is_clean) {
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
  opts.selected = cli.targets;
  opts.child_env = std::move(child_env);
  opts.verbose = cli.verbose;
  opts.jobs = cli.jobs;
  opts.color = color_enabled(cli.color);
  return vmake::build_manifest(*manifest, opts);
}
