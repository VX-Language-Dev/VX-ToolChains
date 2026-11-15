// parser_expr.hpp — 表达式解析实现（内部实现头，仅供 parser.cpp 使用）
#pragma once

#include "parser_decl.hpp"

namespace vx::compiler {

namespace {

// ===== 表达式 =====
inline const Expr* Parser::parse_expr(int min_prec) {
  const Expr* left = parse_unary();
  if (left == nullptr) {
    return nullptr;
  }
  while (true) {
    const Token& t = peek();
    const int prec = binary_precedence(t);
    if (prec == 0 || prec < min_prec) {
      return left;
    }
    const std::string_view op = t.text;
    const int line = t.line;
    advance();
    const Expr* right = parse_expr(prec + 1);
    if (right == nullptr) {
      return nullptr;
    }
    left = arena_.create<Binary>(op, left, right, line);
  }
}

inline const Expr* Parser::parse_unary() {
  const Token& t = peek();
  if (t.kind == Tok::Star) {
    advance();
    const Expr* operand = parse_unary();
    if (operand == nullptr) {
      return nullptr;
    }
    return arena_.create<Deref>(operand, t.line);
  }
  if (t.kind == Tok::Amp) {
    advance();
    std::string_view name;
    if (!parse_name(name)) {
      return nullptr;
    }
    return arena_.create<FuncRef>(name, t.line);
  }
  if (t.kind == Tok::Minus) {
    advance();
    const Expr* operand = parse_unary();
    if (operand == nullptr) {
      return nullptr;
    }
    return arena_.create<Unary>("-", operand, t.line);
  }
  if (t.kind == Tok::Not || (t.kind == Tok::Kw && t.text == "not")) {
    advance();
    const Expr* operand = parse_unary();
    if (operand == nullptr) {
      return nullptr;
    }
    return arena_.create<Unary>("!", operand, t.line);
  }
  return parse_postfix();
}

inline const Expr* Parser::parse_postfix() {
  const Expr* e = parse_primary();
  if (e == nullptr) {
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
        std::vector<const Expr*> args;
        if (!parse_args(args)) {
          return nullptr;
        }
        const auto* prop = arena_.create<Property>(e, name, line);
        e = arena_.create<Call>(prop, arena_span(arena_, args), line);
      } else {
        e = arena_.create<Property>(e, name, line);
      }
      continue;
    }
    if (at(Tok::LParen)) {
      const int line = peek().line;
      std::vector<const Expr*> args;
      if (!parse_args(args)) {
        return nullptr;
      }
      e = arena_.create<Call>(e, arena_span(arena_, args), line);
      continue;
    }
    if (at(Tok::LBracket)) {
      const int line = advance().line;
      const Expr* index = parse_expr(1);
      if (index == nullptr) {
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

inline bool Parser::parse_args(std::vector<const Expr*>& out) {
  advance(); // '('
  while (!at(Tok::RParen)) {
    const Expr* a = parse_expr(1);
    if (a == nullptr) {
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

inline const Expr* Parser::parse_primary() {
  const Token& t = peek();
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
        std::vector<const Expr*> args;
        if (at(Tok::LParen) && (!parse_args(args))) {
          return nullptr;
        }

        return arena_.create<New>(name, arena_span(arena_, args), t.line);
      }
      if (t.text == "int" || t.text == "float" || t.text == "bool") {
        // 类型名关键字可作显式转换调用：int(x) / float(x) / bool(x)
        return arena_.create<Ident>(advance().text, t.line);
      }
      if (is_soft_kw(t.text)) {
        return arena_.create<Ident>(advance().text, t.line);
      }
      break;
    case Tok::LParen: {
      advance();
      const Expr* e = parse_expr(1);
      if (e == nullptr) {
        return nullptr;
      }
      if (!expect(Tok::RParen, "')'")) {
        return nullptr;
      }
      return e;
    }
    case Tok::LBracket: {
      const int line = advance().line;
      std::vector<const Expr*> items;
      while (!at(Tok::RBracket)) {
        const Expr* e = parse_expr(1);
        if (e == nullptr) {
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
        const Expr* key = parse_expr(1);
        if (key == nullptr) {
          return nullptr;
        }
        if (!expect(Tok::Colon, "':'")) {
          return nullptr;
        }
        const Expr* value = parse_expr(1);
        if (value == nullptr) {
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

} // namespace

} // namespace vx::compiler
