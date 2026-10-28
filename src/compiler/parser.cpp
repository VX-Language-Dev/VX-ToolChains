// parser.cpp — VX 语法分析器实现
#include "vx/compiler/parser.hpp"

#include <cstdio>
#include <new>
#include <vector>

namespace vx::compiler {

namespace {

// 二元运算符优先级（对齐自举 op_precedence）
int binary_precedence(const Token &t) {
  switch (t.kind) {
  case Tok::Or:
    return 1;
  case Tok::And:
    return 2;
  case Tok::Eq:
  case Tok::Ne:
    return 3;
  case Tok::Lt:
  case Tok::Gt:
  case Tok::Le:
  case Tok::Ge:
    return 4;
  case Tok::Plus:
  case Tok::Minus:
    return 5;
  case Tok::Star:
  case Tok::Slash:
  case Tok::Percent:
    return 6;
  case Tok::Power:
    return 7;
  case Tok::Kw:
    if (t.text == "and") {
      return 2;
    }
    if (t.text == "or") {
      return 1;
    }
    return 0;
  default:
    return 0;
  }
}

bool is_assign_op(Tok kind) {
  switch (kind) {
  case Tok::Assign:
  case Tok::PlusEq:
  case Tok::MinusEq:
  case Tok::StarEq:
  case Tok::SlashEq:
    return true;
  default:
    return false;
  }
}

// 虚关键词：语法保留但真实源码（codegen.vx 等）将其用作变量名，
// 表达式位置回退为标识符。
bool is_soft_kw(std::string_view text) {
  return text == "loop" || text == "match" || text == "enum" ||
         text == "union" || text == "macro" || text == "move";
}

// 字符串的 Unicode 码点个数（UTF-8 续字节 10xxxxxx 不计入）
std::size_t utf8_len(std::string_view s) {
  std::size_t n = 0;
  for (char c : s) {
    if ((static_cast<unsigned char>(c) & 0xC0) != 0x80) {
      ++n;
    }
  }
  return n;
}

template <class T>
std::span<const T> arena_span(vx::util::Arena &a, const std::vector<T> &v) {
  if (v.empty()) {
    return {};
  }
  T *p = static_cast<T *>(a.allocate(sizeof(T) * v.size(), alignof(T)));
  for (std::size_t i = 0; i < v.size(); ++i) {
    ::new (static_cast<void *>(&p[i])) T(v[i]);
  }
  return std::span<const T>(p, v.size());
}

class Parser {
public:
  Parser(std::span<const Token> toks, std::string_view filename,
         vx::util::Arena &arena, DiagBag &bag)
      : toks_(toks), filename_(filename), arena_(arena), bag_(bag) {}

  std::optional<Program> run() {
    std::vector<const Stmt *> stmts;
    while (true) {
      skip_nls();
      // 顶层语句不应以 Indent 开头；此处出现 Indent 说明上一条（块）语句
      // 解析失败并遗留了孤儿缩进块。静默丢弃该块，避免级联出更多诊断。
      if (at(Tok::Indent)) {
        skip_indented_block();
        continue;
      }
      if (at(Tok::Eof)) {
        break;
      }
      const std::size_t start = pos_;
      const Stmt *s = parse_stmt();
      if (!s) {
        // 错误恢复：跳到下一语句边界，继续解析后续语句，
        // 尽量一次性暴露多个语法错误而非仅首个。
        skip_to_stmt_boundary();
        // 保证前进：若解析与重同步都未消费任何 token（例如停在 Semi/Dedent
        // 边界上，而 skip_nls 不会消费它们），强制跨过该 token，避免死循环。
        if (pos_ == start) {
          advance();
          skip_to_stmt_boundary();
        }
        continue;
      }
      stmts.push_back(s);
      if (!end_stmt()) {
        skip_to_stmt_boundary();
        continue;
      }
    }
    // fail-soft：出错时仍返回已解析的部分 AST，
    // 由调用方通过 bag.has_error() 决定是否终止后续阶段
    // （LSP 借此在编辑中间态提供符号/悬停/补全）。
    return Program{arena_span(arena_, stmts)};
  }

  // 跳到下一语句边界（Nl / Semi / Dedent / Eof），用于错误恢复重同步。
  // 不越过 Dedent/Eof，以免跨越块边界导致更多级联错误。
  void skip_to_stmt_boundary() {
    while (!at(Tok::Eof) && !at(Tok::Nl) && !at(Tok::Semi) &&
           !at(Tok::Dedent)) {
      advance();
    }
  }

  // 从当前 Indent 起，消费到匹配的 Dedent 之后（含嵌套块）。
  // 仅用于丢弃错误恢复中遗留的孤儿缩进块。
  void skip_indented_block() {
    int depth = 0;
    while (!at(Tok::Eof)) {
      if (at(Tok::Indent)) {
        ++depth;
        advance();
        continue;
      }
      if (at(Tok::Dedent)) {
        advance();
        if (--depth <= 0) {
          return;
        }
        continue;
      }
      advance();
    }
  }

private:
  const Token &peek() const { return toks_[pos_]; }

  const Token &advance() { return toks_[pos_++]; }

  bool at(Tok kind) const { return peek().kind == kind; }

  bool at_kw(std::string_view name) const {
    return at(Tok::Kw) && peek().text == name;
  }

  // 语法错误（fail-fast）：推入 bag，由上层据此停止解析。
  bool fail_span(std::string message, int line, int col, int end_line,
                 int end_col) {
    bag_.add(make_diag_span(diag_code::parse_expected_token, Severity::Error,
                            std::move(message), std::string(filename_), line,
                            col, end_line, end_col));
    return false;
  }

  bool fail_here(std::string what) {
    const Token &t = peek();
    char buf[128];
    std::snprintf(buf, sizeof(buf), "期望%s，但得到 %s '%.*s' (行 %d)",
                  what.c_str(), tok_name(t.kind).data(),
                  static_cast<int>(t.text.size()), t.text.data(), t.line);
    // 高亮整个 offending token（按码点计长度；空文本退化为单字符插入符）
    const int end_col = t.col + static_cast<int>(utf8_len(t.text));
    return fail_span(buf, t.line, t.col, t.line, end_col);
  }

  bool expect(Tok kind, std::string_view what) {
    if (!at(kind)) {
      return fail_here(std::string(" ") + std::string(what));
    }
    advance();
    return true;
  }

  void skip_nls() {
    while (at(Tok::Nl)) {
      advance();
    }
  }

  // 消费语句结束符：Nl/Semi 吃掉并返回 true；Dedent/Eof 不消费返回 true；
  // 多行块语句（if/while/func/class...）的 Dedent 已被块消费，其后无需换行。
  bool end_stmt() {
    if (at(Tok::Nl) || at(Tok::Semi)) {
      advance();
      return true;
    }
    if (at(Tok::Dedent) || at(Tok::Eof)) {
      return true;
    }
    if (pos_ > 0 && toks_[pos_ - 1].kind == Tok::Dedent) {
      return true;
    }
    return fail_here("换行");
  }

  // 名称 token：Ident 或关键字（方法名 new/union 等、函数名 new）
  bool parse_name(std::string_view &out) {
    if (at(Tok::Ident) || at(Tok::Kw)) {
      out = advance().text;
      return true;
    }
    return fail_here("标识符");
  }

  // 类型名：Ident 或关键字（int/var/str/...）；缺失时返回空
  std::string_view parse_type_opt() {
    if (at(Tok::Ident) || at(Tok::Kw)) {
      return advance().text;
    }
    return {};
  }

  // ===== 语句 =====
  const Stmt *parse_stmt() {
    const Token &t = peek();
    if (t.kind == Tok::At) {
      return parse_at_directive();
    }
    if (t.kind == Tok::Kw) {
      if (t.text == "var" || t.text == "mut" || t.text == "const") {
        return parse_var_decl();
      }
      if (t.text == "func") {
        return parse_func_decl();
      }
      if (t.text == "extern") {
        return parse_extern_decl();
      }
      if (t.text == "import") {
        return parse_import();
      }
      if (t.text == "export") {
        return parse_export();
      }
      if (t.text == "return") {
        return parse_return();
      }
      if (t.text == "break") {
        advance();
        return arena_.create<Break>(t.line);
      }
      if (t.text == "continue") {
        advance();
        return arena_.create<Continue>(t.line);
      }
      if (t.text == "if") {
        return parse_if();
      }
      if (t.text == "while") {
        return parse_while();
      }
      if (t.text == "for") {
        return parse_for();
      }
      if (t.text == "class") {
        return parse_class();
      }
      if (t.text == "struct") {
        return parse_struct();
      }
    }
    return parse_expr_stmt();
  }

  const Stmt *parse_var_decl() {
    const int line = peek().line;
    const bool is_mut = advance().text == "mut"; // var/mut
    std::string_view name;
    if (!parse_name(name)) {
      return nullptr;
    }
    std::string_view type_ann;
    if (at(Tok::Colon)) {
      advance();
      type_ann = parse_type_opt();
    }
    const Expr *init = nullptr;
    if (at(Tok::Assign)) {
      advance();
      init = parse_expr(1);
      if (!init) {
        return nullptr;
      }
    }
    return arena_.create<VarDecl>(name, type_ann, init, is_mut, line);
  }

  const Stmt *parse_func_decl() {
    const int line = peek().line;
    advance(); // 'func'
    std::string_view name;
    if (!parse_name(name)) { // 方法名可为关键字（如 new）
      return nullptr;
    }
    std::vector<Param> params;
    if (!parse_param_list(params)) {
      return nullptr;
    }
    std::string_view ret_type;
    if (at(Tok::Arrow)) {
      advance();
      ret_type = parse_type_opt();
    }
    if (!expect(Tok::Colon, "':'")) {
      return nullptr;
    }
    std::vector<const Stmt *> body;
    if (!parse_block(body)) {
      return nullptr;
    }
    return arena_.create<FuncDecl>(
        name, arena_span(arena_, params), ret_type, arena_span(arena_, body),
        line);
  }

  bool parse_param_list(std::vector<Param> &out) {
    if (!expect(Tok::LParen, "'('")) {
      return false;
    }
    skip_nls();
    while (!at(Tok::RParen)) {
      std::string_view pname;
      if (!parse_name(pname)) {
        return false;
      }
      std::string_view ptype;
      if (at(Tok::Colon)) {
        advance();
        ptype = parse_type_opt();
      }
      out.push_back(Param{pname, ptype});
      if (at(Tok::Comma)) {
        advance();
        skip_nls();
        continue;
      }
      break;
    }
    return expect(Tok::RParen, "')'");
  }

  const Stmt *parse_extern_decl() {
    const int line = peek().line;
    advance(); // 'extern'
    if (at_kw("func")) {
      advance();
    }
    std::string_view name;
    if (!parse_name(name)) {
      return nullptr;
    }
    std::vector<Param> params;
    if (!parse_param_list(params)) {
      return nullptr;
    }
    std::string_view ret_type;
    if (at(Tok::Arrow)) {
      advance();
      ret_type = parse_type_opt();
    }
    return arena_.create<ExternDecl>(name, arena_span(arena_, params),
                                     ret_type, line);
  }

  const Stmt *parse_import() {
    const int line = peek().line;
    advance(); // 'import'
    std::string_view path;
    if (!parse_name(path)) {
      return nullptr;
    }
    // 点分路径拼回一个 string_view 不连续，改用首段 + 段数？
    // 简化：path 存完整文本需要拼接；这里记录最后一段前的整串不可行，
    // 故用 arena 拼接字符串。
    std::string full(path);
    while (at(Tok::Dot)) {
      advance();
      std::string_view seg;
      if (!parse_name(seg)) {
        return nullptr;
      }
      full += ".";
      full += seg;
    }
    if (at_kw("as")) {
      advance();
      std::string_view alias;
      if (!parse_name(alias)) {
        return nullptr;
      }
    }
    auto *stored = arena_.create<std::string>(std::move(full));
    return arena_.create<Import>(*stored, line);
  }

  // 导入注解：@imp(<库引用>) [as alias] / @cppimp(<头文件路径>) [as alias]
  // 参数文本由若干 token 无分隔拼接而成（路径中的 '/' '.' '-' 均为独立 token）。
  const Stmt *parse_at_directive() {
    const int line = peek().line;
    advance(); // '@'
    if (!at(Tok::Ident)) {
      fail_here("注解名 imp 或 cppimp");
      return nullptr;
    }
    const std::string_view name = peek().text;
    if (name != "imp" && name != "cppimp") {
      fail_here("注解名 imp 或 cppimp");
      return nullptr;
    }
    advance();
    if (!expect(Tok::LParen, "'('")) {
      return nullptr;
    }
    std::string ref;
    if (!read_lib_ref(ref)) {
      return nullptr;
    }
    if (!expect(Tok::RParen, "')'")) {
      return nullptr;
    }
    std::string_view alias;
    if (at_kw("as")) {
      advance();
      if (!parse_name(alias)) {
        return nullptr;
      }
    }
    auto *stored = arena_.create<std::string>(std::move(ref));
    if (name == "imp") {
      return arena_.create<ImpDecl>(*stored, alias, line);
    }
    return arena_.create<CppImpDecl>(*stored, alias, line);
  }

  // 读取括号内的库引用/路径文本：字符串字面量直接取解码值；
  // 否则无缝拼接 Ident/Kw/Int 与 '/' '.' '-' 直到右括号。
  bool read_lib_ref(std::string &out) {
    if (at(Tok::Str)) {
      out = std::string(peek().text);
      advance();
      return true;
    }
    while (!at(Tok::RParen) && !at(Tok::Nl) && !at(Tok::Eof)) {
      switch (peek().kind) {
      case Tok::Ident:
      case Tok::Kw:
      case Tok::Int:
      case Tok::Slash:
      case Tok::Dot:
      case Tok::Minus:
        out += peek().text;
        advance();
        break;
      default:
        return fail_here("库名/路径字符");
      }
    }
    if (out.empty()) {
      return fail_here("库名/路径");
    }
    return true;
  }

  const Stmt *parse_export() {
    const int line = peek().line;
    advance(); // 'export'
    std::string_view name;
    if (!parse_name(name)) {
      return nullptr;
    }
    return arena_.create<Export>(name, line);
  }

  const Stmt *parse_return() {
    const int line = peek().line;
    advance(); // 'return'
    const Expr *value = nullptr;
    if (!at(Tok::Nl) && !at(Tok::Semi) && !at(Tok::Dedent) &&
        !at(Tok::Eof)) {
      value = parse_expr(1);
      if (!value) {
        return nullptr;
      }
    }
    return arena_.create<Return>(value, line);
  }

  const Stmt *parse_if() {
    const int line = peek().line;
    advance(); // 'if'
    const Expr *cond = parse_expr(1);
    if (!cond) {
      return nullptr;
    }
    if (!expect(Tok::Colon, "':'")) {
      return nullptr;
    }
    std::vector<const Stmt *> then_body;
    if (!parse_block(then_body)) {
      return nullptr;
    }
    std::vector<ElifArm> elifs;
    while (peek_past_nl_is_kw("elif")) {
      skip_nls();
      advance(); // 'elif'
      const Expr *econd = parse_expr(1);
      if (!econd) {
        return nullptr;
      }
      if (!expect(Tok::Colon, "':'")) {
        return nullptr;
      }
      std::vector<const Stmt *> ebody;
      if (!parse_block(ebody)) {
        return nullptr;
      }
      elifs.push_back(ElifArm{econd, arena_span(arena_, ebody)});
    }
    std::vector<const Stmt *> else_body;
    if (peek_past_nl_is_kw("else")) {
      skip_nls();
      advance(); // 'else'
      if (!expect(Tok::Colon, "':'")) {
        return nullptr;
      }
      if (!parse_block(else_body)) {
        return nullptr;
      }
    }
    return arena_.create<If>(cond, arena_span(arena_, then_body),
                             arena_span(arena_, elifs),
                             arena_span(arena_, else_body), line);
  }

  // 前瞻（跳过 Nl）判断下一有效 token 是否为指定关键字；不消费
  bool peek_past_nl_is_kw(std::string_view name) const {
    std::size_t i = pos_;
    while (i < toks_.size() && toks_[i].kind == Tok::Nl) {
      ++i;
    }
    return i < toks_.size() && toks_[i].kind == Tok::Kw &&
           toks_[i].text == name;
  }

  const Stmt *parse_while() {
    const int line = peek().line;
    advance(); // 'while'
    const Expr *cond = parse_expr(1);
    if (!cond) {
      return nullptr;
    }
    if (!expect(Tok::Colon, "':'")) {
      return nullptr;
    }
    std::vector<const Stmt *> body;
    if (!parse_block(body)) {
      return nullptr;
    }
    return arena_.create<While>(cond, arena_span(arena_, body), line);
  }

  const Stmt *parse_for() {
    const int line = peek().line;
    advance(); // 'for'
    std::string_view var_name;
    if (!parse_name(var_name)) {
      return nullptr;
    }
    if (!at_kw("in")) {
      fail_here("'in'");
      return nullptr;
    }
    advance();
    const Expr *iterable = parse_expr(1);
    if (!iterable) {
      return nullptr;
    }
    if (!expect(Tok::Colon, "':'")) {
      return nullptr;
    }
    std::vector<const Stmt *> body;
    if (!parse_block(body)) {
      return nullptr;
    }
    return arena_.create<For>(var_name, iterable, arena_span(arena_, body),
                              line);
  }

  const Stmt *parse_class() {
    const int line = peek().line;
    advance(); // 'class'
    std::string_view name;
    if (!parse_name(name)) {
      return nullptr;
    }
    if (!expect(Tok::Colon, "':'")) {
      return nullptr;
    }
    std::vector<ClassField> fields;
    std::vector<const FuncDecl *> methods;
    skip_nls();
    if (at(Tok::Indent)) {
      advance();
      while (true) {
        skip_nls();
        if (at(Tok::Dedent) || at(Tok::Eof)) {
          break;
        }
        if (at_kw("func")) {
          const Stmt *m = parse_func_decl();
          if (!m) {
            return nullptr;
          }
          methods.push_back(static_cast<const FuncDecl *>(m));
          if (!end_stmt()) {
            return nullptr;
          }
        } else {
          std::string_view fname;
          if (!parse_name(fname)) {
            return nullptr;
          }
          if (at(Tok::Assign)) {
            // 类常量字段：NAME = expr（无类型注解，不占实例布局）
            advance();
            const Expr *init = parse_expr(0);
            if (!init) {
              return nullptr;
            }
            fields.push_back(ClassField{fname, "", init});
          } else {
            if (!expect(Tok::Colon, "':' 或方法定义")) {
              return nullptr;
            }
            std::string_view ftype = parse_type_opt();
            fields.push_back(ClassField{fname, ftype});
          }
          if (!end_stmt()) {
            return nullptr;
          }
        }
      }
      if (at(Tok::Dedent)) {
        advance();
      }
    }
    return arena_.create<ClassDecl>(name, arena_span(arena_, fields),
                                    arena_span(arena_, methods), line);
  }

  const Stmt *parse_struct() {
    const int line = peek().line;
    advance(); // 'struct'
    std::string_view name;
    if (!parse_name(name)) {
      return nullptr;
    }
    if (!expect(Tok::Colon, "':'")) {
      return nullptr;
    }
    std::vector<ClassField> fields;
    skip_nls();
    if (at(Tok::Indent)) {
      advance();
      while (true) {
        skip_nls();
        if (at(Tok::Dedent) || at(Tok::Eof)) {
          break;
        }
        std::string_view fname;
        if (!parse_name(fname)) {
          return nullptr;
        }
        if (!expect(Tok::Colon, "':'")) {
          return nullptr;
        }
        std::string_view ftype = parse_type_opt();
        fields.push_back(ClassField{fname, ftype});
        if (!end_stmt()) {
          return nullptr;
        }
      }
      if (at(Tok::Dedent)) {
        advance();
      }
    }
    return arena_.create<StructDecl>(name, arena_span(arena_, fields), line);
  }

  // 块：INDENT 多语句，或 ':' 后同行语句列表（Semi 分隔）
  bool parse_block(std::vector<const Stmt *> &out) {
    if (at(Tok::Nl)) {
      const std::size_t save = pos_;
      skip_nls();
      if (at(Tok::Indent)) {
        advance();
        while (true) {
          skip_nls();
          if (at(Tok::Dedent) || at(Tok::Eof)) {
            break;
          }
          const Stmt *s = parse_stmt();
          if (!s) {
            return false;
          }
          out.push_back(s);
          if (!end_stmt()) {
            return false;
          }
        }
        if (at(Tok::Dedent)) {
          advance();
        }
        return true;
      }
      // 缩进块缺失（如仅含注释的空体）：合法，返回空块。
      // 恢复首个 Nl 不消费，留给调用方 end_stmt 作语句边界。
      pos_ = save;
      return true;
    }
    // 同行体：if cond: a(); b()
    while (true) {
      const Stmt *s = parse_stmt();
      if (!s) {
        return false;
      }
      out.push_back(s);
      if (at(Tok::Semi)) {
        advance();
        continue;
      }
      break;
    }
    return true;
  }

  // 表达式语句或赋值语句
  const Stmt *parse_expr_stmt() {
    const int line = peek().line;
    const Expr *e = parse_expr(1);
    if (!e) {
      return nullptr;
    }
    if (is_assign_op(peek().kind)) {
      const std::string_view op = advance().text;
      const Expr *value = parse_expr(1);
      if (!value) {
        return nullptr;
      }
      return arena_.create<Assign>(e, op, value, line);
    }
    return arena_.create<ExprStmt>(e, line);
  }

  // ===== 表达式 =====
  const Expr *parse_expr(int min_prec) {
    const Expr *left = parse_unary();
    if (!left) {
      return nullptr;
    }
    while (true) {
      const Token &t = peek();
      const int prec = binary_precedence(t);
      if (prec == 0 || prec < min_prec) {
        return left;
      }
      const std::string_view op = t.text;
      const int line = t.line;
      advance();
      const Expr *right = parse_expr(prec + 1);
      if (!right) {
        return nullptr;
      }
      left = arena_.create<Binary>(op, left, right, line);
    }
  }

  const Expr *parse_unary() {
    const Token &t = peek();
    if (t.kind == Tok::Minus) {
      advance();
      const Expr *operand = parse_unary();
      if (!operand) {
        return nullptr;
      }
      return arena_.create<Unary>("-", operand, t.line);
    }
    if (t.kind == Tok::Not || (t.kind == Tok::Kw && t.text == "not")) {
      advance();
      const Expr *operand = parse_unary();
      if (!operand) {
        return nullptr;
      }
      return arena_.create<Unary>("!", operand, t.line);
    }
    return parse_postfix();
  }

  const Expr *parse_postfix() {
    const Expr *e = parse_primary();
    if (!e) {
      return nullptr;
    }
    while (true) {
      if (at(Tok::Dot)) {
        const int line = advance().line;
        std::string_view name;
        if (!parse_name(name)) { // 成员名可为关键字（Set.union / Lexer.new）
          return nullptr;
        }
        if (at(Tok::LParen)) {
          std::vector<const Expr *> args;
          if (!parse_args(args)) {
            return nullptr;
          }
          const auto *prop =
              arena_.create<Property>(e, name, line);
          e = arena_.create<Call>(prop, arena_span(arena_, args), line);
        } else {
          e = arena_.create<Property>(e, name, line);
        }
        continue;
      }
      if (at(Tok::LParen)) {
        const int line = peek().line;
        std::vector<const Expr *> args;
        if (!parse_args(args)) {
          return nullptr;
        }
        e = arena_.create<Call>(e, arena_span(arena_, args), line);
        continue;
      }
      if (at(Tok::LBracket)) {
        const int line = advance().line;
        const Expr *index = parse_expr(1);
        if (!index) {
          return nullptr;
        }
        if (!expect(Tok::RBracket, "']'")) {
          return nullptr;
        }
        e = arena_.create<Index>(e, index, line);
        continue;
      }
      return e;
    }
  }

  bool parse_args(std::vector<const Expr *> &out) {
    advance(); // '('
    while (!at(Tok::RParen)) {
      const Expr *a = parse_expr(1);
      if (!a) {
        return false;
      }
      out.push_back(a);
      if (at(Tok::Comma)) {
        advance();
        if (at(Tok::RParen)) {
          break; // 允许尾逗号
        }
        continue;
      }
      break;
    }
    return expect(Tok::RParen, "')'");
  }

  const Expr *parse_primary() {
    const Token &t = peek();
    switch (t.kind) {
    case Tok::Int:
      return arena_.create<IntLit>(advance().text, t.line);
    case Tok::Float:
      return arena_.create<FloatLit>(advance().text, t.line);
    case Tok::Str:
      return arena_.create<StrLit>(advance().text, t.line);
    case Tok::Ident:
      return arena_.create<Ident>(advance().text, t.line);
    case Tok::Kw:
      if (t.text == "true") {
        advance();
        return arena_.create<BoolLit>(true, t.line);
      }
      if (t.text == "false") {
        advance();
        return arena_.create<BoolLit>(false, t.line);
      }
      if (t.text == "nil") {
        advance();
        return arena_.create<NilLit>(t.line);
      }
      if (t.text == "new") {
        advance();
        std::string_view name;
        if (!parse_name(name)) {
          return nullptr;
        }
        std::vector<const Expr *> args;
        if (at(Tok::LParen)) {
          if (!parse_args(args)) {
            return nullptr;
          }
        }
        return arena_.create<New>(name, arena_span(arena_, args), t.line);
      }
      if (is_soft_kw(t.text)) {
        return arena_.create<Ident>(advance().text, t.line);
      }
      break;
    case Tok::LParen: {
      advance();
      const Expr *e = parse_expr(1);
      if (!e) {
        return nullptr;
      }
      if (!expect(Tok::RParen, "')'")) {
        return nullptr;
      }
      return e;
    }
    case Tok::LBracket: {
      const int line = advance().line;
      std::vector<const Expr *> items;
      while (!at(Tok::RBracket)) {
        const Expr *e = parse_expr(1);
        if (!e) {
          return nullptr;
        }
        items.push_back(e);
        if (at(Tok::Comma)) {
          advance();
          continue;
        }
        break;
      }
      if (!expect(Tok::RBracket, "']'")) {
        return nullptr;
      }
      return arena_.create<ListLit>(arena_span(arena_, items), line);
    }
    case Tok::LBrace: {
      const int line = advance().line;
      std::vector<DictEntry> entries;
      while (!at(Tok::RBrace)) {
        const Expr *key = parse_expr(1);
        if (!key) {
          return nullptr;
        }
        if (!expect(Tok::Colon, "':'")) {
          return nullptr;
        }
        const Expr *value = parse_expr(1);
        if (!value) {
          return nullptr;
        }
        entries.push_back(DictEntry{key, value});
        if (at(Tok::Comma)) {
          advance();
          continue;
        }
        break;
      }
      if (!expect(Tok::RBrace, "'}'")) {
        return nullptr;
      }
      return arena_.create<DictLit>(arena_span(arena_, entries), line);
    }
    default:
      break;
    }
    fail_here("表达式");
    return nullptr;
  }

  std::span<const Token> toks_;
  std::string_view filename_; // 诊断定位用（可为空）
  vx::util::Arena &arena_;
  DiagBag &bag_;              // 共享诊断收集器
  std::size_t pos_ = 0;
};

} // namespace

std::string_view node_kind_name(NodeKind kind) {
  switch (kind) {
  case NodeKind::IntLit:
    return "IntLit";
  case NodeKind::FloatLit:
    return "FloatLit";
  case NodeKind::StrLit:
    return "StrLit";
  case NodeKind::BoolLit:
    return "BoolLit";
  case NodeKind::NilLit:
    return "NilLit";
  case NodeKind::Ident:
    return "Ident";
  case NodeKind::ListLit:
    return "ListLit";
  case NodeKind::DictLit:
    return "DictLit";
  case NodeKind::Index:
    return "Index";
  case NodeKind::Property:
    return "Property";
  case NodeKind::Call:
    return "Call";
  case NodeKind::New:
    return "New";
  case NodeKind::Unary:
    return "Unary";
  case NodeKind::Binary:
    return "Binary";
  case NodeKind::ExprStmt:
    return "ExprStmt";
  case NodeKind::VarDecl:
    return "VarDecl";
  case NodeKind::Assign:
    return "Assign";
  case NodeKind::FuncDecl:
    return "FuncDecl";
  case NodeKind::ExternDecl:
    return "ExternDecl";
  case NodeKind::Import:
    return "Import";
  case NodeKind::Export:
    return "Export";
  case NodeKind::Return:
    return "Return";
  case NodeKind::Break:
    return "Break";
  case NodeKind::Continue:
    return "Continue";
  case NodeKind::If:
    return "If";
  case NodeKind::While:
    return "While";
  case NodeKind::For:
    return "For";
  case NodeKind::ClassDecl:
    return "ClassDecl";
  case NodeKind::StructDecl:
    return "StructDecl";
  case NodeKind::ImpDecl:
    return "ImpDecl";
  case NodeKind::CppImpDecl:
    return "CppImpDecl";
  }
  return "?";
}

std::optional<Program> parse(std::span<const Token> tokens,
                             std::string_view filename,
                             vx::util::Arena &arena, DiagBag &bag) {
  if (tokens.empty() || tokens.back().kind != Tok::Eof) {
    bag.add(make_diag(diag_code::parse_invalid_token_stream, Severity::Error,
                      "token 流必须以 Eof 结尾", std::string(filename), 0, 0));
    return std::nullopt;
  }
  Parser p(tokens, filename, arena, bag);
  return p.run();
}

} // namespace vx::compiler
