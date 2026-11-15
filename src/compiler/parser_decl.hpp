// parser_decl.hpp — Parser 类声明（内部实现头，仅供 parser.cpp 使用）
#pragma once

#include "parser_util.hpp"
#include "vx/compiler/parser.hpp"

#include <cstdio>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace vx::compiler {

namespace {

class Parser {
public:
  Parser(std::span<const Token> toks, std::string_view filename, vx::util::Arena& arena, DiagBag& bag)
      : toks_(toks), filename_(filename), arena_(arena), bag_(bag) {}

  std::optional<Program> run();

  // 跳到下一语句边界（Nl / Semi / Dedent / Eof），用于错误恢复重同步。
  // 不越过 Dedent/Eof，以免跨越块边界导致更多级联错误。
  void skip_to_stmt_boundary() {
    while (!at(Tok::Eof) && !at(Tok::Nl) && !at(Tok::Semi) && !at(Tok::Dedent)) {
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
  const Token& peek() const { return toks_[pos_]; }

  const Token& advance() { return toks_[pos_++]; }

  bool at(Tok kind) const { return peek().kind == kind; }

  bool at_kw(std::string_view name) const { return at(Tok::Kw) && peek().text == name; }

  // 语法错误（fail-fast）：推入 bag，由上层据此停止解析。
  bool fail_span(std::string message, int line, int col, int end_line, int end_col) {
    bag_.add(make_diag_span(diag_code::parse_expected_token, Severity::Error, std::move(message),
                            std::string(filename_), line, col, end_line, end_col));
    return false;
  }

  bool fail_here(std::string what) {
    const Token& t = peek();
    char buf[128];
    std::snprintf(buf, sizeof(buf), "期望%s，但得到 %s '%.*s' (行 %d)", what.c_str(),
                  std::string(tok_name(t.kind)).c_str(), static_cast<int>(t.text.size()), t.text.data(), t.line);
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
  bool parse_name(std::string_view& out) {
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
  const Stmt* parse_stmt();

  const Stmt* parse_var_decl();

  const Stmt* parse_func_decl();

  bool parse_param_list(std::vector<Param>& out);

  const Stmt* parse_extern_decl();

  const Stmt* parse_import();

  // 导入注解：@imp(<库引用>) [as alias] / @cppimp(<头文件路径>) [as alias]
  // 参数文本由若干 token 无分隔拼接而成（路径中的 '/' '.' '-' 均为独立 token）。
  // 导入注解：@imp(<库引用>) [as alias] / @cppimp(<头文件路径>) [as alias]
  // 参数文本由若干 token 无分隔拼接而成（路径中的 '/' '.' '-' 均为独立 token）。
  const Stmt* parse_at_directive();

  // 读取括号内的库引用/路径文本：字符串字面量直接取解码值；
  // 否则无缝拼接 Ident/Kw/Int 与 '/' '.' '-' 直到右括号。
  // 读取括号内的库引用/路径文本：字符串字面量直接取解码值；
  // 否则无缝拼接 Ident/Kw/Int 与 '/' '.' '-' 直到右括号。
  bool read_lib_ref(std::string& out);

  const Stmt* parse_export();

  const Stmt* parse_return();

  const Stmt* parse_if();

  // 前瞻（跳过 Nl）判断下一有效 token 是否为指定关键字；不消费
  bool peek_past_nl_is_kw(std::string_view name) const {
    std::size_t i = pos_;
    while (i < toks_.size() && toks_[i].kind == Tok::Nl) {
      ++i;
    }
    return i < toks_.size() && toks_[i].kind == Tok::Kw && toks_[i].text == name;
  }

  const Stmt* parse_while();

  const Stmt* parse_for();

  const Stmt* parse_class();

  const Stmt* parse_struct();

  // enum Name: 每行 MEMBER [= <整数字面量>]；缺省取上一成员 + 1（首个 0）
  const Stmt* parse_enum();

  // 块：INDENT 多语句，或 ':' 后同行语句列表（Semi 分隔）
  bool parse_block(std::vector<const Stmt*>& out);

  // 表达式语句或赋值语句
  const Stmt* parse_expr_stmt();

  // ===== 表达式 =====
  const Expr* parse_expr(int min_prec);

  const Expr* parse_unary();

  const Expr* parse_postfix();

  bool parse_args(std::vector<const Expr*>& out);

  const Expr* parse_primary();

  std::span<const Token> toks_;
  std::string_view filename_; // 诊断定位用（可为空）
  vx::util::Arena& arena_;
  DiagBag& bag_; // 共享诊断收集器
  std::size_t pos_ = 0;
};

} // namespace

} // namespace vx::compiler
