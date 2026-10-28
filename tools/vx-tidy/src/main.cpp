// main.cpp — vx-tidy CLI：VX 源码静态分析器。
// 用法：
//   vx-tidy [选项] <file.vx>...   分析文件（支持多文件）
//   vx-tidy --list-rules          列出全部规则
// 选项：
//   --format=text|json   输出格式（默认 text）
//   --no-syntax          不透传词法/语法错误（只跑语义规则）
// 退出码：0 = 无 Error 级发现；1 = 有 Error 或文件读取失败；2 = 用法错误
#include "tidy.hpp"

#include "vx/util/fs.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

constexpr std::string_view VERSION = "0.1.0";

void print_usage() {
  std::fprintf(stdout,
      "vx-tidy %s — VX static analysis engine\n"
      "Usage: vx-tidy [options] <file.vx>...\n"
      "Options:\n"
      "  --format=text|json  Output format (default: text)\n"
      "  --no-syntax        Skip lexer/parser errors, semantic rules only\n"
      "  --list-rules       List all check rules\n"
      "  -V, --version      Print version\n"
      "  -h, --help         Show this help\n"
      "Exit code: 0 = clean, 1 = errors found, 2 = usage error\n",
      VERSION.data());
}

} // namespace

int main(int argc, char **argv) {
  std::vector<std::string> files;
  std::string format = "text";
  bool syntax_filter = false;
  bool list_rules = false;

  for (int i = 1; i < argc; ++i) {
    const std::string_view a = argv[i];
    if (a == "-h" || a == "--help") {
      print_usage();
      return 0;
    }
    if (a == "-V" || a == "--version") {
      std::printf("vx-tidy %s\n", VERSION.data());
      return 0;
    }
    if (a == "--list-rules") {
      list_rules = true;
    } else if (a == "--no-syntax") {
      syntax_filter = true;
    } else if (a == "--format" || a.starts_with("--format=")) {
      if (a == "--format") {
        if (i + 1 >= argc) {
          std::fputs("error: --format requires an argument\n", stderr);
          return 2;
        }
        format = argv[++i];
      } else {
        format = std::string(a.substr(9));
      }
      if (format != "text" && format != "json") {
        std::fputs("error: invalid --format value (text|json)\n", stderr);
        return 2;
      }
    } else if (a.starts_with("-")) {
      std::fprintf(stderr, "error: unknown option: %s (try --help)\n", argv[i]);
      return 2;
    } else {
      files.emplace_back(a);
    }
  }

  if (list_rules) {
    std::puts("VX-Tidy rules:");
    for (const auto &r : vxtidy::all_rules()) {
      std::printf("  [%s] %-18s %s  — %s\n", r.rule.c_str(), r.name.c_str(),
                  r.severity == 1 ? "error" : "warning", r.desc.c_str());
    }
    return 0;
  }

  if (files.empty()) {
    std::fputs("error: no input files\n", stderr);
    print_usage();
    return 2;
  }

  bool any_error = false;
  bool first_json = true;
  if (format == "json") std::fputs("[", stdout);

  for (const std::string &file : files) {
    auto src = vx::util::read_file(file);
    if (!src) {
      std::fprintf(stderr, "error: cannot read %s: %s\n", file.c_str(),
                   src.error().c_str());
      any_error = true;
      continue;
    }
    std::vector<vxtidy::Finding> findings = vxtidy::check(*src, file);
    if (syntax_filter) {
      findings.erase(std::remove_if(findings.begin(), findings.end(),
                                    [](const vxtidy::Finding &f) {
                                      return f.rule == "VXTIDY900";
                                    }),
                     findings.end());
    }
    for (const auto &f : findings) {
      if (f.severity == 1) any_error = true;
    }
    if (format == "json") {
      std::string arr = vxtidy::format_json(findings, file);
      if (!arr.empty() && arr.size() > 2) {
        if (!first_json) std::fputs(",", stdout);
        std::fputs(arr.substr(1, arr.size() - 2).c_str(), stdout);
        first_json = false;
      }
    } else {
      for (const auto &f : findings) {
        std::puts(vxtidy::format_text(f, file).c_str());
      }
      if (!findings.empty()) {
        std::size_t errors = 0, warns = 0;
        for (const auto &f : findings) {
          (f.severity == 1 ? errors : warns)++;
        }
        std::printf("%zu error(s), %zu warning(s)\n", errors, warns);
      }
    }
  }

  if (format == "json") std::fputs("]\n", stdout);
  return any_error ? 1 : 0;
}
