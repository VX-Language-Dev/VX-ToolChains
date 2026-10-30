// config.cpp — VMakeSet.vmake 解析器（Python 风格，函数调用式声明）。
// 语法示例：
//   exe("hello", sources=["src/main.vx", "src/util.vx"])
//   dylib("mylib", sources=["lib.vx"], output="libmylib.dylib")
// 支持注释、尾随逗号、括号内换行。字段：name（位置或关键字）、sources、output、
// deps、libs、lib_dirs、cflags、defines、ldflags、tests。
#include "config.hpp"

#include <cctype>
#include <cstdio>
#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace vmake {
namespace {

namespace fs = std::filesystem;

bool is_ident_char(char c) {
  const auto u = static_cast<unsigned char>(c);
  return std::isalnum(u) != 0 || c == '_';
}

bool valid_target_name(std::string_view name) {
  if (name.empty())
    return false;
  for (char c : name) {
    const auto u = static_cast<unsigned char>(c);
    if (!(std::isalnum(u) != 0 || c == '_' || c == '-'))
      return false;
  }
  return true;
}

// ---- 词法 ----
enum class TokType { Ident, Str, LBracket, RBracket, LParen, RParen, Comma, Equals, Newline, End };

struct Token {
  TokType type = TokType::End;
  std::string text;
  std::size_t line = 1;
};

struct Lexer {
  std::string_view text;
  std::size_t pos = 0;
  std::size_t line = 1;
  std::vector<Token> toks;

  bool eof() const { return pos >= text.size(); }
  char peek() const { return eof() ? '\0' : text[pos]; }

  void append_utf8(std::string &out, std::uint32_t cp) {
    if (cp < 0x80) out += static_cast<char>(cp);
    else if (cp < 0x800) { out += static_cast<char>(0xC0 | (cp >> 6)); out += static_cast<char>(0x80 | (cp & 0x3F)); }
    else if (cp < 0x10000) { out += static_cast<char>(0xE0 | (cp >> 12)); out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F)); out += static_cast<char>(0x80 | (cp & 0x3F)); }
    else { out += static_cast<char>(0xF0 | (cp >> 18)); out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F)); out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F)); out += static_cast<char>(0x80 | (cp & 0x3F)); }
  }

  bool parse_string(std::string &out) {
    const char quote = text[pos++];
    while (pos < text.size()) {
      char c = text[pos++];
      if (c == quote) return true;
      if (c == '\n') return false;
      if (quote == '"' && c == '\\') {
        if (pos >= text.size()) return false;
        char esc = text[pos++];
        switch (esc) {
          case 'n': out += '\n'; break;
          case 't': out += '\t'; break;
          case 'r': out += '\r'; break;
          case '"': out += '"'; break;
          case '\\': out += '\\'; break;
          case 'u': {
            std::uint32_t cp = 0;
            for (int i = 0; i < 4; ++i) {
              if (pos >= text.size()) return false;
              char h = text[pos++];
              if (std::isxdigit(static_cast<unsigned char>(h)) == 0) return false;
              cp = cp * 16 + (std::isdigit(static_cast<unsigned char>(h)) ? h - '0' : (std::tolower(static_cast<unsigned char>(h)) - 'a' + 10));
            }
            append_utf8(out, cp);
            break;
          }
          default: return false;
        }
      } else {
        out += c;
      }
    }
    return false;
  }

  void run() {
    while (pos < text.size()) {
      char c = text[pos];
      if (c == ' ' || c == '\t' || c == '\r') { ++pos; continue; }
      if (c == '\n') { toks.push_back({TokType::Newline, "\n", line}); ++pos; ++line; continue; }
      if (c == '#') { while (pos < text.size() && text[pos] != '\n') ++pos; continue; }
      if (c == '"' || c == '\'') {
        std::string s;
        std::size_t sl = line;
        if (!parse_string(s)) {
          toks.push_back({TokType::End, "unterminated string", sl});
          return;
        }
        toks.push_back({TokType::Str, std::move(s), sl});
        continue;
      }
      if (c == '[') { toks.push_back({TokType::LBracket, "[", line}); ++pos; continue; }
      if (c == ']') { toks.push_back({TokType::RBracket, "]", line}); ++pos; continue; }
      if (c == '(') { toks.push_back({TokType::LParen, "(", line}); ++pos; continue; }
      if (c == ')') { toks.push_back({TokType::RParen, ")", line}); ++pos; continue; }
      if (c == ',') { toks.push_back({TokType::Comma, ",", line}); ++pos; continue; }
      if (c == '=') { toks.push_back({TokType::Equals, "=", line}); ++pos; continue; }
      if (is_ident_char(c)) {
        std::string id;
        std::size_t sl = line;
        while (pos < text.size() && is_ident_char(text[pos])) id += text[pos++];
        toks.push_back({TokType::Ident, std::move(id), sl});
        continue;
      }
      // unknown char: skip
      ++pos;
    }
    toks.push_back({TokType::End, "", line});
  }
};

// ---- 解析器 ----
class Parser {
public:
  explicit Parser(std::string_view text) {
    Lexer lex{text};
    lex.run();
    toks_ = std::move(lex.toks);
  }

  std::optional<Manifest> run() {
    if (!parse_document()) return std::nullopt;
    if (!validate()) return std::nullopt;
    Manifest m;
    m.targets = std::move(targets_);
    return m;
  }

  const ParseError &error() const { return error_; }

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
    bool seen_cflags = false;
    bool seen_defines = false;
    bool seen_ldflags = false;
    bool seen_tests = false;
  };

  std::vector<Token> toks_;
  std::size_t pos_ = 0;
  std::vector<Pending> pending_;
  std::vector<Target> targets_;
  ParseError error_;

  const Token &cur() const { return toks_[pos_ < toks_.size() ? pos_ : toks_.size() - 1]; }
  bool eof() const { return pos_ >= toks_.size() || cur().type == TokType::End; }

  void skip_newlines() {
    while (pos_ < toks_.size() && cur().type == TokType::Newline) ++pos_;
  }

  bool fail(std::size_t line, std::string message) {
    error_.line = line;
    error_.message = std::move(message);
    return false;
  }

  // 解析字符串数组字面量（[...], 支持换行与尾随逗号）；失败时写 error_。
  bool parse_string_array(const char *field, std::vector<std::string> &out) {
    if (cur().type != TokType::LBracket) {
      return fail(cur().line, std::string(field) + " 期望字符串数组");
    }
    ++pos_;
    while (true) {
      // 跳过括号内的换行与空白
      while (cur().type == TokType::Newline) ++pos_;
      if (cur().type == TokType::RBracket) break;
      if (eof()) return fail(cur().line, std::string(field) + " 数组未闭合");
      if (cur().type != TokType::Str)
        return fail(cur().line, "数组元素必须是字符串");
      out.push_back(cur().text);
      ++pos_;
      while (cur().type == TokType::Newline) ++pos_;
      if (cur().type == TokType::Comma) { ++pos_; continue; }
      if (cur().type == TokType::RBracket) break;
      return fail(cur().line, "数组元素之间缺少 ','");
    }
    if (cur().type != TokType::RBracket)
      return fail(cur().line, std::string(field) + " 数组未闭合");
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
    if (cur().type != TokType::Ident) {
      return fail(cur().line, "期望目标声明 exe(...) / dylib(...) / staticlib(...)");
    }
    std::string kindname = cur().text;
    if (kindname != "exe" && kindname != "dylib" && kindname != "staticlib") {
      return fail(cur().line, "未知目标类型 '" + kindname + "'（支持 exe / dylib / staticlib）");
    }
    std::size_t line = cur().line;
    ++pos_;
    if (cur().type != TokType::LParen) {
      return fail(line, "目标声明需要 '('");
    }
    ++pos_;
    Pending p;
    p.kind = (kindname == "exe") ? TargetKind::Exe :
             (kindname == "dylib") ? TargetKind::Dylib : TargetKind::StaticLib;
    p.line = line;

    // name: positional string or name=...
    if (cur().type == TokType::Str) {
      p.name = cur().text;
      p.seen_name = true;
      ++pos_;
      if (cur().type == TokType::Comma) ++pos_;
    }
    while (cur().type == TokType::Newline) ++pos_;

    // keyword args
    while (cur().type != TokType::RParen && !eof()) {
      while (cur().type == TokType::Newline) ++pos_;
      if (cur().type == TokType::RParen) break;
      if (cur().type != TokType::Ident) {
        return fail(cur().line,
                    "期望字段名 (name/sources/output/deps/libs/lib_dirs/"
                    "cflags/defines/ldflags/tests)");
      }
      std::string key = cur().text;
      std::size_t kline = cur().line;
      ++pos_;
      if (cur().type != TokType::Equals) {
        return fail(kline, "字段 '" + key + "' 后缺少 '='");
      }
      ++pos_;
      if (key == "name") {
        if (p.seen_name) return fail(kline, "字段 name 重复");
        if (cur().type != TokType::Str) return fail(cur().line, "name 期望字符串");
        p.name = cur().text;
        p.seen_name = true;
        ++pos_;
      } else if (key == "sources") {
        if (p.seen_sources) return fail(kline, "字段 sources 重复");
        std::vector<std::string> arr;
        if (!parse_string_array("sources", arr)) return false;
        p.sources = std::move(arr);
        p.seen_sources = true;
      } else if (key == "output") {
        if (p.seen_output) return fail(kline, "字段 output 重复");
        if (cur().type != TokType::Str) return fail(cur().line, "output 期望字符串");
        p.output = cur().text;
        p.seen_output = true;
        ++pos_;
      } else if (key == "deps") {
        if (p.seen_deps) return fail(kline, "字段 deps 重复");
        std::vector<std::string> arr;
        if (!parse_string_array("deps", arr)) return false;
        p.deps = std::move(arr);
        p.seen_deps = true;
      } else if (key == "libs") {
        if (p.seen_libs) return fail(kline, "字段 libs 重复");
        std::vector<std::string> arr;
        if (!parse_string_array("libs", arr)) return false;
        p.libs = std::move(arr);
        p.seen_libs = true;
      } else if (key == "lib_dirs") {
        if (p.seen_lib_dirs) return fail(kline, "字段 lib_dirs 重复");
        std::vector<std::string> arr;
        if (!parse_string_array("lib_dirs", arr)) return false;
        p.lib_dirs = std::move(arr);
        p.seen_lib_dirs = true;
      } else if (key == "cflags") {
        if (p.seen_cflags) return fail(kline, "字段 cflags 重复");
        std::vector<std::string> arr;
        if (!parse_string_array("cflags", arr)) return false;
        p.cflags = std::move(arr);
        p.seen_cflags = true;
      } else if (key == "defines") {
        if (p.seen_defines) return fail(kline, "字段 defines 重复");
        std::vector<std::string> arr;
        if (!parse_string_array("defines", arr)) return false;
        p.defines = std::move(arr);
        p.seen_defines = true;
      } else if (key == "ldflags") {
        if (p.seen_ldflags) return fail(kline, "字段 ldflags 重复");
        std::vector<std::string> arr;
        if (!parse_string_array("ldflags", arr)) return false;
        p.ldflags = std::move(arr);
        p.seen_ldflags = true;
      } else if (key == "tests") {
        if (p.seen_tests) return fail(kline, "字段 tests 重复");
        std::vector<std::string> arr;
        if (!parse_string_array("tests", arr)) return false;
        p.tests = std::move(arr);
        p.seen_tests = true;
      } else {
        return fail(kline, "未知字段 '" + key +
                               "'（支持 name / sources / output / deps / libs /"
                               " lib_dirs / cflags / defines / ldflags / tests）");
      }
      if (cur().type == TokType::Comma) ++pos_;
      while (cur().type == TokType::Newline) ++pos_;
    }

    if (cur().type != TokType::RParen) {
      return fail(cur().line, "目标声明缺少 ')'");
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
    for (const Pending &p : pending_) {
      if (!p.name) return fail(p.line, "目标缺少 name 字段");
      if (!valid_target_name(*p.name)) return fail(p.line, "目标名 '" + *p.name + "' 非法");
      const auto [it, inserted] = names.emplace(*p.name, p.line);
      if (!inserted) return fail(p.line, "目标名 '" + *p.name + "' 重复");
      if (!p.sources) return fail(p.line, "目标 '" + *p.name + "' 缺少 sources 字段");
      if (p.sources->empty()) return fail(p.line, "目标 '" + *p.name + "' 的 sources 不能为空");
      for (const std::string &src : *p.sources) {
        if (src.empty()) return fail(p.line, "目标 '" + *p.name + "' 含空源文件路径");
        if (src.size() < 4 || src.substr(src.size() - 3) != ".vx") return fail(p.line, "源文件必须是 .vx 文件: " + src);
      }
      if (p.output) {
        if (p.output->empty()) return fail(p.line, "目标 '" + *p.name + "' 的 output 不能为空");
        const fs::path op(*p.output);
        if (op.is_absolute()) return fail(p.line, "output 必须是相对构建目录的路径: " + *p.output);
        for (const fs::path &part : op) {
          if (part == "..") return fail(p.line, "output 不得包含 '..': " + *p.output);
        }
      }
      if (p.deps) {
        for (const std::string &dep : *p.deps) {
          if (dep.empty()) return fail(p.line, "目标 '" + *p.name + "' 含空依赖名");
        }
      }
      // libs / lib_dirs：仅 exe/dylib 允许；staticlib 无链接阶段故禁止
      if ((p.libs || p.lib_dirs) && p.kind == TargetKind::StaticLib)
        return fail(p.line, "staticlib 目标不允许 libs / lib_dirs（无链接阶段）");
      if (p.libs) {
        for (const std::string &lib : *p.libs) {
          if (lib.empty()) return fail(p.line, "目标 '" + *p.name + "' 含空 libs 条目");
        }
      }
      if (p.lib_dirs) {
        for (const std::string &d : *p.lib_dirs) {
          if (d.empty()) return fail(p.line, "目标 '" + *p.name + "' 含空 lib_dirs 条目");
        }
      }
      // cflags / defines / ldflags：链接期才调用 clang，staticlib 只做 ar 归档
      if ((p.cflags || p.defines || p.ldflags) && p.kind == TargetKind::StaticLib)
        return fail(p.line, "staticlib 目标不允许 cflags / defines / ldflags"
                            "（归档阶段不调用编译器或链接器）");
      if (p.cflags) {
        for (const std::string &f : *p.cflags) {
          if (f.empty()) return fail(p.line, "目标 '" + *p.name + "' 含空 cflags 条目");
        }
      }
      if (p.defines) {
        for (const std::string &d : *p.defines) {
          if (d.empty()) return fail(p.line, "目标 '" + *p.name + "' 含空 defines 条目");
        }
      }
      if (p.ldflags) {
        for (const std::string &f : *p.ldflags) {
          if (f.empty()) return fail(p.line, "目标 '" + *p.name + "' 含空 ldflags 条目");
        }
      }
      // tests：测试源文件，与 sources 同规则（非空、必须 .vx）
      if (p.tests) {
        if (p.tests->empty())
          return fail(p.line, "目标 '" + *p.name + "' 的 tests 不能为空");
        for (const std::string &tc : *p.tests) {
          if (tc.empty())
            return fail(p.line, "目标 '" + *p.name + "' 含空测试文件路径");
          if (tc.size() < 4 || tc.substr(tc.size() - 3) != ".vx")
            return fail(p.line, "测试文件必须是 .vx 文件: " + tc);
        }
      }
      Target t;
      t.name = *p.name;
      t.kind = p.kind;
      t.sources = *p.sources;
      t.output = p.output;
      if (p.deps) t.deps = *p.deps;
      if (p.libs) t.libs = *p.libs;
      if (p.lib_dirs) t.lib_dirs = *p.lib_dirs;
      if (p.cflags) t.cflags = *p.cflags;
      if (p.defines) t.defines = *p.defines;
      if (p.ldflags) t.ldflags = *p.ldflags;
      if (p.tests) t.tests = *p.tests;
      t.line = p.line;
      targets_.push_back(std::move(t));
    }
    // deps 跨目标校验：
    //   - staticlib 不允许 deps（静态库是对象归档，无链接依赖）
    //   - 依赖必须存在、不得自引用
    //   - 被依赖方必须是 dylib 或 staticlib（exe 不产出可链接库）
    for (const Target &t : targets_) {
      if (t.kind == TargetKind::StaticLib && !t.deps.empty())
        return fail(t.line, "staticlib 目标 '" + t.name + "' 不允许 deps（无链接阶段）");
      for (const std::string &dep : t.deps) {
        if (dep == t.name)
          return fail(t.line, "目标 '" + t.name + "' 不能依赖自身");
        const auto it = names.find(dep);
        if (it == names.end())
          return fail(t.line, "目标 '" + t.name + "' 依赖了未声明的目标 '" + dep + "'");
        const Target *dt = find_target(dep);
        if (dt != nullptr && dt->kind != TargetKind::Dylib && dt->kind != TargetKind::StaticLib)
          return fail(t.line, "目标 '" + t.name + "' 依赖 '" + dep +
                                  "'，但被依赖方必须是 dylib 或 staticlib 目标");
      }
    }
    return true;
  }

  const Target *find_target(std::string_view name) const {
    for (const Target &t : targets_) {
      if (t.name == name) return &t;
    }
    return nullptr;
  }

};

} // namespace

const char *kind_name(TargetKind kind) {
  switch (kind) {
    case TargetKind::Dylib: return "dylib";
    case TargetKind::StaticLib: return "staticlib";
    case TargetKind::Exe: default: return "exe";
  }
}

std::optional<Manifest> parse_manifest(std::string_view text,
                                       std::vector<ParseError> &errors) {
  Parser parser(text);
  auto manifest = parser.run();
  if (!manifest)
    errors.push_back(parser.error());
  return manifest;
}

} // namespace vmake