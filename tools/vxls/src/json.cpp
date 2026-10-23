// json.hpp — vxls 极简 JSON 值与编解码实现（LSP 协议收发专用）。
// 对象成员保留插入顺序，便于构造响应时字段次序稳定。
#include "json.hpp"

#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace vxls::json {

Value Value::boolean(bool b) {
  Value v;
  v.type_ = Type::Bool;
  v.bool_ = b;
  return v;
}

Value Value::number(double n) {
  Value v;
  v.type_ = Type::Number;
  v.num_ = n;
  return v;
}

Value Value::string(std::string s) {
  Value v;
  v.type_ = Type::String;
  v.str_ = std::move(s);
  return v;
}

Value Value::array(std::vector<Value> items) {
  Value v;
  v.type_ = Type::Array;
  v.arr_ = std::move(items);
  return v;
}

Value Value::object(std::vector<std::pair<std::string, Value>> members) {
  Value v;
  v.type_ = Type::Object;
  v.obj_ = std::move(members);
  return v;
}

bool Value::as_bool(bool fallback) const {
  return type_ == Type::Bool ? bool_ : fallback;
}

double Value::as_number(double fallback) const {
  return type_ == Type::Number ? num_ : fallback;
}

const std::string &Value::as_string() const {
  static const std::string kEmpty;
  return type_ == Type::String ? str_ : kEmpty;
}

const std::vector<Value> &Value::as_array() const {
  static const std::vector<Value> kEmpty;
  return type_ == Type::Array ? arr_ : kEmpty;
}

const std::vector<std::pair<std::string, Value>> &Value::as_object() const {
  static const std::vector<std::pair<std::string, Value>> kEmpty;
  return type_ == Type::Object ? obj_ : kEmpty;
}

const Value *Value::find(std::string_view key) const {
  if (type_ != Type::Object) return nullptr;
  for (const auto &kv : obj_) {
    if (kv.first == key) return &kv.second;
  }
  return nullptr;
}

std::string_view Value::str_or(std::string_view key,
                               std::string_view fallback) const {
  const Value *v = find(key);
  return (v != nullptr && v->type_ == Type::String) ? std::string_view(v->str_)
                                                    : fallback;
}

int Value::int_or(std::string_view key, int fallback) const {
  const Value *v = find(key);
  return (v != nullptr && v->type_ == Type::Number)
             ? static_cast<int>(v->num_)
             : fallback;
}

std::string escape_string(std::string_view s) {
  std::string out;
  out.reserve(s.size() + 2);
  out += '"';
  for (unsigned char c : s) {
    switch (c) {
    case '"': out += "\\\""; break;
    case '\\': out += "\\\\"; break;
    case '\n': out += "\\n"; break;
    case '\r': out += "\\r"; break;
    case '\t': out += "\\t"; break;
    case '\b': out += "\\b"; break;
    case '\f': out += "\\f"; break;
    default:
      if (c < 0x20) {
        char buf[8];
        std::snprintf(buf, sizeof(buf), "\\u%04x", c);
        out += buf;
      } else {
        out += static_cast<char>(c);
      }
    }
  }
  out += '"';
  return out;
}

void Value::dump_to(std::string &out) const {
  switch (type_) {
  case Type::Null:
    out += "null";
    break;
  case Type::Bool:
    out += bool_ ? "true" : "false";
    break;
  case Type::Number: {
    char buf[32];
    if (std::floor(num_) == num_ && std::fabs(num_) < 1e15) {
      std::snprintf(buf, sizeof(buf), "%lld",
                    static_cast<long long>(num_));
    } else {
      std::snprintf(buf, sizeof(buf), "%.17g", num_);
    }
    out += buf;
    break;
  }
  case Type::String:
    out += escape_string(str_);
    break;
  case Type::Array: {
    out += '[';
    for (std::size_t i = 0; i < arr_.size(); ++i) {
      if (i) out += ',';
      arr_[i].dump_to(out);
    }
    out += ']';
    break;
  }
  case Type::Object: {
    out += '{';
    for (std::size_t i = 0; i < obj_.size(); ++i) {
      if (i) out += ',';
      out += escape_string(obj_[i].first);
      out += ':';
      obj_[i].second.dump_to(out);
    }
    out += '}';
    break;
  }
  }
}

std::string Value::dump() const {
  std::string out;
  dump_to(out);
  return out;
}

namespace {

// 递归下降解析器
class JsonParser {
public:
  explicit JsonParser(std::string_view text) : text_(text) {}

  std::optional<Value> run() {
    skip_ws();
    std::optional<Value> v = parse_value();
    if (!v) return std::nullopt;
    skip_ws();
    if (pos_ != text_.size()) return std::nullopt; // 尾部残留非法
    return v;
  }

private:
  std::string_view text_;
  std::size_t pos_ = 0;

  void skip_ws() {
    while (pos_ < text_.size()) {
      const char c = text_[pos_];
      if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
        ++pos_;
      } else {
        break;
      }
    }
  }

  bool peek(char c) const {
    return pos_ < text_.size() && text_[pos_] == c;
  }

  bool consume(char c) {
    if (!peek(c)) return false;
    ++pos_;
    return true;
  }

  std::optional<Value> parse_value() {
    if (pos_ >= text_.size()) return std::nullopt;
    const char c = text_[pos_];
    if (c == 'n') return parse_literal("null", Value{});
    if (c == 't') return parse_literal("true", Value::boolean(true));
    if (c == 'f') return parse_literal("false", Value::boolean(false));
    if (c == '"') {
      auto s = parse_string();
      if (!s) return std::nullopt;
      return Value::string(std::move(*s));
    }
    if (c == '[') return parse_array();
    if (c == '{') return parse_object();
    if (c == '-' || (c >= '0' && c <= '9')) return parse_number();
    return std::nullopt;
  }

  std::optional<Value> parse_literal(std::string_view lit, Value v) {
    if (text_.substr(pos_, lit.size()) != lit) return std::nullopt;
    pos_ += lit.size();
    return v;
  }

  std::optional<Value> parse_number() {
    const std::size_t start = pos_;
    if (consume('-')) {}
    while (pos_ < text_.size() &&
           (std::isdigit(static_cast<unsigned char>(text_[pos_])) ||
            text_[pos_] == '.' || text_[pos_] == 'e' || text_[pos_] == 'E' ||
            text_[pos_] == '+' || text_[pos_] == '-')) {
      // 起始 '-' 已消费；数字体内仅接受合法尾随字符
      if ((text_[pos_] == '+' || text_[pos_] == '-') &&
          text_[pos_ - 1] != 'e' && text_[pos_ - 1] != 'E') {
        break;
      }
      ++pos_;
    }
    if (pos_ == start) return std::nullopt;
    const std::string buf(text_.substr(start, pos_ - start));
    char *end = nullptr;
    const double n = std::strtod(buf.c_str(), &end);
    if (end == nullptr || *end != '\0') return std::nullopt;
    return Value::number(n);
  }

  // 解析字符串（含 \uXXXX 与常见转义），结果为 UTF-8
  std::optional<std::string> parse_string() {
    if (!consume('"')) return std::nullopt;
    std::string out;
    while (pos_ < text_.size()) {
      const char c = text_[pos_++];
      if (c == '"') return out;
      if (c != '\\') {
        out += c;
        continue;
      }
      if (pos_ >= text_.size()) return std::nullopt;
      const char e = text_[pos_++];
      switch (e) {
      case '"': out += '"'; break;
      case '\\': out += '\\'; break;
      case '/': out += '/'; break;
      case 'b': out += '\b'; break;
      case 'f': out += '\f'; break;
      case 'n': out += '\n'; break;
      case 'r': out += '\r'; break;
      case 't': out += '\t'; break;
      case 'u': {
        unsigned cp = 0;
        if (!read_hex4(cp)) return std::nullopt;
        // 代理对
        if (cp >= 0xD800 && cp <= 0xDBFF && pos_ + 1 < text_.size() &&
            text_[pos_] == '\\' && text_[pos_ + 1] == 'u') {
          pos_ += 2;
          unsigned lo = 0;
          if (!read_hex4(lo)) return std::nullopt;
          if (lo >= 0xDC00 && lo <= 0xDFFF) {
            cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
          }
        }
        append_utf8(out, cp);
        break;
      }
      default:
        return std::nullopt;
      }
    }
    return std::nullopt; // 未闭合
  }

  bool read_hex4(unsigned &out) {
    if (pos_ + 4 > text_.size()) return false;
    out = 0;
    for (int i = 0; i < 4; ++i) {
      const char c = text_[pos_++];
      out <<= 4;
      if (c >= '0' && c <= '9') out |= static_cast<unsigned>(c - '0');
      else if (c >= 'a' && c <= 'f') out |= static_cast<unsigned>(c - 'a' + 10);
      else if (c >= 'A' && c <= 'F') out |= static_cast<unsigned>(c - 'A' + 10);
      else return false;
    }
    return true;
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

  std::optional<Value> parse_array() {
    if (!consume('[')) return std::nullopt;
    std::vector<Value> items;
    skip_ws();
    if (consume(']')) return Value::array(std::move(items));
    while (true) {
      skip_ws();
      auto v = parse_value();
      if (!v) return std::nullopt;
      items.push_back(std::move(*v));
      skip_ws();
      if (consume(',')) continue;
      if (consume(']')) return Value::array(std::move(items));
      return std::nullopt;
    }
  }

  std::optional<Value> parse_object() {
    if (!consume('{')) return std::nullopt;
    std::vector<std::pair<std::string, Value>> members;
    skip_ws();
    if (consume('}')) return Value::object(std::move(members));
    while (true) {
      skip_ws();
      auto key = parse_string();
      if (!key) return std::nullopt;
      skip_ws();
      if (!consume(':')) return std::nullopt;
      skip_ws();
      auto v = parse_value();
      if (!v) return std::nullopt;
      members.emplace_back(std::move(*key), std::move(*v));
      skip_ws();
      if (consume(',')) continue;
      if (consume('}')) return Value::object(std::move(members));
      return std::nullopt;
    }
  }
};

} // namespace

std::optional<Value> parse(std::string_view text) {
  return JsonParser(text).run();
}

} // namespace vxls::json
