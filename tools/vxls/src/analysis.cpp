// analysis.cpp — vxls 文档分析实现：lexer/parser → LSP 诊断/符号/补全/悬停。
// 各功能实现拆分于 analysis_*.hpp；本文件为主入口（公共 API 实现）。
#include "analysis.hpp"

#include "analysis_completion.hpp"
#include "analysis_hover.hpp"
#include "analysis_inlay.hpp"
#include "analysis_text.hpp"
#include "analysis_tidy.hpp"

#include <algorithm>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <filesystem>

namespace vxls {

using vx::compiler::Diagnostic;
using vx::compiler::EnumMember;
using vx::compiler::NodeKind;
using vx::compiler::Severity;
using vx::compiler::Stmt;
using vx::compiler::Tok;
using vx::compiler::Token;

LspPosition diag_pos_to_lsp(std::string_view text, int line, int col) {
  if (line < 1)
    return {0, 0};
  const std::string_view l = line_text(text, line);
  // col 为 1 起始码点列 → 取前 col-1 个码点的 UTF-16 宽度
  std::size_t points = 0;
  std::size_t i = 0;
  while (i < l.size() && points + 1 < static_cast<std::size_t>(col)) {
    const auto u = static_cast<unsigned char>(l[i]);
    std::size_t adv = 1;
    if ((u & 0xE0u) == 0xC0u)
      adv = 2;
    else if ((u & 0xF0u) == 0xE0u)
      adv = 3;
    else if ((u & 0xF8u) == 0xF0u)
      adv = 4;
    i += adv;
    ++points;
  }
  return {line - 1, static_cast<int>(utf16_len(l.substr(0, i)))};
}

std::size_t lsp_pos_to_offset(std::string_view text, LspPosition pos) {
  // 定位到 pos.line 行首
  std::size_t start = 0;
  int cur = 0;
  while (cur < pos.line) {
    const std::size_t nl = text.find('\n', start);
    if (nl == std::string_view::npos) {
      start = text.size();
      break;
    }
    start = nl + 1;
    ++cur;
  }
  start = std::min(start, text.size());
  // 行尾
  std::size_t line_end = text.find('\n', start);
  if (line_end == std::string_view::npos)
    line_end = text.size();
  // 在行内推进 pos.character 个 UTF-16 码元
  std::size_t i = start;
  int utf16 = 0;
  while (i < line_end && utf16 < pos.character) {
    const auto c = static_cast<unsigned char>(text[i]);
    std::size_t cp_len = 1;
    if ((c & 0x80u) == 0)
      cp_len = 1;
    else if ((c & 0xE0u) == 0xC0u)
      cp_len = 2;
    else if ((c & 0xF0u) == 0xE0u)
      cp_len = 3;
    else if ((c & 0xF8u) == 0xF0u)
      cp_len = 4;
    int add = (cp_len == 4) ? 2 : 1;
    if (utf16 + add > pos.character)
      break;
    if (i + cp_len > line_end)
      break;
    i += cp_len;
    utf16 += add;
  }
  i = std::min(i, line_end);
  return i;
}

LspDiagnostic to_lsp_diagnostic(const Diagnostic& d, std::string_view text) {
  LspDiagnostic out;
  out.severity = d.severity == Severity::Error ? 1 : d.severity == Severity::Warning ? 2 : 3;
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
    for (const Stmt* s : prog->stmts) {
      collect_symbol(s, text, a.symbols);
    }
  }

  return a;
}

std::vector<LspCompletion> completion_at(std::string_view text, int line, int character,
                                         const std::filesystem::path& headers_root,
                                         const std::vector<std::filesystem::path>& include_dirs, bool snippet_support) {
  const CursorContext ctx = detect_context(text, line, character);
  std::vector<LspCompletion> raw;

  // 解析尝试：原文 → 光标处插入占位标识符（修复残缺 `接收者.` 等）→ 删除当前行
  bool parsed = false;
  auto attempt = [&](std::string& src) -> bool {
    vx::util::Arena arena;
    vx::compiler::DiagBag bag;
    auto toks = vx::compiler::tokenize(src, "<document>", arena, bag);
    auto prog = vx::compiler::parse(toks, "<document>", arena, bag);
    if (!prog)
      return false;
    raw = collect_from_prog(text, ctx, *prog, headers_root, include_dirs, snippet_support);
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
  if (!parsed)
    raw = collect_fallback(text, ctx, snippet_support);

  // 前缀过滤 + 按 label 去重（保留 sort 更小者）+ 排序
  std::map<std::string, LspCompletion> best;
  for (LspCompletion& c : raw) {
    if (!ctx.prefix.empty() && !c.label.starts_with(ctx.prefix))
      continue;
    auto [it, inserted] = best.emplace(c.label, c);
    if (!inserted && c.sort_text < it->second.sort_text)
      it->second = std::move(c);
  }
  std::vector<LspCompletion> out;
  out.reserve(best.size());
  for (auto& [label, c] : best)
    out.push_back(std::move(c));
  std::ranges::sort(out, [](const LspCompletion& a, const LspCompletion& b) {
    return a.sort_text != b.sort_text ? a.sort_text < b.sort_text : a.label < b.label;
  });
  return out;
}

// 导入库符号 hover：光标在 `库前缀.符号` 的符号上时返回库信息 markdown。
// 需要 headers_root 定位 @imp 库；失败/未命中返回空。
std::string lib_symbol_hover(std::string_view text, int line, int character, const std::filesystem::path& headers_root,
                             const std::vector<std::filesystem::path>& include_dirs) {
  if (headers_root.empty())
    return {};
  vx::util::Arena arena;
  vx::compiler::DiagBag bag;
  auto toks = vx::compiler::tokenize(text, "<document>", arena, bag);
  // 定位光标处 token，并检查其前是否为 `Ident .`
  std::size_t idx = std::string_view::npos;
  std::string name;
  // 用同一 token 流做位置判定（避免重复 tokenize）
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
      idx = i;
      name = std::string(t.text);
      break;
    }
  }
  if (idx == std::string_view::npos || idx < 2)
    return {};
  if (toks[idx - 1].kind != Tok::Dot || toks[idx - 2].kind != Tok::Ident) {
    return {};
  }
  const std::string prefix(toks[idx - 2].text);
  auto prog = vx::compiler::parse(toks, "<document>", arena, bag);
  if (!prog)
    return {};
  vx::compiler::ImportContext ctx{headers_root, headers_root, include_dirs, {}};
  auto table = vx::compiler::build_import_table(*prog, ctx, "<document>", bag);
  if (!table)
    return {};
  const vx::compiler::ImportedLib* lib = table->lib_by_prefix(prefix);
  if (lib == nullptr)
    return {};
  const vx::compiler::ImportedSymbol* sym = vx::compiler::ImportTable::find_symbol(*lib, name);
  if (sym == nullptr)
    return {};

  std::string md = "```vx\n" + std::string(lib->name) + "." + sym->vx_name;
  md += "(";
  for (std::size_t i = 0; i < sym->params.size(); ++i) {
    if (i != 0U)
      md += ", ";
    md += sym->params[i].name + ": " + sym->params[i].type;
  }
  md += ")";
  if (!sym->ret_type.empty() && sym->ret_type != "void") {
    md += " -> " + sym->ret_type;
  }
  md += "\n```\n---\n";
  md += "**导入库** `" + lib->name + "`";
  if (lib->from_cpp)
    md += "（C++）";
  md += "\n**VX ABI** `" + sym->abi_symbol + "`\n";
  md += "**来源** `" + lib->decl_file + "`\n";
  return md;
}

std::string hover_at(std::string_view text, int line, int character, const std::filesystem::path& headers_root,
                     const std::vector<std::filesystem::path>& include_dirs) {
  std::string name;
  int tl = 0;
  int tc = 0;
  if (ident_token_at(text, line, character, name, tl, tc) == 0U)
    return {};

  // 优先：导入库符号（库前缀.成员 形态，需 headers_root）
  const std::string lib_md = lib_symbol_hover(text, line, character, headers_root, include_dirs);
  if (!lib_md.empty())
    return lib_md;

  // 内置运行时符号
  if (const auto it = builtin_docs().find(name); it != builtin_docs().end()) {
    return "```vx\n" + std::string(it->second.sig) + "\n```\n---\n**内置运行时**\n" + std::string(it->second.desc) +
           "\n";
  }

  DeclInfo info;
  if (!find_decl(text, name, info))
    return {};
  return hover_markdown(info);
}

LspRange definition_at(std::string_view text, int line, int character) {
  LspRange bad;
  bad.start = {-1, 0};
  bad.end = {-1, 0};
  std::string name;
  int tl = 0;
  int tc = 0;
  if (!ident_at(text, line, character, name, tl, tc))
    return bad;
  DeclInfo info;
  if (!find_decl(text, name, info))
    return bad;
  LspRange r;
  r.start = {info.line - 1, static_cast<int>(info.name_col)};
  r.end = {info.line - 1, static_cast<int>(info.name_col + utf16_len(name))};
  return r;
}

std::vector<LspDiagnostic> tidy_diagnostics(std::string_view text, std::string_view filename) {
  std::vector<LspDiagnostic> out;
  for (const vxtidy::Finding& f : vxtidy::check(text, filename)) {
    if (f.rule == "VXTIDY900")
      continue; // 语法错误由前端诊断负责
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

std::vector<LspWorkspaceSymbol> workspace_symbols(const std::string& query,
                                                  const std::map<std::string, std::string>& docs) {
  std::vector<LspWorkspaceSymbol> out;
  for (const auto& [uri, text] : docs) {
    Analysis a = analyze(text);
    for (const auto& sym : a.symbols) {
      if (!query.empty() && !contains_ci(sym.name, query))
        continue;
      LspWorkspaceSymbol ws;
      ws.name = sym.name;
      ws.kind = sym.kind;
      ws.uri = uri;
      ws.range = sym.selection_range;
      out.push_back(std::move(ws));
    }
  }
  return out;
}

LspLocation definition_at_cross(const std::string& uri, std::string_view text, int line, int character,
                                const std::map<std::string, std::string>& docs) {
  LspLocation bad{uri, {{-1, 0}, {-1, 0}}};

  // 先在当前文档查找
  std::string name;
  int tl = 0;
  int tc = 0;
  if (ident_at(text, line, character, name, tl, tc)) {
    LspRange r = definition_at(text, line, character);
    if (r.start.line >= 0) {
      LspLocation loc;
      loc.uri = uri;
      loc.range = r;
      return loc;
    }
  }

  // 当前文档未找到，遍历其它文档
  if (name.empty()) {
    // 尝试获取标识符名
    ident_at(text, line, character, name, tl, tc);
  }
  if (name.empty())
    return bad;

  for (const auto& [other_uri, other_text] : docs) {
    if (other_uri == uri)
      continue;
    LspRange r = find_decl_range_in_doc(other_text, name);
    if (r.start.line >= 0) {
      LspLocation loc;
      loc.uri = other_uri;
      loc.range = r;
      return loc;
    }
  }
  return bad;
}

std::vector<LspLocation> references_at(const std::string& uri, std::string_view text, int line, int character,
                                       const std::map<std::string, std::string>& docs, bool include_declaration) {
  (void)uri;
  (void)include_declaration;
  std::vector<LspLocation> out;

  std::string name;
  int tl = 0;
  int tc = 0;
  if (!ident_at(text, line, character, name, tl, tc))
    return out;

  for (const auto& [doc_uri, doc_text] : docs) {
    auto refs = find_ident_refs_in_doc(doc_text, name);
    for (const auto& r : refs) {
      // 如果不包含声明，跳过声明位置
      if (!include_declaration) {
        // 简单启发：声明位置的行与 find_decl_range 匹配时跳过
        // 这里保留全部，由调用方决定
      }
      LspLocation loc;
      loc.uri = doc_uri;
      loc.range = r;
      out.push_back(std::move(loc));
    }
  }
  return out;
}

std::map<std::string, std::vector<LspTextEdit>> rename_at(const std::string& uri, std::string_view text, int line,
                                                          int character, const std::string& new_name,
                                                          const std::map<std::string, std::string>& docs) {
  (void)uri;
  std::map<std::string, std::vector<LspTextEdit>> edits;

  std::string name;
  int tl = 0;
  int tc = 0;
  if (!ident_at(text, line, character, name, tl, tc))
    return edits;

  for (const auto& [doc_uri, doc_text] : docs) {
    auto refs = find_ident_refs_in_doc(doc_text, name);
    std::vector<LspTextEdit> file_edits;
    for (const auto& r : refs) {
      LspTextEdit te;
      te.range = r;
      te.new_text = new_name;
      file_edits.push_back(std::move(te));
    }
    if (!file_edits.empty()) {
      edits[doc_uri] = std::move(file_edits);
    }
  }
  return edits;
}

// ==== P1 新增公共辅助 ====

LspRange ident_range_at(std::string_view text, int line, int character) {
  LspRange bad{{-1, 0}, {-1, 0}};
  vx::util::Arena arena;
  vx::compiler::DiagBag bag;
  auto toks = vx::compiler::tokenize(text, "<document>", arena, bag);
  for (const auto& t : toks) {
    if (t.kind != Tok::Ident && t.kind != Tok::Kw)
      continue;
    const LspPosition start = diag_pos_to_lsp(text, t.line, t.col);
    const LspPosition end = diag_pos_to_lsp(text, t.line, t.col + static_cast<int>(utf8_len(t.text)));
    if (line != start.line && line != end.line)
      continue;
    const int from = line == start.line ? start.character : 0;
    const int to = line == end.line ? end.character : static_cast<int>(1u << 20u);
    if (character >= from && character <= to) {
      LspRange r;
      r.start = start;
      r.end = end;
      return r;
    }
  }
  return bad;
}

std::string ident_name_at(std::string_view text, int line, int character) {
  vx::util::Arena arena;
  vx::compiler::DiagBag bag;
  auto toks = vx::compiler::tokenize(text, "<document>", arena, bag);
  for (const auto& t : toks) {
    if (t.kind != Tok::Ident && t.kind != Tok::Kw)
      continue;
    const LspPosition start = diag_pos_to_lsp(text, t.line, t.col);
    const LspPosition end = diag_pos_to_lsp(text, t.line, t.col + static_cast<int>(utf8_len(t.text)));
    if (line != start.line && line != end.line)
      continue;
    const int from = line == start.line ? start.character : 0;
    const int to = line == end.line ? end.character : static_cast<int>(1u << 20u);
    if (character >= from && character <= to) {
      return std::string(t.text);
    }
  }
  return {};
}

std::vector<LspRange> document_highlights(std::string_view text, int line, int character) {
  std::vector<LspRange> out;
  std::string name = ident_name_at(text, line, character);
  if (name.empty())
    return out;
  vx::util::Arena arena;
  vx::compiler::DiagBag bag;
  auto toks = vx::compiler::tokenize(text, "<document>", arena, bag);
  for (const auto& t : toks) {
    if ((t.kind != Tok::Ident && t.kind != Tok::Kw) || t.text != name)
      continue;
    LspRange r;
    r.start = diag_pos_to_lsp(text, t.line, t.col);
    r.end = diag_pos_to_lsp(text, t.line, t.col + static_cast<int>(utf8_len(t.text)));
    out.push_back(r);
  }
  return out;
}

std::vector<LspInlayHint> inlay_hints(std::string_view text, const std::filesystem::path& headers_root,
                                      const std::vector<std::filesystem::path>& include_dirs,
                                      const std::map<std::string, std::string>& docs, const std::string& uri) {
  std::vector<LspInlayHint> out;
  vx::util::Arena arena;
  vx::compiler::DiagBag bag;
  const auto toks = vx::compiler::tokenize(text, "<document>", arena, bag);
  const auto prog = vx::compiler::parse(toks, "<document>", arena, bag);
  if (!prog)
    return out;

  // 跨文件函数签名：其它打开文档先入表，当前文档覆盖同名项
  SigMap funcs;
  for (const auto& [other_uri, other_text] : docs) {
    if (other_uri == uri)
      continue;
    vx::util::Arena other_arena;
    vx::compiler::DiagBag other_bag;
    const auto other_toks = vx::compiler::tokenize(other_text, "<document>", other_arena, other_bag);
    const auto other_prog = vx::compiler::parse(other_toks, "<document>", other_arena, other_bag);
    if (other_prog)
      collect_top_sigs(*other_prog, funcs, "函数（跨文件）");
  }
  collect_top_sigs(*prog, funcs, "函数");

  std::optional<vx::compiler::ImportTable> imports;
  if (!headers_root.empty()) {
    vx::compiler::DiagBag ibag;
    vx::compiler::ImportContext ictx{headers_root, headers_root, include_dirs, {}};
    imports = vx::compiler::build_import_table(*prog, ictx, "<document>", ibag);
  }

  InlayContext ctx;
  ctx.funcs = &funcs;
  ctx.builtins = &builtin_signatures();
  ctx.imports = imports ? &*imports : nullptr;

  InlayBuilder builder(text, ctx);
  return builder.build(*prog, toks);
}

LspSignatureHelp signature_help_at(std::string_view text, int line, int character,
                                   const std::filesystem::path& /*headers_root*/,
                                   const std::vector<std::filesystem::path>& /*include_dirs*/) {
  LspSignatureHelp help;
  LspPosition pos{line, character};
  std::size_t off = lsp_pos_to_offset(text, pos);
  std::size_t open_pos = std::string_view::npos;
  int depth = 0;
  bool in_str = false;
  for (std::size_t k = off; k-- > 0;) {
    char c = text[k];
    if (in_str) {
      if (c == '"' || c == '\'')
        in_str = false;
      continue;
    }
    if (c == '"' || c == '\'') {
      in_str = true;
      continue;
    }
    if (c == ')') {
      ++depth;
    } else if (c == '(') {
      if (depth == 0) {
        open_pos = k;
        break;
      }
      --depth;
    }
  }
  if (open_pos == std::string_view::npos)
    return help;
  std::size_t p = open_pos;
  while (p > 0 && (text[p - 1] == ' ' || text[p - 1] == '\t'))
    --p;
  std::size_t end = p;
  while (p > 0 && ((std::isalnum(static_cast<unsigned char>(text[p - 1])) != 0) || text[p - 1] == '_'))
    --p;
  std::string fname(text.substr(p, end - p));
  if (fname.empty())
    return help;
  int active = 0;
  int d2 = 0;
  for (std::size_t k = open_pos + 1; k < off && k < text.size(); ++k) {
    char c = text[k];
    if (c == '(') {
      ++d2;
    } else if (c == ')') {
      if (d2 > 0)
        --d2;
    } else if (c == ',' && d2 == 0) {
      ++active;
    }
  }
  vx::util::Arena arena;
  vx::compiler::DiagBag bag;
  auto toks = vx::compiler::tokenize(text, "<document>", arena, bag);
  auto prog = vx::compiler::parse(toks, "<document>", arena, bag);
  if (prog) {
    std::function<void(const vx::compiler::Stmt*)> walk = nullptr;
    walk = [&](const vx::compiler::Stmt* s) {
      if (s == nullptr)
        return;
      using namespace vx::compiler;
      if (s->kind == NodeKind::FuncDecl) {
        const auto* f = static_cast<const FuncDecl*>(s);
        if (f->name == fname) {
          std::string label = std::string(fname) + "(";
          for (std::size_t i = 0; i < f->params.size(); ++i) {
            if (i)
              label += ", ";
            label += std::string(f->params[i].name);
            if (!f->params[i].type.empty())
              label += ": " + std::string(f->params[i].type);
          }
          label += ")";
          if (!f->ret_type.empty() && f->ret_type != "void")
            label += " -> " + std::string(f->ret_type);
          LspSignatureInformation sig;
          sig.label = label;
          for (const auto& pm : f->params) {
            LspParameterInformation pi;
            pi.label = std::string(pm.name);
            if (!pm.type.empty())
              pi.label += ": " + std::string(pm.type);
            sig.parameters.push_back(pi);
          }
          help.signatures.push_back(sig);
        }
      } else if (s->kind == NodeKind::ExternDecl) {
        const auto* e = static_cast<const ExternDecl*>(s);
        if (e->name == fname) {
          std::string label = std::string(fname) + "(";
          for (std::size_t i = 0; i < e->params.size(); ++i) {
            if (i)
              label += ", ";
            label += std::string(e->params[i].name);
            if (!e->params[i].type.empty())
              label += ": " + std::string(e->params[i].type);
          }
          label += ")";
          if (!e->ret_type.empty() && e->ret_type != "void")
            label += " -> " + std::string(e->ret_type);
          LspSignatureInformation sig;
          sig.label = label;
          for (const auto& pm : e->params) {
            LspParameterInformation pi;
            pi.label = std::string(pm.name);
            if (!pm.type.empty())
              pi.label += ": " + std::string(pm.type);
            sig.parameters.push_back(pi);
          }
          help.signatures.push_back(sig);
        }
      }
      if (s->kind == NodeKind::FuncDecl) {
        const auto* f = static_cast<const FuncDecl*>(s);
        for (const auto* st : f->body)
          walk(st);
      } else if (s->kind == NodeKind::If) {
        const auto* i_ = static_cast<const vx::compiler::If*>(s);
        for (const auto* st : i_->then_body)
          walk(st);
        for (const auto& arm : i_->elifs)
          for (const auto* st : arm.body)
            walk(st);
        for (const auto* st : i_->else_body)
          walk(st);
      } else if (s->kind == NodeKind::While) {
        const auto* w = static_cast<const vx::compiler::While*>(s);
        for (const auto* st : w->body)
          walk(st);
      } else if (s->kind == NodeKind::For) {
        const auto* f = static_cast<const vx::compiler::For*>(s);
        for (const auto* st : f->body)
          walk(st);
      }
    };
    for (const auto* s : prog->stmts)
      walk(s);
  }
  if (!help.signatures.empty()) {
    help.active_parameter = active;
    if (active >= 0 && std::cmp_less(active, help.signatures[0].parameters.size()))
      help.active_parameter = active;
  }
  return help;
}

} // namespace vxls
