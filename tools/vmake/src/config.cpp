// config.cpp — VMakeSet.vmake 解析器（Python 风格，函数调用式声明）。
// 语法示例：
//   exe("hello", sources=["src/main.vx", "src/util.vx"])
//   dylib("mylib", sources=["lib.vx"], output="libmylib.dylib")
// 支持注释、尾随逗号、括号内换行。字段：name（位置或关键字）、sources、output。
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
    bool seen_name = false;
    bool seen_sources = false;
    bool seen_output = false;
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
      return fail(cur().line, "期望目标声明 exe(...) 或 dylib(...)");
    }
    std::string kindname = cur().text;
    if (kindname != "exe" && kindname != "dylib") {
      return fail(cur().line, "未知目标类型 '" + kindname + "'（支持 exe / dylib）");
    }
    std::size_t line = cur().line;
    ++pos_;
    if (cur().type != TokType::LParen) {
      return fail(line, "目标声明需要 '('");
    }
    ++pos_;
    Pending p;
    p.kind = (kindname == "exe") ? TargetKind::Exe : TargetKind::Dylib;
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
        return fail(cur().line, "期望字段名 (name/sources/output)");
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
        if (cur().type != TokType::LBracket) return fail(cur().line, "sources 期望字符串数组");
        ++pos_;
        std::vector<std::string> arr;
        while (true) {
          // 跳过括号内的换行与空白
          while (cur().type == TokType::Newline) ++pos_;
          if (cur().type == TokType::RBracket) break;
          if (eof()) return fail(cur().line, "sources 数组未闭合");
          if (cur().type != TokType::Str) return fail(cur().line, "数组元素必须是字符串");
          arr.push_back(cur().text);
          ++pos_;
          while (cur().type == TokType::Newline) ++pos_;
          if (cur().type == TokType::Comma) { ++pos_; continue; }
          if (cur().type == TokType::RBracket) break;
          return fail(cur().line, "数组元素之间缺少 ','");
        }
        if (cur().type != TokType::RBracket) return fail(cur().line, "sources 数组未闭合");
        ++pos_;
        p.sources = std::move(arr);
        p.seen_sources = true;
      } else if (key == "output") {
        if (p.seen_output) return fail(kline, "字段 output 重复");
        if (cur().type != TokType::Str) return fail(cur().line, "output 期望字符串");
        p.output = cur().text;
        p.seen_output = true;
        ++pos_;
      } else {
        return fail(kline, "未知字段 '" + key + "'（支持 name / sources / output）");
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
      return fail(1, "未声明任何目标（使用 exe(...) 或 dylib(...)）");
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
      Target t;
      t.name = *p.name;
      t.kind = p.kind;
      t.sources = *p.sources;
      t.output = p.output;
      t.line = p.line;
      targets_.push_back(std::move(t));
    }
    return true;
  }

};

} // namespace

const char *kind_name(TargetKind kind) {
  return kind == TargetKind::Dylib ? "dylib" : "exe";
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