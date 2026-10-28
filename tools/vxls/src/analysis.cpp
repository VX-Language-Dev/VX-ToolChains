// analysis.cpp — vxls 文档分析实现：lexer/parser → LSP 诊断/符号/补全/悬停。
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

namespace vxls {

using vx::compiler::Diagnostic;
using vx::compiler::NodeKind;
using vx::compiler::Severity;
using vx::compiler::Stmt;
using vx::compiler::Tok;
using vx::compiler::Token;

namespace {

// UTF-8 码点计数（列号语义：1 起始，按码点）
std::size_t utf8_len(std::string_view s) {
  std::size_t n = 0;
  for (char c : s) {
    if ((static_cast<unsigned char>(c) & 0xC0) != 0x80) ++n;
  }
  return n;
}

// UTF-8 码点序列 → UTF-16 码元数（LSP character 列语义）
std::size_t utf16_len(std::string_view s) {
  std::size_t n = 0;
  for (char c : s) {
    const unsigned char u = static_cast<unsigned char>(c);
    if ((u & 0xC0) == 0x80) continue;     // 续字节
    n += (u >= 0xF0) ? 2 : 1;             // 4 字节码点 = 代理对 2 码元
  }
  return n;
}

// 取第 line 行（1 起始）的文本（不含换行）
std::string_view line_text(std::string_view text, int line) {
  std::size_t start = 0;
  int cur = 1;
  while (cur < line) {
    const std::size_t nl = text.find('\n', start);
    if (nl == std::string_view::npos) return {};
    start = nl + 1;
    ++cur;
  }
  const std::size_t nl = text.find('\n', start);
  std::string_view l = text.substr(start, nl == std::string_view::npos
                                             ? std::string_view::npos
                                             : nl - start);
  if (!l.empty() && l.back() == '\r') l.remove_suffix(1);
  return l;
}

int count_lines(std::string_view text) {
  int n = 1;
  for (char c : text) {
    if (c == '\n') ++n;
  }
  return n;
}

// 声明签名文本（悬停/符号 detail 用）
std::string func_signature(std::string_view keyword, std::string_view name,
                           std::span<const vx::compiler::Param> params,
                           std::string_view ret) {
  std::string s = std::string(keyword) + " " + std::string(name) + "(";
  for (std::size_t i = 0; i < params.size(); ++i) {
    if (i) s += ", ";
    s += std::string(params[i].name);
    if (!params[i].type.empty()) s += ": " + std::string(params[i].type);
  }
  s += ")";
  if (!ret.empty() && ret != "void") s += " -> " + std::string(ret);
  return s;
}

// 语句 → 符号（顶层与类方法均可）
void collect_symbol(const Stmt *s, std::string_view text,
                    std::vector<LspSymbol> &out, bool in_class = false) {
  if (s == nullptr) return;
  auto add = [&](std::string_view name, std::string_view detail, int kind,
                 int line, std::size_t name_col) {
    LspSymbol sym;
    sym.name = std::string(name);
    sym.detail = std::string(detail);
    sym.kind = kind;
    sym.range = {{line - 1, 0}, {line, 0}};
    sym.selection_range = {
        {line - 1, static_cast<int>(name_col)},
        {line - 1, static_cast<int>(name_col + utf16_len(name))}};
    out.push_back(std::move(sym));
    (void)text;
  };
  // 名称起始列：在该行中从声明关键字后搜索名称
  auto name_col_of = [&](int line, std::string_view prefix,
                         std::string_view name) -> std::size_t {
    const std::string_view l = line_text(text, line);
    const std::size_t p = l.find(prefix);
    if (p == std::string_view::npos) return 0;
    const std::size_t n = l.find(name, p + prefix.size());
    return n == std::string_view::npos ? 0 : utf16_len(l.substr(0, n));
  };

  switch (s->kind) {
  case NodeKind::FuncDecl: {
    const auto *f = static_cast<const vx::compiler::FuncDecl *>(s);
    const char *kw = in_class ? "func" : "func";
    add(f->name, func_signature(kw, f->name, f->params, f->ret_type),
        in_class ? 6 /*Method*/ : 12 /*Function*/, f->line,
        name_col_of(f->line, "func", f->name));
    break;
  }
  case NodeKind::ExternDecl: {
    const auto *e = static_cast<const vx::compiler::ExternDecl *>(s);
    add(e->name,
        func_signature("extern", e->name, e->params, e->ret_type), 12,
        e->line, name_col_of(e->line, "extern", e->name));
    break;
  }
  case NodeKind::ClassDecl: {
    const auto *c = static_cast<const vx::compiler::ClassDecl *>(s);
    add(c->name, "class " + std::string(c->name), 5 /*Class*/, c->line,
        name_col_of(c->line, "class", c->name));
    for (const auto *m : c->methods) {
      collect_symbol(m, text, out, true);
    }
    break;
  }
  case NodeKind::StructDecl: {
    const auto *st = static_cast<const vx::compiler::StructDecl *>(s);
    add(st->name, "struct " + std::string(st->name), 23 /*Struct*/, st->line,
        name_col_of(st->line, "struct", st->name));
    break;
  }
  case NodeKind::VarDecl: {
    const auto *v = static_cast<const vx::compiler::VarDecl *>(s);
    std::string detail = std::string(v->is_mut ? "mut " : "var ");
    detail += std::string(v->name);
    if (!v->type_ann.empty()) detail += ": " + std::string(v->type_ann);
    add(v->name, detail, 13 /*Variable*/, v->line,
        name_col_of(v->line, v->is_mut ? "mut" : "var", v->name));
    break;
  }
  case NodeKind::Import: {
    const auto *im = static_cast<const vx::compiler::Import *>(s);
    add(im->path, "import " + std::string(im->path), 2 /*Module*/, im->line,
        name_col_of(im->line, "import", im->path));
    break;
  }
  case NodeKind::ImpDecl: {
    const auto *d = static_cast<const vx::compiler::ImpDecl *>(s);
    add(d->lib, "@imp(" + std::string(d->lib) + ")", 2 /*Module*/, d->line, 0);
    break;
  }
  case NodeKind::CppImpDecl: {
    const auto *d = static_cast<const vx::compiler::CppImpDecl *>(s);
    add(d->header, "@cppimp(" + std::string(d->header) + ")", 2, d->line, 0);
    break;
  }
  default:
    break;
  }
}

} // namespace

LspPosition diag_pos_to_lsp(std::string_view text, int line, int col) {
  if (line < 1) return {0, 0};
  const std::string_view l = line_text(text, line);
  // col 为 1 起始码点列 → 取前 col-1 个码点的 UTF-16 宽度
  std::size_t points = 0;
  std::size_t i = 0;
  while (i < l.size() && points + 1 < static_cast<std::size_t>(col)) {
    const unsigned char u = static_cast<unsigned char>(l[i]);
    std::size_t adv = 1;
    if ((u & 0xE0) == 0xC0) adv = 2;
    else if ((u & 0xF0) == 0xE0) adv = 3;
    else if ((u & 0xF8) == 0xF0) adv = 4;
    i += adv;
    ++points;
  }
  return {line - 1, static_cast<int>(utf16_len(l.substr(0, i)))};
}

LspDiagnostic to_lsp_diagnostic(const Diagnostic &d, std::string_view text) {
  LspDiagnostic out;
  out.severity = d.severity == Severity::Error
                     ? 1
                     : d.severity == Severity::Warning ? 2 : 3;
  out.code = d.code;
  out.message = d.message;
  out.range.start = diag_pos_to_lsp(text, d.line, d.col > 0 ? d.col : 1);
  if (d.end_line > 0 && d.end_col > 0) {
    out.range.end = diag_pos_to_lsp(text, d.end_line, d.end_col);
  } else {
    out.range.end = out.range.start;
    out.range.end.character += 1;
  }
  return out;
}

Analysis analyze(std::string_view text) {
  Analysis a;
  a.line_count = count_lines(text);
  vx::util::Arena arena;
  vx::compiler::DiagBag bag;
  auto toks = vx::compiler::tokenize(text, "<document>", arena, bag);
  auto prog = vx::compiler::parse(toks, "<document>", arena, bag);
  // fail-soft 解析：prog 总有值（可能为部分 AST）；parse_ok 反映是否零错误
  a.parse_ok = !bag.has_error();
  a.diags = bag.items();

  if (prog) {
    for (const Stmt *s : prog->stmts) {
      collect_symbol(s, text, a.symbols);
    }
  }

  return a;
}

namespace {

// 在 token 流中定位 (line,character)（0 起始）处的标识符 token。
// 返回 token 索引（找不到返回 npos），并输出其 1 起始行列。
std::size_t ident_token_at(std::string_view text, int line, int character,
                           std::string &name, int &tok_line, int &tok_col) {
  vx::util::Arena arena;
  vx::compiler::DiagBag bag;
  auto toks = vx::compiler::tokenize(text, "<document>", arena, bag);
  for (std::size_t i = 0; i < toks.size(); ++i) {
    const Token &t = toks[i];
    if (t.kind != Tok::Ident && t.kind != Tok::Kw) continue;
    const LspPosition start = diag_pos_to_lsp(text, t.line, t.col);
    const LspPosition end =
        diag_pos_to_lsp(text, t.line,
                        t.col + static_cast<int>(utf8_len(t.text)));
    if (line != start.line && line != end.line) continue;
    const int from = line == start.line ? start.character : 0;
    const int to = line == end.line ? end.character : 1 << 20;
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
bool ident_at(std::string_view text, int line, int character,
              std::string &name, int &tok_line, int &tok_col) {
  return ident_token_at(text, line, character, name, tok_line, tok_col) !=
         std::string_view::npos;
}

// 取声明上方紧邻的连续 # 文档注释（每行去掉 '#' 与前导空格，最多 8 行）
std::string doc_above(std::string_view text, int decl_line) {
  std::string out;
  int l = decl_line - 1;
  std::vector<std::string> lines;
  while (l >= 1 && lines.size() < 8) {
    std::string_view t = line_text(text, l);
    const std::size_t first = t.find_first_not_of(" \t\r");
    if (first == std::string_view::npos || t[first] != '#') break;
    std::string_view doc = t.substr(first + 1);
    const std::size_t b = doc.find_first_not_of(' ');
    if (b != std::string_view::npos) doc = doc.substr(b);
    if (!doc.empty() && doc.back() == '\r') doc.remove_suffix(1);
    lines.emplace_back(doc);
    --l;
  }
  for (auto it = lines.rbegin(); it != lines.rend(); ++it) {
    if (!out.empty()) out += '\n';
    out += *it;
  }
  return out;
}

// 在 AST 中按名称查找声明，收集 hover 所需的全部结构化信息
struct DeclInfo {
  std::string signature;   // 代码块中的签名
  std::string kind;        // "func" | "extern" | "class" | "struct" | "var"
  int line = 0;
  std::size_t name_col = 0; // UTF-16 列
  std::vector<std::pair<std::string, std::string>> params; // 参数 (名, 类型)
  std::string ret;         // 返回类型（空 = 无/void）
  std::vector<std::string> members; // 字段 "x: int" 或方法签名
  std::string doc;         // 声明上方 # 注释
  std::string init;        // var 初始化表达式原文
  std::string owner;       // 所属声明（参数时为 "func X"）
  bool is_mut = false;
};

bool find_decl(std::string_view text, std::string_view name, DeclInfo &out) {
  vx::util::Arena arena;
  vx::compiler::DiagBag bag;
  auto toks = vx::compiler::tokenize(text, "<document>", arena, bag);
  auto prog = vx::compiler::parse(toks, "<document>", arena, bag);
  if (!prog) return false;

  auto name_col_of = [&](int line, std::string_view prefix,
                         std::string_view n) -> std::size_t {
    const std::string_view l = line_text(text, line);
    const std::size_t p = l.find(prefix);
    const std::size_t pos =
        p == std::string_view::npos ? l.find(n) : l.find(n, p + prefix.size());
    return pos == std::string_view::npos ? 0 : utf16_len(l.substr(0, pos));
  };

  // 收集参数表 + 初始化表达式（从源码行截取）
  auto fill_params = [&](auto span_params) {
    for (const auto &p : span_params) {
      out.params.emplace_back(std::string(p.name),
                              p.type.empty() ? "var" : std::string(p.type));
    }
  };
  auto fill_init = [&](int line, std::string_view var) {
    const std::string_view l = line_text(text, line);
    const std::size_t at = l.find(var);
    if (at == std::string_view::npos) return;
    const std::size_t eq = l.find('=', at + var.size());
    if (eq == std::string_view::npos) return;
    std::string_view v = l.substr(eq + 1);
    const std::size_t b = v.find_first_not_of(' ');
    if (b != std::string_view::npos) v = v.substr(b);
    while (!v.empty() && (v.back() == ' ' || v.back() == '\r')) v.remove_suffix(1);
    out.init = std::string(v);
  };

  std::function<bool(const Stmt *, bool)> walk = nullptr;
  // 遍历语句列表（块作用域）
  auto walk_stmts = [&](const std::span<const Stmt *const> &body) -> bool {
    for (const Stmt *s : body) {
      if (walk(s, false)) return true;
    }
    return false;
  };

  walk = [&](const Stmt *s, bool in_class) -> bool {
    if (s == nullptr) return false;
    switch (s->kind) {
    case NodeKind::FuncDecl: {
      const auto *f = static_cast<const vx::compiler::FuncDecl *>(s);
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
      for (const auto &p : f->params) {
        if (p.name != name) continue;
        out.kind = "param";
        out.signature = std::string(p.name) + ": " +
                        (p.type.empty() ? "var" : std::string(p.type));
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
      const auto *i = static_cast<const vx::compiler::If *>(s);
      if (walk_stmts(i->then_body)) return true;
      for (const vx::compiler::ElifArm &arm : i->elifs) {
        if (walk_stmts(arm.body)) return true;
      }
      return walk_stmts(i->else_body);
    }
    case NodeKind::While: {
      const auto *w = static_cast<const vx::compiler::While *>(s);
      return walk_stmts(w->body);
    }
    case NodeKind::For: {
      const auto *f = static_cast<const vx::compiler::For *>(s);
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
      const auto *e = static_cast<const vx::compiler::ExternDecl *>(s);
      if (e->name == name) {
        out.kind = "extern";
        out.signature =
            func_signature("extern", e->name, e->params, e->ret_type);
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
      const auto *c = static_cast<const vx::compiler::ClassDecl *>(s);
      if (c->name == name) {
        out.kind = "class";
        out.signature = "class " + std::string(c->name);
        out.line = c->line;
        out.name_col = name_col_of(c->line, "class", name);
        out.doc = doc_above(text, c->line);
        for (const auto &fl : c->fields) {
          out.members.push_back(std::string(fl.name) + ": " +
                                (fl.type.empty() ? "var" : std::string(fl.type)));
        }
        for (const auto *m : c->methods) {
          out.members.push_back("  " +
                                func_signature("func", m->name, m->params,
                                               m->ret_type));
        }
        return true;
      }
      for (const auto *m : c->methods) {
        if (walk(m, true)) return true;
      }
      return false;
    }
    case NodeKind::StructDecl: {
      const auto *st = static_cast<const vx::compiler::StructDecl *>(s);
      if (st->name == name) {
        out.kind = "struct";
        out.signature = "struct " + std::string(st->name);
        out.line = st->line;
        out.name_col = name_col_of(st->line, "struct", name);
        out.doc = doc_above(text, st->line);
        for (const auto &fl : st->fields) {
          out.members.push_back(std::string(fl.name) + ": " +
                                (fl.type.empty() ? "var" : std::string(fl.type)));
        }
        return true;
      }
      return false;
    }
    case NodeKind::VarDecl: {
      const auto *v = static_cast<const vx::compiler::VarDecl *>(s);
      if (v->name == name) {
        out.kind = "var";
        out.signature = std::string(v->is_mut ? "mut " : "var ") +
                        std::string(v->name);
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

  for (const Stmt *s : prog->stmts) {
    if (walk(s, false)) return true;
  }
  return false;
}

// ---- 内置运行时符号文档（emitter Builtin 表对应）----
struct BuiltinDoc {
  const char *sig;
  const char *desc;
};
const std::map<std::string_view, BuiltinDoc> &builtin_docs() {
  static const std::map<std::string_view, BuiltinDoc> table = {
      {"out", {"out(msg: string)", "输出字符串到 stdout（不追加换行）"}},
      {"out_int", {"out_int(v: int)", "输出整数"}},
      {"out_bool", {"out_bool(v: bool)", "输出布尔值 true / false"}},
      {"out_float", {"out_float(v: float)", "输出浮点数（%g 格式）"}},
      {"puts", {"puts(s: string) -> int", "libc puts：输出并追加换行"}},
      {"malloc", {"malloc(size: int) -> ptr", "分配 size 字节内存（libc）"}},
      {"free", {"free(p: ptr)", "释放 malloc 分配的内存"}},
      {"exit", {"exit(code: int)", "以退出码 code 结束进程"}},
      {"vx_str_concat",
       {"vx_str_concat(a: string, b: string) -> string",
        "运行时拼接两个字符串（返回新分配的内存）"}},
      {"sys_argv_count",
       {"sys_argv_count() -> int", "命令行参数个数（含可执行文件名）"}},
      {"sys_argv_get",
       {"sys_argv_get(i: int) -> string", "取第 i 个命令行参数（越界返回空串）"}},
      {"file_read",
       {"file_read(path: string) -> string",
        "读取整个文件内容；失败返回空串（可用 file_exists 判断是否存在）"}},
      {"file_write",
       {"file_write(path: string, content: string) -> int",
        "覆写写入文件，返回写入字节数；失败返回 -1"}},
      {"file_exists",
       {"file_exists(path: string) -> bool", "判断文件是否存在（可读）"}},
      {"system",
       {"system(cmd: string) -> int", "执行 shell 命令，返回退出码"}},
  };
  return table;
}

// markdown 组装：代码块 + 分隔线 + 结构化细节
std::string hover_markdown(const DeclInfo &d) {
  std::string md = "```vx\n" + d.signature + "\n```";
  if (!d.params.empty()) {
    md += "\n---\n**参数**\n";
    for (const auto &[n, t] : d.params) {
      md += "- `" + n + ": " + t + "`\n";
    }
    if (!d.ret.empty()) {
      md += "\n**返回** `" + d.ret + "`\n";
    }
  } else if (d.kind == "class" || d.kind == "struct") {
    md += "\n---\n**成员**\n";
    for (const auto &m : d.members) {
      md += "- `" + m + "`\n";
    }
  } else if (d.kind == "var") {
    md += "\n---\n";
    md += d.ret.empty() ? "**类型** 由初始化表达式推断\n"
                        : "**类型** `" + d.ret + "`\n";
    if (d.is_mut) md += "**可变**（mut）\n";
    if (!d.owner.empty()) md += "**" + d.owner + "**\n";
    if (!d.init.empty()) md += "**初始值** `" + d.init + "`\n";
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
    } else if ((u & 0xE0) == 0xC0) {
      i += 2;
      units += 1;
    } else if ((u & 0xF0) == 0xE0) {
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
    if (nl == std::string_view::npos) return start;
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
  while (ps > 0 && comp_ident_char(ln[ps - 1])) --ps;
  c.prefix = std::string(ln.substr(ps, c.byte - ps));

  std::string_view head = ln.substr(0, ps); // prefix 之前的文本
  while (!head.empty() &&
         (head.back() == ' ' || head.back() == '\t' || head.back() == '\r'))
    head.remove_suffix(1);
  c.statement_pos = c.prefix.empty() && head.empty();

  // 成员上下文：head 以 '.' 结尾 → 接收者为点前标识符
  if (!head.empty() && head.back() == '.') {
    const std::size_t end = head.size() - 1;
    std::size_t beg = end;
    while (beg > 0 && comp_ident_char(head[beg - 1])) --beg;
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
  int paren = 0, brace = 0;
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
    } else if (ch == '=' && paren == 0 && brace == 0 &&
               top_eq == std::string_view::npos &&
               !(i + 1 < head.size() && head[i + 1] == '=') &&
               !(i > 0 && head[i - 1] == '=')) {
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
      const bool is_var = decl.starts_with("var ") || decl.starts_with("mut ") ||
                          decl == "var" || decl == "mut";
      if (is_var && (top_eq == std::string_view::npos || top_eq > colon)) {
        c.kind = CompCtx::Type; // var x: |
        return c;
      }
      // 单个非关键字标识符独占一行的冒号 → 字段注解（`x: |`）
      const bool bare_ident =
          !decl.empty() &&
          std::all_of(decl.begin(), decl.end(), comp_ident_char);
      if (bare_ident && !vx::compiler::is_keyword(decl)) {
        c.kind = CompCtx::Type;
        return c;
      }
    }
  }
  return c;
}

// 可调用项 snippet：name(${1:a}, ${2:b})；无参 → name()
std::string snippet_from_params(std::string_view name,
                                std::span<const vx::compiler::Param> params) {
  std::string s(name);
  s += "(";
  for (std::size_t i = 0; i < params.size(); ++i) {
    if (i) s += ", ";
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
    const std::string inner = close == std::string_view::npos
                                  ? std::string()
                                  : std::string(sig.substr(open + 1, close - open - 1));
    int idx = 1;
    bool any = false;
    std::size_t start = 0;
    while (start <= inner.size()) {
      const std::size_t comma = inner.find(", ", start);
      const std::string param = comma == std::string_view::npos
                                    ? inner.substr(start)
                                    : inner.substr(start, comma - start);
      const std::size_t colon = param.find(':');
      const std::string pname = colon == std::string_view::npos
                                    ? param
                                    : param.substr(0, colon);
      if (!pname.empty()) {
        if (any) s += ", ";
        s += "${" + std::to_string(idx++) + ":" + pname + "}";
        any = true;
      }
      if (comma == std::string_view::npos) break;
      start = comma + 2;
    }
  }
  s += ")";
  return s;
}

// 关键字项（两种收集路径共用）：语句关键字在行首位置排最前
void emit_keyword_items(std::vector<LspCompletion> &items, bool stmt_pos) {
  static const char *kStmt[] = {"if",     "elif", "else",   "for",
                                "while",  "break", "continue", "return",
                                "var",    "mut",   "func",   "extern",
                                "class",  "struct", "import", "export"};
  static const char *kExpr[] = {"true", "false", "nil",  "and", "or",
                                "not",  "new",   "as",   "in",  "const"};
  for (const char *kw : kStmt) {
    items.push_back({kw, 14 /*Keyword*/, "关键字", {}, {}, {},
                     stmt_pos ? "0" : "3", false});
  }
  for (const char *kw : kExpr) {
    items.push_back(
        {kw, 14 /*Keyword*/, "关键字", {}, {}, {}, "3", false});
  }
  for (const char *d : {"@imp", "@cppimp"}) {
    items.push_back(
        {d, 14 /*Keyword*/, "导入注解", {}, {}, {}, stmt_pos ? "0" : "3", false});
  }
}

// 内置运行时项（builtin_docs 表），带签名与文档
void emit_builtin_items(std::vector<LspCompletion> &items, bool snippet) {
  for (const auto &[name, doc] : builtin_docs()) {
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
std::vector<LspCompletion> collect_fallback(std::string_view text,
                                            const CursorContext &ctx,
                                            bool snippet) {
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
  Cls *cur = nullptr; // 当前所处的 class/struct（depth==1 内）
  int depth = 0;
  bool in_paren = false;

  auto tok_at = [&](std::size_t i) -> const Token * {
    return i < toks.size() ? &toks[i] : nullptr;
  };
  auto is_name = [](const Token *t) {
    return t != nullptr && (t->kind == Tok::Ident || t->kind == Tok::Kw);
  };

  // 解析形参列表：j 指向 '('；填充 params_text，返回 ')' 之后的索引
  auto scan_params = [&](std::size_t j, std::string &params_text,
                         std::string &ret_text) -> std::size_t {
    std::string out;
    while (const Token *t = tok_at(j)) {
      if (t->kind == Tok::RParen) {
        ++j;
        break;
      }
      if (t->kind == Tok::Eof || t->kind == Tok::Nl) break;
      if (is_name(t)) {
        const std::string pname(t->text);
        std::string ptype;
        std::size_t nj = j + 1;
        if (const Token *c = tok_at(nj); c != nullptr && c->kind == Tok::Colon) {
          if (const Token *pt = tok_at(nj + 1); is_name(pt)) {
            ptype = std::string(pt->text);
            nj += 2;
          } else {
            nj += 1;
          }
        }
        if (!out.empty()) out += ", ";
        out += pname;
        if (!ptype.empty()) out += ": " + ptype;
        j = nj;
        continue;
      }
      ++j;
    }
    params_text = out;
    // 可选返回类型 `-> T`
    if (const Token *a = tok_at(j); a != nullptr && a->kind == Tok::Arrow) {
      if (const Token *rt = tok_at(j + 1); is_name(rt)) {
        ret_text = std::string(rt->text);
        j += 2;
      }
    }
    return j;
  };

  for (std::size_t i = 0; i < toks.size(); ++i) {
    const Token &t = toks[i];
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
    if (t.kind == Tok::LParen) in_paren = true;
    if (t.kind == Tok::RParen) in_paren = false;
    if (t.kind != Tok::Kw) continue;
    const std::string kw(t.text);

    if (kw == "func" || kw == "extern") {
      const Token *name_t = tok_at(i + 1);
      if (!is_name(name_t) || tok_at(i + 2) == nullptr ||
          tok_at(i + 2)->kind != Tok::LParen)
        continue;
      const std::string name(name_t->text);
      std::string params, ret;
      const std::size_t after = scan_params(i + 3, params, ret);
      std::string detail = kw + " " + name + "(" + params + ")";
      if (!ret.empty()) detail += " -> " + ret;
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
      const Token *name_t = tok_at(i + 1);
      if (!is_name(name_t)) continue;
      classes.push_back({});
      cur = &classes.back();
      cur->name = std::string(name_t->text);
      cur->is_class = (kw == "class");
      cur->detail = kw + " " + cur->name;
      continue;
    }
    if (kw == "var" || kw == "mut") {
      const Token *name_t = tok_at(i + 1);
      if (!is_name(name_t) || in_paren) continue;
      // 跳过形参/类型位置的误命中：下一个应为标识符且其后非 '('（类型名带 ( 的情形不存在，直接接受）
      std::string detail(kw + " " + std::string(name_t->text));
      if (const Token *c = tok_at(i + 2); c != nullptr && c->kind == Tok::Colon) {
        if (const Token *ty = tok_at(i + 3); is_name(ty)) {
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
      const Token *name_t = tok_at(i + 1);
      if (!is_name(name_t)) continue;
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
  Cls *owner = nullptr;
  std::size_t class_idx = 0;
  for (std::size_t i = 0; i < toks.size(); ++i) {
    const Token &t = toks[i];
    if (t.kind == Tok::Indent) {
      ++depth;
      continue;
    }
    if (t.kind == Tok::Dedent) {
      --depth;
      continue;
    }
    if (t.kind == Tok::Kw && (t.text == "class" || t.text == "struct") &&
        depth == 0) {
      owner = class_idx < classes.size() ? &classes[class_idx] : nullptr;
      ++class_idx;
      continue;
    }
    if (depth != 1 || t.kind != Tok::Ident) continue;
    const Token *c = tok_at(i + 1);
    if (c == nullptr || c->kind != Tok::Colon) continue;
    // 形参误命中：所在行内前面出现 '(' 则跳过
    bool skip = false;
    for (std::size_t k = i; k > 0 && toks[k].kind != Tok::Nl; --k) {
      if (toks[k].kind == Tok::LParen) {
        skip = true;
        break;
      }
    }
    if (skip || owner == nullptr) continue;
    std::string ftype;
    if (const Token *ty = tok_at(i + 2); is_name(ty)) ftype = std::string(ty->text);
    owner->fields.emplace_back(
        std::string(t.text),
        ftype.empty() ? std::string(t.text) + ": var"
                      : std::string(t.text) + ": " + ftype);
  }

  emit_builtin_items(items, snippet);
  emit_keyword_items(items, stmt_pos);

  if (ctx.kind == CompCtx::Type) {
    std::vector<LspCompletion> out;
    for (const char *t : {"int", "float", "bool", "string", "void", "ptr"}) {
      out.push_back({t, 14, "类型", {}, {}, {}, "0", false});
    }
    for (const Cls &cl : classes) {
      out.push_back({cl.name, cl.is_class ? 7 : 22, cl.detail, {}, {}, {}, "0",
                     false});
    }
    return out;
  }
  if (ctx.kind == CompCtx::Member) {
    std::vector<LspCompletion> out;
    auto emit_cls = [&](const Cls &cl) {
      for (const auto &[n, d] : cl.fields)
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
    for (const Cls &cl : classes) {
      if (cl.name == ctx.receiver) {
        emit_cls(cl);
        matched = true;
      }
    }
    if (!matched) {
      for (const Cls &cl : classes) emit_cls(cl); // 回退：全部类成员
    }
    return out;
  }
  return items;
}

// 从（修补后）AST 收集补全项。需要 arena 存活于本函数期间
//（Param/字段等 span 指向 arena；产物字符串均自有）。
std::vector<LspCompletion> collect_from_prog(
    std::string_view text, const CursorContext &ctx,
    const vx::compiler::Program &prog, const std::filesystem::path &headers_root,
    bool snippet) {
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

  const auto &stmts = prog.stmts;
  const int cursor1 = ctx.cursor_line;
  std::map<std::string, TypeEntry> types;
  std::map<std::string, Callable> funcs;
  std::map<std::string, VarInfo> top_vars;

  auto next_top_line = [&](std::size_t i) -> int {
    return (i + 1 < stmts.size()) ? stmts[i + 1]->line : INT_MAX;
  };

  // 第一遍：顶层 func/extern + class/struct
  for (std::size_t i = 0; i < stmts.size(); ++i) {
    const Stmt *s = stmts[i];
    switch (s->kind) {
    case NodeKind::FuncDecl: {
      const auto *f = static_cast<const FuncDecl *>(s);
      funcs[std::string(f->name)] =
          Callable{func_signature("func", f->name, f->params, f->ret_type),
                   std::string(f->ret_type), doc_above(text, f->line),
                   f->params};
      break;
    }
    case NodeKind::ExternDecl: {
      const auto *e = static_cast<const ExternDecl *>(s);
      funcs[std::string(e->name)] =
          Callable{func_signature("extern", e->name, e->params, e->ret_type),
                   std::string(e->ret_type), doc_above(text, e->line),
                   e->params};
      break;
    }
    case NodeKind::ClassDecl: {
      const auto *c = static_cast<const ClassDecl *>(s);
      TypeEntry te;
      te.name = std::string(c->name);
      te.kind = 7;
      te.detail = "class " + te.name;
      te.doc = doc_above(text, c->line);
      for (const ClassField &fl : c->fields) {
        te.fields.emplace_back(std::string(fl.name),
                               std::string(fl.name) + ": " +
                                   (fl.type.empty() ? "var" : std::string(fl.type)));
      }
      for (const FuncDecl *m : c->methods) {
        te.methods.push_back(Method{
            std::string(m->name),
            func_signature("func", m->name, m->params, m->ret_type),
            doc_above(text, m->line), m->params});
      }
      types[te.name] = std::move(te);
      break;
    }
    case NodeKind::StructDecl: {
      const auto *st = static_cast<const StructDecl *>(s);
      TypeEntry te;
      te.name = std::string(st->name);
      te.kind = 22;
      te.detail = "struct " + te.name;
      te.doc = doc_above(text, st->line);
      for (const ClassField &fl : st->fields) {
        te.fields.emplace_back(std::string(fl.name),
                               std::string(fl.name) + ": " +
                                   (fl.type.empty() ? "var" : std::string(fl.type)));
      }
      types[te.name] = std::move(te);
      break;
    }
    default:
      break;
    }
  }

  // 第二遍：顶层 var/mut（初始化表达式可推断类型）
  auto type_of_init = [&](const Expr *init) -> std::string {
    if (init == nullptr) return {};
    if (init->kind == NodeKind::New)
      return std::string(static_cast<const New *>(init)->class_name);
    if (init->kind == NodeKind::Call) {
      const auto *call = static_cast<const Call *>(init);
      if (call->callee->kind == NodeKind::Ident) {
        const auto fn =
            funcs.find(std::string(static_cast<const Ident *>(call->callee)->name));
        if (fn != funcs.end()) return fn->second.ret;
      }
    }
    return {};
  };
  for (const Stmt *s : stmts) {
    if (s->kind != NodeKind::VarDecl) continue;
    const auto *v = static_cast<const VarDecl *>(s);
    VarInfo info;
    std::string detail = v->is_mut ? "mut " : "var ";
    detail += std::string(v->name);
    if (!v->type_ann.empty()) detail += ": " + std::string(v->type_ann);
    info.detail = detail;
    info.type = v->type_ann.empty() ? type_of_init(v->init)
                                    : std::string(v->type_ann);
    top_vars[std::string(v->name)] = std::move(info);
  }

  // 第三遍：确定光标所在的作用域（顶层函数 / 类方法），收集可见局部
  std::map<std::string, VarInfo> locals;
  std::string method_class; // 光标所在方法的宿主类（self 可用）
  auto add_params = [&](std::span<const Param> ps) {
    for (const Param &p : ps) {
      locals[std::string(p.name)] = VarInfo{
          "参数 " + (p.type.empty() ? std::string("var") : std::string(p.type)),
          std::string(p.type)};
    }
  };
  std::function<void(std::span<const Stmt *const>)> gather = nullptr;
  gather = [&](std::span<const Stmt *const> body) {
    for (const Stmt *s : body) {
      if (s->line > cursor1) continue; // 声明尚未到达光标
      switch (s->kind) {
      case NodeKind::VarDecl: {
        const auto *v = static_cast<const VarDecl *>(s);
        VarInfo info;
        std::string detail = v->is_mut ? "mut " : "var ";
        detail += std::string(v->name);
        if (!v->type_ann.empty()) detail += ": " + std::string(v->type_ann);
        info.detail = detail;
        info.type =
            v->type_ann.empty() ? type_of_init(v->init) : std::string(v->type_ann);
        locals[std::string(v->name)] = std::move(info);
        break;
      }
      case NodeKind::For: {
        const auto *f = static_cast<const For *>(s);
        locals[std::string(f->var)] = VarInfo{"循环变量", {}};
        gather(f->body);
        break;
      }
      case NodeKind::If: {
        const auto *in = static_cast<const If *>(s);
        gather(in->then_body);
        for (const ElifArm &arm : in->elifs) gather(arm.body);
        gather(in->else_body);
        break;
      }
      case NodeKind::While: {
        const auto *w = static_cast<const While *>(s);
        gather(w->body);
        break;
      }
      default:
        break;
      }
    }
  };
  for (std::size_t i = 0; i < stmts.size(); ++i) {
    const Stmt *s = stmts[i];
    const int span_end = next_top_line(i);
    if (cursor1 < s->line || cursor1 >= span_end) continue;
    if (s->kind == NodeKind::FuncDecl) {
      const auto *f = static_cast<const FuncDecl *>(s);
      add_params(f->params);
      gather(f->body);
    } else if (s->kind == NodeKind::ClassDecl) {
      const auto *c = static_cast<const ClassDecl *>(s);
      for (const FuncDecl *m : c->methods) {
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
    for (const char *t : {"int", "float", "bool", "string", "void", "ptr"}) {
      items.push_back({t, 14 /*Keyword*/, "类型", {}, {}, {}, "0", false});
    }
    for (const auto &[name, te] : types) {
      items.push_back({name, te.kind, te.detail, te.doc, {}, {}, "0", false});
    }
    return items;
  }

  // ---- 成员位置：导入库符号优先，其次类/结构成员 ----
  if (ctx.kind == CompCtx::Member) {
    if (!headers_root.empty()) {
      DiagBag ibag;
      ImportContext ictx{headers_root, headers_root};
      if (auto table = build_import_table(prog, ictx, "<document>", ibag)) {
        if (const ImportedLib *lib = table->lib_by_prefix(ctx.receiver)) {
          for (const ImportedSymbol &sym : lib->symbols) {
            LspCompletion c;
            c.label = sym.vx_name;
            c.kind = 3 /*Function*/;
            std::string params;
            for (std::size_t pi = 0; pi < sym.params.size(); ++pi) {
              if (pi) params += ", ";
              params += sym.params[pi].name + ": " + sym.params[pi].type;
            }
            c.detail = lib->name + "." + sym.vx_name + "(" + params + ")";
            if (!sym.ret_type.empty() && sym.ret_type != "void")
              c.detail += " -> " + sym.ret_type;
            c.documentation = "**导入库** `" + lib->name + "` · **VX ABI** `" +
                              sym.abi_symbol + "`";
            c.sort_text = "0" + c.label;
            if (snippet) {
              c.is_snippet = true;
              c.insert_text = snippet_from_sig(sym.vx_name, "(" + params + ")");
              c.filter_text = c.label;
            }
            items.push_back(std::move(c));
          }
          if (!items.empty()) return items; // 库命中 → 只给库符号
        }
      }
    }

    auto emit_members = [&](const TypeEntry &te) {
      for (const auto &[n, d] : te.fields) {
        items.push_back({n, 5 /*Field*/, d, te.doc, {}, {}, "0", false});
      }
      for (const Method &m : te.methods) {
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
    } else if (types.count(ctx.receiver)) {
      resolved = ctx.receiver;
      found = true;
    } else if (const auto lt = locals.find(ctx.receiver); lt != locals.end()) {
      found = true;
      resolved = lt->second.type;
    } else if (const auto tt = top_vars.find(ctx.receiver);
               tt != top_vars.end()) {
      found = true;
      resolved = tt->second.type;
    } else if (const auto ft = funcs.find(ctx.receiver); ft != funcs.end()) {
      found = true;
      resolved = ft->second.ret;
    }

    static const std::map<std::string_view, int> kPrimitives = {
        {"int", 1},  {"float", 1}, {"bool", 1},
        {"string", 1}, {"void", 1}, {"ptr", 1}, {"nil", 1}};
    if (found && !resolved.empty() && !kPrimitives.count(resolved)) {
      if (const auto te = types.find(resolved); te != types.end()) {
        emit_members(te->second);
        if (!items.empty()) return items;
      }
      found = false; // 解析到未知类型 → 走全类回退
    } else if (found && kPrimitives.count(resolved)) {
      return items; // 基础类型无成员
    }
    if (!found) {
      for (const auto &[name, te] : types) emit_members(te); // 启发式回退
    }
    return items;
  }

  // ---- 一般位置：作用域 + 顶层声明 + 内置 + 关键字 ----
  const bool stmt_pos = ctx.statement_pos;
  for (const auto &[name, info] : locals) {
    items.push_back({name, 6 /*Variable*/, info.detail, {}, {}, {},
                     stmt_pos ? "2" : "0", false});
  }
  for (const auto &[name, fn] : funcs) {
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
  for (const auto &[name, te] : types) {
    items.push_back({name, te.kind, te.detail, te.doc, {}, {}, "1", false});
  }
  for (const auto &[name, info] : top_vars) {
    items.push_back({name, 6 /*Variable*/, info.detail, {}, {}, {}, "1", false});
  }
  emit_builtin_items(items, snippet);
  emit_keyword_items(items, stmt_pos);
  return items;
}

} // namespace

std::vector<LspCompletion> completion_at(std::string_view text, int line,
                                         int character,
                                         const std::filesystem::path &headers_root,
                                         bool snippet_support) {
  const CursorContext ctx = detect_context(text, line, character);
  std::vector<LspCompletion> raw;

  // 解析尝试：原文 → 光标处插入占位标识符（修复残缺 `接收者.` 等）→ 删除当前行
  bool parsed = false;
  auto attempt = [&](std::string &src) -> bool {
    vx::util::Arena arena;
    vx::compiler::DiagBag bag;
    auto toks = vx::compiler::tokenize(src, "<document>", arena, bag);
    auto prog = vx::compiler::parse(toks, "<document>", arena, bag);
    if (!prog) return false;
    raw = collect_from_prog(text, ctx, *prog, headers_root, snippet_support);
    return true;
  };
  {
    std::string src(text);
    parsed = attempt(src);
  }
  if (!parsed) {
    std::string src(text);
    const std::size_t at = std::min(ctx.line_start + ctx.byte, src.size());
    src.insert(at, "__vxcmp__");
    parsed = attempt(src);
  }
  if (!parsed) {
    std::string src(text);
    const std::size_t e = text.find('\n', ctx.line_start);
    if (e == std::string_view::npos) {
      src.erase(ctx.line_start);
    } else {
      src.erase(ctx.line_start, e - ctx.line_start + 1);
    }
    parsed = attempt(src);
  }
  if (!parsed) raw = collect_fallback(text, ctx, snippet_support);

  // 前缀过滤 + 按 label 去重（保留 sort 更小者）+ 排序
  std::map<std::string, LspCompletion> best;
  for (LspCompletion &c : raw) {
    if (!ctx.prefix.empty() && !c.label.starts_with(ctx.prefix)) continue;
    auto [it, inserted] = best.emplace(c.label, c);
    if (!inserted && c.sort_text < it->second.sort_text) it->second = std::move(c);
  }
  std::vector<LspCompletion> out;
  out.reserve(best.size());
  for (auto &[label, c] : best) out.push_back(std::move(c));
  std::sort(out.begin(), out.end(),
            [](const LspCompletion &a, const LspCompletion &b) {
              return a.sort_text != b.sort_text ? a.sort_text < b.sort_text
                                                : a.label < b.label;
            });
  return out;
}

// 导入库符号 hover：光标在 `库前缀.符号` 的符号上时返回库信息 markdown。
// 需要 headers_root 定位 @imp 库；失败/未命中返回空。
std::string lib_symbol_hover(std::string_view text, int line, int character,
                             const std::filesystem::path &headers_root) {
  if (headers_root.empty()) return {};
  vx::util::Arena arena;
  vx::compiler::DiagBag bag;
  auto toks = vx::compiler::tokenize(text, "<document>", arena, bag);
  // 定位光标处 token，并检查其前是否为 `Ident .`
  std::size_t idx = std::string_view::npos;
  std::string name;
  // 用同一 token 流做位置判定（避免重复 tokenize）
  for (std::size_t i = 0; i < toks.size(); ++i) {
    const Token &t = toks[i];
    if (t.kind != Tok::Ident && t.kind != Tok::Kw) continue;
    const LspPosition start = diag_pos_to_lsp(text, t.line, t.col);
    const LspPosition end = diag_pos_to_lsp(
        text, t.line, t.col + static_cast<int>(utf8_len(t.text)));
    if (line != start.line && line != end.line) continue;
    const int from = line == start.line ? start.character : 0;
    const int to = line == end.line ? end.character : 1 << 20;
    if (character >= from && character <= to) {
      idx = i;
      name = std::string(t.text);
      break;
    }
  }
  if (idx == std::string_view::npos || idx < 2) return {};
  if (toks[idx - 1].kind != Tok::Dot || toks[idx - 2].kind != Tok::Ident) {
    return {};
  }
  const std::string prefix(toks[idx - 2].text);
  auto prog = vx::compiler::parse(toks, "<document>", arena, bag);
  if (!prog) return {};
  vx::compiler::ImportContext ctx{headers_root, headers_root};
  auto table = vx::compiler::build_import_table(*prog, ctx, "<document>", bag);
  if (!table) return {};
  const vx::compiler::ImportedLib *lib = table->lib_by_prefix(prefix);
  if (lib == nullptr) return {};
  const vx::compiler::ImportedSymbol *sym =
      vx::compiler::ImportTable::find_symbol(*lib, name);
  if (sym == nullptr) return {};

  std::string md = "```vx\n" + std::string(lib->name) + "." + sym->vx_name;
  md += "(";
  for (std::size_t i = 0; i < sym->params.size(); ++i) {
    if (i) md += ", ";
    md += sym->params[i].name + ": " + sym->params[i].type;
  }
  md += ")";
  if (!sym->ret_type.empty() && sym->ret_type != "void") {
    md += " -> " + sym->ret_type;
  }
  md += "\n```\n---\n";
  md += "**导入库** `" + lib->name + "`";
  if (lib->from_cpp) md += "（C++）";
  md += "\n**VX ABI** `" + sym->abi_symbol + "`\n";
  md += "**来源** `" + lib->decl_file + "`\n";
  return md;
}

std::string hover_at(std::string_view text, int line, int character,
                     const std::filesystem::path &headers_root) {
  std::string name;
  int tl = 0, tc = 0;
  if (!ident_token_at(text, line, character, name, tl, tc)) return {};

  // 优先：导入库符号（库前缀.成员 形态，需 headers_root）
  const std::string lib_md = lib_symbol_hover(text, line, character, headers_root);
  if (!lib_md.empty()) return lib_md;

  // 内置运行时符号
  if (const auto it = builtin_docs().find(name); it != builtin_docs().end()) {
    return "```vx\n" + std::string(it->second.sig) + "\n```\n---\n**内置运行时**\n" +
           std::string(it->second.desc) + "\n";
  }

  DeclInfo info;
  if (!find_decl(text, name, info)) return {};
  return hover_markdown(info);
}

LspRange definition_at(std::string_view text, int line, int character) {
  LspRange bad;
  bad.start = {-1, 0};
  bad.end = {-1, 0};
  std::string name;
  int tl = 0, tc = 0;
  if (!ident_at(text, line, character, name, tl, tc)) return bad;
  DeclInfo info;
  if (!find_decl(text, name, info)) return bad;
  LspRange r;
  r.start = {info.line - 1, static_cast<int>(info.name_col)};
  r.end = {info.line - 1,
           static_cast<int>(info.name_col + utf16_len(name))};
  return r;
}

std::vector<LspDiagnostic> tidy_diagnostics(std::string_view text,
                                            std::string_view filename) {
  std::vector<LspDiagnostic> out;
  for (const vxtidy::Finding &f : vxtidy::check(text, filename)) {
    if (f.rule == "VXTIDY900") continue; // 语法错误由前端诊断负责
    LspDiagnostic d;
    d.severity = f.severity;
    d.code = f.rule;
    d.message = f.message;
    d.source = "vx-tidy";
    // VXTIDY 语义规则多为行级（col=0）：range 覆盖整行便于编辑器高亮
    if (f.col > 0) {
      d.range.start = diag_pos_to_lsp(text, f.line, f.col);
      d.range.end = d.range.start;
      d.range.end.character += 1;
    } else {
      const std::string_view ln = line_text(text, f.line);
      d.range.start = {f.line - 1, 0};
      d.range.end = {f.line - 1, static_cast<int>(utf16_len(ln))};
      if (d.range.end.character <= d.range.start.character) {
        d.range.end.character = d.range.start.character + 1;
      }
    }
    out.push_back(std::move(d));
  }
  return out;
}

} // namespace vxls
