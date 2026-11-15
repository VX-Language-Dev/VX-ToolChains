// parser_util.hpp — 解析器辅助函数（内部实现头，仅供 parser.cpp 使用）
#pragma once

#include "vx/compiler/parser.hpp"

#include <new>
#include <span>
#include <string_view>
#include <vector>

namespace vx::compiler {

namespace {

// 二元运算符优先级
int binary_precedence(const Token& t) {
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
  return text == "loop" || text == "match" || text == "enum" || text == "union" || text == "macro" || text == "move";
}

// 字符串的 Unicode 码点个数（UTF-8 续字节 10xxxxxx 不计入）
std::size_t utf8_len(std::string_view s) {
  std::size_t n = 0;
  for (char c : s) {
    if ((static_cast<unsigned char>(c) & 0xC0u) != 0x80u) {
      ++n;
    }
  }
  return n;
}

template <class T> std::span<const T> arena_span(vx::util::Arena& a, const std::vector<T>& v) {
  if (v.empty()) {
    return {};
  }
  T* p = static_cast<T*>(a.allocate(sizeof(T) * v.size(), alignof(T)));
  for (std::size_t i = 0; i < v.size(); ++i) {
    ::new (static_cast<void*>(&p[i])) T(v[i]);
  }
  return std::span<const T>(p, v.size());
}

} // namespace

} // namespace vx::compiler
