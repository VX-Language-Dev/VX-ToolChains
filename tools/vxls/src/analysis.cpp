// analysis.cpp — vxls 文档分析实现：lexer/parser → LSP 诊断/符号/补全/悬停。
#include "analysis.hpp"

#include "tidy.hpp"
#include "vx/compiler/importer.hpp"
#include "vx/compiler/lexer.hpp"
#include "vx/compiler/parser.hpp"

#include <functional>
#include <map>

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
  a.parse_ok = prog.has_value();
  a.diags = bag.items();

  if (prog) {
    for (const Stmt *s : prog->stmts) {
      collect_symbol(s, text, a.symbols);
    }
  }

  // 补全：关键字 + 文档符号 + 内置运行时
  static const char *kKeywords[] = {
      "if", "elif", "else", "for", "while", "break", "continue", "func",
      "return", "true", "false", "nil", "var", "mut", "import", "struct",
      "class", "new", "extern", "int", "float", "bool", "void", "and", "or",
      "not", "as", "in", "const", "export"};
  for (const char *kw : kKeywords) {
    a.completions.push_back({kw, 14 /*Keyword*/, "关键字"});
  }
  for (const LspSymbol &s : a.symbols) {
    const int kind = (s.kind == 5 || s.kind == 23) ? 7 /*Class*/ : s.kind;
    a.completions.push_back({s.name, kind, s.detail});
  }
  static const char *kBuiltins[] = {"out", "out_int", "out_bool", "out_float",
                                    "puts", "malloc", "free", "exit"};
  for (const char *b : kBuiltins) {
    a.completions.push_back({b, 3 /*Function*/, "内置运行时"});
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
