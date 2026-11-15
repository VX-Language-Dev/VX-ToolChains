// analysis_inlay.hpp — 内嵌提示（inlay hints）（内部实现头，仅供 analysis.cpp 使用）
#pragma once

#include "analysis.hpp"
#include "analysis_tidy.hpp"
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

// ==== 内嵌提示（inlay hints）====

namespace {

using vx::compiler::Expr;
using vx::compiler::Program;
using vx::compiler::Stmt;
using vx::compiler::Token;

struct CallParam {
  std::string name;
  std::string type;
};

// 可调用对象签名（函数 / extern / 内置 / 导入库符号统一表示）
struct CallableSignature {
  std::vector<CallParam> params;
  std::string ret;
  std::string origin; // 提示来源（tooltip 用）
};

using SigMap = std::map<std::string, CallableSignature>;
using Scope = std::map<std::string, std::string>; // 变量名 → 类型

std::string_view trim_view(std::string_view s) {
  const std::size_t b = s.find_first_not_of(" \t");
  if (b == std::string_view::npos)
    return {};
  const std::size_t e = s.find_last_not_of(" \t");
  return s.substr(b, e - b + 1);
}

CallableSignature make_signature(std::string_view ret, std::span<const vx::compiler::Param> params,
                                 std::string_view origin) {
  CallableSignature sig;
  sig.ret = std::string(ret);
  sig.origin = std::string(origin);
  for (const auto& p : params) {
    sig.params.push_back(CallParam{std::string(p.name), std::string(p.type)});
  }
  return sig;
}

// 内置运行时签名来自固定文档表："file_read(path: string) -> string"
CallableSignature parse_builtin_signature(std::string_view sig_text, std::string_view origin) {
  CallableSignature sig;
  sig.origin = std::string(origin);
  const std::size_t lp = sig_text.find('(');
  const std::size_t rp = lp == std::string_view::npos ? std::string_view::npos : sig_text.find(')', lp + 1);
  if (lp == std::string_view::npos || rp == std::string_view::npos)
    return sig;
  const std::string_view inside = sig_text.substr(lp + 1, rp - lp - 1);
  std::size_t pos = 0;
  while (pos < inside.size()) {
    const std::size_t comma = inside.find(',', pos);
    const std::string_view seg =
        trim_view(inside.substr(pos, comma == std::string_view::npos ? std::string_view::npos : comma - pos));
    if (!seg.empty()) {
      const std::size_t colon = seg.find(':');
      sig.params.push_back(CallParam{
          std::string(trim_view(seg.substr(0, colon))),
          colon == std::string_view::npos ? std::string() : std::string(trim_view(seg.substr(colon + 1))),
      });
    }
    if (comma == std::string_view::npos)
      break;
    pos = comma + 1;
  }
  const std::size_t arrow = sig_text.find("->", rp + 1);
  if (arrow != std::string_view::npos)
    sig.ret = std::string(trim_view(sig_text.substr(arrow + 2)));
  return sig;
}

const SigMap& builtin_signatures() {
  static const SigMap table = [] {
    SigMap m;
    for (const auto& [name, doc] : builtin_docs()) {
      m.emplace(std::string(name), parse_builtin_signature(doc.sig, "内置运行时"));
    }
    return m;
  }();
  return table;
}

// 收集顶层函数 / extern 签名（origin 区分本文件与其它打开文档）
void collect_top_sigs(const Program& prog, SigMap& out, std::string_view origin) {
  for (const Stmt* s : prog.stmts) {
    if (s == nullptr)
      continue;
    if (s->kind == NodeKind::FuncDecl) {
      const auto* f = static_cast<const vx::compiler::FuncDecl*>(s);
      out[std::string(f->name)] = make_signature(f->ret_type, f->params, origin);
    } else if (s->kind == NodeKind::ExternDecl) {
      const auto* e = static_cast<const vx::compiler::ExternDecl*>(s);
      out[std::string(e->name)] = make_signature(e->ret_type, e->params, origin);
    }
  }
}

struct InlayContext {
  const SigMap* funcs = nullptr;                      // 当前文档优先 + 其它打开文档
  const SigMap* builtins = nullptr;                   // 内置运行时
  const vx::compiler::ImportTable* imports = nullptr; // @imp/@cppimp 导入表（可空）
};

struct ResolvedCall {
  std::vector<CallParam> params;
  std::string ret;
  std::string origin;
};

bool resolve_call(const InlayContext& ctx, std::string_view prefix, std::string_view name, ResolvedCall& out) {
  if (!prefix.empty()) {
    // 库前缀.符号：经导入表取 .vxh 签名（跨文件库解析）
    if (ctx.imports == nullptr)
      return false;
    const vx::compiler::ImportedLib* lib = ctx.imports->lib_by_prefix(prefix);
    if (lib == nullptr)
      return false;
    const vx::compiler::ImportedSymbol* sym = vx::compiler::ImportTable::find_symbol(*lib, name);
    if (sym == nullptr)
      return false;
    out.params.clear();
    for (const auto& p : sym->params) {
      out.params.push_back(CallParam{p.name, p.type});
    }
    out.ret = sym->ret_type;
    out.origin = "导入库 " + lib->name;
    if (lib->from_cpp)
      out.origin += "（C++）";
    return true;
  }
  if (ctx.funcs != nullptr) {
    if (const auto it = ctx.funcs->find(std::string(name)); it != ctx.funcs->end()) {
      out.params = it->second.params;
      out.ret = it->second.ret;
      out.origin = it->second.origin;
      return true;
    }
  }
  if (ctx.builtins != nullptr) {
    if (const auto it = ctx.builtins->find(std::string(name)); it != ctx.builtins->end()) {
      out.params = it->second.params;
      out.ret = it->second.ret;
      out.origin = it->second.origin;
      return true;
    }
  }
  return false;
}

bool resolve_call_expr(const InlayContext& ctx, const vx::compiler::Call* call, ResolvedCall& out) {
  if (call == nullptr || call->callee == nullptr)
    return false;
  if (call->callee->kind == NodeKind::Ident) {
    const auto* id = static_cast<const vx::compiler::Ident*>(call->callee);
    return resolve_call(ctx, {}, id->name, out);
  }
  if (call->callee->kind == NodeKind::Property) {
    const auto* prop = static_cast<const vx::compiler::Property*>(call->callee);
    if (prop->obj != nullptr && prop->obj->kind == NodeKind::Ident) {
      const auto* recv = static_cast<const vx::compiler::Ident*>(prop->obj);
      return resolve_call(ctx, recv->name, prop->name, out);
    }
  }
  return false;
}

// 调用实参起始 token 下标（按顶层逗号分隔，忽略嵌套括号/列表/字典内部）
std::vector<std::size_t> arg_start_tokens(std::span<const Token> toks, std::size_t lparen) {
  std::vector<std::size_t> starts;
  int depth = 0;
  bool expect = true;
  for (std::size_t j = lparen + 1; j < toks.size(); ++j) {
    const Token& t = toks[j];
    if (t.kind == Tok::Eof)
      break;
    if (t.kind == Tok::RParen) {
      if (depth == 0)
        break;
      --depth;
      continue;
    }
    if (t.kind == Tok::RBracket || t.kind == Tok::RBrace) {
      if (depth > 0)
        --depth;
      continue;
    }
    if (t.kind == Tok::Comma && depth == 0) {
      expect = true;
      continue;
    }
    if (expect) {
      starts.push_back(j);
      expect = false;
    }
    if (t.kind == Tok::LParen || t.kind == Tok::LBracket || t.kind == Tok::LBrace)
      ++depth;
  }
  return starts;
}

struct VarPos {
  LspPosition name_end;
  bool annotated = false;
};

class InlayBuilder {
public:
  InlayBuilder(std::string_view text, const InlayContext& ctx) : text_(text), ctx_(ctx) {}

  std::vector<LspInlayHint> build(const Program& prog, std::span<const Token> toks) {
    collect_var_positions(toks);
    // 顶层变量先推断一遍：函数体内可引用同名全局变量
    Scope globals;
    for (const Stmt* s : prog.stmts) {
      if (s == nullptr || s->kind != NodeKind::VarDecl)
        continue;
      const auto* v = static_cast<const vx::compiler::VarDecl*>(s);
      const std::string ty = v->type_ann.empty() ? infer(v->init, globals) : std::string(v->type_ann);
      if (is_hint_type(ty))
        globals[std::string(v->name)] = ty;
    }
    walk_stmts(prog.stmts, globals);
    collect_call_hints(toks);

    std::ranges::sort(hints_, [](const LspInlayHint& a, const LspInlayHint& b) {
      if (a.position.line != b.position.line)
        return a.position.line < b.position.line;
      if (a.position.character != b.position.character)
        return a.position.character < b.position.character;
      return a.kind < b.kind;
    });
    hints_.erase(std::unique(hints_.begin(), hints_.end(),
                             [](const LspInlayHint& a, const LspInlayHint& b) {
                               return a.position.line == b.position.line &&
                                      a.position.character == b.position.character && a.label == b.label &&
                                      a.kind == b.kind;
                             }),
                 hints_.end());
    return std::move(hints_);
  }

private:
  static bool is_hint_type(std::string_view ty) { return !ty.empty() && ty != "void" && ty != "nil" && ty != "var"; }

  // 变量名 token 结束位置（按 (行, 名) 索引，供 AST VarDecl 精确定位）
  void collect_var_positions(std::span<const Token> toks) {
    for (std::size_t i = 0; i + 1 < toks.size(); ++i) {
      const Token& t = toks[i];
      if (t.kind != Tok::Kw || (t.text != "var" && t.text != "mut" && t.text != "const"))
        continue;
      const Token& name = toks[i + 1];
      if (name.kind != Tok::Ident)
        continue;
      VarPos pos;
      pos.name_end = diag_pos_to_lsp(text_, name.line, name.col + static_cast<int>(utf8_len(name.text)));
      pos.annotated = i + 2 < toks.size() && toks[i + 2].kind == Tok::Colon;
      var_positions_[{name.line, std::string(name.text)}] = pos;
    }
  }

  std::string infer(const Expr* e, const Scope& vars) const {
    if (e == nullptr)
      return {};
    switch (e->kind) {
      case NodeKind::IntLit:
        return "int";
      case NodeKind::FloatLit:
        return "float";
      case NodeKind::StrLit:
        return "string";
      case NodeKind::BoolLit:
        return "bool";
      case NodeKind::NilLit:
        return {};
      case NodeKind::Ident: {
        const auto* id = static_cast<const vx::compiler::Ident*>(e);
        const auto it = vars.find(std::string(id->name));
        return it == vars.end() ? std::string() : it->second;
      }
      case NodeKind::New:
        return std::string(static_cast<const vx::compiler::New*>(e)->class_name);
      case NodeKind::Deref:
        return "int"; // *p 读取 32 位字
      case NodeKind::FuncRef:
        return "ptr"; // &fn 取函数值
      case NodeKind::Unary: {
        const auto* u = static_cast<const vx::compiler::Unary*>(e);
        if (u->op == "!" || u->op == "not")
          return "bool";
        return infer(u->operand, vars);
      }
      case NodeKind::Binary: {
        const auto* b = static_cast<const vx::compiler::Binary*>(e);
        if (b->op == "==" || b->op == "!=" || b->op == "<" || b->op == "<=" || b->op == ">" || b->op == ">=" ||
            b->op == "and" || b->op == "or" || b->op == "&&" || b->op == "||")
          return "bool";
        const std::string lt = infer(b->left, vars);
        const std::string rt = infer(b->right, vars);
        if (b->op == "+" && (lt == "string" || rt == "string"))
          return "string";
        if (!lt.empty() && lt == rt)
          return lt;
        if (lt == "float" || rt == "float")
          return "float";
        return lt.empty() ? rt : lt;
      }
      case NodeKind::Call: {
        ResolvedCall rc;
        if (resolve_call_expr(ctx_, static_cast<const vx::compiler::Call*>(e), rc))
          return rc.ret;
        return {};
      }
      default:
        return {};
    }
  }

  void emit_type_hint(const vx::compiler::VarDecl* v, const std::string& ty) {
    const auto it = var_positions_.find({v->line, std::string(v->name)});
    if (it == var_positions_.end() || it->second.annotated)
      return;
    LspInlayHint h;
    h.position = it->second.name_end;
    h.label = ": " + ty;
    h.kind = 1; // Type
    h.tooltip = "推断类型: " + ty;
    hints_.push_back(std::move(h));
  }

  void add_params(Scope& scope, std::span<const vx::compiler::Param> params) {
    for (const auto& p : params) {
      if (!p.type.empty())
        scope[std::string(p.name)] = std::string(p.type);
    }
  }

  void walk_stmts(std::span<const Stmt* const> body, Scope vars) {
    for (const Stmt* s : body) {
      if (s == nullptr)
        continue;
      switch (s->kind) {
        case NodeKind::VarDecl: {
          const auto* v = static_cast<const vx::compiler::VarDecl*>(s);
          const std::string ty = v->type_ann.empty() ? infer(v->init, vars) : std::string(v->type_ann);
          if (v->type_ann.empty() && is_hint_type(ty))
            emit_type_hint(v, ty);
          if (is_hint_type(ty))
            vars[std::string(v->name)] = ty;
          break;
        }
        case NodeKind::FuncDecl: {
          const auto* f = static_cast<const vx::compiler::FuncDecl*>(s);
          Scope inner = vars;
          add_params(inner, f->params);
          walk_stmts(f->body, std::move(inner));
          break;
        }
        case NodeKind::ClassDecl: {
          const auto* c = static_cast<const vx::compiler::ClassDecl*>(s);
          for (const auto* m : c->methods) {
            Scope inner = vars;
            add_params(inner, m->params);
            walk_stmts(m->body, std::move(inner));
          }
          break;
        }
        case NodeKind::If: {
          const auto* n = static_cast<const vx::compiler::If*>(s);
          Scope then_scope = vars;
          walk_stmts(n->then_body, std::move(then_scope));
          for (const auto& arm : n->elifs) {
            Scope arm_scope = vars;
            walk_stmts(arm.body, std::move(arm_scope));
          }
          Scope else_scope = vars;
          walk_stmts(n->else_body, std::move(else_scope));
          break;
        }
        case NodeKind::While: {
          const auto* w = static_cast<const vx::compiler::While*>(s);
          Scope inner = vars;
          walk_stmts(w->body, std::move(inner));
          break;
        }
        case NodeKind::For: {
          const auto* f = static_cast<const vx::compiler::For*>(s);
          Scope inner = vars;
          walk_stmts(f->body, std::move(inner));
          break;
        }
        default:
          break;
      }
    }
  }

  // 是否函数声明/定义的 '('（func name(...) / extern [func] name(...) / new Name(...)）
  static bool is_decl_lparen(std::span<const Token> toks, std::size_t i) {
    if (i < 2)
      return false;
    if (toks[i - 1].kind != Tok::Ident)
      return false;
    const Token& p2 = toks[i - 2];
    return p2.kind == Tok::Kw && (p2.text == "func" || p2.text == "extern" || p2.text == "new");
  }

  void collect_call_hints(std::span<const Token> toks) {
    for (std::size_t i = 0; i < toks.size(); ++i) {
      if (toks[i].kind != Tok::LParen || i == 0)
        continue;
      const Token& prev = toks[i - 1];
      if (prev.kind != Tok::Ident || is_decl_lparen(toks, i))
        continue;
      // 库前缀（`math.add(`）；链式成员调用（`a.b.c(`）暂不解析
      std::string_view prefix;
      if (i >= 2 && toks[i - 2].kind == Tok::Dot) {
        if (i < 3 || toks[i - 3].kind != Tok::Ident)
          continue;
        if (i >= 4 && toks[i - 4].kind == Tok::Dot)
          continue;
        prefix = toks[i - 3].text;
      }
      ResolvedCall rc;
      if (!resolve_call(ctx_, prefix, prev.text, rc) || rc.params.empty())
        continue;
      const std::vector<std::size_t> starts = arg_start_tokens(toks, i);
      for (std::size_t ai = 0; ai < starts.size() && ai < rc.params.size(); ++ai) {
        const CallParam& param = rc.params[ai];
        if (param.name.empty())
          continue;
        const Token& at = toks[starts[ai]];
        LspInlayHint h;
        h.position = diag_pos_to_lsp(text_, at.line, at.col);
        h.label = param.name + ":";
        h.kind = 2; // Parameter
        h.padding_right = true;
        h.tooltip = "参数 " + param.name + (param.type.empty() ? "" : ": " + param.type) + " · " + rc.origin;
        hints_.push_back(std::move(h));
      }
    }
  }

  std::string_view text_;
  const InlayContext& ctx_;
  std::map<std::pair<int, std::string>, VarPos> var_positions_;
  std::vector<LspInlayHint> hints_;
};

} // namespace

} // namespace

} // namespace vxls
