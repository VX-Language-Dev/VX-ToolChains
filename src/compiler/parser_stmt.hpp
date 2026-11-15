// parser_stmt.hpp — 语句级解析实现（内部实现头，仅供 parser.cpp 使用）
#pragma once

#include "parser_decl.hpp"

namespace vx::compiler {

namespace {

inline std::optional<Program> Parser::run() {
  std::vector<const Stmt*> stmts;
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
    const Stmt* s = parse_stmt();
    if (s == nullptr) {
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

// ===== 语句 =====
inline const Stmt* Parser::parse_stmt() {
  const Token& t = peek();
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
    if (t.text == "enum") {
      return parse_enum();
    }
    if (t.text == "pass") {
      // 空语句：用 nil 表达式语句占位，便于块内出现纯粹的 no-op
      advance();
      const Expr* nil = arena_.create<NilLit>(t.line);
      return arena_.create<ExprStmt>(nil, t.line);
    }
  }
  return parse_expr_stmt();
}

inline const Stmt* Parser::parse_var_decl() {
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
  const Expr* init = nullptr;
  if (at(Tok::Assign)) {
    advance();
    init = parse_expr(1);
    if (init == nullptr) {
      return nullptr;
    }
  }
  return arena_.create<VarDecl>(name, type_ann, init, is_mut, line);
}

inline const Stmt* Parser::parse_func_decl() {
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
  std::vector<const Stmt*> body;
  if (!parse_block(body)) {
    return nullptr;
  }
  return arena_.create<FuncDecl>(name, arena_span(arena_, params), ret_type, arena_span(arena_, body), line);
}

inline bool Parser::parse_param_list(std::vector<Param>& out) {
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

inline const Stmt* Parser::parse_extern_decl() {
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
  return arena_.create<ExternDecl>(name, arena_span(arena_, params), ret_type, line);
}

inline const Stmt* Parser::parse_import() {
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
  std::string_view alias;
  if (at_kw("as")) {
    advance();
    if (!parse_name(alias)) {
      return nullptr;
    }
  }
  auto* stored = arena_.create<std::string>(std::move(full));
  return arena_.create<Import>(*stored, alias, line);
}

// 导入注解：@imp(<库引用>) [as alias] / @cppimp(<头文件路径>) [as alias]
// 参数文本由若干 token 无分隔拼接而成（路径中的 '/' '.' '-' 均为独立 token）。
inline const Stmt* Parser::parse_at_directive() {
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
  auto* stored = arena_.create<std::string>(std::move(ref));
  if (name == "imp") {
    return arena_.create<ImpDecl>(*stored, alias, line);
  }
  return arena_.create<CppImpDecl>(*stored, alias, line);
}

// 读取括号内的库引用/路径文本：字符串字面量直接取解码值；
// 否则无缝拼接 Ident/Kw/Int 与 '/' '.' '-' 直到右括号。
inline bool Parser::read_lib_ref(std::string& out) {
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

inline const Stmt* Parser::parse_export() {
  const int line = peek().line;
  advance(); // 'export'
  std::string_view name;
  if (!parse_name(name)) {
    return nullptr;
  }
  return arena_.create<Export>(name, line);
}

inline const Stmt* Parser::parse_return() {
  const int line = peek().line;
  advance(); // 'return'
  const Expr* value = nullptr;
  if (!at(Tok::Nl) && !at(Tok::Semi) && !at(Tok::Dedent) && !at(Tok::Eof)) {
    value = parse_expr(1);
    if (value == nullptr) {
      return nullptr;
    }
  }
  return arena_.create<Return>(value, line);
}

inline const Stmt* Parser::parse_if() {
  const int line = peek().line;
  advance(); // 'if'
  const Expr* cond = parse_expr(1);
  if (cond == nullptr) {
    return nullptr;
  }
  if (!expect(Tok::Colon, "':'")) {
    return nullptr;
  }
  std::vector<const Stmt*> then_body;
  if (!parse_block(then_body)) {
    return nullptr;
  }
  std::vector<ElifArm> elifs;
  while (peek_past_nl_is_kw("elif")) {
    skip_nls();
    advance(); // 'elif'
    const Expr* econd = parse_expr(1);
    if (econd == nullptr) {
      return nullptr;
    }
    if (!expect(Tok::Colon, "':'")) {
      return nullptr;
    }
    std::vector<const Stmt*> ebody;
    if (!parse_block(ebody)) {
      return nullptr;
    }
    elifs.push_back(ElifArm{econd, arena_span(arena_, ebody)});
  }
  std::vector<const Stmt*> else_body;
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
  return arena_.create<If>(cond, arena_span(arena_, then_body), arena_span(arena_, elifs),
                           arena_span(arena_, else_body), line);
}

inline const Stmt* Parser::parse_while() {
  const int line = peek().line;
  advance(); // 'while'
  const Expr* cond = parse_expr(1);
  if (cond == nullptr) {
    return nullptr;
  }
  if (!expect(Tok::Colon, "':'")) {
    return nullptr;
  }
  std::vector<const Stmt*> body;
  if (!parse_block(body)) {
    return nullptr;
  }
  return arena_.create<While>(cond, arena_span(arena_, body), line);
}

inline const Stmt* Parser::parse_for() {
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
  const Expr* iterable = parse_expr(1);
  if (iterable == nullptr) {
    return nullptr;
  }
  if (!expect(Tok::Colon, "':'")) {
    return nullptr;
  }
  std::vector<const Stmt*> body;
  if (!parse_block(body)) {
    return nullptr;
  }
  return arena_.create<For>(var_name, iterable, arena_span(arena_, body), line);
}

inline const Stmt* Parser::parse_class() {
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
  std::vector<const FuncDecl*> methods;
  skip_nls();
  if (at(Tok::Indent)) {
    advance();
    while (true) {
      skip_nls();
      if (at(Tok::Dedent) || at(Tok::Eof)) {
        break;
      }
      if (at_kw("func")) {
        const Stmt* m = parse_func_decl();
        if (m == nullptr) {
          return nullptr;
        }
        methods.push_back(static_cast<const FuncDecl*>(m));
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
          const Expr* init = parse_expr(0);
          if (init == nullptr) {
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
  return arena_.create<ClassDecl>(name, arena_span(arena_, fields), arena_span(arena_, methods), line);
}

inline const Stmt* Parser::parse_struct() {
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

// enum Name: 每行 MEMBER [= <整数字面量>]；缺省取上一成员 + 1（首个 0）
inline const Stmt* Parser::parse_enum() {
  const int line = peek().line;
  advance(); // 'enum'
  std::string_view name;
  if (!parse_name(name)) {
    return nullptr;
  }
  if (!expect(Tok::Colon, "':'")) {
    return nullptr;
  }
  std::vector<EnumMember> members;
  std::int64_t next_value = 0;
  skip_nls();
  if (at(Tok::Indent)) {
    advance();
    while (true) {
      skip_nls();
      if (at(Tok::Dedent) || at(Tok::Eof)) {
        break;
      }
      const int mline = peek().line;
      std::string_view mname;
      if (!parse_name(mname)) {
        return nullptr;
      }
      std::int64_t value = next_value;
      if (at(Tok::Assign)) {
        advance();
        bool negative = false;
        if (at(Tok::Minus)) { // 负值：仅允许 -<整数字面量>
          negative = true;
          advance();
        }
        if (!at(Tok::Int)) {
          // 记录诊断后吃到行尾，保持块结构完整（避免级联错误）
          fail_span("枚举成员取值须为整数字面量", peek().line, peek().col, peek().line,
                    peek().col + static_cast<int>(utf8_len(peek().text)));
          skip_to_stmt_boundary();
        } else {
          const std::string_view lit = advance().text;
          std::int64_t parsed = 0;
          bool overflow = false;
          for (char c : lit) {
            if (c < '0' || c > '9' || parsed > (std::int64_t{1} << 40)) {
              overflow = true;
              break;
            }
            parsed = parsed * 10 + (c - '0');
          }
          if (overflow) {
            fail_span("枚举成员取值须为受支持的十进制整数字面量", peek().line, peek().col, peek().line,
                      peek().col + static_cast<int>(utf8_len(peek().text)));
            skip_to_stmt_boundary();
          } else {
            value = negative ? -parsed : parsed;
          }
        }
      }
      next_value = value + 1;
      members.push_back(EnumMember{mname, value, mline});
      if (!end_stmt()) {
        return nullptr;
      }
    }
    if (at(Tok::Dedent)) {
      advance();
    }
  }
  return arena_.create<EnumDecl>(name, arena_span(arena_, members), line);
}

// 块：INDENT 多语句，或 ':' 后同行语句列表（Semi 分隔）
inline bool Parser::parse_block(std::vector<const Stmt*>& out) {
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
        const Stmt* s = parse_stmt();
        if (s == nullptr) {
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
    const Stmt* s = parse_stmt();
    if (s == nullptr) {
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
inline const Stmt* Parser::parse_expr_stmt() {
  const int line = peek().line;
  const Expr* e = parse_expr(1);
  if (e == nullptr) {
    return nullptr;
  }
  if (is_assign_op(peek().kind)) {
    const std::string_view op = advance().text;
    const Expr* value = parse_expr(1);
    if (value == nullptr) {
      return nullptr;
    }
    return arena_.create<Assign>(e, op, value, line);
  }
  return arena_.create<ExprStmt>(e, line);
}

} // namespace

} // namespace vx::compiler
