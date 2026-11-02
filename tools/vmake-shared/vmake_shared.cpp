// vmake_shared.cpp — VMakeSet.vmake 共享词法与校验规则实现。
#include "vmake_shared.hpp"

#include <cctype>
#include <cstdint>

namespace vmake_shared {
namespace {

bool is_ident_start(char c) {
  return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_';
}

bool is_ident_char(char c) {
  return is_ident_start(c) || (c >= '0' && c <= '9') || c == '-';
}

// 码点 → UTF-8 编码
void append_utf8(std::string& out, std::uint32_t cp) {
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

int hex_value(char c) {
  if (c >= '0' && c <= '9')
    return c - '0';
  if (c >= 'a' && c <= 'f')
    return c - 'a' + 10;
  if (c >= 'A' && c <= 'F')
    return c - 'A' + 10;
  return -1;
}

} // namespace

std::vector<Token> lex(std::string_view text) {
  std::vector<Token> toks;
  std::size_t pos = 0;
  int line = 0;
  int column = 0;

  // 前进 n 个字节并同步行列位置
  auto advance = [&](std::size_t n) {
    for (std::size_t i = 0; i < n && pos < text.size(); ++i, ++pos) {
      if (text[pos] == '\n') {
        ++line;
        column = 0;
      } else {
        ++column;
      }
    }
  };

  while (pos < text.size()) {
    const char c = text[pos];

    if (c == ' ' || c == '\t' || c == '\r') {
      advance(1);
      continue;
    }

    if (c == '\n') {
      Token tok;
      tok.type = TokType::Newline;
      tok.text = "\n";
      tok.line = line;
      tok.column = column;
      tok.end_line = line;
      tok.end_column = column + 1;
      toks.push_back(std::move(tok));
      advance(1);
      continue;
    }

    if (c == '#') { // 注释：跳到行尾
      while (pos < text.size() && text[pos] != '\n')
        advance(1);
      continue;
    }

    if (c == '"' || c == '\'') {
      const char quote = c;
      const int start_line = line;
      const int start_column = column;
      const std::size_t start_pos = pos;
      advance(1); // 消费起始引号
      std::string value;
      bool ok = false;
      bool esc_ok = true;
      while (pos < text.size()) {
        const char sc = text[pos];
        if (sc == quote) {
          advance(1);
          ok = true;
          break;
        }
        if (sc == '\n')
          break; // 字符串不能跨行
        if (quote == '"' && sc == '\\') {
          advance(1); // 消费反斜杠
          if (pos >= text.size())
            break;
          const char esc = text[pos];
          switch (esc) {
            case 'n':
              value += '\n';
              advance(1);
              break;
            case 't':
              value += '\t';
              advance(1);
              break;
            case 'r':
              value += '\r';
              advance(1);
              break;
            case '"':
              value += '"';
              advance(1);
              break;
            case '\\':
              value += '\\';
              advance(1);
              break;
            case 'u': {
              advance(1); // 消费 u
              std::uint32_t cp = 0;
              bool digits_ok = true;
              for (int i = 0; i < 4; ++i) {
                if (pos >= text.size()) {
                  digits_ok = false;
                  break;
                }
                const int h = hex_value(text[pos]);
                if (h < 0) {
                  digits_ok = false;
                  break;
                }
                cp = cp * 16 + static_cast<std::uint32_t>(h);
                advance(1);
              }
              if (!digits_ok) {
                esc_ok = false;
                break;
              }
              append_utf8(value, cp);
              break;
            }
            default:
              esc_ok = false; // 未知转义：非法
              advance(1);
              break;
          }
          if (!esc_ok)
            break;
          continue;
        }
        value += sc;
        advance(1);
      }
      Token tok;
      tok.type = ok ? TokType::Str : TokType::Bad;
      tok.text = std::string(text.substr(start_pos, pos - start_pos));
      tok.value = ok ? std::move(value) : std::string("unterminated string");
      tok.line = start_line;
      tok.column = start_column;
      tok.end_line = line;
      tok.end_column = column;
      toks.push_back(std::move(tok));
      if (!ok)
        return toks; // 非法字符串：终止词法分析
      continue;
    }

    if (c == '(' || c == ')' || c == '[' || c == ']' || c == ',' || c == '=') {
      Token tok;
      switch (c) {
        case '(':
          tok.type = TokType::LParen;
          break;
        case ')':
          tok.type = TokType::RParen;
          break;
        case '[':
          tok.type = TokType::LBracket;
          break;
        case ']':
          tok.type = TokType::RBracket;
          break;
        case ',':
          tok.type = TokType::Comma;
          break;
        default:
          tok.type = TokType::Equals;
          break;
      }
      tok.text = std::string(1, c);
      tok.line = line;
      tok.column = column;
      advance(1);
      tok.end_line = line;
      tok.end_column = column;
      toks.push_back(std::move(tok));
      continue;
    }

    if (is_ident_start(c)) {
      const int start_line = line;
      const int start_column = column;
      const std::size_t start_pos = pos;
      while (pos < text.size() && is_ident_char(text[pos]))
        advance(1);
      Token tok;
      tok.type = TokType::Ident;
      tok.text = std::string(text.substr(start_pos, pos - start_pos));
      tok.value = tok.text;
      tok.line = start_line;
      tok.column = start_column;
      tok.end_line = line;
      tok.end_column = column;
      toks.push_back(std::move(tok));
      continue;
    }

    if (c >= '0' && c <= '9') {
      const int start_line = line;
      const int start_column = column;
      const std::size_t start_pos = pos;
      while (pos < text.size() && text[pos] >= '0' && text[pos] <= '9')
        advance(1);
      Token tok;
      tok.type = TokType::Num;
      tok.text = std::string(text.substr(start_pos, pos - start_pos));
      tok.value = tok.text;
      tok.line = start_line;
      tok.column = start_column;
      tok.end_line = line;
      tok.end_column = column;
      toks.push_back(std::move(tok));
      continue;
    }

    advance(1); // 未知字符：跳过
  }

  Token end;
  end.type = TokType::End;
  end.line = line;
  end.column = column;
  end.end_line = line;
  end.end_column = column;
  toks.push_back(std::move(end));
  return toks;
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

bool is_absolute_path(std::string_view p) {
  if (p.empty())
    return false;
  if (p[0] == '/' || p[0] == '\\')
    return true;
  return p.size() >= 2 && std::isalpha(static_cast<unsigned char>(p[0])) != 0 && p[1] == ':';
}

bool has_dotdot_component(std::string_view p) {
  std::size_t i = 0;
  while (i <= p.size()) {
    std::size_t j = p.find_first_of("/\\", i);
    if (j == std::string_view::npos)
      j = p.size();
    if (p.substr(i, j - i) == "..")
      return true;
    if (j == p.size())
      break;
    i = j + 1;
  }
  return false;
}

bool is_vx_source(std::string_view s) {
  return s.size() >= 3 && s.compare(s.size() - 3, 3, ".vx") == 0;
}

bool is_c_source(std::string_view s) {
  return s.size() >= 2 && s.back() == 'c' && s[s.size() - 2] == '.';
}

bool is_cxx_source(std::string_view s) {
  constexpr std::string_view exts[] = {".cpp", ".cc", ".cxx"};
  for (std::string_view e : exts) {
    if (s.size() >= e.size() && s.compare(s.size() - e.size(), e.size(), e) == 0)
      return true;
  }
  return false;
}

} // namespace vmake_shared
