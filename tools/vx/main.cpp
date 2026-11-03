// main.cpp — vx: VX toolchain meta-command.
// Dispatches subcommands to the appropriate tool.
//
// Usage:
//   vx compile <source.vx> [options]   → vxc
//   vx link <input.vxobj>... [opts]   → vlnk
//   vx build [target...]              → vmake build
//   vx run [target] [-- args...]      → vmake run
//   vx test [target...] [-- args...]  → vmake test
//   vx fmt <file.vx>... [options]     → vfmt
//   vx lsp                           → vxls
//   vx vmake-lsp                     → vmake-lsp
//   vx write-settings <editor>       → 在编辑器设置文件中注册 VX 语言
//   vx --version                     → show toolchain version
//   vx --help                        → show help
#include "settings.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <unistd.h>
#if defined(__APPLE__)
#include <mach-o/dyld.h>
#endif

namespace fs = std::filesystem;

static constexpr const char* kVersion = "1.16.4";

static std::string find_sibling(const std::string& name) {
  // Try next to this executable, searching up to 3 parent directories
  char buf[4096];
  uint32_t size = sizeof(buf);
  fs::path exe_dir;
#if defined(__APPLE__)
  if (_NSGetExecutablePath(buf, &size) == 0) {
    exe_dir = fs::path(buf).parent_path();
  }
#else
  ssize_t len = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
  if (len > 0) {
    buf[len] = '\0';
    exe_dir = fs::path(buf).parent_path();
  }
#endif
  fs::path dir = exe_dir;
  for (int i = 0; i < 4; ++i) {
    fs::path cand = dir / name;
    if (fs::is_regular_file(cand))
      return cand.string();
    // 构建树布局：<build>/tools/<name>/<name>
    fs::path tool_cand = dir / "tools" / name / name;
    if (fs::is_regular_file(tool_cand))
      return tool_cand.string();
    if (!dir.has_parent_path())
      break;
    fs::path parent = dir.parent_path();
    if (parent == dir)
      break;
    dir = parent;
  }
  // Fallback: search PATH
  const char* path_env = std::getenv("PATH");
  if (path_env != nullptr) {
    std::string p(path_env);
    size_t start = 0;
    while (start <= p.size()) {
      size_t end = p.find(':', start);
      std::string dir = (end == std::string::npos) ? p.substr(start) : p.substr(start, end - start);
      if (!dir.empty()) {
        fs::path cand = fs::path(dir) / name;
        if (fs::is_regular_file(cand))
          return cand.string();
      }
      if (end == std::string::npos)
        break;
      start = end + 1;
    }
  }
  return name; // hope it's in PATH
}

// 解析导入库根目录（headers），供子进程继承：
//   VX_HEADERS_DIR > VX_HEADERS > <vx 二进制所在目录>/headers > 仓库/安装目录 headers
static std::string resolve_vx_headers() {
  for (const char* var : {"VX_HEADERS_DIR", "VX_HEADERS"}) {
    if (const char* env = std::getenv(var); (env != nullptr) && (env[0] != '\0'))
      return env;
  }
  // vx 二进制所在目录
  char buf[4096];
  fs::path exe_dir;
#if defined(__APPLE__)
  uint32_t size = sizeof(buf);
  if (_NSGetExecutablePath(buf, &size) == 0) {
    exe_dir = fs::path(buf).parent_path();
  }
#else
  ssize_t len = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
  if (len > 0) {
    buf[len] = '\0';
    exe_dir = fs::path(buf).parent_path();
  }
#endif
  if (!exe_dir.empty()) {
    std::error_code ec;
    fs::path cand = exe_dir / "headers";
    if (fs::is_directory(cand, ec) && !ec)
      return cand.string();
  }
  return {};
}

static void print_help() {
  std::fputs("vx — VX Toolchain\n"
             "Usage: vx <command> [options]\n"
             "\n"
             "Commands:\n"
             "  compile, c    Compile .vx source to .vxobj (vxc)\n"
             "  link, l       Link .vxobj to native executable (vlnk)\n"
             "  build, b      Build project (vmake)\n"
             "  run           Build and run an exe target (vmake run)\n"
             "  test          Build and run test targets (vmake test)\n"
             "  fmt           Format VX source files (vfmt)\n"
             "  lsp           Start VX language server (vxls)\n"
             "  vmake-lsp     Start VMake language server (vmake-lsp)\n"
             "  clean         Clean build directory (vmake clean)\n"
             "  write-settings <editor>\n"
             "                Register the VX language (.vx) in an editor's settings\n"
             "                file: nvim/neovim, vim, helix, emacs (--dry-run to preview)\n"
             "\n"
             "Options:\n"
             "  -V, --version  Print version\n"
             "  -h, --help     Show this help\n"
             "\n"
             "Environment:\n"
             "  VX_VXC, VX_VLNK  Override tool paths\n"
             "  VX_HEADERS_DIR   Override import library root (preferred)\n"
             "  VX_HEADERS       Override import library root (legacy alias)\n",
             stdout);
}

// ---- write-settings 子命令 ----

static void print_settings_help() {
  std::fputs("Usage: vx write-settings <editor> [--dry-run]\n"
             "\n"
             "在编辑器设置文件中注册 VX 语言（.vx）：\n"
             "  nvim, neovim   ~/.config/nvim/init.lua\n"
             "  vim            ~/.vimrc\n"
             "  helix, hx      ~/.config/helix/languages.toml\n"
             "  emacs          ~/.emacs.d/init.el\n"
             "\n"
             "重复执行会就地更新已写入的配置块（>>> vx write-settings >>> 标记）；\n"
             "--dry-run 仅打印将要写入的内容，不改动文件。\n",
             stdout);
}

// 读取文本文件；不存在或读取失败时返回空串。
static std::string read_file(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in)
    return {};
  return {(std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>()};
}

static int run_write_settings(int argc, char** argv) {
  std::optional<std::string> name;
  bool dry_run = false;
  for (int i = 2; i < argc; ++i) {
    const std::string_view a = argv[i];
    if (a == "-h" || a == "--help") {
      print_settings_help();
      return 0;
    }
    if (a == "--dry-run" || a == "-n") {
      dry_run = true;
      continue;
    }
    if (!name) {
      name = std::string(a);
      continue;
    }
    std::fprintf(stderr, "error: write-settings 不接受多余参数: %s\n", std::string(a).c_str());
    return 2;
  }
  if (!name) {
    print_settings_help();
    return 2;
  }
  const std::optional<vx::settings::Editor> editor = vx::settings::editor_from_name(*name);
  if (!editor) {
    std::fprintf(stderr, "error: 未知编辑器: %s（支持 nvim/neovim、vim、helix、emacs）\n", name->c_str());
    return 2;
  }
  const char* home = std::getenv("HOME");
  if (home == nullptr || home[0] == '\0') {
    std::fputs("error: 未设置 HOME 环境变量，无法定位编辑器设置文件\n", stderr);
    return 2;
  }

  const std::string path = vx::settings::settings_path(*editor, home);
  const std::string block = vx::settings::settings_block(*editor);
  const std::string merged = vx::settings::merge_block(read_file(path), block);

  if (dry_run) {
    std::printf("# %s → %s\n%s", vx::settings::editor_name(*editor).c_str(), path.c_str(), merged.c_str());
    return 0;
  }

  std::error_code ec;
  const fs::path parent = fs::path(path).parent_path();
  if (!parent.empty())
    fs::create_directories(parent, ec);
  if (ec) {
    std::fprintf(stderr, "error: 无法创建目录 %s: %s\n", parent.string().c_str(), ec.message().c_str());
    return 1;
  }
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  if (!out) {
    std::fprintf(stderr, "error: 无法写入 %s\n", path.c_str());
    return 1;
  }
  out << merged;
  if (!out) {
    std::fprintf(stderr, "error: 写入 %s 失败\n", path.c_str());
    return 1;
  }
  std::printf("已注册 VX 语言: %s (%s)\n", path.c_str(), vx::settings::editor_name(*editor).c_str());
  return 0;
}

int main(int argc, char** argv) {
  if (argc < 2) {
    print_help();
    return 0;
  }

  std::string cmd = argv[1];

  if (cmd == "-h" || cmd == "--help") {
    print_help();
    return 0;
  }
  if (cmd == "-V" || cmd == "--version") {
    std::printf("vx %s\n", kVersion);
    return 0;
  }
  // write-settings 由本进程自行处理，不转发给其它工具
  if (cmd == "write-settings") {
    return run_write_settings(argc, argv);
  }

  // Map subcommand to tool name
  std::string tool;
  std::vector<std::string> extra_args;

  if (cmd == "compile" || cmd == "c") {
    tool = "vxc";
  } else if (cmd == "link" || cmd == "l") {
    tool = "vlnk";
  } else if (cmd == "build" || cmd == "b") {
    tool = "vmake";
    extra_args.emplace_back("build");
  } else if (cmd == "run") {
    tool = "vmake";
    extra_args.emplace_back("run");
  } else if (cmd == "test") {
    tool = "vmake";
    extra_args.emplace_back("test");
  } else if (cmd == "clean") {
    tool = "vmake";
    extra_args.emplace_back("clean");
  } else if (cmd == "fmt" || cmd == "f") {
    tool = "vfmt";
  } else if (cmd == "lsp") {
    tool = "vxls";
  } else if (cmd == "vmake-lsp") {
    tool = "vmake-lsp";
  } else {
    std::fprintf(stderr, "error: unknown command: %s (try --help)\n", cmd.c_str());
    return 2;
  }

  // Find the tool
  std::string tool_path = find_sibling(tool);
  // Also check common install locations
  if (!fs::is_regular_file(tool_path)) {
    // Try ~/.local/bin
    const char* home = std::getenv("HOME");
    if (home != nullptr) {
      fs::path cand = fs::path(home) / ".local" / "bin" / tool;
      if (fs::is_regular_file(cand))
        tool_path = cand.string();
    }
  }

  // 把导入库根目录传播给子进程（vxc/vxls/vmake 通过 VX_HEADERS_DIR 解析）；
  // 用户已显式设置时保持原值。
  if (std::getenv("VX_HEADERS_DIR") == nullptr || std::getenv("VX_HEADERS_DIR")[0] == '\0') {
    const std::string hdrs = resolve_vx_headers();
    if (!hdrs.empty())
      setenv("VX_HEADERS_DIR", hdrs.c_str(), 1);
  }

  // Build argv for exec
  std::vector<char*> new_argv;
  new_argv.push_back(const_cast<char*>(tool_path.c_str()));
  for (auto& a : extra_args)
    new_argv.push_back(const_cast<char*>(a.c_str()));
  for (int i = 2; i < argc; ++i)
    new_argv.push_back(argv[i]);
  new_argv.push_back(nullptr);

  execvp(tool_path.c_str(), new_argv.data());
  std::perror("execvp");
  return 127;
}