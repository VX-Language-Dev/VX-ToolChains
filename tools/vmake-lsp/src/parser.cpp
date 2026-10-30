// parser.cpp — VMakeSet.vmake 解析器实现（带行/列位置与错误恢复）。
#include "parser.hpp"

#include <cctype>
#include <cstdio>
#include <cstdint>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace vmake_lsp {
namespace {

// 与 tools/vmake/src/config.cpp 的 valid_target_name 保持一致：
// 目标名仅允许字母、数字、下划线与连字符。
bool valid_target_name(std::string_view name) {
  if (name.empty()) return false;
  for (char c : name) {
    const auto u = static_cast<unsigned char>(c);
    if (!(std::isalnum(u) != 0 || c == '_' || c == '-')) return false;
  }
  return true;
}

// 绝对路径判定：POSIX 的 `/` 开头，以及 Windows 盘符前缀（如 `C:\`）。
bool is_absolute_path(std::string_view p) {
  if (p.empty()) return false;
  if (p[0] == '/' || p[0] == '\\') return true;
  return p.size() >= 2 && std::isalpha(static_cast<unsigned char>(p[0])) &&
         p[1] == ':';
}

// 路径分量中是否含 `..`（按 `/` 与 `\` 切分）。
bool has_dotdot_component(std::string_view p) {
  std::size_t i = 0;
  while (i <= p.size()) {
    std::size_t j = p.find_first_of("/\\", i);
    if (j == std::string_view::npos) j = p.size();
    if (p.substr(i, j - i) == "..") return true;
    if (j == p.size()) break;
    i = j + 1;
  }
  return false;
}

enum class TokType {
  Ident,
  Num,
  Str,
  LParen,
  RParen,
  LBracket,
  RBracket,
  Comma,
  Equals,
  Newline,
  End,
  Bad,
};

struct Token {
  TokType type = TokType::End;
  std::string text;   // 原始源文本
  std::string value;  // 字符串：未转义值；标识符：同 text
  Position start;
  Position end; // 不含
};

bool is_ident_start(char c) {
  return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_';
}

bool is_ident_char(char c) {
  return is_ident_start(c) || (c >= '0' && c <= '9') || c == '-';
}

class Lexer {
public:
  explicit Lexer(std::string_view text) : text_(text) {}

  void run() {
    while (pos_ < text_.size()) {
      char c = text_[pos_];
      if (c == ' ' || c == '\t' || c == '\r') {
        advance(1);
        continue;
      }
      if (c == '\n') {
        tokens_.push_back({TokType::Newline, "\n", "", cur_pos(), advance_line(1)});
        continue;
      }
      if (c == '#') {
        // 注释：跳到行尾
        while (pos_ < text_.size() && text_[pos_] != '\n') advance(1);
        continue;
      }
      if (c == '"' || c == '\'') {
        Token tok;
        tok.start = cur_pos();
        if (!parse_string(c, tok)) {
          tok.type = TokType::Bad;
          tok.text = "unterminated string";
          tok.end = cur_pos();
          tokens_.push_back(std::move(tok));
          return;
        }
        tokens_.push_back(std::move(tok));
        continue;
      }
      if (c == '[') {
        tokens_.push_back(make_tok(TokType::LBracket, "["));
        continue;
      }
      if (c == ']') {
        tokens_.push_back(make_tok(TokType::RBracket, "]"));
        continue;
      }
      if (c == '(') {
        tokens_.push_back(make_tok(TokType::LParen, "("));
        continue;
      }
      if (c == ')') {
        tokens_.push_back(make_tok(TokType::RParen, ")"));
        continue;
      }
      if (c == ',') {
        tokens_.push_back(make_tok(TokType::Comma, ","));
        continue;
      }
      if (c == '=') {
        tokens_.push_back(make_tok(TokType::Equals, "="));
        continue;
      }
      if (is_ident_start(c)) {
        std::size_t start = pos_;
        while (pos_ < text_.size() && is_ident_char(text_[pos_])) advance(1);
        std::string id(text_.substr(start, pos_ - start));
        tokens_.push_back({TokType::Ident, id, id, cur_pos_at(start), cur_pos()});
        continue;
      }
      if (c >= '0' && c <= '9') {
        // 数字不是合法值（字符串数组里只允许字符串），单独成词便于诊断
        std::size_t start = pos_;
        while (pos_ < text_.size() && text_[pos_] >= '0' && text_[pos_] <= '9') {
          advance(1);
        }
        std::string num(text_.substr(start, pos_ - start));
        tokens_.push_back(
            {TokType::Num, num, num, cur_pos_at(start), cur_pos()});
        continue;
      }
      // 未知字符：跳过
      advance(1);
    }
    tokens_.push_back({TokType::End, "", "", cur_pos(), cur_pos()});
  }

  const std::vector<Token> &tokens() const { return tokens_; }

private:
  std::string_view text_;
  std::size_t pos_ = 0;
  int line_ = 0;
  int column_ = 0;
  std::vector<Token> tokens_;

  Position cur_pos() const { return {line_, column_}; }
  Position cur_pos_at(std::size_t at) const {
    // 计算给定位置的行/列（简化：从行首算起）
    int l = 0, col = 0;
    for (std::size_t i = 0; i < at && i < text_.size(); ++i) {
      if (text_[i] == '\n') {
        ++l;
        col = 0;
      } else {
        ++col;
      }
    }
    return {l, col};
  }

  void advance(std::size_t n) {
    for (std::size_t i = 0; i < n && pos_ < text_.size(); ++i, ++pos_) {
      if (text_[pos_] == '\n') {
        ++line_;
        column_ = 0;
      } else {
        ++column_;
      }
    }
  }

  Position advance_line(std::size_t n) {
    Position before = cur_pos();
    advance(n);
    return before;
  }

  Token make_tok(TokType type, std::string_view text) {
    Position s = cur_pos();
    advance(text.size());
    return {type, std::string(text), std::string(text), s, cur_pos()};
  }

  bool parse_string(char quote, Token &tok) {
    // quote 已由调用者确认，pos_ 指向 quote
    advance(1); // 消费 quote
    std::string out;
    while (pos_ < text_.size()) {
      char c = text_[pos_];
      if (c == quote) {
        advance(1);
        tok.type = TokType::Str;
        tok.text = std::string(1, quote) + out + std::string(1, quote);
        tok.value = std::move(out);
        tok.end = cur_pos();
        return true;
      }
      if (c == '\n') return false; // 字符串不能跨行
      if (quote == '"' && c == '\\') {
        advance(1); // 消费反斜杠
        if (pos_ >= text_.size()) return false;
        char esc = text_[pos_];
        switch (esc) {
        case 'n': out += '\n'; advance(1); break;
        case 't': out += '\t'; advance(1); break;
        case 'r': out += '\r'; advance(1); break;
        case '"': out += '"'; advance(1); break;
        case '\\': out += '\\'; advance(1); break;
        case 'u': {
          advance(1); // 消费 u
          if (pos_ + 4 > text_.size()) return false;
          unsigned cp = 0;
          for (int i = 0; i < 4; ++i) {
            char h = text_[pos_];
            if (!(std::isxdigit(static_cast<unsigned char>(h)) != 0)) return false;
            cp = cp * 16 +
                 (std::isdigit(static_cast<unsigned char>(h)) ? h - '0'
                                                              : (std::tolower(static_cast<unsigned char>(h)) - 'a' + 10));
            advance(1);
          }
          append_utf8(out, cp);
          break; // 4 位十六进制已全部消费，不再额外前进
        }
        default: return false;
        }
        continue;
      }
      out += c;
      advance(1);
    }
    return false;
  }

  static void append_utf8(std::string &out, unsigned cp) {
    if (cp < 0x80) {
      out += static_cast<char>(cp);
    } else if (cp < 0x800) {
      out += static_cast<char>(0xC0 | (cp >> 6));
      out += static_cast<char>(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
      out += static_cast<char>(0xE0 | (cp >> 12));
      out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
      out += static_cast<char>(0x80 | (cp & 0x3F));
    } else {
      out += static_cast<char>(0xF0 | (cp >> 18));
      out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
      out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
      out += static_cast<char>(0x80 | (cp & 0x3F));
    }
  }
};

class Parser {
public:
  explicit Parser(std::string_view text) {
    Lexer lex(text);
    lex.run();
    toks_ = std::move(lex.tokens());
  }

  ParseResult run() {
    ParseResult result;
    parse_document(result);
    result.ok = result.diagnostics.empty();
    return result;
  }

private:
  std::vector<Token> toks_;
  std::size_t pos_ = 0;
  std::set<std::string> seen_names_; // 已声明的目标名（用于查重）

  const Token &cur() const {
    return toks_[pos_ < toks_.size() ? pos_ : toks_.size() - 1];
  }
  bool eof() const {
    return pos_ >= toks_.size() || cur().type == TokType::End;
  }

  void add_diag(ParseResult &result, const Range &r, std::string message) {
    result.diagnostics.push_back({r, 1, std::move(message)});
  }

  void skip_newlines() {
    while (!eof() && cur().type == TokType::Newline) ++pos_;
  }

  // 解析整个文档
  void parse_document(ParseResult &result) {
    skip_newlines();
    while (!eof()) {
      if (!parse_statement(result)) {
        // 错误恢复：跳到下一行或结束
        while (!eof() && cur().type != TokType::Newline) ++pos_;
        skip_newlines();
      } else {
        skip_newlines();
      }
    }
    // 空清单：与 vmake 一致地报「未声明任何目标」
    if (result.targets.empty() && result.diagnostics.empty()) {
      add_diag(result, Range{Position{0, 0}, Position{0, 0}},
               "未声明任何目标（使用 exe(...) / dylib(...) / staticlib(...)）");
    }
  }

  // 解析字符串数组字段（cflags/defines/ldflags/tests），记录字段名、整个数组
  // 与各元素的位置；失败时写诊断并返回 false。
  bool parse_array_field(ParseResult &result, VTarget &t,
                         const std::string &key, const Range &key_range) {
    if (cur().type != TokType::LBracket) {
      add_diag(result, cur_range(), key + " 期望字符串数组");
      return false;
    }
    const Range lbracket_range = cur_range();
    ++pos_;
    std::vector<std::string> arr;
    std::vector<Range> arr_ranges;
    while (true) {
      skip_newlines();
      if (cur().type == TokType::RBracket) break;
      if (eof()) {
        add_diag(result, cur_range(), key + " 数组未闭合");
        return false;
      }
      if (cur().type != TokType::Str) {
        add_diag(result, cur_range(), "数组元素必须是字符串");
        return false;
      }
      arr.push_back(cur().value);
      arr_ranges.push_back(value_range(cur()));
      ++pos_;
      skip_newlines();
      if (cur().type == TokType::Comma) {
        ++pos_;
        continue;
      }
      if (cur().type == TokType::RBracket) break;
      add_diag(result, cur_range(), "数组元素之间缺少 ','");
      return false;
    }
    if (cur().type != TokType::RBracket) {
      add_diag(result, cur_range(), key + " 数组未闭合");
      return false;
    }
    const Range rbracket_range = cur_range();
    ++pos_;
    ArrayField f;
    f.key = key;
    f.kw_range = key_range;
    f.range.start = lbracket_range.start;
    f.range.end = rbracket_range.end;
    f.values = std::move(arr);
    f.value_ranges = std::move(arr_ranges);
    t.extra_arrays.push_back(std::move(f));
    return true;
  }

  // 解析单条语句：exe(...) / dylib(...)
  bool parse_statement(ParseResult &result) {
    if (cur().type != TokType::Ident) {
      add_diag(result, cur_range(),
               "期望目标声明 exe(...) 或 dylib(...)");
      return false;
    }
    std::string kindname = cur().text;
    if (kindname != "exe" && kindname != "dylib" && kindname != "staticlib") {
      add_diag(result, cur_range(),
               "未知目标类型 '" + kindname + "'（支持 exe / dylib / staticlib）");
      return false;
    }
    TargetKind kind = (kindname == "exe") ? TargetKind::Exe :
                      (kindname == "dylib") ? TargetKind::Dylib : TargetKind::StaticLib;
    Range kind_range = cur_range();
    ++pos_;

    if (cur().type != TokType::LParen) {
      add_diag(result, cur_range(), "目标声明需要 '('");
      return false;
    }
    Range lparen_range = cur_range();
    ++pos_;

    VTarget t;
    t.kind = kind;
    t.kind_range = kind_range;
    t.call_range.start = kind_range.start;

    bool seen_name = false;
    bool seen_sources = false;
    bool seen_output = false;
    bool seen_deps = false;

    // 位置参数 name（字符串）
    if (cur().type == TokType::Str) {
      t.name = cur().value;
      t.name_range = value_range(cur());
      seen_name = true;
      ++pos_;
      if (cur().type == TokType::Comma) ++pos_;
    }
    skip_newlines();

    // 关键字参数
    while (!eof() && cur().type != TokType::RParen) {
      skip_newlines();
      if (cur().type == TokType::RParen) break;
      if (cur().type != TokType::Ident) {
        add_diag(result, cur_range(),
                 "期望字段名 (name/sources/output/deps/libs/lib_dirs/"
                 "cflags/defines/ldflags/tests)");
        return false;
      }
      std::string key = cur().text;
      Range key_range = cur_range();
      ++pos_;
      if (cur().type != TokType::Equals) {
        add_diag(result, cur_range(), "字段 '" + key + "' 后缺少 '='");
        return false;
      }
      ++pos_;

      if (key == "name") {
        if (seen_name) {
          add_diag(result, key_range, "字段 name 重复");
          return false;
        }
        if (cur().type != TokType::Str) {
          add_diag(result, cur_range(), "name 期望字符串");
          return false;
        }
        t.name = cur().value;
        t.name_range = value_range(cur());
        seen_name = true;
        ++pos_;
      } else if (key == "sources") {
        if (seen_sources) {
          add_diag(result, key_range, "字段 sources 重复");
          return false;
        }
        if (cur().type != TokType::LBracket) {
          add_diag(result, cur_range(), "sources 期望字符串数组");
          return false;
        }
        Range lbracket_range = cur_range();
        ++pos_;
        std::vector<std::string> arr;
        std::vector<Range> arr_ranges;
        Range array_start = lbracket_range;
        while (true) {
          skip_newlines();
          if (cur().type == TokType::RBracket) break;
          if (eof()) {
            add_diag(result, cur_range(), "sources 数组未闭合");
            return false;
          }
          if (cur().type != TokType::Str) {
            add_diag(result, cur_range(), "数组元素必须是字符串");
            return false;
          }
          arr.push_back(cur().value);
          arr_ranges.push_back(value_range(cur()));
          ++pos_;
          skip_newlines();
          if (cur().type == TokType::Comma) {
            ++pos_;
            continue;
          }
          if (cur().type == TokType::RBracket) break;
          add_diag(result, cur_range(), "数组元素之间缺少 ','");
          return false;
        }
        if (cur().type != TokType::RBracket) {
          add_diag(result, cur_range(), "sources 数组未闭合");
          return false;
        }
        Range rbracket_range = cur_range();
        ++pos_;
        t.sources = std::move(arr);
        t.source_ranges = std::move(arr_ranges);
        t.sources_range.start = array_start.start;
        t.sources_range.end = rbracket_range.end;
        t.sources_kw_range = key_range;
        seen_sources = true;
      } else if (key == "output") {
        if (seen_output) {
          add_diag(result, key_range, "字段 output 重复");
          return false;
        }
        if (cur().type != TokType::Str) {
          add_diag(result, cur_range(), "output 期望字符串");
          return false;
        }
        t.output = cur().value;
        t.output_range = value_range(cur());
        t.output_kw_range = key_range;
        seen_output = true;
        ++pos_;
      } else if (key == "deps") {
        if (seen_deps) {
          add_diag(result, key_range, "字段 deps 重复");
          return false;
        }
        if (cur().type != TokType::LBracket) {
          add_diag(result, cur_range(), "deps 期望字符串数组");
          return false;
        }
        Range lbracket_range = cur_range();
        ++pos_;
        std::vector<std::string> arr;
        std::vector<Range> arr_ranges;
        Range array_start = lbracket_range;
        while (true) {
          skip_newlines();
          if (cur().type == TokType::RBracket) break;
          if (eof()) {
            add_diag(result, cur_range(), "deps 数组未闭合");
            return false;
          }
          if (cur().type != TokType::Str) {
            add_diag(result, cur_range(), "数组元素必须是字符串");
            return false;
          }
          arr.push_back(cur().value);
          arr_ranges.push_back(value_range(cur()));
          ++pos_;
          skip_newlines();
          if (cur().type == TokType::Comma) {
            ++pos_;
            continue;
          }
          if (cur().type == TokType::RBracket) break;
          add_diag(result, cur_range(), "数组元素之间缺少 ','");
          return false;
        }
        if (cur().type != TokType::RBracket) {
          add_diag(result, cur_range(), "deps 数组未闭合");
          return false;
        }
        Range rbracket_range = cur_range();
        ++pos_;
        t.deps = std::move(arr);
        t.dep_ranges = std::move(arr_ranges);
        t.deps_range.start = array_start.start;
        t.deps_range.end = rbracket_range.end;
        t.deps_kw_range = key_range;
        t.has_deps = true;
        seen_deps = true;
      } else if (key == "libs") {
        if (cur().type != TokType::LBracket) {
          add_diag(result, cur_range(), "libs 期望字符串数组");
          return false;
        }
        Range lbracket_range = cur_range();
        ++pos_;
        std::vector<std::string> arr;
        std::vector<Range> arr_ranges;
        Range array_start = lbracket_range;
        while (true) {
          skip_newlines();
          if (cur().type == TokType::RBracket) break;
          if (eof()) { add_diag(result, cur_range(), "libs 数组未闭合"); return false; }
          if (cur().type != TokType::Str) { add_diag(result, cur_range(), "数组元素必须是字符串"); return false; }
          arr.push_back(cur().value);
          arr_ranges.push_back(value_range(cur()));
          ++pos_;
          skip_newlines();
          if (cur().type == TokType::Comma) { ++pos_; continue; }
          if (cur().type == TokType::RBracket) break;
          add_diag(result, cur_range(), "数组元素之间缺少 ','"); return false;
        }
        if (cur().type != TokType::RBracket) { add_diag(result, cur_range(), "libs 数组未闭合"); return false; }
        Range rbracket_range = cur_range(); ++pos_;
        t.libs = std::move(arr);
        t.lib_ranges = std::move(arr_ranges);
        t.libs_range.start = array_start.start;
        t.libs_range.end = rbracket_range.end;
        t.libs_kw_range = key_range;
        t.has_libs = true;
      } else if (key == "lib_dirs") {
        if (cur().type != TokType::LBracket) {
          add_diag(result, cur_range(), "lib_dirs 期望字符串数组");
          return false;
        }
        Range lbracket_range = cur_range();
        ++pos_;
        std::vector<std::string> arr;
        std::vector<Range> arr_ranges;
        Range array_start = lbracket_range;
        while (true) {
          skip_newlines();
          if (cur().type == TokType::RBracket) break;
          if (eof()) { add_diag(result, cur_range(), "lib_dirs 数组未闭合"); return false; }
          if (cur().type != TokType::Str) { add_diag(result, cur_range(), "数组元素必须是字符串"); return false; }
          arr.push_back(cur().value);
          arr_ranges.push_back(value_range(cur()));
          ++pos_;
          skip_newlines();
          if (cur().type == TokType::Comma) { ++pos_; continue; }
          if (cur().type == TokType::RBracket) break;
          add_diag(result, cur_range(), "数组元素之间缺少 ','"); return false;
        }
        if (cur().type != TokType::RBracket) { add_diag(result, cur_range(), "lib_dirs 数组未闭合"); return false; }
        Range rbracket_range = cur_range(); ++pos_;
        t.lib_dirs = std::move(arr);
        t.lib_dir_ranges = std::move(arr_ranges);
        t.lib_dirs_range.start = array_start.start;
        t.lib_dirs_range.end = rbracket_range.end;
        t.lib_dirs_kw_range = key_range;
        t.has_lib_dirs = true;
      } else if (key == "cflags" || key == "defines" || key == "ldflags" ||
                 key == "tests") {
        bool dup = false;
        for (const ArrayField &f : t.extra_arrays) {
          if (f.key == key) {
            add_diag(result, key_range, "字段 " + key + " 重复");
            dup = true;
            break;
          }
        }
        if (dup) return false;
        if (!parse_array_field(result, t, key, key_range)) return false;
      } else {
        add_diag(result, key_range, "未知字段 '" + key +
                                       "'（支持 name / sources / output / deps /"
                                       " libs / lib_dirs / cflags / defines /"
                                       " ldflags / tests）");
        return false;
      }
      if (cur().type == TokType::Comma) ++pos_;
      skip_newlines();
    }

    if (cur().type != TokType::RParen) {
      add_diag(result, cur_range(), "目标声明缺少 ')'");
      return false;
    }
    Range rparen_range = cur_range();
    ++pos_;
    t.call_range.end = rparen_range.end;

    // 基础校验
    if (!seen_name) {
      add_diag(result, lparen_range, "目标缺少 name 字段");
      return false;
    }
    if (!seen_sources) {
      add_diag(result, lparen_range, "目标 '" + t.name + "' 缺少 sources 字段");
      return false;
    }

    // 语义校验：与 vmake 的 validate() 规则一致，报错但不丢弃目标
    validate_target(result, t);

    result.targets.push_back(std::move(t));
    return true;
  }

  // 目标级语义校验（名称合法性/唯一性、源文件后缀、output 路径约束）
  void validate_target(ParseResult &result, const VTarget &t) {
    if (t.sources.empty()) {
      add_diag(result, t.sources_range,
               "目标 '" + t.name + "' 的 sources 不能为空");
    }

    if (!valid_target_name(t.name)) {
      add_diag(result, t.name_range, "目标名 '" + t.name + "' 非法");
    } else if (!seen_names_.insert(t.name).second) {
      add_diag(result, t.name_range, "目标名 '" + t.name + "' 重复");
    }

    for (std::size_t i = 0; i < t.sources.size(); ++i) {
      const std::string &src = t.sources[i];
      if (src.empty()) {
        add_diag(result, t.source_ranges[i],
                 "目标 '" + t.name + "' 含空源文件路径");
      } else if (src.size() < 4 || src.compare(src.size() - 3, 3, ".vx") != 0) {
        add_diag(result, t.source_ranges[i],
                 "源文件必须是 .vx 文件: " + src);
      }
    }

    if (t.output) {
      const std::string &out = *t.output;
      if (out.empty()) {
        add_diag(result, t.output_range,
                 "目标 '" + t.name + "' 的 output 不能为空");
      } else if (is_absolute_path(out)) {
        add_diag(result, t.output_range,
                 "output 必须是相对构建目录的路径: " + out);
      } else if (has_dotdot_component(out)) {
        add_diag(result, t.output_range, "output 不得包含 '..': " + out);
      }
    }

    // cflags / defines / ldflags / tests：与 vmake config.cpp 规则对齐
    const ArrayField *first_flag = nullptr;
    for (const ArrayField &f : t.extra_arrays) {
      if (f.key == "cflags" || f.key == "defines" || f.key == "ldflags") {
        if (first_flag == nullptr)
          first_flag = &f;
        for (std::size_t i = 0; i < f.values.size(); ++i) {
          if (f.values[i].empty())
            add_diag(result, f.value_ranges[i],
                     "目标 '" + t.name + "' 含空 " + f.key + " 条目");
        }
      } else if (f.key == "tests") {
        if (f.values.empty())
          add_diag(result, f.range, "目标 '" + t.name + "' 的 tests 不能为空");
        for (std::size_t i = 0; i < f.values.size(); ++i) {
          const std::string &v = f.values[i];
          if (v.empty()) {
            add_diag(result, f.value_ranges[i],
                     "目标 '" + t.name + "' 含空测试文件路径");
          } else if (v.size() < 4 || v.compare(v.size() - 3, 3, ".vx") != 0) {
            add_diag(result, f.value_ranges[i],
                     "测试文件必须是 .vx 文件: " + v);
          }
        }
      }
    }
    if (first_flag != nullptr && t.kind == TargetKind::StaticLib) {
      add_diag(result, first_flag->kw_range,
               "staticlib 目标不允许 cflags / defines / ldflags"
               "（归档阶段不调用编译器或链接器）");
    }
  }

  Range cur_range() const {
    const Token &t = cur();
    return {t.start, t.end};
  }

  // 字符串值的范围（不含引号）：start+1 .. end-1
  static Range value_range(const Token &tok) {
    if (tok.text.size() >= 2) {
      return {Position{tok.start.line, tok.start.character + 1},
              Position{tok.end.line, tok.end.character - 1}};
    }
    return {tok.start, tok.end};
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

ParseResult parse_vmake(std::string_view text) {
  Parser parser(text);
  return parser.run();
}

Hit hit_test(const std::vector<VTarget> &targets, int line, int character) {
  Hit hit;
  auto in_range = [&](const Range &r) {
    if (line < r.start.line || line > r.end.line) return false;
    if (line == r.start.line && character < r.start.character) return false;
    if (line == r.end.line && character >= r.end.character) return false;
    return true;
  };
  for (std::size_t i = 0; i < targets.size(); ++i) {
    const auto &t = targets[i];
    if (in_range(t.kind_range)) {
      hit.target_index = i;
      hit.part = Hit::Part::Kind;
      return hit;
    }
    if (in_range(t.name_range)) {
      hit.target_index = i;
      hit.part = Hit::Part::Name;
      return hit;
    }
    if (in_range(t.sources_kw_range)) {
      hit.target_index = i;
      hit.part = Hit::Part::Sources;
      return hit;
    }
    for (std::size_t j = 0; j < t.source_ranges.size(); ++j) {
      if (in_range(t.source_ranges[j])) {
        hit.target_index = i;
        hit.part = Hit::Part::SourceElem;
        return hit;
      }
    }
    if (in_range(t.sources_range)) {
      hit.target_index = i;
      hit.part = Hit::Part::Sources;
      return hit;
    }
    if (in_range(t.output_kw_range)) {
      hit.target_index = i;
      hit.part = Hit::Part::Output;
      return hit;
    }
    if (t.output && in_range(t.output_range)) {
      hit.target_index = i;
      hit.part = Hit::Part::Output;
      return hit;
    }
    if (t.has_deps && in_range(t.deps_kw_range)) {
      hit.target_index = i;
      hit.part = Hit::Part::Deps;
      return hit;
    }
    for (std::size_t j = 0; j < t.dep_ranges.size(); ++j) {
      if (in_range(t.dep_ranges[j])) {
        hit.target_index = i;
        hit.part = Hit::Part::DepElem;
        return hit;
      }
    }
    if (t.has_deps && in_range(t.deps_range)) {
      hit.target_index = i;
      hit.part = Hit::Part::Deps;
      return hit;
    }
    // libs / lib_dirs
    if (t.has_libs && in_range(t.libs_kw_range)) {
      hit.target_index = i; hit.part = Hit::Part::Libs; return hit;
    }
    for (std::size_t j = 0; j < t.lib_ranges.size(); ++j) {
      if (in_range(t.lib_ranges[j])) { hit.target_index = i; hit.part = Hit::Part::LibElem; return hit; }
    }
    if (t.has_libs && in_range(t.libs_range)) {
      hit.target_index = i; hit.part = Hit::Part::Libs; return hit;
    }
    if (t.has_lib_dirs && in_range(t.lib_dirs_kw_range)) {
      hit.target_index = i; hit.part = Hit::Part::LibDirs; return hit;
    }
    for (std::size_t j = 0; j < t.lib_dir_ranges.size(); ++j) {
      if (in_range(t.lib_dir_ranges[j])) { hit.target_index = i; hit.part = Hit::Part::LibDirElem; return hit; }
    }
    if (t.has_lib_dirs && in_range(t.lib_dirs_range)) {
      hit.target_index = i; hit.part = Hit::Part::LibDirs; return hit;
    }
    // cflags / defines / ldflags / tests
    for (std::size_t j = 0; j < t.extra_arrays.size(); ++j) {
      const ArrayField &f = t.extra_arrays[j];
      if (in_range(f.kw_range)) {
        hit.target_index = i;
        hit.part = Hit::Part::ExtraField;
        hit.extra_index = j;
        return hit;
      }
      for (std::size_t k = 0; k < f.value_ranges.size(); ++k) {
        if (in_range(f.value_ranges[k])) {
          hit.target_index = i;
          hit.part = Hit::Part::ExtraElem;
          hit.extra_index = j;
          hit.elem_index = k;
          return hit;
        }
      }
      if (in_range(f.range)) {
        hit.target_index = i;
        hit.part = Hit::Part::ExtraField;
        hit.extra_index = j;
        return hit;
      }
    }
    if (in_range(t.call_range)) {
      hit.target_index = i;
      hit.part = Hit::Part::None;
      return hit;
    }
  }
  return hit;
}

} // namespace vmake_lsp
