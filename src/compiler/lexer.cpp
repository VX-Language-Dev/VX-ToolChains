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
  Lexer(std::string_view src, std::string_view filename,
        vx::util::Arena &arena, DiagBag &bag)
      : src_(src), filename_(filename), arena_(arena), bag_(bag) {}

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
      begin_token(); // 记录本 token 起始行列，供 emit 使用

      if (c == '\n') {
        if (paren_lvl_ == 0) { // Nl 归属其终止的逻辑行，列为其终止符列
          emit(Tok::Nl);
          at_start_ = true;
        }
        advance();
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

private:
  char ch() const { return pos_ < src_.size() ? src_[pos_] : '\0'; }

  char peek() const {
    return pos_ + 1 < src_.size() ? src_[pos_ + 1] : '\0';
  }

  void advance() {
    if (pos_ >= src_.size()) {
      return;
    }
    const char c = src_[pos_];
    if (c == '\n') {
      ++line_;
      col_ = 1;
    } else if (c == '\t') {
      // 制表符按 4 列宽计，与 handle_indent() 的缩进宽度保持一致
      col_ += 4;
    } else if ((static_cast<unsigned char>(c) & 0xC0) != 0x80) {
      // 列按 Unicode 码点计：UTF-8 后续字节（10xxxxxx）不推进列号
      ++col_;
    }
    ++pos_;
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
    tokens_.push_back(Token{kind, text, tok_line_, tok_col_});
  }

  // 记录当前 token 起始位置：读取 token 文本前调用
  void begin_token() {
    tok_line_ = line_;
    tok_col_ = col_;
  }

  // 结构性 token（Indent/Dedent）不对应具体字符，定位到逻辑行首
  void begin_line_token() {
    tok_line_ = line_;
    tok_col_ = 1;
  }

  // 词法错误（fail-soft）：把带 span 的诊断推入 bag。
  // 调用方随后做最小恢复并继续词法分析，不中断整个流程。
  void fail_span(std::string_view code, std::string message, int sline,
                 int scol, int eline, int ecol) {
    bag_.add(make_diag_span(code, Severity::Error, std::move(message),
                            std::string(filename_), sline, scol, eline, ecol));
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
        begin_line_token();
        while (indents_.size() > 1 && !indents_.empty()) {
          indents_.pop_back();
          emit(Tok::Dedent);
        }
        return true;
      }
      const int top = indents_.back();
      if (indent > top) {
        if (paren_lvl_ == 0) { // 括号内续行不产生 INDENT
          indents_.push_back(indent);
          begin_line_token();
          emit(Tok::Indent);
        }
      } else if (indent < top) {
        begin_line_token();
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
        // span：自起始引号覆盖到文件末尾
        fail_span(diag_code::lex_unterminated_string, "字符串未闭合", tok_line_,
                  tok_col_, line_, col_);
        return true; // 恢复：字符串已读到文件末尾，交由 run() 收尾
      }
      if (ch() == quote) {
        advance();
        break;
      }
      if (ch() == '\\') {
        advance();
        if (pos_ >= src_.size()) {
          // span：高亮行尾反斜杠本身（col_ 已越过它）
          fail_span(diag_code::lex_incomplete_escape, "转义序列不完整", line_,
                    col_ - 1, line_, col_);
          return true;
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
      // 字符串允许跨行（与自举实现一致）；换行由 advance() 统一维护行/列
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
    case '@':
      advance();
      emit(Tok::At, "@");
      return true;
    default: {
      char buf[32];
      const auto u = static_cast<unsigned char>(c);
      std::snprintf(buf, sizeof(buf), "无法识别的字符 0x%02X", u);
      // span：高亮该字符本身（1 列）
      fail_span(diag_code::lex_unknown_char, buf, tok_line_, tok_col_, tok_line_,
                tok_col_ + 1);
      advance(); // 恢复：跳过该字符，继续词法分析
      return true;
    }
    }
  }

  // 文件结束：补终止换行（Nl/Indent/Dedent 之后不再补）、回退缩进、追加 Eof
  void finish() {
    if (!at_start_ && !tokens_.empty()) {
      const Tok last = tokens_.back().kind;
      if (last != Tok::Nl && last != Tok::Indent && last != Tok::Dedent) {
        begin_token(); // 合成的 Nl 定位到当前行末
        emit(Tok::Nl);
      }
    }
    begin_line_token();
    while (indents_.size() > 1 && !indents_.empty()) {
      indents_.pop_back();
      emit(Tok::Dedent);
    }
    begin_token(); // Eof 定位到文件结束处
    emit(Tok::Eof);
  }

  std::string_view src_;
  std::string_view filename_; // 诊断定位用（可为空）
  vx::util::Arena &arena_;
  DiagBag &bag_;              // 共享诊断收集器
  std::vector<Token> tokens_;
  std::vector<int> indents_{0}; // 缩进栈，栈底为 0
  std::string decoded_;         // 字符串解码缓冲
  std::size_t pos_ = 0;
  int line_ = 1;
  int col_ = 1;
  int tok_line_ = 1; // 当前 token 起始行（emit 使用）
  int tok_col_ = 1;  // 当前 token 起始列
  int paren_lvl_ = 0;
  bool at_start_ = true; // 是否位于行首（待处理缩进）
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
  case Tok::At:
    return "At";
  }
  return "?";
}

std::span<const Token> tokenize(std::string_view src, std::string_view filename,
                                vx::util::Arena &arena, DiagBag &bag) {
  Lexer lex(src, filename, arena, bag);
  lex.run(); // fail-soft：诊断已推入 bag，token 流始终可用
  const std::span<const Token> toks = lex.tokens();
  auto *arr = static_cast<Token *>(
      arena.allocate(sizeof(Token) * toks.size(), alignof(Token)));
  std::memcpy(arr, toks.data(), sizeof(Token) * toks.size());
  return std::span<const Token>(arr, toks.size());
}

} // namespace vx::compiler
