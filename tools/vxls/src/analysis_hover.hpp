// analysis_hover.hpp — 悬停与声明查找（内部实现头，仅供 analysis.cpp 使用）
#pragma once

#include "analysis.hpp"
#include "analysis_text.hpp"
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

// 在 token 流中定位 (line,character)（0 起始）处的标识符 token。
// 返回 token 索引（找不到返回 npos），并输出其 1 起始行列。
std::size_t ident_token_at(std::string_view text, int line, int character, std::string& name, int& tok_line,
                           int& tok_col) {
  vx::util::Arena arena;
  vx::compiler::DiagBag bag;
  auto toks = vx::compiler::tokenize(text, "<document>", arena, bag);
  for (std::size_t i = 0; i < toks.size(); ++i) {
    const Token& t = toks[i];
    if (t.kind != Tok::Ident && t.kind != Tok::Kw)
      continue;
    const LspPosition start = diag_pos_to_lsp(text, t.line, t.col);
    const LspPosition end = diag_pos_to_lsp(text, t.line, t.col + static_cast<int>(utf8_len(t.text)));
    if (line != start.line && line != end.line)
      continue;
    const int from = line == start.line ? start.character : 0;
    const int to = line == end.line ? end.character : static_cast<int>(1u << 20u);
    if (character >= from && character <= to) {
      name = std::string(t.text);
      tok_line = t.line;
      tok_col = t.col;
      return i;
    }
  }
  return std::string_view::npos;
}

// 兼容旧调用：仅需命中判定与位置
bool ident_at(std::string_view text, int line, int character, std::string& name, int& tok_line, int& tok_col) {
  return ident_token_at(text, line, character, name, tok_line, tok_col) != std::string_view::npos;
}

// 取声明上方紧邻的连续 # 文档注释（每行去掉 '#' 与前导空格，最多 8 行）
std::string doc_above(std::string_view text, int decl_line) {
  std::string out;
  int l = decl_line - 1;
  std::vector<std::string> lines;
  while (l >= 1 && lines.size() < 8) {
    std::string_view t = line_text(text, l);
    const std::size_t first = t.find_first_not_of(" \t\r");
    if (first == std::string_view::npos || t[first] != '#')
      break;
    std::string_view doc = t.substr(first + 1);
    const std::size_t b = doc.find_first_not_of(' ');
    if (b != std::string_view::npos)
      doc = doc.substr(b);
    if (!doc.empty() && doc.back() == '\r')
      doc.remove_suffix(1);
    lines.emplace_back(doc);
    --l;
  }
  for (auto& line : std::views::reverse(lines)) {
    if (!out.empty())
      out += '\n';
    out += line;
  }
  return out;
}

// 在 AST 中按名称查找声明，收集 hover 所需的全部结构化信息
struct DeclInfo {
  std::string signature; // 代码块中的签名
  std::string kind;      // "func" | "extern" | "class" | "struct" | "var"
  int line = 0;
  std::size_t name_col = 0;                                // UTF-16 列
  std::vector<std::pair<std::string, std::string>> params; // 参数 (名, 类型)
  std::string ret;                                         // 返回类型（空 = 无/void）
  std::vector<std::string> members;                        // 字段 "x: int" 或方法签名
  std::string doc;                                         // 声明上方 # 注释
  std::string init;                                        // var 初始化表达式原文
  std::string owner;                                       // 所属声明（参数时为 "func X"）
  bool is_mut = false;
};

bool find_decl(std::string_view text, std::string_view name, DeclInfo& out) {
  vx::util::Arena arena;
  vx::compiler::DiagBag bag;
  auto toks = vx::compiler::tokenize(text, "<document>", arena, bag);
  auto prog = vx::compiler::parse(toks, "<document>", arena, bag);
  if (!prog)
    return false;

  auto name_col_of = [&](int line, std::string_view prefix, std::string_view n) -> std::size_t {
    const std::string_view l = line_text(text, line);
    const std::size_t p = l.find(prefix);
    const std::size_t pos = p == std::string_view::npos ? l.find(n) : l.find(n, p + prefix.size());
    return pos == std::string_view::npos ? 0 : utf16_len(l.substr(0, pos));
  };

  // 收集参数表 + 初始化表达式（从源码行截取）
  auto fill_params = [&](auto span_params) {
    for (const auto& p : span_params) {
      out.params.emplace_back(std::string(p.name), p.type.empty() ? "var" : std::string(p.type));
    }
  };
  auto fill_init = [&](int line, std::string_view var) {
    const std::string_view l = line_text(text, line);
    const std::size_t at = l.find(var);
    if (at == std::string_view::npos)
      return;
    const std::size_t eq = l.find('=', at + var.size());
    if (eq == std::string_view::npos)
      return;
    std::string_view v = l.substr(eq + 1);
    const std::size_t b = v.find_first_not_of(' ');
    if (b != std::string_view::npos)
      v = v.substr(b);
    while (!v.empty() && (v.back() == ' ' || v.back() == '\r'))
      v.remove_suffix(1);
    out.init = std::string(v);
  };

  std::function<bool(const Stmt*, bool)> walk = nullptr;
  // 遍历语句列表（块作用域）
  auto walk_stmts = [&](const std::span<const Stmt* const>& body) -> bool {
    for (const Stmt* s : body) {
      if (walk(s, false))
        return true;
    }
    return false;
  };

  walk = [&](const Stmt* s, bool in_class) -> bool {
    if (s == nullptr)
      return false;
    switch (s->kind) {
      case NodeKind::FuncDecl: {
        const auto* f = static_cast<const vx::compiler::FuncDecl*>(s);
        if (f->name == name) {
          out.kind = "func";
          out.signature = func_signature("func", f->name, f->params, f->ret_type);
          out.line = f->line;
          out.name_col = name_col_of(f->line, "func", name);
          fill_params(f->params);
          out.ret = std::string(f->ret_type);
          out.doc = doc_above(text, f->line);
          return true;
        }
        // 参数名命中 → hover 显示参数类型与所属函数
        for (const auto& p : f->params) {
          if (p.name != name)
            continue;
          out.kind = "param";
          out.signature = std::string(p.name) + ": " + (p.type.empty() ? "var" : std::string(p.type));
          out.ret = std::string(p.type);
          out.owner = "func " + std::string(f->name);
          out.line = f->line;
          out.name_col = name_col_of(f->line, "func", f->name);
          return true;
        }
        // 局部变量：递归进函数体
        return walk_stmts(f->body);
      }
      case NodeKind::If: {
        const auto* i = static_cast<const vx::compiler::If*>(s);
        if (walk_stmts(i->then_body))
          return true;
        for (const vx::compiler::ElifArm& arm : i->elifs) {
          if (walk_stmts(arm.body))
            return true;
        }
        return walk_stmts(i->else_body);
      }
      case NodeKind::While: {
        const auto* w = static_cast<const vx::compiler::While*>(s);
        return walk_stmts(w->body);
      }
      case NodeKind::For: {
        const auto* f = static_cast<const vx::compiler::For*>(s);
        if (f->var == name) { // 循环变量命中
          out.kind = "var";
          out.signature = "for " + std::string(f->var);
          out.line = f->line;
          out.name_col = name_col_of(f->line, "for", f->var);
          out.owner = "循环变量";
          return true;
        }
        return walk_stmts(f->body);
      }
      case NodeKind::ExternDecl: {
        const auto* e = static_cast<const vx::compiler::ExternDecl*>(s);
        if (e->name == name) {
          out.kind = "extern";
          out.signature = func_signature("extern", e->name, e->params, e->ret_type);
          out.line = e->line;
          out.name_col = name_col_of(e->line, "extern", name);
          fill_params(e->params);
          out.ret = std::string(e->ret_type);
          out.doc = doc_above(text, e->line);
          return true;
        }
        return false;
      }
      case NodeKind::ClassDecl: {
        const auto* c = static_cast<const vx::compiler::ClassDecl*>(s);
        if (c->name == name) {
          out.kind = "class";
          out.signature = "class " + std::string(c->name);
          out.line = c->line;
          out.name_col = name_col_of(c->line, "class", name);
          out.doc = doc_above(text, c->line);
          for (const auto& fl : c->fields) {
            out.members.push_back(std::string(fl.name) + ": " + (fl.type.empty() ? "var" : std::string(fl.type)));
          }
          for (const auto* m : c->methods) {
            out.members.push_back("  " + func_signature("func", m->name, m->params, m->ret_type));
          }
          return true;
        }
        for (const auto* m : c->methods) {
          if (walk(m, true))
            return true;
        }
        return false;
      }
      case NodeKind::StructDecl: {
        const auto* st = static_cast<const vx::compiler::StructDecl*>(s);
        if (st->name == name) {
          out.kind = "struct";
          out.signature = "struct " + std::string(st->name);
          out.line = st->line;
          out.name_col = name_col_of(st->line, "struct", name);
          out.doc = doc_above(text, st->line);
          for (const auto& fl : st->fields) {
            out.members.push_back(std::string(fl.name) + ": " + (fl.type.empty() ? "var" : std::string(fl.type)));
          }
          return true;
        }
        return false;
      }
      case NodeKind::EnumDecl: {
        const auto* en = static_cast<const vx::compiler::EnumDecl*>(s);
        if (en->name == name) {
          out.kind = "enum";
          out.signature = "enum " + std::string(en->name);
          out.line = en->line;
          out.name_col = name_col_of(en->line, "enum", name);
          out.doc = doc_above(text, en->line);
          for (const EnumMember& m : en->members) {
            out.members.push_back(std::string(m.name) + " = " + std::to_string(m.value));
          }
          return true;
        }
        for (const EnumMember& m : en->members) {
          if (m.name != name)
            continue;
          out.kind = "enumMember";
          out.signature = std::string(en->name) + "." + std::string(m.name) + " = " + std::to_string(m.value);
          out.ret = "int";
          out.owner = "enum " + std::string(en->name);
          out.line = m.line;
          out.doc = doc_above(text, m.line);
          return true;
        }
        return false;
      }
      case NodeKind::VarDecl: {
        const auto* v = static_cast<const vx::compiler::VarDecl*>(s);
        if (v->name == name) {
          out.kind = "var";
          out.signature = std::string(v->is_mut ? "mut " : "var ") + std::string(v->name);
          if (!v->type_ann.empty())
            out.signature += ": " + std::string(v->type_ann);
          out.line = v->line;
          out.name_col = name_col_of(v->line, v->is_mut ? "mut" : "var", name);
          out.ret = std::string(v->type_ann);
          out.is_mut = v->is_mut;
          out.doc = doc_above(text, v->line);
          fill_init(v->line, v->name);
          return true;
        }
        return false;
      }
      default:
        return false;
    }
    (void)in_class;
  };

  for (const Stmt* s : prog->stmts) {
    if (walk(s, false))
      return true;
  }
  return false;
}

// ---- 内置运行时符号文档（emitter Builtin 表对应）----
struct BuiltinDoc {
  const char* sig;
  const char* desc;
};
const std::map<std::string_view, BuiltinDoc>& builtin_docs() {
  static const std::map<std::string_view, BuiltinDoc> table = {
      {"out", {"out(msg: string)", "输出字符串到 stdout（不追加换行）"}},
      {"out_int", {"out_int(v: int)", "输出整数"}},
      {"out_bool", {"out_bool(v: bool)", "输出布尔值 true / false"}},
      {"out_float", {"out_float(v: float)", "输出浮点数（%g 格式）"}},
      {"puts", {"puts(s: string) -> int", "libc puts：输出并追加换行"}},
      {"malloc", {"malloc(size: int) -> ptr", "分配 size 字节内存（libc）"}},
      {"free", {"free(p: ptr)", "释放 malloc 分配的内存"}},
      {"exit", {"exit(code: int)", "以退出码 code 结束进程"}},
      {"vx_str_concat", {"vx_str_concat(a: string, b: string) -> string", "运行时拼接两个字符串（返回新分配的内存）"}},
      {"sys_argv_count", {"sys_argv_count() -> int", "命令行参数个数（含可执行文件名）"}},
      {"sys_argv_get", {"sys_argv_get(i: int) -> string", "取第 i 个命令行参数（越界返回空串）"}},
      {
          "file_read",
          {"file_read(path: string) -> string", "读取整个文件内容；失败返回空串（可用 file_exists 判断是否存在）"},
      },
      {"file_write", {"file_write(path: string, content: string) -> int", "覆写写入文件，返回写入字节数；失败返回 -1"}},
      {"file_exists", {"file_exists(path: string) -> bool", "判断文件是否存在（可读）"}},
  };
  return table;
}

// markdown 组装：代码块 + 分隔线 + 结构化细节
std::string hover_markdown(const DeclInfo& d) {
  std::string md = "```vx\n" + d.signature + "\n```";
  if (!d.params.empty()) {
    md += "\n---\n**参数**\n";
    for (const auto& [n, t] : d.params) {
      md += "- `" + n + ": " + t + "`\n";
    }
    if (!d.ret.empty()) {
      md += "\n**返回** `" + d.ret + "`\n";
    }
  } else if (d.kind == "class" || d.kind == "struct") {
    md += "\n---\n**成员**\n";
    for (const auto& m : d.members) {
      md += "- `" + m + "`\n";
    }
  } else if (d.kind == "var") {
    md += "\n---\n";
    md += d.ret.empty() ? "**类型** 由初始化表达式推断\n" : "**类型** `" + d.ret + "`\n";
    if (d.is_mut)
      md += "**可变**（mut）\n";
    if (!d.owner.empty())
      md += "**" + d.owner + "**\n";
    if (!d.init.empty())
      md += "**初始值** `" + d.init + "`\n";
  } else if (d.kind == "param") {
    md += "\n---\n**参数**（" + d.owner + "）\n";
    md += d.ret.empty() ? "**类型** `var`\n" : "**类型** `" + d.ret + "`\n";
  }
  if (!d.doc.empty()) {
    md += "\n> " + d.doc + "\n";
  }
  return md;
}

} // namespace

} // namespace

} // namespace vxls
