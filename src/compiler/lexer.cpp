// lexer.cpp — VX 源码词法分析器实现（规格对齐自举编译器 compiler/lexer.vx）
#include "vx/compiler/lexer.hpp"

#include <cstring>
#include <cstdio>
#include <utility>
#include <vector>

namespace vx::compiler {

namespace {

constexpr std::string_view kKeywords[] = {
    "if",    "elif",  "else",  "for",   "while", "break", "continue",
    "func",  "return", "true", "false", "nil",   "var",   "mut",
    "import", "struct", "class", "new",  "extern",
    "int",   "float", "bool",  "void",
    "and",   "or",    "not",   "as",    "in",    "loop",  "match",
    "enum",  "union", "macro", "move",
    // 真实源码（mod.vx / lexer.vx）使用而自举关键字表缺失的声明关键字
    "const", "export",
};

// 字母类：ASCII 字母、下划线，以及 >= 0x80 的字节（兼容 UTF-8 标识符）
bool is_letter(char c) {
  const auto u = static_cast<unsigned char>(c);
  return (u >= 'a' && u <= 'z') || (u >= 'A' && u <= 'Z') || u == '_' ||
         u >= 0x80;
}

bool is_digit(char c) { return c >= '0' && c <= '9'; }

class Lexer {
public:
  Lexer(std::string_view src, vx::util::Arena &arena)
      : src_(src), arena_(arena) {}

  bool run() {
    while (true) {
      if (at_start_ && !handle_indent()) {
        return false;
      }
      skip_spaces();
      if (pos_ >= src_.size()) {
        finish();
        return true;
      }

      const char c = src_[pos_];
      const char n = peek();

      if (c == '\n') {
        const int ln = line_; // Nl 归属其终止的逻辑行，便于诊断定位
        advance();
        if (paren_lvl_ == 0) {
          tokens_.push_back(Token{Tok::Nl, {}, ln});
          at_start_ = true;
        }
        continue;
      }
      if (c == '\r') { // CRLF 兼容：回车视为空白
        advance();
        continue;
      }
      if (c == '#') {
        skip_comment();
        continue;
      }
      if (c == ';') { // 单行多语句分隔符：发 Semi，由 Parser 处理语句边界。
        // 不发 Nl 也不置 at_start_：同一物理行内 a(); b() 的后续语句
        // 仍属于当前语句上下文（如 if 单行体），否则 elif 链会断裂。
        advance();
        if (paren_lvl_ == 0) {
          skip_spaces();
          emit(Tok::Semi);
        }
        continue;
      }
      if (c == '"' || c == '\'') {
        if (!read_string(c)) {
          return false;
        }
        continue;
      }
      if (is_digit(c)) {
        read_number();
        continue;
      }
      if (is_letter(c)) {
        read_ident();
        continue;
      }
      if (!read_operator(c, n)) {
        return false;
      }
    }
  }

  std::span<const Token> tokens() const { return tokens_; }
  const LexError &error() const { return err_; }

private:
  char ch() const { return pos_ < src_.size() ? src_[pos_] : '\0'; }

  char peek() const {
    return pos_ + 1 < src_.size() ? src_[pos_ + 1] : '\0';
  }

  void advance() {
    if (pos_ < src_.size()) {
      if (src_[pos_] == '\n') {
        ++line_;
      }
      ++pos_;
    }
  }

  void skip_spaces() {
    while (ch() == ' ' || ch() == '\t') {
      advance();
    }
  }

  void skip_comment() {
    while (ch() != '\0' && ch() != '\n') {
      advance();
    }
  }

  void emit(Tok kind, std::string_view text = {}) {
    tokens_.push_back(Token{kind, text, line_});
  }

  bool err(std::string_view message) {
    err_ = LexError{std::string(message), line_};
    return false;
  }

  // 行首缩进处理：跳过空行/注释行，产出 INDENT/DEDENT
  bool handle_indent() {
    at_start_ = false;
    while (true) {
      int indent = 0;
      while (ch() == ' ') {
        ++indent;
        advance();
      }
      while (ch() == '\t') {
        indent += 4;
        advance();
      }
      if (ch() == '\r') { // CRLF 兼容
        advance();
        continue;
      }
      if (ch() == '\n') { // 空行不参与缩进比较
        advance();
        continue;
      }
      if (ch() == '#') { // 整行注释不参与缩进比较
        skip_comment();
        if (ch() == '\n') {
          advance();
        }
        continue;
      }
      if (pos_ >= src_.size()) { // 文件结束：回退全部缩进
        while (indents_.size() > 1) {
          indents_.pop_back();
          emit(Tok::Dedent);
        }
        return true;
      }
      const int top = indents_.back();
      if (indent > top) {
        if (paren_lvl_ == 0) { // 括号内续行不产生 INDENT
          indents_.push_back(indent);
          emit(Tok::Indent);
        }
      } else if (indent < top) {
        while (indents_.size() > 1 && indents_.back() > indent) {
          indents_.pop_back();
          emit(Tok::Dedent);
        }
      }
      return true;
    }
  }

  bool read_string(char quote) {
    advance(); // 跳过起始引号
    decoded_.clear();
    while (true) {
      if (pos_ >= src_.size()) {
        return err("字符串未闭合");
      }
      if (ch() == quote) {
        advance();
        break;
      }
      if (ch() == '\\') {
        advance();
        if (pos_ >= src_.size()) {
          return err("转义序列不完整");
        }
        const char e = ch();
        advance();
        switch (e) {
        case 'n':
          decoded_ += '\n';
          break;
        case 't':
          decoded_ += '\t';
          break;
        case '\\':
          decoded_ += '\\';
          break;
        case '"':
          decoded_ += '"';
          break;
        case '\'':
          decoded_ += '\'';
          break;
        default: // 未知转义：保留原字符（与自举实现一致）
          decoded_ += e;
          break;
        }
        continue;
      }
      if (ch() == '\n') { // 字符串允许跨行（与自举实现一致）
        ++line_;
      }
      decoded_ += ch();
      advance();
    }
    const auto *stored = arena_.create<std::string>(std::move(decoded_));
    emit(Tok::Str, *stored);
    return true;
  }

  void read_number() {
    const std::size_t start = pos_;
    while (is_digit(ch())) {
      advance();
    }
    bool is_float = false;
    if (ch() == '.' && is_digit(peek())) {
      is_float = true;
      advance(); // '.'
      while (is_digit(ch())) {
        advance();
      }
    }
    emit(is_float ? Tok::Float : Tok::Int, src_.substr(start, pos_ - start));
  }

  void read_ident() {
    const std::size_t start = pos_;
    while (is_letter(ch()) || is_digit(ch())) {
      advance();
    }
    const std::string_view word = src_.substr(start, pos_ - start);
    if (is_keyword(word)) {
      emit(Tok::Kw, word);
    } else {
      emit(Tok::Ident, word);
    }
  }

  bool read_operator(char c, char n) {
    const auto two = [&](std::string_view op) {
      return c == op[0] && n == op[1];
    };
    if (two("+=")) {
      advance();
      advance();
      emit(Tok::PlusEq, "+=");
      return true;
    }
    if (two("-=")) {
      advance();
      advance();
      emit(Tok::MinusEq, "-=");
      return true;
    }
    if (two("->")) {
      advance();
      advance();
      emit(Tok::Arrow, "->");
      return true;
    }
    if (two("*=")) {
      advance();
      advance();
      emit(Tok::StarEq, "*=");
      return true;
    }
    if (two("/=")) {
      advance();
      advance();
      emit(Tok::SlashEq, "/=");
      return true;
    }
    if (two("==")) {
      advance();
      advance();
      emit(Tok::Eq, "==");
      return true;
    }
    if (two("!=")) {
      advance();
      advance();
      emit(Tok::Ne, "!=");
      return true;
    }
    if (two("<=")) {
      advance();
      advance();
      emit(Tok::Le, "<=");
      return true;
    }
    if (two(">=")) {
      advance();
      advance();
      emit(Tok::Ge, ">=");
      return true;
    }
    if (two("&&")) {
      advance();
      advance();
      emit(Tok::And, "and");
      return true;
    }
    if (two("||")) {
      advance();
      advance();
      emit(Tok::Or, "or");
      return true;
    }

    switch (c) {
    case '(':
      ++paren_lvl_;
      advance();
      emit(Tok::LParen, "(");
      return true;
    case ')':
      --paren_lvl_;
      advance();
      emit(Tok::RParen, ")");
      return true;
    case '[':
      ++paren_lvl_;
      advance();
      emit(Tok::LBracket, "[");
      return true;
    case ']':
      --paren_lvl_;
      advance();
      emit(Tok::RBracket, "]");
      return true;
    case '{':
      ++paren_lvl_;
      advance();
      emit(Tok::LBrace, "{");
      return true;
    case '}':
      --paren_lvl_;
      advance();
      emit(Tok::RBrace, "}");
      return true;
    case '+':
      advance();
      emit(Tok::Plus, "+");
      return true;
    case '-':
      advance();
      emit(Tok::Minus, "-");
      return true;
    case '*':
      advance();
      emit(Tok::Star, "*");
      return true;
    case '/':
      advance();
      emit(Tok::Slash, "/");
      return true;
    case '%':
      advance();
      emit(Tok::Percent, "%");
      return true;
    case '^':
      advance();
      emit(Tok::Power, "^");
      return true;
    case '<':
      advance();
      emit(Tok::Lt, "<");
      return true;
    case '>':
      advance();
      emit(Tok::Gt, ">");
      return true;
    case '=':
      advance();
      emit(Tok::Assign, "=");
      return true;
    case '!':
      advance();
      emit(Tok::Not, "!");
      return true;
    case ':':
      advance();
      emit(Tok::Colon, ":");
      return true;
    case ',':
      advance();
      emit(Tok::Comma, ",");
      return true;
    case '.':
      advance();
      emit(Tok::Dot, ".");
      return true;
    default: {
      char buf[32];
      const auto u = static_cast<unsigned char>(c);
      std::snprintf(buf, sizeof(buf), "无法识别的字符 0x%02X", u);
      return err(buf);
    }
    }
  }

  // 文件结束：补终止换行（Nl/Indent/Dedent 之后不再补）、回退缩进、追加 Eof
  void finish() {
    if (!at_start_ && !tokens_.empty()) {
      const Tok last = tokens_.back().kind;
      if (last != Tok::Nl && last != Tok::Indent && last != Tok::Dedent) {
        emit(Tok::Nl);
      }
    }
    while (indents_.size() > 1) {
      indents_.pop_back();
      emit(Tok::Dedent);
    }
    emit(Tok::Eof);
  }

  std::string_view src_;
  vx::util::Arena &arena_;
  std::vector<Token> tokens_;
  std::vector<int> indents_{0}; // 缩进栈，栈底为 0
  std::string decoded_;         // 字符串解码缓冲
  std::size_t pos_ = 0;
  int line_ = 1;
  int paren_lvl_ = 0;
  bool at_start_ = true; // 是否位于行首（待处理缩进）
  LexError err_;
};

} // namespace

bool is_keyword(std::string_view word) {
  for (std::string_view kw : kKeywords) {
    if (kw == word) {
      return true;
    }
  }
  return false;
}

std::string_view tok_name(Tok kind) {
  switch (kind) {
  case Tok::Eof:
    return "Eof";
  case Tok::Nl:
    return "Nl";
  case Tok::Semi:
    return "Semi";
  case Tok::Indent:
    return "Indent";
  case Tok::Dedent:
    return "Dedent";
  case Tok::Ident:
    return "Ident";
  case Tok::Int:
    return "Int";
  case Tok::Float:
    return "Float";
  case Tok::Str:
    return "Str";
  case Tok::Kw:
    return "Kw";
  case Tok::Plus:
    return "Plus";
  case Tok::Minus:
    return "Minus";
  case Tok::Star:
    return "Star";
  case Tok::Slash:
    return "Slash";
  case Tok::Percent:
    return "Percent";
  case Tok::Power:
    return "Power";
  case Tok::Eq:
    return "Eq";
  case Tok::Ne:
    return "Ne";
  case Tok::Lt:
    return "Lt";
  case Tok::Gt:
    return "Gt";
  case Tok::Le:
    return "Le";
  case Tok::Ge:
    return "Ge";
  case Tok::Assign:
    return "Assign";
  case Tok::PlusEq:
    return "PlusEq";
  case Tok::MinusEq:
    return "MinusEq";
  case Tok::StarEq:
    return "StarEq";
  case Tok::SlashEq:
    return "SlashEq";
  case Tok::Arrow:
    return "Arrow";
  case Tok::And:
    return "And";
  case Tok::Or:
    return "Or";
  case Tok::Not:
    return "Not";
  case Tok::LParen:
    return "LParen";
  case Tok::RParen:
    return "RParen";
  case Tok::LBracket:
    return "LBracket";
  case Tok::RBracket:
    return "RBracket";
  case Tok::LBrace:
    return "LBrace";
  case Tok::RBrace:
    return "RBrace";
  case Tok::Colon:
    return "Colon";
  case Tok::Comma:
    return "Comma";
  case Tok::Dot:
    return "Dot";
  }
  return "?";
}

std::expected<std::span<const Token>, LexError>
tokenize(std::string_view src, vx::util::Arena &arena) {
  Lexer lex(src, arena);
  if (!lex.run()) {
    return std::unexpected(lex.error());
  }
  const std::span<const Token> toks = lex.tokens();
  auto *arr = static_cast<Token *>(
      arena.allocate(sizeof(Token) * toks.size(), alignof(Token)));
  std::memcpy(arr, toks.data(), sizeof(Token) * toks.size());
  return std::span<const Token>(arr, toks.size());
}

} // namespace vx::compiler
