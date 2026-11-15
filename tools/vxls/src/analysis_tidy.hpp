// analysis_tidy.hpp — 跨文件语义实现（工作区符号 / 跨文件定义 / 重命名引用）（内部实现头，仅供 analysis.cpp 使用）
#pragma once

#include "analysis.hpp"
#include "analysis_completion.hpp"
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

// ---- 跨文件语义实现 ----

namespace {

// 大小写不敏感子串匹配
bool contains_ci(std::string_view haystack, std::string_view needle) {
  if (needle.empty())
    return true;
  std::string h(haystack);
  std::string n(needle);
  std::ranges::transform(h, h.begin(), [](unsigned char c) { return std::tolower(c); });
  std::ranges::transform(n, n.begin(), [](unsigned char c) { return std::tolower(c); });
  return h.contains(n);
}

// 在单个文档中查找名为 name 的顶层声明位置（函数/类/结构/变量/extern）
// 返回 start.line>=0 表示找到
LspRange find_decl_range_in_doc(std::string_view text, std::string_view name) {
  LspRange bad{{-1, 0}, {-1, 0}};
  vx::util::Arena arena;
  vx::compiler::DiagBag bag;
  auto toks = vx::compiler::tokenize(text, "<document>", arena, bag);
  auto prog = vx::compiler::parse(toks, "<document>", arena, bag);
  if (!prog)
    return bad;

  // 遍历 AST 找匹配声明
  std::function<bool(const vx::compiler::Stmt*)> walk = nullptr;
  walk = [&](const vx::compiler::Stmt* s) -> bool {
    if (s == nullptr)
      return false;
    using namespace vx::compiler;
    switch (s->kind) {
      case NodeKind::FuncDecl: {
        const auto* f = static_cast<const FuncDecl*>(s);
        if (f->name == name) {
          // 计算 name_col
          const std::string_view l = line_text(text, f->line);
          std::size_t pos = l.find(f->name);
          if (pos == std::string_view::npos)
            pos = 0;
          LspRange r;
          r.start = {f->line - 1, static_cast<int>(utf16_len(l.substr(0, pos)))};
          r.end = {f->line - 1, static_cast<int>(utf16_len(l.substr(0, pos)) + utf16_len(name))};
          // store via out param hack: use a static? Instead return via exception-like
          // We'll handle below by setting a captured variable.
          return true;
        }
        for (const Stmt* st : f->body) {
          if (walk(st))
            return true;
        }
        return false;
      }
      case NodeKind::ExternDecl: {
        const auto* e = static_cast<const ExternDecl*>(s);
        return e->name == name;
      }
      case NodeKind::ClassDecl: {
        const auto* c = static_cast<const ClassDecl*>(s);
        if (c->name == name)
          return true;
        for (const FuncDecl* m : c->methods) {
          if (m->name == name)
            return true;
        }
        return false;
      }
      case NodeKind::StructDecl: {
        const auto* st = static_cast<const StructDecl*>(s);
        return st->name == name;
      }
      case NodeKind::EnumDecl: {
        const auto* en = static_cast<const EnumDecl*>(s);
        if (en->name == name)
          return true;
        for (const EnumMember& m : en->members) {
          if (m.name == name)
            return true;
        }
        return false;
      }
      case NodeKind::VarDecl: {
        const auto* v = static_cast<const VarDecl*>(s);
        return v->name == name;
      }
      case NodeKind::If: {
        const auto* i = static_cast<const If*>(s);
        for (const Stmt* st : i->then_body)
          if (walk(st))
            return true;
        for (const ElifArm& arm : i->elifs)
          for (const Stmt* st : arm.body)
            if (walk(st))
              return true;
        for (const Stmt* st : i->else_body)
          if (walk(st))
            return true;
        return false;
      }
      case NodeKind::While: {
        const auto* w = static_cast<const While*>(s);
        for (const Stmt* st : w->body)
          if (walk(st))
            return true;
        return false;
      }
      case NodeKind::For: {
        const auto* f = static_cast<const For*>(s);
        if (f->var == name)
          return true;
        for (const Stmt* st : f->body)
          if (walk(st))
            return true;
        return false;
      }
      default:
        return false;
    }
  };

  // We need the actual range. Re-walk to capture the declaration node.
  // Simpler: do a second pass storing the matched decl line.
  int matched_line = -1;
  std::string matched_kind;
  std::function<void(const vx::compiler::Stmt*)> find = nullptr;
  find = [&](const vx::compiler::Stmt* s) {
    if (s == nullptr || matched_line >= 0)
      return;
    using namespace vx::compiler;
    switch (s->kind) {
      case NodeKind::FuncDecl: {
        const auto* f = static_cast<const FuncDecl*>(s);
        if (f->name == name) {
          matched_line = f->line;
          matched_kind = "func";
          return;
        }
        for (const Stmt* st : f->body)
          find(st);
        break;
      }
      case NodeKind::ExternDecl: {
        const auto* e = static_cast<const ExternDecl*>(s);
        if (e->name == name) {
          matched_line = e->line;
          matched_kind = "extern";
          return;
        }
        break;
      }
      case NodeKind::ClassDecl: {
        const auto* c = static_cast<const ClassDecl*>(s);
        if (c->name == name) {
          matched_line = c->line;
          matched_kind = "class";
          return;
        }
        for (const FuncDecl* m : c->methods)
          if (m->name == name) {
            matched_line = m->line;
            matched_kind = "method";
            return;
          }
        break;
      }
      case NodeKind::StructDecl: {
        const auto* st = static_cast<const StructDecl*>(s);
        if (st->name == name) {
          matched_line = st->line;
          matched_kind = "struct";
          return;
        }
        break;
      }
      case NodeKind::EnumDecl: {
        const auto* en = static_cast<const EnumDecl*>(s);
        if (en->name == name) {
          matched_line = en->line;
          matched_kind = "enum";
          return;
        }
        for (const EnumMember& m : en->members) {
          if (m.name == name) {
            matched_line = m.line;
            matched_kind = "enumMember";
            return;
          }
        }
        break;
      }
      case NodeKind::VarDecl: {
        const auto* v = static_cast<const VarDecl*>(s);
        if (v->name == name) {
          matched_line = v->line;
          matched_kind = "var";
          return;
        }
        break;
      }
      case NodeKind::If: {
        const auto* i = static_cast<const If*>(s);
        for (const Stmt* st : i->then_body)
          find(st);
        for (const ElifArm& arm : i->elifs)
          for (const Stmt* st : arm.body)
            find(st);
        for (const Stmt* st : i->else_body)
          find(st);
        break;
      }
      case NodeKind::While: {
        const auto* w = static_cast<const While*>(s);
        for (const Stmt* st : w->body)
          find(st);
        break;
      }
      case NodeKind::For: {
        const auto* f = static_cast<const For*>(s);
        if (f->var == name) {
          matched_line = f->line;
          matched_kind = "var";
          return;
        }
        for (const Stmt* st : f->body)
          find(st);
        break;
      }
      default:
        break;
    }
  };
  for (const Stmt* s : prog->stmts)
    find(s);
  if (matched_line < 0)
    return bad;

  const std::string_view l = line_text(text, matched_line);
  // find name position in line
  std::size_t pos = l.find(name);
  if (pos == std::string_view::npos)
    pos = 0;
  // Adjust: skip keyword prefix if present (e.g., "func add")
  // Try to find after a keyword
  for (const char* kw : {"func", "extern", "class", "struct", "var", "mut"}) {
    std::string_view k(kw);
    std::size_t kp = l.find(k);
    if (kp != std::string_view::npos) {
      std::size_t np = l.find(name, kp + k.size());
      if (np != std::string_view::npos) {
        pos = np;
        break;
      }
    }
  }
  LspRange r;
  r.start = {matched_line - 1, static_cast<int>(utf16_len(l.substr(0, pos)))};
  r.end = {matched_line - 1, static_cast<int>(utf16_len(l.substr(0, pos)) + utf16_len(name))};
  return r;
}

// 收集文档中所有名为 name 的标识符引用位置
std::vector<LspRange> find_ident_refs_in_doc(std::string_view text, std::string_view name) {
  std::vector<LspRange> out;
  vx::util::Arena arena;
  vx::compiler::DiagBag bag;
  auto toks = vx::compiler::tokenize(text, "<document>", arena, bag);
  for (const auto& t : toks) {
    if (t.kind != vx::compiler::Tok::Ident && t.kind != vx::compiler::Tok::Kw)
      continue;
    if (t.text != name)
      continue;
    LspRange r;
    r.start = diag_pos_to_lsp(text, t.line, t.col);
    r.end = diag_pos_to_lsp(text, t.line, t.col + static_cast<int>(utf8_len(t.text)));
    out.push_back(r);
  }
  return out;
}

} // namespace

} // namespace

} // namespace vxls
