// semantics_checker_stmt.hpp — 诊断辅助 + 语句级检查（内部实现头，仅供 semantics.cpp 使用）
#pragma once

#include "semantics_checker.hpp"

namespace vx::compiler {

namespace {

// ---- 诊断 ----
inline void TypeChecker::err(std::string_view code, std::string msg, int line, std::string note) {
  Diagnostic d = make_diag(code, Severity::Error, std::move(msg), std::string(filename_), line, 0);
  if (!note.empty()) {
    d.note = std::move(note);
  }
  bag_.add(std::move(d));
  ok_ = false;
}

// 重复定义的 note：指向同作用域内首次定义的位置
inline std::string TypeChecker::first_def_note(std::string_view name) const {
  if (const VarInfo* vi = env_.lookup(name); vi != nullptr && vi->decl_line > 0) {
    return "首次定义于第 " + std::to_string(vi->decl_line) + " 行";
  }
  return "";
}

// 函数/类重复定义的 note：指向符号表中首次声明的位置
inline std::string TypeChecker::first_sym_note(std::string_view name) const {
  if (const FuncSig* sig = symtab_.find_func(name); sig != nullptr && sig->decl_line > 0) {
    return "首次声明于第 " + std::to_string(sig->decl_line) + " 行";
  }
  if (const ClassInfo* cls = symtab_.find_class(name); cls != nullptr && cls->decl_line > 0) {
    return "首次声明于第 " + std::to_string(cls->decl_line) + " 行";
  }
  return "";
}

// 未定义变量的诊断：区分 E0010（从未声明）与 E0012（曾在内层作用域声明）
inline void TypeChecker::err_undefined_var(std::string_view name, int line) {
  auto it = env_.leaked().find(std::string(name));
  if (it != env_.leaked().end()) {
    std::string note;
    if (it->second > 0) {
      note =
          "'" + std::string(name) + "' 声明于第 " + std::to_string(it->second) + " 行，所在作用域已结束，此处不可见\n";
    }
    note += help_of("在当前作用域重新声明，或把声明移到使用处所在的作用域");
    err(diag_code::sem_scope_leak, "变量 '" + std::string(name) + "' 在内层作用域中声明，当前作用域不可见", line,
        std::move(note));
    return;
  }
  err(diag_code::sem_undefined, "未定义变量 '" + std::string(name) + "'", line,
      help_of("检查拼写，或先用 `var " + std::string(name) + " = ...` 声明"));
}

// ---- 语句检查 ----
inline void TypeChecker::check_stmt(const Stmt* s) {
  if (s == nullptr) {
    return;
  }
  switch (s->kind) {
    case NodeKind::VarDecl: {
      const auto* v = static_cast<const VarDecl*>(s);
      VarInfo info;
      info.is_mut = v->is_mut;
      std::string struct_name;
      if (!v->type_ann.empty()) {
        info.type = resolve_ann(v->type_ann, struct_name, v->line);
        info.struct_name = struct_name;
      }
      if (v->init != nullptr) {
        VarInfo init = check_expr(v->init);
        if (info.type == TypeTag::TYPE_UNKNOWN) {
          // 未标注（或标注为 var）：由初始化表达式推断变量类型
          info = init;
          info.is_mut = v->is_mut;
        } else {
          // 有显式标注：校验初始化表达式（无隐式转换）
          expect_type(info, init, "变量 '" + std::string(v->name) + "' 的初始化", v->line);
        }
      }
      info.decl_line = v->line;
      // '_' 是丢弃标识符：允许在同一作用域内重复声明
      if (v->name == "_") {
        break;
      }
      if (!env_.declare(v->name, info)) {
        err(diag_code::sem_redefined, "变量 '" + std::string(v->name) + "' 在同一作用域内重复定义", v->line,
            first_def_note(v->name));
      }
      break;
    }
    case NodeKind::Assign: {
      const auto* a = static_cast<const Assign*>(s);
      VarInfo target = check_assign_target(a->target);
      VarInfo value = check_expr(a->value);
      check_assign_types(a, target, value);
      break;
    }
    case NodeKind::Return: {
      const auto* r = static_cast<const Return*>(s);
      if (!in_function_) {
        err(diag_code::sem_flow, "return 不能出现在函数外", r->line);
      }
      if (r->value != nullptr) {
        VarInfo value = check_expr(r->value);
        if (ret_info_.type == TypeTag::TYPE_VOID) {
          err(diag_code::sem_type_mismatch, "void 函数不能返回值", r->line);
        } else {
          expect_type(ret_info_, value, "返回值", r->line);
        }
      }
      // 非 void 函数的裸 return 由 codegen 补默认返回（不在此报错）
      break;
    }
    case NodeKind::Break:
    case NodeKind::Continue: {
      if (loop_depth_ == 0) {
        err(diag_code::sem_flow,
            s->kind == NodeKind::Break ? "break 只能出现在循环体内" : "continue 只能出现在循环体内", s->line);
      }
      break;
    }
    case NodeKind::ExprStmt:
      check_expr(static_cast<const ExprStmt*>(s)->expr);
      break;
    case NodeKind::If: {
      const auto* i = static_cast<const If*>(s);
      check_expr(i->cond);
      check_block(i->then_body);
      for (const ElifArm& arm : i->elifs) {
        check_expr(arm.cond);
        check_block(arm.body);
      }
      check_block(i->else_body);
      break;
    }
    case NodeKind::While: {
      const auto* w = static_cast<const While*>(s);
      check_expr(w->cond);
      ++loop_depth_;
      check_block(w->body);
      --loop_depth_;
      break;
    }
    case NodeKind::For: {
      const auto* f = static_cast<const For*>(s);
      VarInfo iter = check_expr(f->iterable);
      // 循环变量类型：列表按推断的元素类型，其余（map / var / 未知）保持通配
      VarInfo loop_var{TypeTag::TYPE_UNKNOWN, true, ""};
      if (iter.type == TypeTag::TYPE_ARRAY && iter.elem != TypeTag::TYPE_UNKNOWN) {
        loop_var.type = iter.elem;
      }
      env_.push_scope();
      env_.declare(f->var, loop_var);
      ++loop_depth_;
      for (const Stmt* st : f->body) {
        check_stmt(st);
      }
      --loop_depth_;
      env_.pop_scope();
      break;
    }
    default:
      break; // 嵌套函数声明等由 codegen 报告
  }
}

inline void TypeChecker::check_block(std::span<const Stmt* const> stmts) {
  env_.push_scope();
  for (const Stmt* st : stmts) {
    check_stmt(st);
  }
  env_.pop_scope();
}

// 赋值目标的类型（用于「无隐式转换」校验）；无法确定时返回通配。
inline VarInfo TypeChecker::check_assign_target(const Expr* t) {
  if (t == nullptr) {
    return {};
  }
  if (t->kind == NodeKind::Ident) {
    const auto* id = static_cast<const Ident*>(t);
    if (const VarInfo* vi = env_.lookup(id->name)) {
      return *vi;
    }
    err_undefined_var(id->name, t->line);
    return {};
  }
  if (t->kind == NodeKind::Property) {
    // 属性赋值：字段/类常量存在性 + 声明类型（由 check_property 给出）
    return check_property(static_cast<const Property*>(t));
  }
  if (t->kind == NodeKind::Index) {
    const auto* ix = static_cast<const Index*>(t);
    VarInfo base = check_expr(ix->obj);
    check_expr(ix->index);
    if (base.type == TypeTag::TYPE_ARRAY && base.elem != TypeTag::TYPE_UNKNOWN) {
      return VarInfo{base.elem, false, ""};
    }
    return {};
  }
  if (t->kind == NodeKind::Deref) {
    const auto* d = static_cast<const Deref*>(t);
    VarInfo base = check_expr(d->ptr);
    if (base.type != TypeTag::TYPE_UNKNOWN && base.type != TypeTag::TYPE_POINTER) {
      err(diag_code::sem_type_mismatch, "指针解引用要求 ptr 类型（实际 '" + type_text(base) + "'）", t->line);
    }
    return {TypeTag::TYPE_INT, false, ""}; // *p 的 32 位字按 int 读写
  }
  err(diag_code::sem_undefined, "不支持的赋值目标: " + std::string(node_kind_name(t->kind)), t->line);
  return {};
}

// 类型不匹配诊断（无隐式转换：不做 int↔float、string↔int 等任何转换）
inline void TypeChecker::expect_type(const VarInfo& want, const VarInfo& got, std::string_view what, int line) {
  if (same_type(want, got)) {
    return;
  }
  err(diag_code::sem_type_mismatch,
      "类型不匹配：" + std::string(what) + "（期望 '" + type_text(want) + "'，实际 '" + type_text(got) + "'）", line);
}

// 赋值检查：= 直接校验；复合赋值按 target op value 推导结果类型再校验。
inline void TypeChecker::check_assign_types(const Assign* a, const VarInfo& target, const VarInfo& value) {
  if (a->op == "=") {
    expect_type(target, value, "赋值", a->line);
    // 赋值给类型尚未确定的变量时细化其类型（var x 后首次赋值）
    if (a->target->kind == NodeKind::Ident && value.type != TypeTag::TYPE_UNKNOWN) {
      VarInfo refined = value;
      refined.is_mut = target.is_mut;
      env_.refine(static_cast<const Ident*>(a->target)->name, refined);
    }
    return;
  }
  const std::string op(1, a->op[0]);
  check_binary_types(op, target, value, a->line);
  expect_type(target, infer_binary(op, target, value), "复合赋值", a->line);
}

} // namespace

} // namespace vx::compiler
