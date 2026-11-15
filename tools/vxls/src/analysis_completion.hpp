// analysis_completion.hpp — 位置补全（内部实现头，仅供 analysis.cpp 使用）
#pragma once

#include "analysis.hpp"
#include "analysis_hover.hpp"
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

// ==== 位置补全（completion_at）====

namespace {

enum class CompCtx { General, Member, Type };

// 光标上下文：基于光标所在行文本的轻量判定（不依赖解析成功）
struct CursorContext {
  CompCtx kind = CompCtx::General;
  bool statement_pos = false; // 光标前仅有缩进（行首语句位置）
  std::string prefix;         // 光标前的标识符片段
  std::string receiver;       // Member：点前的接收者标识符
  std::size_t line_start = 0; // 当前行起点在全文中的字节偏移
  std::size_t byte = 0;       // 光标在当前行内的字节偏移
  int cursor_line = 1;        // 1 起始行号
};

bool comp_ident_char(char c) {
  const auto u = static_cast<unsigned char>(c);
  return std::isalnum(u) != 0 || c == '_' || u >= 0x80;
}

// UTF-16 列（LSP character）→ 行内 UTF-8 字节偏移
std::size_t utf16_to_byte(std::string_view line, int utf16_pos) {
  std::size_t i = 0;
  int units = 0;
  while (i < line.size() && units < utf16_pos) {
    const auto u = static_cast<unsigned char>(line[i]);
    if (u < 0x80) {
      i += 1;
      units += 1;
    } else if ((u & 0xE0u) == 0xC0u) {
      i += 2;
      units += 1;
    } else if ((u & 0xF0u) == 0xE0u) {
      i += 3;
      units += 1;
    } else {
      i += 4;
      units += 2;
    }
  }
  return i;
}

// 第 line1 行（1 起始）在全文中的字节起点
std::size_t line_start_of(std::string_view text, int line1) {
  std::size_t start = 0;
  int cur = 1;
  while (cur < line1) {
    const std::size_t nl = text.find('\n', start);
    if (nl == std::string_view::npos)
      return start;
    start = nl + 1;
    ++cur;
  }
  return start;
}

CursorContext detect_context(std::string_view text, int line0, int character) {
  CursorContext c;
  c.cursor_line = line0 + 1;
  c.line_start = line_start_of(text, line0 + 1);
  const std::string_view ln = line_text(text, line0 + 1);
  c.byte = std::min(utf16_to_byte(ln, character), ln.size());

  // 光标前的标识符片段（prefix）
  std::size_t ps = c.byte;
  while (ps > 0 && comp_ident_char(ln[ps - 1]))
    --ps;
  c.prefix = std::string(ln.substr(ps, c.byte - ps));

  std::string_view head = ln.substr(0, ps); // prefix 之前的文本
  while (!head.empty() && (head.back() == ' ' || head.back() == '\t' || head.back() == '\r'))
    head.remove_suffix(1);
  c.statement_pos = c.prefix.empty() && head.empty();

  // 成员上下文：head 以 '.' 结尾 → 接收者为点前标识符
  if (!head.empty() && head.back() == '.') {
    const std::size_t end = head.size() - 1;
    std::size_t beg = end;
    while (beg > 0 && comp_ident_char(head[beg - 1]))
      --beg;
    std::string recv(head.substr(beg, end - beg));
    if (!recv.empty()) {
      c.kind = CompCtx::Member;
      c.receiver = std::move(recv);
      return c;
    }
  }
  // 类型上下文①：箭头后（`func f() -> ` / `extern e() -> `）
  if (head.ends_with("->")) {
    c.kind = CompCtx::Type;
    return c;
  }
  // 类型上下文②：冒号判定。花括号内的冒号（字典字面量）忽略；
  // 括号内的冒号 = 形参注解；顶层冒号仅当 var/mut 声明（此前无 '='）
  // 或「单个非关键字标识符」（class/struct 字段注解）时为类型位置。
  int paren = 0;
  int brace = 0;
  std::size_t top_eq = std::string_view::npos;
  std::size_t colon = std::string_view::npos;
  int colon_paren = 0;
  for (std::size_t i = 0; i < head.size(); ++i) {
    const char ch = head[i];
    if (ch == '(') {
      ++paren;
    } else if (ch == ')') {
      --paren;
    } else if (ch == '{') {
      ++brace;
    } else if (ch == '}') {
      --brace;
    } else if (ch == '=' && paren == 0 && brace == 0 && top_eq == std::string_view::npos &&
               (i + 1 >= head.size() || head[i + 1] != '=') && (i <= 0 || head[i - 1] != '=')) {
      top_eq = i;
    } else if (ch == ':' && brace == 0) {
      colon = i;
      colon_paren = paren;
    }
  }
  if (colon != std::string_view::npos) {
    if (colon_paren > 0) {
      c.kind = CompCtx::Type; // 形参注解
      return c;
    }
    std::string_view decl = head.substr(0, colon);
    const std::size_t first = decl.find_first_not_of(" \t");
    if (first != std::string_view::npos) {
      decl.remove_prefix(first);
      const bool is_var = decl.starts_with("var ") || decl.starts_with("mut ") || decl == "var" || decl == "mut";
      if (is_var && (top_eq == std::string_view::npos || top_eq > colon)) {
        c.kind = CompCtx::Type; // var x: |
        return c;
      }
      // 单个非关键字标识符独占一行的冒号 → 字段注解（`x: |`）
      const bool bare_ident = !decl.empty() && std::ranges::all_of(decl, comp_ident_char);
      if (bare_ident && !vx::compiler::is_keyword(decl)) {
        c.kind = CompCtx::Type;
        return c;
      }
    }
  }
  return c;
}

// 可调用项 snippet：name(${1:a}, ${2:b})；无参 → name()
std::string snippet_from_params(std::string_view name, std::span<const vx::compiler::Param> params) {
  std::string s(name);
  s += "(";
  for (std::size_t i = 0; i < params.size(); ++i) {
    if (i != 0U)
      s += ", ";
    s += "${" + std::to_string(i + 1) + ":" + std::string(params[i].name) + "}";
  }
  s += ")";
  return s;
}

// 由 "name(a: int, b: int)" 形态的签名生成 snippet（内置运行时用）
std::string snippet_from_sig(std::string_view name, std::string_view sig) {
  const std::size_t open = sig.find('(');
  std::string s(name);
  s += "(";
  if (open != std::string_view::npos) {
    const std::size_t close = sig.find(')', open);
    const std::string inner =
        close == std::string_view::npos ? std::string() : std::string(sig.substr(open + 1, close - open - 1));
    int idx = 1;
    bool any = false;
    std::size_t start = 0;
    while (start <= inner.size()) {
      const std::size_t comma = inner.find(", ", start);
      const std::string param =
          comma == std::string_view::npos ? inner.substr(start) : inner.substr(start, comma - start);
      const std::size_t colon = param.find(':');
      const std::string pname = colon == std::string_view::npos ? param : param.substr(0, colon);
      if (!pname.empty()) {
        if (any)
          s += ", ";
        s += "${" + std::to_string(idx++) + ":" + pname + "}";
        any = true;
      }
      if (comma == std::string_view::npos)
        break;
      start = comma + 2;
    }
  }
  s += ")";
  return s;
}

// 关键字项（两种收集路径共用）：语句关键字在行首位置排最前
void emit_keyword_items(std::vector<LspCompletion>& items, bool stmt_pos) {
  static const char* kStmt[] = {
      "if",  "elif", "else", "for",    "while", "break",  "continue", "return",
      "var", "mut",  "func", "extern", "class", "struct", "import",   "export",
  };
  static const char* kExpr[] = {"true", "false", "nil", "and", "or", "not", "new", "as", "in", "const"};
  for (const char* kw : kStmt) {
    items.push_back({kw, 14 /*Keyword*/, "关键字", {}, {}, {}, stmt_pos ? "0" : "3", false});
  }
  for (const char* kw : kExpr) {
    items.push_back({kw, 14 /*Keyword*/, "关键字", {}, {}, {}, "3", false});
  }
  for (const char* d : {"@imp", "@cppimp"}) {
    items.push_back({d, 14 /*Keyword*/, "导入注解", {}, {}, {}, stmt_pos ? "0" : "3", false});
  }
}

// 内置运行时项（builtin_docs 表），带签名与文档
void emit_builtin_items(std::vector<LspCompletion>& items, bool snippet) {
  for (const auto& [name, doc] : builtin_docs()) {
    LspCompletion c;
    c.label = std::string(name);
    c.kind = 3 /*Function*/;
    c.detail = doc.sig;
    c.documentation = doc.desc;
    c.sort_text = "2";
    if (snippet) {
      c.is_snippet = true;
      c.insert_text = snippet_from_sig(name, doc.sig);
      c.filter_text = c.label;
    }
    items.push_back(std::move(c));
  }
}

// 解析失败时的降级路径：直接从 token 流粗提取声明（无作用域/无文档）
std::vector<LspCompletion> collect_fallback(std::string_view text, const CursorContext& ctx, bool snippet) {
  using vx::compiler::Tok;
  vx::util::Arena arena;
  vx::compiler::DiagBag bag;
  const auto toks = vx::compiler::tokenize(text, "<document>", arena, bag);

  std::vector<LspCompletion> items;
  const bool stmt_pos = ctx.statement_pos;

  struct Cls {
    std::string name;
    bool is_class = true;
    std::string detail;
    std::vector<std::pair<std::string, std::string>> fields;  // 名 → "x: int"
    std::vector<std::pair<std::string, std::string>> methods; // 名 → 签名
    std::vector<std::string> method_snippets;
  };
  std::vector<Cls> classes;
  Cls* cur = nullptr; // 当前所处的 class/struct（depth==1 内）
  int depth = 0;
  bool in_paren = false;

  auto tok_at = [&](std::size_t i) -> const Token* { return i < toks.size() ? &toks[i] : nullptr; };
  auto is_name = [](const Token* t) { return t != nullptr && (t->kind == Tok::Ident || t->kind == Tok::Kw); };

  // 解析形参列表：j 指向 '('；填充 params_text，返回 ')' 之后的索引
  auto scan_params = [&](std::size_t j, std::string& params_text, std::string& ret_text) -> std::size_t {
    std::string out;
    while (const Token* t = tok_at(j)) {
      if (t->kind == Tok::RParen) {
        ++j;
        break;
      }
      if (t->kind == Tok::Eof || t->kind == Tok::Nl)
        break;
      if (is_name(t)) {
        const std::string pname(t->text);
        std::string ptype;
        std::size_t nj = j + 1;
        if (const Token* c = tok_at(nj); c != nullptr && c->kind == Tok::Colon) {
          if (const Token* pt = tok_at(nj + 1); is_name(pt)) {
            ptype = std::string(pt->text);
            nj += 2;
          } else {
            nj += 1;
          }
        }
        if (!out.empty())
          out += ", ";
        out += pname;
        if (!ptype.empty())
          out += ": " + ptype;
        j = nj;
        continue;
      }
      ++j;
    }
    params_text = out;
    // 可选返回类型 `-> T`
    if (const Token* a = tok_at(j); a != nullptr && a->kind == Tok::Arrow) {
      if (const Token* rt = tok_at(j + 1); is_name(rt)) {
        ret_text = std::string(rt->text);
        j += 2;
      }
    }
    return j;
  };

  for (std::size_t i = 0; i < toks.size(); ++i) {
    const Token& t = toks[i];
    if (t.kind == Tok::Indent) {
      ++depth;
      continue;
    }
    if (t.kind == Tok::Dedent) {
      if (--depth <= 0) {
        depth = 0;
        cur = nullptr;
      }
      continue;
    }
    if (t.kind == Tok::LParen)
      in_paren = true;
    if (t.kind == Tok::RParen)
      in_paren = false;
    if (t.kind != Tok::Kw)
      continue;
    const std::string kw(t.text);

    if (kw == "func" || kw == "extern") {
      const Token* name_t = tok_at(i + 1);
      if (!is_name(name_t) || tok_at(i + 2) == nullptr || tok_at(i + 2)->kind != Tok::LParen)
        continue;
      const std::string name(name_t->text);
      std::string params;
      std::string ret;
      const std::size_t after = scan_params(i + 3, params, ret);
      std::string detail = kw + " " + name + "(" + params + ")";
      if (!ret.empty())
        detail += " -> " + ret;
      LspCompletion c;
      c.label = name;
      c.kind = 3;
      c.detail = detail;
      c.sort_text = "1";
      if (snippet) {
        c.is_snippet = true;
        c.insert_text = snippet_from_sig(name, "(" + params + ")");
        c.filter_text = name;
      }
      items.push_back(std::move(c));
      // 类体内的方法（depth==1）额外记入成员表
      if (depth == 1 && cur != nullptr && kw == "func") {
        cur->methods.emplace_back(name, detail);
        cur->method_snippets.push_back(snippet_from_sig(name, "(" + params + ")"));
      }
      (void)after;
      continue;
    }
    if ((kw == "class" || kw == "struct") && depth == 0) {
      const Token* name_t = tok_at(i + 1);
      if (!is_name(name_t))
        continue;
      classes.push_back({});
      cur = &classes.back();
      cur->name = std::string(name_t->text);
      cur->is_class = (kw == "class");
      cur->detail = kw + " " + cur->name;
      continue;
    }
    if (kw == "var" || kw == "mut") {
      const Token* name_t = tok_at(i + 1);
      if (!is_name(name_t) || in_paren)
        continue;
      // 跳过形参/类型位置的误命中：下一个应为标识符且其后非 '('（类型名带 ( 的情形不存在，直接接受）
      std::string detail(kw + " " + std::string(name_t->text));
      if (const Token* c = tok_at(i + 2); c != nullptr && c->kind == Tok::Colon) {
        if (const Token* ty = tok_at(i + 3); is_name(ty)) {
          detail += ": " + std::string(ty->text);
        }
      }
      LspCompletion item;
      item.label = std::string(name_t->text);
      item.kind = 6 /*Variable*/;
      item.detail = detail;
      item.sort_text = (depth == 0) ? "1" : (stmt_pos ? "2" : "0");
      items.push_back(std::move(item));
      continue;
    }
    if (kw == "for" && depth > 0) {
      const Token* name_t = tok_at(i + 1);
      if (!is_name(name_t))
        continue;
      LspCompletion item;
      item.label = std::string(name_t->text);
      item.kind = 6;
      item.detail = "循环变量";
      item.sort_text = stmt_pos ? "2" : "0";
      items.push_back(std::move(item));
      continue;
    }
  }

  // 类字段（depth==1 内 Ident + Colon）：补扫一遍
  depth = 0;
  Cls* owner = nullptr;
  std::size_t class_idx = 0;
  for (std::size_t i = 0; i < toks.size(); ++i) {
    const Token& t = toks[i];
    if (t.kind == Tok::Indent) {
      ++depth;
      continue;
    }
    if (t.kind == Tok::Dedent) {
      --depth;
      continue;
    }
    if (t.kind == Tok::Kw && (t.text == "class" || t.text == "struct") && depth == 0) {
      owner = class_idx < classes.size() ? &classes[class_idx] : nullptr;
      ++class_idx;
      continue;
    }
    if (depth != 1 || t.kind != Tok::Ident)
      continue;
    const Token* c = tok_at(i + 1);
    if (c == nullptr || c->kind != Tok::Colon)
      continue;
    // 形参误命中：所在行内前面出现 '(' 则跳过
    bool skip = false;
    for (std::size_t k = i; k > 0 && toks[k].kind != Tok::Nl; --k) {
      if (toks[k].kind == Tok::LParen) {
        skip = true;
        break;
      }
    }
    if (skip || owner == nullptr)
      continue;
    std::string ftype;
    if (const Token* ty = tok_at(i + 2); is_name(ty))
      ftype = std::string(ty->text);
    owner->fields.emplace_back(std::string(t.text),
                               ftype.empty() ? std::string(t.text) + ": var" : std::string(t.text) + ": " + ftype);
  }

  emit_builtin_items(items, snippet);
  emit_keyword_items(items, stmt_pos);

  if (ctx.kind == CompCtx::Type) {
    std::vector<LspCompletion> out;
    for (const char* t : {"int", "float", "bool", "string", "void", "ptr"}) {
      out.push_back({t, 14, "类型", {}, {}, {}, "0", false});
    }
    for (const Cls& cl : classes) {
      out.push_back({cl.name, cl.is_class ? 7 : 22, cl.detail, {}, {}, {}, "0", false});
    }
    return out;
  }
  if (ctx.kind == CompCtx::Member) {
    std::vector<LspCompletion> out;
    auto emit_cls = [&](const Cls& cl) {
      for (const auto& [n, d] : cl.fields)
        out.push_back({n, 5 /*Field*/, d, {}, {}, {}, "0", false});
      for (std::size_t mi = 0; mi < cl.methods.size(); ++mi) {
        LspCompletion c;
        c.label = cl.methods[mi].first;
        c.kind = 2 /*Method*/;
        c.detail = cl.methods[mi].second;
        c.sort_text = "0";
        if (snippet) {
          c.is_snippet = true;
          c.insert_text = cl.method_snippets[mi];
          c.filter_text = c.label;
        }
        out.push_back(std::move(c));
      }
    };
    bool matched = false;
    for (const Cls& cl : classes) {
      if (cl.name == ctx.receiver) {
        emit_cls(cl);
        matched = true;
      }
    }
    if (!matched) {
      for (const Cls& cl : classes)
        emit_cls(cl); // 回退：全部类成员
    }
    return out;
  }
  return items;
}

// 从（修补后）AST 收集补全项。需要 arena 存活于本函数期间
// （Param/字段等 span 指向 arena；产物字符串均自有）。
std::vector<LspCompletion> collect_from_prog(std::string_view text, const CursorContext& ctx,
                                             const vx::compiler::Program& prog,
                                             const std::filesystem::path& headers_root,
                                             const std::vector<std::filesystem::path>& include_dirs, bool snippet) {
  using namespace vx::compiler;

  struct Method {
    std::string name;
    std::string detail;
    std::string doc;
    std::span<const Param> params;
  };
  struct TypeEntry {
    std::string name;
    int kind = 7; // Class / Struct(22)
    std::string detail;
    std::string doc;
    std::vector<std::pair<std::string, std::string>> fields; // 名 → "x: int"
    std::vector<Method> methods;
  };
  struct Callable {
    std::string detail;
    std::string ret;
    std::string doc;
    std::span<const Param> params;
  };
  struct VarInfo {
    std::string detail; // 展示详情
    std::string type;   // 解析出的类型（可空）
  };

  const auto& stmts = prog.stmts;
  const int cursor1 = ctx.cursor_line;
  std::map<std::string, TypeEntry> types;
  std::map<std::string, Callable> funcs;
  std::map<std::string, VarInfo> top_vars;

  auto next_top_line = [&](std::size_t i) -> int { return (i + 1 < stmts.size()) ? stmts[i + 1]->line : INT_MAX; };

  // 第一遍：顶层 func/extern + class/struct
  for (const auto* s : stmts) {
    switch (s->kind) {
      case NodeKind::FuncDecl: {
        const auto* f = static_cast<const FuncDecl*>(s);
        funcs[std::string(f->name)] = Callable{
            func_signature("func", f->name, f->params, f->ret_type),
            std::string(f->ret_type),
            doc_above(text, f->line),
            f->params,
        };
        break;
      }
      case NodeKind::ExternDecl: {
        const auto* e = static_cast<const ExternDecl*>(s);
        funcs[std::string(e->name)] = Callable{
            func_signature("extern", e->name, e->params, e->ret_type),
            std::string(e->ret_type),
            doc_above(text, e->line),
            e->params,
        };
        break;
      }
      case NodeKind::ClassDecl: {
        const auto* c = static_cast<const ClassDecl*>(s);
        TypeEntry te;
        te.name = std::string(c->name);
        te.kind = 7;
        te.detail = "class " + te.name;
        te.doc = doc_above(text, c->line);
        for (const ClassField& fl : c->fields) {
          te.fields.emplace_back(std::string(fl.name),
                                 std::string(fl.name) + ": " + (fl.type.empty() ? "var" : std::string(fl.type)));
        }
        for (const FuncDecl* m : c->methods) {
          te.methods.push_back(Method{
              std::string(m->name),
              func_signature("func", m->name, m->params, m->ret_type),
              doc_above(text, m->line),
              m->params,
          });
        }
        types[te.name] = std::move(te);
        break;
      }
      case NodeKind::StructDecl: {
        const auto* st = static_cast<const StructDecl*>(s);
        TypeEntry te;
        te.name = std::string(st->name);
        te.kind = 22;
        te.detail = "struct " + te.name;
        te.doc = doc_above(text, st->line);
        for (const ClassField& fl : st->fields) {
          te.fields.emplace_back(std::string(fl.name),
                                 std::string(fl.name) + ": " + (fl.type.empty() ? "var" : std::string(fl.type)));
        }
        types[te.name] = std::move(te);
        break;
      }
      case NodeKind::EnumDecl: {
        const auto* en = static_cast<const EnumDecl*>(s);
        TypeEntry te;
        te.name = std::string(en->name);
        te.kind = 13;
        te.detail = "enum " + te.name;
        te.doc = doc_above(text, en->line);
        for (const EnumMember& m : en->members) {
          te.fields.emplace_back(std::string(m.name), std::string(m.name) + " = " + std::to_string(m.value));
        }
        types[te.name] = std::move(te);
        break;
      }
      default:
        break;
    }
  }

  // 第二遍：顶层 var/mut（初始化表达式可推断类型）
  auto type_of_init = [&](const Expr* init) -> std::string {
    if (init == nullptr)
      return {};
    if (init->kind == NodeKind::New)
      return std::string(static_cast<const New*>(init)->class_name);
    if (init->kind == NodeKind::Call) {
      const auto* call = static_cast<const Call*>(init);
      if (call->callee->kind == NodeKind::Ident) {
        const auto fn = funcs.find(std::string(static_cast<const Ident*>(call->callee)->name));
        if (fn != funcs.end())
          return fn->second.ret;
      }
    }
    return {};
  };
  for (const Stmt* s : stmts) {
    if (s->kind != NodeKind::VarDecl)
      continue;
    const auto* v = static_cast<const VarDecl*>(s);
    VarInfo info;
    std::string detail = v->is_mut ? "mut " : "var ";
    detail += std::string(v->name);
    if (!v->type_ann.empty())
      detail += ": " + std::string(v->type_ann);
    info.detail = detail;
    info.type = v->type_ann.empty() ? type_of_init(v->init) : std::string(v->type_ann);
    top_vars[std::string(v->name)] = std::move(info);
  }

  // 第三遍：确定光标所在的作用域（顶层函数 / 类方法），收集可见局部
  std::map<std::string, VarInfo> locals;
  std::string method_class; // 光标所在方法的宿主类（self 可用）
  auto add_params = [&](std::span<const Param> ps) {
    for (const Param& p : ps) {
      locals[std::string(p.name)] =
          VarInfo{"参数 " + (p.type.empty() ? std::string("var") : std::string(p.type)), std::string(p.type)};
    }
  };
  std::function<void(std::span<const Stmt* const>)> gather = nullptr;
  gather = [&](std::span<const Stmt* const> body) {
    for (const Stmt* s : body) {
      if (s->line > cursor1)
        continue; // 声明尚未到达光标
      switch (s->kind) {
        case NodeKind::VarDecl: {
          const auto* v = static_cast<const VarDecl*>(s);
          VarInfo info;
          std::string detail = v->is_mut ? "mut " : "var ";
          detail += std::string(v->name);
          if (!v->type_ann.empty())
            detail += ": " + std::string(v->type_ann);
          info.detail = detail;
          info.type = v->type_ann.empty() ? type_of_init(v->init) : std::string(v->type_ann);
          locals[std::string(v->name)] = std::move(info);
          break;
        }
        case NodeKind::For: {
          const auto* f = static_cast<const For*>(s);
          locals[std::string(f->var)] = VarInfo{"循环变量", {}};
          gather(f->body);
          break;
        }
        case NodeKind::If: {
          const auto* in = static_cast<const If*>(s);
          gather(in->then_body);
          for (const ElifArm& arm : in->elifs)
            gather(arm.body);
          gather(in->else_body);
          break;
        }
        case NodeKind::While: {
          const auto* w = static_cast<const While*>(s);
          gather(w->body);
          break;
        }
        default:
          break;
      }
    }
  };
  for (std::size_t i = 0; i < stmts.size(); ++i) {
    const Stmt* s = stmts[i];
    const int span_end = next_top_line(i);
    if (cursor1 < s->line || cursor1 >= span_end)
      continue;
    if (s->kind == NodeKind::FuncDecl) {
      const auto* f = static_cast<const FuncDecl*>(s);
      add_params(f->params);
      gather(f->body);
    } else if (s->kind == NodeKind::ClassDecl) {
      const auto* c = static_cast<const ClassDecl*>(s);
      for (const FuncDecl* m : c->methods) {
        if (cursor1 >= m->line && cursor1 < span_end) {
          method_class = std::string(c->name);
          add_params(m->params);
          gather(m->body);
        }
      }
    }
  }
  if (!method_class.empty()) {
    locals["self"] = VarInfo{"self: " + method_class, method_class};
  }

  std::vector<LspCompletion> items;

  // ---- 类型位置：基础类型 + class/struct 名 ----
  if (ctx.kind == CompCtx::Type) {
    for (const char* t : {"int", "float", "bool", "string", "void", "ptr"}) {
      items.push_back({t, 14 /*Keyword*/, "类型", {}, {}, {}, "0", false});
    }
    for (const auto& [name, te] : types) {
      items.push_back({name, te.kind, te.detail, te.doc, {}, {}, "0", false});
    }
    return items;
  }

  // ---- 成员位置：导入库符号优先，其次类/结构成员 ----
  if (ctx.kind == CompCtx::Member) {
    if (!headers_root.empty()) {
      DiagBag ibag;
      ImportContext ictx{headers_root, headers_root, include_dirs, {}};
      if (auto table = build_import_table(prog, ictx, "<document>", ibag)) {
        if (const ImportedLib* lib = table->lib_by_prefix(ctx.receiver)) {
          for (const ImportedSymbol& sym : lib->symbols) {
            LspCompletion c;
            c.label = sym.vx_name;
            c.kind = 3 /*Function*/;
            std::string params;
            for (std::size_t pi = 0; pi < sym.params.size(); ++pi) {
              if (pi != 0U)
                params += ", ";
              params += sym.params[pi].name + ": " + sym.params[pi].type;
            }
            c.detail = lib->name + "." + sym.vx_name + "(" + params + ")";
            if (!sym.ret_type.empty() && sym.ret_type != "void")
              c.detail += " -> " + sym.ret_type;
            c.documentation = "**导入库** `" + lib->name + "` · **VX ABI** `" + sym.abi_symbol + "`";
            c.sort_text = "0" + c.label;
            if (snippet) {
              c.is_snippet = true;
              c.insert_text = snippet_from_sig(sym.vx_name, "(" + params + ")");
              c.filter_text = c.label;
            }
            items.push_back(std::move(c));
          }
          if (!items.empty())
            return items; // 库命中 → 只给库符号
        }
      }
    }

    auto emit_members = [&](const TypeEntry& te) {
      for (const auto& [n, d] : te.fields) {
        items.push_back({n, 5 /*Field*/, d, te.doc, {}, {}, "0", false});
      }
      for (const Method& m : te.methods) {
        LspCompletion c;
        c.label = m.name;
        c.kind = 2 /*Method*/;
        c.detail = m.detail;
        c.documentation = m.doc;
        c.sort_text = "0";
        if (snippet) {
          c.is_snippet = true;
          c.insert_text = snippet_from_params(m.name, m.params);
          c.filter_text = m.name;
        }
        items.push_back(std::move(c));
      }
    };

    // 接收者 → 类型名解析
    std::string resolved;
    bool found = false;
    if (ctx.receiver == "self" && !method_class.empty()) {
      resolved = method_class;
      found = true;
    } else if (static_cast<unsigned int>(types.contains(ctx.receiver)) != 0U) {
      resolved = ctx.receiver;
      found = true;
    } else if (const auto lt = locals.find(ctx.receiver); lt != locals.end()) {
      found = true;
      resolved = lt->second.type;
    } else if (const auto tt = top_vars.find(ctx.receiver); tt != top_vars.end()) {
      found = true;
      resolved = tt->second.type;
    } else if (const auto ft = funcs.find(ctx.receiver); ft != funcs.end()) {
      found = true;
      resolved = ft->second.ret;
    }

    static const std::map<std::string_view, int> kPrimitives = {
        {"int", 1}, {"float", 1}, {"bool", 1}, {"string", 1}, {"void", 1}, {"ptr", 1}, {"nil", 1},
    };
    if (found && !resolved.empty() && (static_cast<unsigned int>(kPrimitives.contains(resolved)) == 0U)) {
      if (const auto te = types.find(resolved); te != types.end()) {
        emit_members(te->second);
        if (!items.empty())
          return items;
      }
      found = false; // 解析到未知类型 → 走全类回退
    } else if (found && (static_cast<unsigned int>(kPrimitives.contains(resolved)) != 0U)) {
      return items; // 基础类型无成员
    }
    if (!found) {
      for (const auto& [name, te] : types)
        emit_members(te); // 启发式回退
    }
    return items;
  }

  // ---- 一般位置：作用域 + 顶层声明 + 内置 + 关键字 ----
  const bool stmt_pos = ctx.statement_pos;
  for (const auto& [name, info] : locals) {
    items.push_back({name, 6 /*Variable*/, info.detail, {}, {}, {}, stmt_pos ? "2" : "0", false});
  }
  for (const auto& [name, fn] : funcs) {
    LspCompletion c;
    c.label = name;
    c.kind = 3 /*Function*/;
    c.detail = fn.detail;
    c.documentation = fn.doc;
    c.sort_text = "1";
    if (snippet) {
      c.is_snippet = true;
      c.insert_text = snippet_from_params(name, fn.params);
      c.filter_text = name;
    }
    items.push_back(std::move(c));
  }
  for (const auto& [name, te] : types) {
    items.push_back({name, te.kind, te.detail, te.doc, {}, {}, "1", false});
  }
  for (const auto& [name, info] : top_vars) {
    items.push_back({name, 6 /*Variable*/, info.detail, {}, {}, {}, "1", false});
  }
  emit_builtin_items(items, snippet);
  emit_keyword_items(items, stmt_pos);
  return items;
}

} // namespace

} // namespace

} // namespace vxls
