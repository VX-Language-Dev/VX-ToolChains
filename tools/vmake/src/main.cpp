// main.cpp — vmake CLI：解析 VMakeLists.toml 并编排 vxc/vlnk。
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

static constexpr std::string_view VERSION = "0.1.0";

struct CliOptions {
  fs::path build_dir;
  bool has_build_dir = false;
  std::vector<std::string> targets;
  std::string color = "auto";
  bool verbose = false;
  bool help = false;
  bool version = false;
};

static void print_help() {
  std::fprintf(stdout,
               "VX Make %s\n"
               "Usage: vmake [build] -B <dir> [options] [target...]\n"
               "\n"
               "读取当前目录的 VMakeLists.toml，按依赖顺序在后台调用工具链：\n"
               "  vxc <source> -o <obj>   （每个源文件）\n"
               "  vlnk <obj...> -o <out>  （合并链接；dylib 目标加 --shared）\n"
               "VMake 自身不编译任何代码，只负责编排参数与顺序。\n"
               "\n"
               "Options:\n"
               "  -B <dir>        构建目录（必填；不存在会自动创建）\n"
               "  --color <when>  诊断颜色：auto|always|never（默认 auto）\n"
               "  -v, --verbose   打印实际执行的命令与中间阶段\n"
               "  -V, --version   打印版本\n"
               "  -h, --help      显示本帮助\n"
               "\n"
               "VMakeLists.toml（当前仅支持 exe / dylib 两类目标）：\n"
               "  [[exe]]\n"
               "  name = \"hello\"\n"
               "  sources = [\"src/main.vx\"]\n"
               "\n"
               "  [[dylib]]\n"
               "  name = \"mylib\"\n"
               "  sources = [\"src/lib.vx\", \"src/util.vx\"]\n"
               "  output = \"libmylib.dylib\"   # 可选，相对 -B 构建目录\n"
               "\n"
               "环境变量：VX_VXC / VX_VLNK 指定工具链路径；VX_HEADERS "
               "指定导入库根目录。\n",
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
    } else if (a == "-B" || a.starts_with("-B")) {
      if (a == "-B") {
        if (i + 1 >= argc) {
          std::fprintf(stderr, "error: -B 需要一个目录参数\n");
          return 2;
        }
        opts.build_dir = argv[++i];
      } else {
        opts.build_dir = std::string(a.substr(2));
      }
      opts.has_build_dir = true;
    } else if (a == "--color" || a.starts_with("--color=")) {
      if (a == "--color") {
        if (i + 1 >= argc) {
          std::fprintf(stderr, "error: --color 需要一个参数\n");
          return 2;
        }
        opts.color = argv[++i];
      } else {
        opts.color = std::string(a.substr(8));
      }
      if (opts.color != "auto" && opts.color != "always" &&
          opts.color != "never") {
        std::fprintf(stderr, "error: 非法的 --color 值: %s\n",
                     opts.color.c_str());
        return 2;
      }
    } else if (a.starts_with("-")) {
      std::fprintf(stderr, "error: 未知选项: %.*s\n",
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
  if (!cli.has_build_dir || cli.build_dir.empty()) {
    std::fprintf(stderr,
                 "error: 缺少 -B <dir> 参数（构建目录必须显式指定）\n\n");
    print_help();
    return 2;
  }

  std::error_code ec;
  const fs::path cwd = fs::current_path(ec);
  if (ec) {
    std::fprintf(stderr, "error: 无法获取当前目录: %s\n", ec.message().c_str());
    return 1;
  }
  const fs::path manifest_path = cwd / "VMakeLists.toml";
  auto text = vx::util::read_file(manifest_path);
  if (!text) {
    std::fprintf(stderr,
                 "error: 未找到 VMakeLists.toml（请在项目根目录运行 vmake）\n");
    return 1;
  }

  std::vector<vmake::ParseError> errors;
  auto manifest = vmake::parse_manifest(*text, errors);
  if (!manifest) {
    for (const vmake::ParseError &e : errors) {
      std::fprintf(stderr, "error: VMakeLists.toml:%zu: %s\n", e.line,
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
      std::fprintf(stderr, "error: 未找到目标 '%s'（可用目标:", name.c_str());
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
    std::fprintf(stderr, "error: 无法创建构建目录 %s: %s\n",
                 build_dir.string().c_str(), ec.message().c_str());
    return 1;
  }

  // 传递导入库根目录：显式 VX_HEADERS > 项目 headers/ > 编译器旁的 headers/
  std::vector<std::pair<std::string, std::string>> child_env;
  const char *inherited_headers = std::getenv("VX_HEADERS");
  if (inherited_headers == nullptr || inherited_headers[0] == '\0') {
    std::optional<fs::path> headers;
    if (fs::is_directory(manifest_path.parent_path() / "headers", ec) && !ec) {
      headers = manifest_path.parent_path() / "headers";
    } else {
      headers = headers_near(toolchain->vxc);
    }
    if (headers) {
      child_env.emplace_back("VX_HEADERS", fs::absolute(*headers).string());
    }
  }

  vmake::BuildOptions opts;
  opts.build_dir = build_dir;
  opts.manifest_dir = manifest_path.parent_path();
  opts.manifest_path = manifest_path;
  opts.vxc = toolchain->vxc;
  opts.vlnk = toolchain->vlnk;
  opts.selected = cli.targets;
  opts.child_env = std::move(child_env);
  opts.verbose = cli.verbose;
  opts.color = color_enabled(cli.color);
  return vmake::build_manifest(*manifest, opts);
}
