// analysis_text.hpp — 文本工具与符号收集（内部实现头，仅供 analysis.cpp 使用）
#pragma once

#include "analysis.hpp"
#include "tidy.hpp"
#include "vx/compiler/importer.hpp"
#include "vx/compiler/lexer.hpp"
#include "vx/compiler/parser.hpp"

#include <algorithm>
#include <cctype>
#include <climits>
#include <functional>
#include <map>
#include <optional>
#include <ranges>
#include <utility>

namespace vxls {

using vx::compiler::Diagnostic;
using vx::compiler::EnumMember;
using vx::compiler::NodeKind;
using vx::compiler::Severity;
using vx::compiler::Stmt;
using vx::compiler::Tok;
using vx::compiler::Token;

namespace {

namespace {

// UTF-8 码点计数（列号语义：1 起始，按码点）
std::size_t utf8_len(std::string_view s) {
  std::size_t n = 0;
  for (char c : s) {
    if ((static_cast<unsigned char>(c) & 0xC0u) != 0x80u)
      ++n;
  }
  return n;
}

// UTF-8 码点序列 → UTF-16 码元数（LSP character 列语义）
std::size_t utf16_len(std::string_view s) {
  std::size_t n = 0;
  for (char c : s) {
    const auto u = static_cast<unsigned char>(c);
    if ((u & 0xC0u) == 0x80u)
      continue;                // 续字节
    n += (u >= 0xF0u) ? 2 : 1; // 4 字节码点 = 代理对 2 码元
  }
  return n;
}

// 取第 line 行（1 起始）的文本（不含换行）
std::string_view line_text(std::string_view text, int line) {
  std::size_t start = 0;
  int cur = 1;
  while (cur < line) {
    const std::size_t nl = text.find('\n', start);
    if (nl == std::string_view::npos)
      return {};
    start = nl + 1;
    ++cur;
  }
  const std::size_t nl = text.find('\n', start);
  std::string_view l = text.substr(start, nl == std::string_view::npos ? std::string_view::npos : nl - start);
  if (!l.empty() && l.back() == '\r')
    l.remove_suffix(1);
  return l;
}

int count_lines(std::string_view text) {
  int n = 1;
  for (char c : text) {
    if (c == '\n')
      ++n;
  }
  return n;
}

// 声明签名文本（悬停/符号 detail 用）
std::string func_signature(std::string_view keyword, std::string_view name, std::span<const vx::compiler::Param> params,
                           std::string_view ret) {
  std::string s = std::string(keyword) + " " + std::string(name) + "(";
  for (std::size_t i = 0; i < params.size(); ++i) {
    if (i != 0U)
      s += ", ";
    s += std::string(params[i].name);
    if (!params[i].type.empty())
      s += ": " + std::string(params[i].type);
  }
  s += ")";
  if (!ret.empty() && ret != "void")
    s += " -> " + std::string(ret);
  return s;
}

// 语句 → 符号（顶层与类方法均可）
void collect_symbol(const Stmt* s, std::string_view text, std::vector<LspSymbol>& out, bool in_class = false) {
  if (s == nullptr)
    return;
  auto add = [&](std::string_view name, std::string_view detail, int kind, int line, std::size_t name_col) {
    LspSymbol sym;
    sym.name = std::string(name);
    sym.detail = std::string(detail);
    sym.kind = kind;
    sym.range = {{line - 1, 0}, {line, 0}};
    sym.selection_range = {
        {line - 1, static_cast<int>(name_col)},
        {line - 1, static_cast<int>(name_col + utf16_len(name))},
    };
    out.push_back(std::move(sym));
    (void)text;
  };
  // 名称起始列：在该行中从声明关键字后搜索名称
  auto name_col_of = [&](int line, std::string_view prefix, std::string_view name) -> std::size_t {
    const std::string_view l = line_text(text, line);
    const std::size_t p = l.find(prefix);
    if (p == std::string_view::npos)
      return 0;
    const std::size_t n = l.find(name, p + prefix.size());
    return n == std::string_view::npos ? 0 : utf16_len(l.substr(0, n));
  };

  switch (s->kind) {
    case NodeKind::FuncDecl: {
      const auto* f = static_cast<const vx::compiler::FuncDecl*>(s);
      const char* kw = "func";
      add(f->name, func_signature(kw, f->name, f->params, f->ret_type), in_class ? 6 /*Method*/ : 12 /*Function*/,
          f->line, name_col_of(f->line, "func", f->name));
      break;
    }
    case NodeKind::ExternDecl: {
      const auto* e = static_cast<const vx::compiler::ExternDecl*>(s);
      add(e->name, func_signature("extern", e->name, e->params, e->ret_type), 12, e->line,
          name_col_of(e->line, "extern", e->name));
      break;
    }
    case NodeKind::ClassDecl: {
      const auto* c = static_cast<const vx::compiler::ClassDecl*>(s);
      add(c->name, "class " + std::string(c->name), 5 /*Class*/, c->line, name_col_of(c->line, "class", c->name));
      for (const auto* m : c->methods) {
        collect_symbol(m, text, out, true);
      }
      break;
    }
    case NodeKind::StructDecl: {
      const auto* st = static_cast<const vx::compiler::StructDecl*>(s);
      add(st->name, "struct " + std::string(st->name), 23 /*Struct*/, st->line,
          name_col_of(st->line, "struct", st->name));
      break;
    }
    case NodeKind::EnumDecl: {
      const auto* en = static_cast<const vx::compiler::EnumDecl*>(s);
      add(en->name, "enum " + std::string(en->name), 13 /*Enum*/, en->line, name_col_of(en->line, "enum", en->name));
      for (const EnumMember& m : en->members) {
        add(m.name, std::string(en->name) + "." + std::string(m.name) + " = " + std::to_string(m.value),
            20 /*EnumMember*/, m.line, name_col_of(m.line, std::string(m.name), m.name));
      }
      break;
    }
    case NodeKind::VarDecl: {
      const auto* v = static_cast<const vx::compiler::VarDecl*>(s);
      std::string detail = std::string(v->is_mut ? "mut " : "var ");
      detail += std::string(v->name);
      if (!v->type_ann.empty())
        detail += ": " + std::string(v->type_ann);
      add(v->name, detail, 13 /*Variable*/, v->line, name_col_of(v->line, v->is_mut ? "mut" : "var", v->name));
      break;
    }
    case NodeKind::Import: {
      const auto* im = static_cast<const vx::compiler::Import*>(s);
      add(im->path, "import " + std::string(im->path), 2 /*Module*/, im->line,
          name_col_of(im->line, "import", im->path));
      break;
    }
    case NodeKind::ImpDecl: {
      const auto* d = static_cast<const vx::compiler::ImpDecl*>(s);
      add(d->lib, "@imp(" + std::string(d->lib) + ")", 2 /*Module*/, d->line, 0);
      break;
    }
    case NodeKind::CppImpDecl: {
      const auto* d = static_cast<const vx::compiler::CppImpDecl*>(s);
      add(d->header, "@cppimp(" + std::string(d->header) + ")", 2, d->line, 0);
      break;
    }
    default:
      break;
  }
}

} // namespace

} // namespace

} // namespace vxls
