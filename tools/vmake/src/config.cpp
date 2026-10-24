// config.cpp — VMakeLists.toml 最小 TOML 子集解析器。
// 支持：# 注释、[[exe]] / [[dylib]] 数组表、经转义的 "..." 与字面量 '...'
// 字符串、
// 跨行字符串数组与尾随逗号。不支持内联表、多行字符串、点号键（保持错误信息明确）。
#include "config.hpp"

#include <cctype>
#include <cstdint>
#include <filesystem>
#include <map>
#include <utility>

namespace vmake {
namespace {

namespace fs = std::filesystem;

bool is_bare_key_char(char c) {
  const auto u = static_cast<unsigned char>(c);
  return std::isalnum(u) != 0 || c == '_' || c == '-';
}

bool valid_target_name(std::string_view name) {
  if (name.empty())
    return false;
  for (char c : name) {
    if (!is_bare_key_char(c))
      return false;
  }
  return true;
}

// 追加一个 Unicode 码点的 UTF-8 编码
void append_utf8(std::string &out, std::uint32_t cp) {
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

class Parser {
public:
  explicit Parser(std::string_view text) : text_(text) {}

  std::optional<Manifest> run() {
    if (!parse_document())
      return std::nullopt;
    if (!validate())
      return std::nullopt;
    Manifest m;
    m.targets = std::move(targets_);
    return m;
  }

  const ParseError &error() const { return error_; }

private:
  // 扫描中的目标（字段是否出现用 seen_* 跟踪，便于重复键诊断）
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

  bool eof() const { return pos_ >= text_.size(); }
  char peek() const { return eof() ? '\0' : text_[pos_]; }

  char get() {
    const char c = text_[pos_++];
    if (c == '\n')
      ++line_;
    return c;
  }

  void skip_blank() {
    while (!eof() && (peek() == ' ' || peek() == '\t' || peek() == '\r'))
      get();
  }

  void skip_comment() {
    if (peek() == '#') {
      while (!eof() && peek() != '\n')
        get();
    }
  }

  // 跳过空白、换行与注释（数组内部使用）
  void skip_all() {
    for (;;) {
      skip_blank();
      if (peek() == '#') {
        skip_comment();
        continue;
      }
      if (peek() == '\n') {
        get();
        continue;
      }
      break;
    }
  }

  bool end_of_line() {
    skip_blank();
    skip_comment();
    if (eof())
      return true;
    if (peek() == '\n') {
      get();
      return true;
    }
    return false;
  }

  bool fail(std::size_t line, std::string message) {
    error_.line = line;
    error_.message = std::move(message);
    return false;
  }

  bool read_bare_key(std::string &out) {
    while (!eof() && is_bare_key_char(peek()))
      out += get();
    return !out.empty();
  }

  bool parse_document() {
    while (true) {
      skip_all();
      if (eof())
        break;
      if (peek() == '[') {
        if (!parse_header())
          return false;
      } else {
        if (!parse_key_value())
          return false;
      }
      if (!end_of_line()) {
        return fail(line_, "值后存在多余内容（仅允许注释或换行）");
      }
    }
    return true;
  }

  bool parse_header() {
    const std::size_t line = line_;
    get(); // '['
    bool array = false;
    if (peek() == '[') {
      array = true;
      get();
    }
    skip_blank();
    std::string name;
    if (!read_bare_key(name)) {
      return fail(line, "表头缺少名称");
    }
    skip_blank();
    if (peek() != ']') {
      return fail(line, "表头缺少 ']'");
    }
    get();
    if (array) {
      if (peek() != ']') {
        return fail(line, "数组表头缺少 ']]'");
      }
      get();
    }
    if (!array) {
      return fail(line, "仅支持数组表 [[exe]] / [[dylib]]");
    }
    if (name != "exe" && name != "dylib") {
      return fail(line, "未知目标类型 [[" + name + "]]（支持 exe / dylib）");
    }
    Pending p;
    p.kind = name == "exe" ? TargetKind::Exe : TargetKind::Dylib;
    p.line = line;
    pending_.push_back(std::move(p));
    has_current_ = true;
    return true;
  }

  bool parse_key_value() {
    const std::size_t line = line_;
    if (!has_current_) {
      return fail(line, "键值必须位于 [[exe]] 或 [[dylib]] 表内");
    }
    std::string key;
    if (!read_bare_key(key)) {
      return fail(line, "缺少配置键");
    }
    skip_blank();
    if (peek() != '=') {
      return fail(line, "配置键 '" + key + "' 后缺少 '='");
    }
    get();
    skip_blank();
    Pending &p = pending_.back();

    if (key == "name") {
      if (p.seen_name)
        return fail(line, "字段 name 重复");
      std::string value;
      if (!parse_string(value))
        return false;
      p.name = std::move(value);
      p.seen_name = true;
      return true;
    }
    if (key == "sources") {
      if (p.seen_sources)
        return fail(line, "字段 sources 重复");
      if (peek() != '[')
        return fail(line, "sources 期望字符串数组，如 [\"main.vx\"]");
      std::vector<std::string> value;
      if (!parse_string_array(value))
        return false;
      p.sources = std::move(value);
      p.seen_sources = true;
      return true;
    }
    if (key == "output") {
      if (p.seen_output)
        return fail(line, "字段 output 重复");
      std::string value;
      if (!parse_string(value))
        return false;
      p.output = std::move(value);
      p.seen_output = true;
      return true;
    }
    return fail(line,
                "未知配置键 '" + key + "'（支持 name / sources / output）");
  }

  bool parse_string(std::string &out) {
    const std::size_t line = line_;
    if (eof() || (peek() != '"' && peek() != '\'')) {
      return fail(line, "期望字符串（使用 \"...\" 或 '...'）");
    }
    const char quote = get();
    // 拒绝 """ / ''' 多行字符串（避免误解析）
    if (peek() == quote && pos_ + 1 < text_.size() &&
        text_[pos_ + 1] == quote) {
      return fail(line, "暂不支持多行字符串");
    }
    while (!eof()) {
      const char c = get();
      if (c == quote)
        return true;
      if (c == '\n')
        return fail(line, "字符串不能跨行");
      if (quote == '"' && c == '\\') {
        if (eof())
          return fail(line, "字符串转义不完整");
        const char esc = get();
        switch (esc) {
        case 'n':
          out += '\n';
          break;
        case 't':
          out += '\t';
          break;
        case 'r':
          out += '\r';
          break;
        case 'b':
          out += '\b';
          break;
        case 'f':
          out += '\f';
          break;
        case '"':
          out += '"';
          break;
        case '\\':
          out += '\\';
          break;
        case 'u': {
          std::uint32_t cp = 0;
          for (int i = 0; i < 4; ++i) {
            if (eof())
              return fail(line, "\\u 转义缺少十六进制位");
            const char h = get();
            if (std::isxdigit(static_cast<unsigned char>(h)) == 0) {
              return fail(line, "\\u 转义包含非法十六进制字符");
            }
            cp = cp * 16 +
                 static_cast<std::uint32_t>(
                     std::isdigit(static_cast<unsigned char>(h))
                         ? h - '0'
                         : (std::tolower(static_cast<unsigned char>(h)) - 'a' +
                            10));
          }
          if (cp >= 0xD800 && cp <= 0xDFFF) {
            return fail(line, "\\u 不支持代理对码点");
          }
          append_utf8(out, cp);
          break;
        }
        default:
          return fail(line, std::string("不支持的转义序列 \\") + esc);
        }
      } else {
        out += c;
      }
    }
    return fail(line, "字符串未闭合");
  }

  bool parse_string_array(std::vector<std::string> &out) {
    const std::size_t line = line_;
    get(); // '['
    for (;;) {
      skip_all();
      if (eof())
        return fail(line, "字符串数组未闭合");
      if (peek() == ']') {
        get();
        return true;
      }
      if (peek() != '"' && peek() != '\'') {
        return fail(line_, "数组元素必须是字符串");
      }
      std::string value;
      if (!parse_string(value))
        return false;
      out.push_back(std::move(value));
      skip_all();
      if (eof())
        return fail(line, "字符串数组未闭合");
      if (peek() == ',') {
        get();
        continue;
      }
      if (peek() == ']') {
        get();
        return true;
      }
      return fail(line_, "数组元素之间缺少 ','");
    }
  }

  bool validate() {
    if (pending_.empty()) {
      return fail(1, "未声明任何目标（使用 [[exe]] 或 [[dylib]]）");
    }
    std::map<std::string, std::size_t> names;
    for (const Pending &p : pending_) {
      if (!p.name) {
        return fail(p.line, "目标缺少 name 字段");
      }
      if (!valid_target_name(*p.name)) {
        return fail(p.line, "目标名 '" + *p.name +
                                "' 非法（仅允许字母、数字、'_'、'-'）");
      }
      const auto [it, inserted] = names.emplace(*p.name, p.line);
      if (!inserted) {
        return fail(p.line, "目标名 '" + *p.name + "' 重复（首次声明于第 " +
                                std::to_string(it->second) + " 行）");
      }
      if (!p.sources) {
        return fail(p.line, "目标 '" + *p.name + "' 缺少 sources 字段");
      }
      if (p.sources->empty()) {
        return fail(p.line, "目标 '" + *p.name + "' 的 sources 不能为空");
      }
      for (const std::string &src : *p.sources) {
        if (src.empty()) {
          return fail(p.line, "目标 '" + *p.name + "' 含空源文件路径");
        }
        if (src.size() < 4 || src.substr(src.size() - 3) != ".vx") {
          return fail(p.line, "源文件必须是 .vx 文件: " + src);
        }
      }
      if (p.output) {
        if (p.output->empty()) {
          return fail(p.line, "目标 '" + *p.name + "' 的 output 不能为空");
        }
        const fs::path op(*p.output);
        if (op.is_absolute()) {
          return fail(p.line, "output 必须是相对构建目录的路径: " + *p.output);
        }
        for (const fs::path &part : op) {
          if (part == "..") {
            return fail(p.line, "output 不得包含 '..': " + *p.output);
          }
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

  std::string_view text_;
  std::size_t pos_ = 0;
  std::size_t line_ = 1;
  std::vector<Pending> pending_;
  std::vector<Target> targets_;
  bool has_current_ = false;
  ParseError error_;
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
