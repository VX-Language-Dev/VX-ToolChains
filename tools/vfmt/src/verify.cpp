// verify.cpp — AST 结构指纹实现。
// 遍历 AST 生成规范化文本：结构一致 → 指纹一致。
// 字符串以「长度:内容」编码，避免字面量内容与分隔符混淆。
#include "verify.hpp"

#include <string>
#include <string_view>

namespace vfmt {
namespace {

using namespace vx::compiler;

// 追加字符串：长度前缀编码，保证无歧义。
void put_str(std::string& out, std::string_view s) {
  out += std::to_string(s.size());
  out += ':';
  out.append(s);
}

void fp_expr(std::string& out, const Expr* e);
void fp_stmt(std::string& out, const Stmt* s);

void fp_expr(std::string& out, const Expr* e) {
  if (!e) {
    out += "nil;";
    return;
  }
  switch (e->kind) {
    case NodeKind::IntLit:
      out += "i";
      put_str(out, static_cast<const IntLit*>(e)->text);
      break;
    case NodeKind::FloatLit:
      out += "f";
      put_str(out, static_cast<const FloatLit*>(e)->text);
      break;
    case NodeKind::StrLit:
      out += "s";
      put_str(out, static_cast<const StrLit*>(e)->value);
      break;
    case NodeKind::BoolLit:
      out += static_cast<const BoolLit*>(e)->value ? "b1;" : "b0;";
      break;
    case NodeKind::NilLit:
      out += "z;";
      break;
    case NodeKind::Ident:
      out += "v";
      put_str(out, static_cast<const Ident*>(e)->name);
      break;
    case NodeKind::ListLit: {
      const auto* l = static_cast<const ListLit*>(e);
      out += "l[";
      for (const Expr* it : l->items) {
        fp_expr(out, it);
        out += ',';
      }
      out += ']';
      break;
    }
    case NodeKind::DictLit: {
      const auto* d = static_cast<const DictLit*>(e);
      out += "m{";
      for (const DictEntry& en : d->entries) {
        fp_expr(out, en.key);
        out += ':';
        fp_expr(out, en.value);
        out += ',';
      }
      out += '}';
      break;
    }
    case NodeKind::Index: {
      const auto* x = static_cast<const Index*>(e);
      out += "x(";
      fp_expr(out, x->obj);
      out += ',';
      fp_expr(out, x->index);
      out += ')';
      break;
    }
    case NodeKind::Property: {
      const auto* p = static_cast<const Property*>(e);
      out += "p(";
      fp_expr(out, p->obj);
      out += ',';
      put_str(out, p->name);
      out += ')';
      break;
    }
    case NodeKind::Call: {
      const auto* c = static_cast<const Call*>(e);
      out += "c(";
      fp_expr(out, c->callee);
      for (const Expr* a : c->args) {
        out += ',';
        fp_expr(out, a);
      }
      out += ')';
      break;
    }
    case NodeKind::New: {
      const auto* n = static_cast<const New*>(e);
      out += "n";
      put_str(out, n->class_name);
      for (const Expr* a : n->args) {
        out += ',';
        fp_expr(out, a);
      }
      out += ')';
      break;
    }
    case NodeKind::Unary: {
      const auto* u = static_cast<const Unary*>(e);
      out += "u";
      put_str(out, u->op);
      out += '(';
      fp_expr(out, u->operand);
      out += ')';
      break;
    }
    case NodeKind::Binary: {
      const auto* b = static_cast<const Binary*>(e);
      out += "b";
      put_str(out, b->op);
      out += '(';
      fp_expr(out, b->left);
      out += ',';
      fp_expr(out, b->right);
      out += ')';
      break;
    }
    default:
      out += "?e;";
      break;
  }
}

void fp_params(std::string& out, std::span<const Param> params) {
  out += '(';
  for (const Param& p : params) {
    out += "p";
    put_str(out, p.name);
    out += ':';
    put_str(out, p.type);
    out += ';';
  }
  out += ')';
}

void fp_fields(std::string& out, std::span<const ClassField> fields) {
  out += '(';
  for (const ClassField& f : fields) {
    out += "f";
    put_str(out, f.name);
    out += ':';
    put_str(out, f.type);
    out += '(';
    fp_expr(out, f.init);
    out += ");";
  }
  out += ')';
}

void fp_body(std::string& out, std::span<const Stmt* const> body) {
  out += '{';
  for (const Stmt* s : body) {
    fp_stmt(out, s);
    out += ';';
  }
  out += '}';
}

void fp_stmt(std::string& out, const Stmt* s) {
  if (!s) {
    out += "nil;";
    return;
  }
  switch (s->kind) {
    case NodeKind::ExprStmt:
      out += "E(";
      fp_expr(out, static_cast<const ExprStmt*>(s)->expr);
      out += ')';
      break;
    case NodeKind::VarDecl: {
      const auto* v = static_cast<const VarDecl*>(s);
      out += "V";
      put_str(out, v->name);
      out += ':';
      put_str(out, v->type_ann);
      out += v->is_mut ? ":mut(" : ":imm(";
      fp_expr(out, v->init);
      out += ')';
      break;
    }
    case NodeKind::Assign: {
      const auto* a = static_cast<const Assign*>(s);
      out += "A";
      put_str(out, a->op);
      out += '(';
      fp_expr(out, a->target);
      out += ',';
      fp_expr(out, a->value);
      out += ')';
      break;
    }
    case NodeKind::FuncDecl: {
      const auto* f = static_cast<const FuncDecl*>(s);
      out += "F";
      put_str(out, f->name);
      fp_params(out, f->params);
      out += ':';
      put_str(out, f->ret_type);
      fp_body(out, f->body);
      break;
    }
    case NodeKind::ExternDecl: {
      const auto* e = static_cast<const ExternDecl*>(s);
      out += "X";
      put_str(out, e->name);
      fp_params(out, e->params);
      out += ':';
      put_str(out, e->ret_type);
      break;
    }
    case NodeKind::Import:
      out += "I";
      put_str(out, static_cast<const Import*>(s)->path);
      break;
    case NodeKind::Export:
      out += "P";
      put_str(out, static_cast<const Export*>(s)->name);
      break;
    case NodeKind::ImpDecl: {
      const auto* i = static_cast<const ImpDecl*>(s);
      out += "M";
      put_str(out, i->lib);
      out += ':';
      put_str(out, i->alias);
      break;
    }
    case NodeKind::CppImpDecl: {
      const auto* c = static_cast<const CppImpDecl*>(s);
      out += "C";
      put_str(out, c->header);
      out += ':';
      put_str(out, c->alias);
      break;
    }
    case NodeKind::Return:
      out += "R(";
      fp_expr(out, static_cast<const Return*>(s)->value);
      out += ')';
      break;
    case NodeKind::Break:
      out += "K;";
      break;
    case NodeKind::Continue:
      out += "T;";
      break;
    case NodeKind::If: {
      const auto* i = static_cast<const If*>(s);
      out += "IF(";
      fp_expr(out, i->cond);
      out += ')';
      fp_body(out, i->then_body);
      out += "EL";
      for (const ElifArm& a : i->elifs) {
        out += '(';
        fp_expr(out, a.cond);
        out += ')';
        fp_body(out, a.body);
      }
      out += "ES";
      fp_body(out, i->else_body);
      break;
    }
    case NodeKind::While: {
      const auto* w = static_cast<const While*>(s);
      out += "WH(";
      fp_expr(out, w->cond);
      out += ')';
      fp_body(out, w->body);
      break;
    }
    case NodeKind::For: {
      const auto* f = static_cast<const For*>(s);
      out += "FO";
      put_str(out, f->var);
      out += '(';
      fp_expr(out, f->iterable);
      out += ')';
      fp_body(out, f->body);
      break;
    }
    case NodeKind::ClassDecl: {
      const auto* c = static_cast<const ClassDecl*>(s);
      out += "CL";
      put_str(out, c->name);
      fp_fields(out, c->fields);
      out += '(';
      for (const FuncDecl* m : c->methods) {
        fp_stmt(out, m);
        out += ';';
      }
      out += ')';
      break;
    }
    case NodeKind::StructDecl: {
      const auto* s2 = static_cast<const StructDecl*>(s);
      out += "ST";
      put_str(out, s2->name);
      fp_fields(out, s2->fields);
      break;
    }
    default:
      out += "?s;";
      break;
  }
}

} // namespace

std::string ast_fingerprint(std::span<const vx::compiler::Stmt* const> stmts) {
  std::string out;
  for (const Stmt* s : stmts) {
    fp_stmt(out, s);
    out += ';';
  }
  return out;
}

} // namespace vfmt
