// main.cpp — vfmt：VX 源码格式化工具（CLI）。
#include "format.hpp"

#include "vx/util/fs.hpp"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace fs = std::filesystem;

static constexpr std::string_view VERSION = "0.1.0";

// 退出码：0 成功；1 --check 检出需要格式化的文件；2 用法或处理失败
static constexpr int EXIT_OK = 0;
static constexpr int EXIT_NEEDS_FORMAT = 1;
static constexpr int EXIT_ERROR = 2;

struct Options {
  int indent_width = 4;
  int line_length = 88;
  bool write = false; // -w 就地改写
  bool check = false; // -c 只检查
  bool no_verify = false;
  bool help = false;
  bool version = false;
  std::vector<std::string> files;
};

static void print_help() {
  std::fprintf(stdout,
               "vfmt %s - VX source formatter\n"
               "\n"
               "Usage: vfmt [options] <file.vx>...\n"
               "       vfmt [options] -            read from stdin\n"
               "\n"
               "Options:\n"
               "  -w, --write          Rewrite files in place (only when changed)\n"
               "  -c, --check          Check formatting only; exit 1 if any file\n"
               "                       needs reformatting (for CI)\n"
               "      --indent <n>     Indent width in spaces (default: 4)\n"
               "      --line-length <n>  Maximum line width (default: 88); longer\n"
               "                       lines are reported, not wrapped\n"
               "      --no-verify       Skip AST equivalence self-check\n"
               "  -h, --help           Show this help\n"
               "  -V, --version        Show version\n"
               "\n"
               "Exit codes: 0 ok, 1 needs formatting (--check), 2 error\n",
               std::string(VERSION).c_str());
}

static bool parse_int(const std::string_view s, int& out) {
  if (s.empty()) {
    return false;
  }
  int value = 0;
  for (const char c : s) {
    if (c < '0' || c > '9') {
      return false;
    }
    value = (value * 10) + (c - '0');
    if (value > 10000) {
      return false;
    }
  }
  out = value;
  return true;
}

static int parse_args(int argc, char** argv, Options& opts) {
  for (int i = 1; i < argc; ++i) {
    const std::string_view a = argv[i];
    if (a == "-h" || a == "--help") {
      opts.help = true;
    } else if (a == "-V" || a == "--version") {
      opts.version = true;
    } else if (a == "-w" || a == "--write") {
      opts.write = true;
    } else if (a == "-c" || a == "--check") {
      opts.check = true;
    } else if (a == "--no-verify") {
      opts.no_verify = true;
    } else if (a == "--indent" || a == "--line-length") {
      if (i + 1 >= argc) {
        std::fprintf(stderr, "vfmt: %s requires a value\n", std::string(a).c_str());
        return -1;
      }
      int value = 0;
      if (!parse_int(argv[++i], value) || value <= 0) {
        std::fprintf(stderr, "vfmt: invalid value for %s: %s\n", std::string(a).c_str(), argv[i]);
        return -1;
      }
      if (a == "--indent") {
        opts.indent_width = value;
      } else {
        opts.line_length = value;
      }
    } else if (a.size() > 9 && a.starts_with("--indent=")) {
      int value = 0;
      if (!parse_int(a.substr(9), value) || value <= 0) {
        std::fprintf(stderr, "vfmt: invalid value for --indent\n");
        return -1;
      }
      opts.indent_width = value;
    } else if (a.size() > 14 && a.starts_with("--line-length=")) {
      int value = 0;
      if (!parse_int(a.substr(14), value) || value <= 0) {
        std::fprintf(stderr, "vfmt: invalid value for --line-length\n");
        return -1;
      }
      opts.line_length = value;
    } else if (!a.empty() && a[0] == '-' && a != "-") {
      std::fprintf(stderr, "vfmt: unknown option: %.*s\n", static_cast<int>(a.size()), a.data());
      return -1;
    } else {
      opts.files.emplace_back(a);
    }
  }
  return 0;
}

static std::string read_stdin() {
  std::string content;
  char buf[4096];
  while (std::cin.read(buf, sizeof(buf)) || std::cin.gcount() > 0) {
    content.append(buf, static_cast<std::size_t>(std::cin.gcount()));
  }
  return content;
}

// 处理单个文件；返回 true 表示成功。
static bool process_file(const std::string& path, const Options& opts, bool& needs_format) {
  const fs::path p(path);
  const auto read = vx::util::read_file(p);
  if (!read) {
    std::fprintf(stderr, "vfmt: %s: %s\n", path.c_str(), read.error().c_str());
    return false;
  }
  const std::string& src = *read;

  vfmt::Options fopts;
  fopts.indent_width = opts.indent_width;
  fopts.line_length = opts.line_length;
  fopts.verify = !opts.no_verify;

  const auto res = vfmt::format(src, fopts);
  if (!res) {
    std::fprintf(stderr, "vfmt: %s: %s\n", path.c_str(), res.error().c_str());
    return false;
  }

  const bool changed = (res->text != src);
  needs_format = needs_format || changed;

  if (opts.check) {
    if (changed) {
      std::printf("needs formatting: %s\n", path.c_str());
    }
  } else if (opts.write) {
    if (changed) {
      const auto written = vx::util::write_file(p, res->text);
      if (!written) {
        std::fprintf(stderr, "vfmt: %s: %s\n", path.c_str(), written.error().c_str());
        return false;
      }
      std::printf("reformatted: %s\n", path.c_str());
    }
  } else {
    std::fwrite(res->text.data(), 1, res->text.size(), stdout);
  }

  for (const int ln : res->long_lines) {
    std::fprintf(stderr, "vfmt: %s:%d: line exceeds %d columns\n", path.c_str(), ln, opts.line_length);
  }
  return true;
}

int main(int argc, char** argv) {
  Options opts;
  if (const int rc = parse_args(argc, argv, opts); rc != 0) {
    return EXIT_ERROR;
  }
  if (opts.help) {
    print_help();
    return EXIT_OK;
  }
  if (opts.version) {
    std::printf("vfmt %s\n", std::string(VERSION).c_str());
    return EXIT_OK;
  }
  if (opts.files.empty()) {
    std::fprintf(stderr, "vfmt: no input files\n");
    print_help();
    return EXIT_ERROR;
  }
  if (opts.write && opts.check) {
    std::fprintf(stderr, "vfmt: --write and --check are mutually exclusive\n");
    return EXIT_ERROR;
  }

  bool needs_format = false;
  bool all_ok = true;

  for (const std::string& path : opts.files) {
    if (path != "-") {
      if (!process_file(path, opts, needs_format)) {
        all_ok = false;
      }
      continue;
    }
    // 标准输入
    const std::string src = read_stdin();
    vfmt::Options fopts;
    fopts.indent_width = opts.indent_width;
    fopts.line_length = opts.line_length;
    fopts.verify = !opts.no_verify;
    const auto res = vfmt::format(src, fopts);
    if (!res) {
      std::fprintf(stderr, "vfmt: <stdin>: %s\n", res.error().c_str());
      all_ok = false;
      continue;
    }
    if (res->text != src) {
      needs_format = true;
    }
    if (!opts.check) {
      std::fwrite(res->text.data(), 1, res->text.size(), stdout);
    }
  }

  if (!all_ok) {
    return EXIT_ERROR;
  }
  if (opts.check && needs_format) {
    return EXIT_NEEDS_FORMAT;
  }
  return EXIT_OK;
}
