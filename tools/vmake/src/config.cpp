// config.cpp — VMakeSet.vmake 解析器（Python 风格，函数调用式声明）。
// 语法示例：
//   exe("hello", sources=["src/main.vx", "src/util.vx"])
//   dylib("mylib", sources=["lib.vx"], output="libmylib.dylib")
//   exe("app", sources=["main.vx", "platform.c", "util.cpp"],
//       ccflags=["-Wall"], cxxflags=["-Wall", "-std=c++20"],
//       incs=["include"], std="c++20",
//       cflags=["-g"], defines=["NDEBUG"])
// 支持注释、尾随逗号、括号内换行。sources 接受 .vx / .c / .cpp 混合。
// 词法层与校验规则复用 tools/vmake-shared（与 vmake-lsp 共用同一套 .vmake 底层）。
#include "config.hpp"

#include "vmake_shared.hpp"

#include <filesystem>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace vmake {
namespace {

namespace fs = std::filesystem;

using vmake_shared::has_dotdot_component;
using vmake_shared::is_absolute_path;
using vmake_shared::is_c_source;
using vmake_shared::is_cxx_source;
using vmake_shared::is_vx_source;
using vmake_shared::valid_target_name;
namespace Tok = vmake_shared; // 词法常量别名：Tok::Str / Tok::LBracket / ...

// 允许的源文件后缀：.vx（vxc）、.c（clang -c C）、.cpp/.cc/.cxx（clang -c C++）
bool is_valid_source(std::string_view s) {
  return is_vx_source(s) || is_c_source(s) || is_cxx_source(s);
}
// 测试源仅限 .vx
bool is_valid_test_source(std::string_view s) {
  return is_vx_source(s);
}

// ---- 解析器 ----
class Parser {
public:
  explicit Parser(std::string_view text) : toks_(vmake_shared::lex(text)) {}

  std::optional<Manifest> run() {
    if (!parse_document())
      return std::nullopt;
    if (!validate())
      return std::nullopt;
    Manifest m;
    m.targets = std::move(targets_);
    return m;
  }

  const ParseError& error() const { return error_; }

private:
  struct Pending {
    TargetKind kind = TargetKind::Exe;
    std::size_t line = 0;
    std::optional<std::string> name;
    std::optional<std::vector<std::string>> sources;
    std::optional<std::string> output;
    std::optional<std::vector<std::string>> deps;
    std::optional<std::vector<std::string>> libs;
    std::optional<std::vector<std::string>> lib_dirs;
    std::optional<std::vector<std::string>> ccflags;
    std::optional<std::vector<std::string>> cxxflags;
    std::optional<std::vector<std::string>> incs;
    std::optional<std::string> std_;
    std::optional<std::vector<std::string>> cflags;
    std::optional<std::vector<std::string>> defines;
    std::optional<std::vector<std::string>> ldflags;
    std::optional<std::vector<std::string>> tests;
    bool seen_name = false;
    bool seen_sources = false;
    bool seen_output = false;
    bool seen_deps = false;
    bool seen_libs = false;
    bool seen_lib_dirs = false;
    bool seen_ccflags = false;
    bool seen_cxxflags = false;
    bool seen_incs = false;
    bool seen_std = false;
    bool seen_cflags = false;
    bool seen_defines = false;
    bool seen_ldflags = false;
    bool seen_tests = false;
  };

  // 共享词法产出 0 起始行号；Parser 内统一 +1 到 1 起始（与历史报错行号一致）
  std::size_t cur_line_1() const { return static_cast<std::size_t>(cur().line) + 1; }

  const vmake_shared::Token& cur() const {
    static const vmake_shared::Token k_end = [] {
      vmake_shared::Token t;
      t.type = vmake_shared::TokType::End;
      return t;
    }();
    return (pos_ < toks_.size()) ? toks_[pos_] : k_end;
  }
  bool eof() const {
    return pos_ >= toks_.size() || cur().type == vmake_shared::TokType::End || cur().type == vmake_shared::TokType::Bad;
  }

  void skip_newlines() {
    while (pos_ < toks_.size() && cur().type == vmake_shared::TokType::Newline)
      ++pos_;
  }

  bool fail(std::size_t line, std::string message) {
    error_.line = line;
    error_.message = std::move(message);
    return false;
  }

  // 解析字符串数组字面量（[...], 支持换行与尾随逗号）；失败时写 error_。
  bool parse_string_array(const char* field, std::vector<std::string>& out) {
    if (cur().type != vmake_shared::TokType::LBracket) {
      return fail(cur_line_1(), std::string(field) + " 期望字符串数组");
    }
    ++pos_;
    while (true) {
      // 跳过括号内的换行与空白
      skip_newlines();
      if (cur().type == vmake_shared::TokType::RBracket)
        break;
      if (eof())
        return fail(cur_line_1(), std::string(field) + " 数组未闭合");
      if (cur().type != vmake_shared::TokType::Str)
        return fail(cur_line_1(), "数组元素必须是字符串");
      out.push_back(cur().value);
      ++pos_;
      skip_newlines();
      if (cur().type == vmake_shared::TokType::Comma) {
        ++pos_;
        continue;
      }
      if (cur().type == vmake_shared::TokType::RBracket)
        break;
      return fail(cur_line_1(), "数组元素之间缺少 ','");
    }
    if (cur().type != vmake_shared::TokType::RBracket)
      return fail(cur_line_1(), std::string(field) + " 数组未闭合");
    ++pos_;
    return true;
  }

  bool parse_document() {
    skip_newlines();
    while (!eof()) {
      if (!parse_statement()) {
        return false;
      }
      skip_newlines();
    }
    return true;
  }

  bool parse_statement() {
    if (cur().type != vmake_shared::TokType::Ident) {
      return fail(cur_line_1(), "期望目标声明 exe(...) / dylib(...) / staticlib(...)");
    }
    std::string kindname = cur().text;
    if (kindname != "exe" && kindname != "dylib" && kindname != "staticlib") {
      return fail(cur_line_1(), "未知目标类型 '" + kindname + "'（支持 exe / dylib / staticlib）");
    }
    std::size_t line = cur_line_1();
    ++pos_;
    if (cur().type != vmake_shared::TokType::LParen) {
      return fail(line, "目标声明需要 '('");
    }
    ++pos_;

    Pending p;
    p.kind = (kindname == "exe") ? TargetKind::Exe : (kindname == "dylib") ? TargetKind::Dylib : TargetKind::StaticLib;
    p.line = line;

    // name: positional string or name=...
    if (cur().type == vmake_shared::TokType::Str) {
      p.name = cur().value;
      p.seen_name = true;
      ++pos_;
      if (cur().type == vmake_shared::TokType::Comma)
        ++pos_;
    }
    skip_newlines();

    // keyword args
    while (cur().type != vmake_shared::TokType::RParen && !eof()) {
      skip_newlines();
      if (cur().type == vmake_shared::TokType::RParen)
        break;
      if (cur().type != vmake_shared::TokType::Ident) {
        return fail(cur_line_1(), "期望字段名 (name/sources/output/deps/libs/lib_dirs/"
                                  "ccflags/cxxflags/incs/std/"
                                  "cflags/defines/ldflags/tests)");
      }
      std::string key = cur().text;
      std::size_t kline = cur_line_1();
      ++pos_;
      if (cur().type != vmake_shared::TokType::Equals) {
        return fail(kline, "字段 '" + key + "' 后缺少 '='");
      }
      ++pos_;
      if (key == "name") {
        if (p.seen_name)
          return fail(kline, "字段 name 重复");
        if (cur().type != vmake_shared::TokType::Str)
          return fail(cur_line_1(), "name 期望字符串");
        p.name = cur().value;
        p.seen_name = true;
        ++pos_;
      } else if (key == "sources") {
        if (p.seen_sources)
          return fail(kline, "字段 sources 重复");
        std::vector<std::string> arr;
        if (!parse_string_array("sources", arr))
          return false;
        p.sources = std::move(arr);
        p.seen_sources = true;
      } else if (key == "output") {
        if (p.seen_output)
          return fail(kline, "字段 output 重复");
        if (cur().type != vmake_shared::TokType::Str)
          return fail(cur_line_1(), "output 期望字符串");
        p.output = cur().value;
        p.seen_output = true;
        ++pos_;
      } else if (key == "deps") {
        if (p.seen_deps)
          return fail(kline, "字段 deps 重复");
        std::vector<std::string> arr;
        if (!parse_string_array("deps", arr))
          return false;
        p.deps = std::move(arr);
        p.seen_deps = true;
      } else if (key == "libs") {
        if (p.seen_libs)
          return fail(kline, "字段 libs 重复");
        std::vector<std::string> arr;
        if (!parse_string_array("libs", arr))
          return false;
        p.libs = std::move(arr);
        p.seen_libs = true;
      } else if (key == "lib_dirs") {
        if (p.seen_lib_dirs)
          return fail(kline, "字段 lib_dirs 重复");
        std::vector<std::string> arr;
        if (!parse_string_array("lib_dirs", arr))
          return false;
        p.lib_dirs = std::move(arr);
        p.seen_lib_dirs = true;
      } else if (key == "ccflags") {
        if (p.seen_ccflags)
          return fail(kline, "字段 ccflags 重复");
        std::vector<std::string> arr;
        if (!parse_string_array("ccflags", arr))
          return false;
        p.ccflags = std::move(arr);
        p.seen_ccflags = true;
      } else if (key == "cxxflags") {
        if (p.seen_cxxflags)
          return fail(kline, "字段 cxxflags 重复");
        std::vector<std::string> arr;
        if (!parse_string_array("cxxflags", arr))
          return false;
        p.cxxflags = std::move(arr);
        p.seen_cxxflags = true;
      } else if (key == "incs") {
        if (p.seen_incs)
          return fail(kline, "字段 incs 重复");
        std::vector<std::string> arr;
        if (!parse_string_array("incs", arr))
          return false;
        p.incs = std::move(arr);
        p.seen_incs = true;
      } else if (key == "std") {
        if (p.seen_std)
          return fail(kline, "字段 std 重复");
        if (cur().type != vmake_shared::TokType::Str)
          return fail(cur_line_1(), "std 期望字符串");
        p.std_ = cur().value;
        p.seen_std = true;
        ++pos_;
      } else if (key == "cflags") {
        if (p.seen_cflags)
          return fail(kline, "字段 cflags 重复");
        std::vector<std::string> arr;
        if (!parse_string_array("cflags", arr))
          return false;
        p.cflags = std::move(arr);
        p.seen_cflags = true;
      } else if (key == "defines") {
        if (p.seen_defines)
          return fail(kline, "字段 defines 重复");
        std::vector<std::string> arr;
        if (!parse_string_array("defines", arr))
          return false;
        p.defines = std::move(arr);
        p.seen_defines = true;
      } else if (key == "ldflags") {
        if (p.seen_ldflags)
          return fail(kline, "字段 ldflags 重复");
        std::vector<std::string> arr;
        if (!parse_string_array("ldflags", arr))
          return false;
        p.ldflags = std::move(arr);
        p.seen_ldflags = true;
      } else if (key == "tests") {
        if (p.seen_tests)
          return fail(kline, "字段 tests 重复");
        std::vector<std::string> arr;
        if (!parse_string_array("tests", arr))
          return false;
        p.tests = std::move(arr);
        p.seen_tests = true;
      } else {
        return fail(kline, "未知字段 '" + key +
                               "'（支持 name / sources / output / deps / libs /"
                               " lib_dirs / ccflags / cxxflags / incs / std /"
                               " cflags / defines / ldflags / tests）");
      }
      if (cur().type == vmake_shared::TokType::Comma)
        ++pos_;
      skip_newlines();
    }

    if (cur().type != vmake_shared::TokType::RParen) {
      return fail(cur_line_1(), "目标声明缺少 ')'");
    }
    ++pos_;

    pending_.push_back(std::move(p));
    return true;
  }

  bool validate() {
    if (pending_.empty()) {
      return fail(1, "未声明任何目标（使用 exe(...) / dylib(...) / staticlib(...)）");
    }
    std::map<std::string, std::size_t> names;
    for (const auto& p : pending_) {
      if (!p.name)
        return fail(p.line, "目标缺少 name 字段");
      if (!valid_target_name(*p.name))
        return fail(p.line, "目标名 '" + *p.name + "' 非法");
      const auto [it, inserted] = names.emplace(*p.name, p.line);
      if (!inserted)
        return fail(p.line, "目标名 '" + *p.name + "' 重复");
      if (!p.sources)
        return fail(p.line, "目标 '" + *p.name + "' 缺少 sources 字段");
      if (p.sources->empty())
        return fail(p.line, "目标 '" + *p.name + "' 的 sources 不能为空");
      for (const std::string& src : *p.sources) {
        if (src.empty())
          return fail(p.line, "目标 '" + *p.name + "' 含空源文件路径");
        if (!is_valid_source(src))
          return fail(p.line, "源文件必须是 .vx / .c / .cpp 文件: " + src);
      }
      if (p.output) {
        if (p.output->empty())
          return fail(p.line, "目标 '" + *p.name + "' 的 output 不能为空");
        const fs::path op(*p.output);
        if (op.is_absolute())
          return fail(p.line, "output 必须是相对构建目录的路径: " + *p.output);
        for (const fs::path& part : op) {
          if (part == "..")
            return fail(p.line, "output 不得包含 '..': " + *p.output);
        }
      }
      if (p.deps) {
        for (const std::string& dep : *p.deps) {
          if (dep.empty())
            return fail(p.line, "目标 '" + *p.name + "' 含空依赖名");
        }
      }

      // staticlib 仍无链接阶段 → 禁止 libs / lib_dirs / ldflags
      if ((p.libs || p.lib_dirs) && p.kind == TargetKind::StaticLib)
        return fail(p.line, "staticlib 目标不允许 libs / lib_dirs（无链接阶段）");
      if (p.ldflags && p.kind == TargetKind::StaticLib)
        return fail(p.line, "staticlib 目标不允许 ldflags（无链接阶段）");
      // 但 staticlib 现在可以有 cflags/defines（供其 .c/.cpp 源的 clang -c）
      // 以及 ccflags/cxxflags/incs/std（纯编译期字段）
      // — 这四项 exe/dylib/staticlib 通用，不再禁止

      if (p.libs) {
        for (const std::string& lib : *p.libs) {
          if (lib.empty())
            return fail(p.line, "目标 '" + *p.name + "' 含空 libs 条目");
        }
      }
      if (p.lib_dirs) {
        for (const std::string& d : *p.lib_dirs) {
          if (d.empty())
            return fail(p.line, "目标 '" + *p.name + "' 含空 lib_dirs 条目");
        }
      }
      if (p.ccflags) {
        for (const std::string& f : *p.ccflags) {
          if (f.empty())
            return fail(p.line, "目标 '" + *p.name + "' 含空 ccflags 条目");
        }
      }
      if (p.cxxflags) {
        for (const std::string& f : *p.cxxflags) {
          if (f.empty())
            return fail(p.line, "目标 '" + *p.name + "' 含空 cxxflags 条目");
        }
      }
      if (p.incs) {
        for (const std::string& d : *p.incs) {
          if (d.empty())
            return fail(p.line, "目标 '" + *p.name + "' 含空 incs 条目");
        }
      }
      if (p.cflags) {
        for (const std::string& f : *p.cflags) {
          if (f.empty())
            return fail(p.line, "目标 '" + *p.name + "' 含空 cflags 条目");
        }
      }
      if (p.defines) {
        for (const std::string& d : *p.defines) {
          if (d.empty())
            return fail(p.line, "目标 '" + *p.name + "' 含空 defines 条目");
        }
      }
      if (p.ldflags) {
        for (const std::string& f : *p.ldflags) {
          if (f.empty())
            return fail(p.line, "目标 '" + *p.name + "' 含空 ldflags 条目");
        }
      }
      // tests：仅限 .vx
      if (p.tests) {
        if (p.tests->empty())
          return fail(p.line, "目标 '" + *p.name + "' 的 tests 不能为空");
        for (const std::string& tc : *p.tests) {
          if (tc.empty())
            return fail(p.line, "目标 '" + *p.name + "' 含空测试文件路径");
          if (!is_valid_test_source(tc))
            return fail(p.line, "测试文件必须是 .vx 文件: " + tc);
        }
      }
      Target t;
      t.name = *p.name;
      t.kind = p.kind;
      t.sources = *p.sources;
      t.output = p.output;
      if (p.deps)
        t.deps = *p.deps;
      if (p.libs)
        t.libs = *p.libs;
      if (p.lib_dirs)
        t.lib_dirs = *p.lib_dirs;
      if (p.ccflags)
        t.ccflags = *p.ccflags;
      if (p.cxxflags)
        t.cxxflags = *p.cxxflags;
      if (p.incs)
        t.incs = *p.incs;
      if (p.std_)
        t.std_ = *p.std_;
      if (p.cflags)
        t.cflags = *p.cflags;
      if (p.defines)
        t.defines = *p.defines;
      if (p.ldflags)
        t.ldflags = *p.ldflags;
      if (p.tests)
        t.tests = *p.tests;
      t.line = p.line;
      targets_.push_back(std::move(t));
    }
    // deps 跨目标校验：
    //   - staticlib 不允许 deps（静态库是对象归档，无链接依赖）
    //   - 依赖必须存在、不得自引用
    //   - 被依赖方必须是 dylib 或 staticlib（exe 不产出可链接库）
    for (const Target& t : targets_) {
      if (t.kind == TargetKind::StaticLib && !t.deps.empty())
        return fail(t.line, "staticlib 目标 '" + t.name + "' 不允许 deps（无链接阶段）");
      for (const std::string& dep : t.deps) {
        if (dep == t.name)
          return fail(t.line, "目标 '" + t.name + "' 不能依赖自身");
        const auto it = names.find(dep);
        if (it == names.end())
          return fail(t.line, "目标 '" + t.name + "' 依赖了未声明的目标 '" + dep + "'");
        const Target* dt = find_target(dep);
        if (dt != nullptr && dt->kind != TargetKind::Dylib && dt->kind != TargetKind::StaticLib)
          return fail(t.line, "目标 '" + t.name + "' 依赖 '" + dep + "'，但被依赖方必须是 dylib 或 staticlib 目标");
      }
    }
    return true;
  }

  const Target* find_target(std::string_view name) const {
    for (const Target& t : targets_) {
      if (t.name == name)
        return &t;
    }
    return nullptr;
  }

  std::vector<vmake_shared::Token> toks_;
  std::size_t pos_ = 0;
  std::vector<Pending> pending_;
  std::vector<Target> targets_;
  ParseError error_;
};

} // namespace

const char* kind_name(TargetKind kind) {
  switch (kind) {
    case TargetKind::Dylib:
      return "dylib";
    case TargetKind::StaticLib:
      return "staticlib";
    case TargetKind::Exe:
    default:
      return "exe";
  }
}

std::optional<Manifest> parse_manifest(std::string_view text, std::vector<ParseError>& errors) {
  Parser parser(text);
  auto manifest = parser.run();
  if (!manifest)
    errors.push_back(parser.error());
  return manifest;
}

} // namespace vmake
