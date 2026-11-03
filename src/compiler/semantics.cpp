// semantics.cpp — 独立语义分析：符号表 + 类型环境 + 检查器。
// 两遍扫描：第一遍收集顶层声明（函数/extern/类/结构/顶层变量，允许前向引用），
// 第二遍逐函数检查语句与表达式。诊断聚合输出（不首错即停）。
// 检查范围与 codegen 解耦进度见 semantics.hpp 注释。
#include "vx/compiler/semantics.hpp"

#include <cstddef>
#include <ranges>
#include <string>
#include <utility>

namespace vx::compiler {

namespace {

using ir::TypeTag;

// 内置运行时函数（与后端 llvm_emitter 的 kBuiltinTable 对齐）：
// VX 源码可见调用名 → 参数个数。返回类型由 builtin_ret_tag 单独给出。
constexpr int kBuiltinArityUnchecked = -1; // 低层符号不做参数个数检查
struct BuiltinInfo {
  int arity;
  TypeTag ret;
};

const BuiltinInfo* find_builtin(std::string_view name) {
  static const std::unordered_map<std::string_view, BuiltinInfo> kBuiltins = {
      {"out", {1, TypeTag::TYPE_UNKNOWN}},
      {"out_int", {1, TypeTag::TYPE_UNKNOWN}},
      {"out_bool", {1, TypeTag::TYPE_UNKNOWN}},
      {"out_float", {1, TypeTag::TYPE_UNKNOWN}},
      {"puts", {1, TypeTag::TYPE_INT}},
      {"exit", {1, TypeTag::TYPE_UNKNOWN}},
      {"malloc", {1, TypeTag::TYPE_UNKNOWN}},
      {"free", {1, TypeTag::TYPE_UNKNOWN}},
      {"sys_argv_count", {0, TypeTag::TYPE_INT}},
      {"sys_argv_get", {1, TypeTag::TYPE_STRING}},
      {"file_read", {1, TypeTag::TYPE_STRING}},
      {"file_write", {2, TypeTag::TYPE_INT}},
      {"file_exists", {1, TypeTag::TYPE_BOOL}},
      {"strcmp", {2, TypeTag::TYPE_INT}},
      {"vx_str_concat", {2, TypeTag::TYPE_STRING}},
      {"vx_pow_i32", {2, TypeTag::TYPE_INT}},
      {"vx_pow_f64", {2, TypeTag::TYPE_FLOAT}},
      {"vx_array_new", {kBuiltinArityUnchecked, TypeTag::TYPE_ARRAY}},
      {"vx_array_get", {kBuiltinArityUnchecked, TypeTag::TYPE_UNKNOWN}},
      {"vx_array_set", {kBuiltinArityUnchecked, TypeTag::TYPE_UNKNOWN}},
      {"vx_map_new", {kBuiltinArityUnchecked, TypeTag::TYPE_MAP}},
      {"vx_map_get", {kBuiltinArityUnchecked, TypeTag::TYPE_UNKNOWN}},
      {"vx_map_set", {kBuiltinArityUnchecked, TypeTag::TYPE_UNKNOWN}},
  };
  auto it = kBuiltins.find(name);
  return it == kBuiltins.end() ? nullptr : &it->second;
}

// ---- 类型关系 ----

// TypeTag 显示名（诊断文案用）
std::string_view tag_name(TypeTag t) {
  switch (t) {
    case TypeTag::TYPE_VOID:
      return "void";
    case TypeTag::TYPE_INT:
      return "int";
    case TypeTag::TYPE_FLOAT:
      return "float";
    case TypeTag::TYPE_BOOL:
      return "bool";
    case TypeTag::TYPE_STRING:
      return "string";
    case TypeTag::TYPE_STRUCT:
      return "struct";
    case TypeTag::TYPE_ARRAY:
      return "list";
    case TypeTag::TYPE_MAP:
      return "map";
    case TypeTag::TYPE_FUNC:
      return "func";
    default:
      return "var";
  }
}

// 完整类型名（类/结构带名字）
std::string type_text(const VarInfo& v) {
  if (v.type == TypeTag::TYPE_STRUCT && !v.struct_name.empty()) {
    return v.struct_name;
  }
  return std::string(tag_name(v.type));
}

// 类型兼容判定（无隐式转换）：类型未定（var / 无法推断）视为通配，
// 与任何类型兼容；其余必须完全相同（struct 还要求类名一致）。
bool same_type(const VarInfo& a, const VarInfo& b) {
  if (a.type == TypeTag::TYPE_UNKNOWN || b.type == TypeTag::TYPE_UNKNOWN) {
    return true;
  }
  if (a.type != b.type) {
    return false;
  }
  if (a.type == TypeTag::TYPE_STRUCT) {
    return a.struct_name == b.struct_name;
  }
  return true;
}

// 参数签名是否相同（重载集合内判定重复声明用）：
// 参数个数 + 逐个类型标注文本一致即视为同一签名。
bool params_identical(std::span<const Param> a, std::span<const Param> b) {
  if (a.size() != b.size()) {
    return false;
  }
  for (std::size_t i = 0; i < a.size(); ++i) {
    if (a[i].type != b[i].type) {
      return false;
    }
  }
  return true;
}

class TypeChecker {
public:
  TypeChecker(std::string_view filename, DiagBag& bag, const ImportTable* imports, Analysis* analysis)
      : filename_(filename), bag_(bag), imports_(imports), analysis_(analysis) {}

  bool run(const Program& prog) {
    env_.push_scope(); // 模块级作用域（顶层变量）
    collect_decls(prog);
    check_bodies(prog);
    return ok_;
  }

private:
  // ---- 诊断 ----
  void err(std::string_view code, std::string msg, int line, std::string note = "") {
    Diagnostic d = make_diag(code, Severity::Error, std::move(msg), std::string(filename_), line, 0);
    if (!note.empty()) {
      d.note = std::move(note);
    }
    bag_.add(std::move(d));
    ok_ = false;
  }

  // note/help 组合：rustc 风格的补充说明（help 为修复建议）
  static std::string help_of(std::string_view help) { return "help: " + std::string(help); }

  // 重复定义的 note：指向同作用域内首次定义的位置
  std::string first_def_note(std::string_view name) const {
    if (const VarInfo* vi = env_.lookup(name); vi != nullptr && vi->decl_line > 0) {
      return "首次定义于第 " + std::to_string(vi->decl_line) + " 行";
    }
    return "";
  }

  // 函数/类重复定义的 note：指向符号表中首次声明的位置
  std::string first_sym_note(std::string_view name) const {
    if (const FuncSig* sig = symtab_.find_func(name); sig != nullptr && sig->decl_line > 0) {
      return "首次声明于第 " + std::to_string(sig->decl_line) + " 行";
    }
    if (const ClassInfo* cls = symtab_.find_class(name); cls != nullptr && cls->decl_line > 0) {
      return "首次声明于第 " + std::to_string(cls->decl_line) + " 行";
    }
    return "";
  }

  // 未定义变量的诊断：区分 E0010（从未声明）与 E0012（曾在内层作用域声明）
  void err_undefined_var(std::string_view name, int line) {
    auto it = env_.leaked().find(std::string(name));
    if (it != env_.leaked().end()) {
      std::string note;
      if (it->second > 0) {
        note = "'" + std::string(name) + "' 声明于第 " + std::to_string(it->second) +
               " 行，所在作用域已结束，此处不可见\n";
      }
      note += help_of("在当前作用域重新声明，或把声明移到使用处所在的作用域");
      err(diag_code::sem_scope_leak, "变量 '" + std::string(name) + "' 在内层作用域中声明，当前作用域不可见", line,
          std::move(note));
      return;
    }
    err(diag_code::sem_undefined, "未定义变量 '" + std::string(name) + "'", line,
        help_of("检查拼写，或先用 `var " + std::string(name) + " = ...` 声明"));
  }

  // ---- 类型名解析 ----
  // 返回 false = 未知类型（调用方报 E0003）。
  bool type_name_tag(std::string_view name, TypeTag& out, std::string& struct_name) const {
    struct_name.clear();
    if (name.empty() || name == "var") {
      out = TypeTag::TYPE_UNKNOWN;
      return true;
    }
    if (name == "int") {
      out = TypeTag::TYPE_INT;
      return true;
    }
    if (name == "float") {
      out = TypeTag::TYPE_FLOAT;
      return true;
    }
    if (name == "bool") {
      out = TypeTag::TYPE_BOOL;
      return true;
    }
    if (name == "string") {
      out = TypeTag::TYPE_STRING;
      return true;
    }
    if (name == "void") {
      out = TypeTag::TYPE_VOID;
      return true;
    }
    if (symtab_.is_type(name)) {
      out = TypeTag::TYPE_STRUCT;
      struct_name = std::string(name);
      return true;
    }
    return false;
  }

  // 参数/返回类型标注解析；未知标注报 E0003。
  TypeTag resolve_ann(std::string_view ann, std::string& struct_name, int line) {
    TypeTag t = TypeTag::TYPE_UNKNOWN;
    if (!type_name_tag(ann, t, struct_name)) {
      err(diag_code::sem_unknown_type, "未知类型 '" + std::string(ann) + "'", line);
    }
    return t;
  }

  // ---- 第一遍：收集顶层声明 ----
  void collect_decls(const Program& prog) {
    for (const Stmt* s : prog.stmts) {
      switch (s->kind) {
        case NodeKind::FuncDecl: {
          const auto* f = static_cast<const FuncDecl*>(s);
          FuncSig sig{
              std::string(f->name), {f->params.begin(), f->params.end()}, std::string(f->ret_type), false, false,
          };
          sig.decl_line = f->line;
          if (!symtab_.add_func(std::move(sig))) {
            err(diag_code::sem_redefined, "'" + std::string(f->name) + "' 重复定义", f->line, first_sym_note(f->name));
          }
          break;
        }
        case NodeKind::ExternDecl: {
          const auto* e = static_cast<const ExternDecl*>(s);
          FuncSig sig{
              std::string(e->name), {e->params.begin(), e->params.end()}, std::string(e->ret_type), true, false,
          };
          sig.decl_line = e->line;
          if (!symtab_.add_func(std::move(sig))) {
            err(diag_code::sem_redefined, "'" + std::string(e->name) + "' 重复定义", e->line, first_sym_note(e->name));
          }
          break;
        }
        case NodeKind::ClassDecl:
        case NodeKind::StructDecl:
          collect_class(s);
          break;
        case NodeKind::VarDecl: {
          // 顶层变量进入模块作用域（与 codegen 的 global_vars 对应）。
          const auto* v = static_cast<const VarDecl*>(s);
          VarInfo info;
          std::string struct_name;
          if (!v->type_ann.empty()) {
            info.type = resolve_ann(v->type_ann, struct_name, v->line);
            info.struct_name = struct_name;
          }
          info.is_mut = v->is_mut;
          info.decl_line = v->line;
          if (!env_.declare(v->name, info)) {
            err(diag_code::sem_redefined, "顶层变量 '" + std::string(v->name) + "' 重复定义", v->line,
                first_def_note(v->name));
          }
          break;
        }
        default:
          break; // Import/Export/@imp 等不产生符号
      }
    }
  }

  void collect_class(const Stmt* s) {
    const bool is_class = s->kind == NodeKind::ClassDecl;
    const std::string_view name =
        is_class ? static_cast<const ClassDecl*>(s)->name : static_cast<const StructDecl*>(s)->name;
    const std::span<const ClassField> fields =
        is_class ? static_cast<const ClassDecl*>(s)->fields : static_cast<const StructDecl*>(s)->fields;
    const std::span<const FuncDecl* const> methods =
        is_class ? static_cast<const ClassDecl*>(s)->methods : std::span<const FuncDecl* const>{};

    ClassInfo info;
    info.name = std::string(name);
    info.decl_line = s->line;
    for (const ClassField& f : fields) {
      if (f.init != nullptr) {
        info.consts[std::string(f.name)] = f.init;
      } else {
        info.fields.push_back(f);
      }
    }
    for (const FuncDecl* m : methods) {
      FuncSig sig{
          info.name + "." + std::string(m->name),
          {m->params.begin(), m->params.end()},
          std::string(m->ret_type),
          false,
          true,
      };
      sig.decl_line = m->line;
      auto& set = info.methods[std::string(m->name)];
      // 同签名重复声明 → E0011；不同签名进入重载集合（按参数类型解析）
      const FuncSig* dup_of = nullptr;
      for (const FuncSig& existing : set) {
        if (params_identical(existing.params, sig.params)) {
          dup_of = &existing;
          break;
        }
      }
      if (dup_of != nullptr) {
        std::string note;
        if (dup_of->decl_line > 0) {
          note = "首次声明于第 " + std::to_string(dup_of->decl_line) + " 行\n";
        }
        note += help_of("同签名方法只允许声明一次；需要重载请使用不同的参数类型");
        err(diag_code::sem_redefined,
            "类 '" + info.name + "' 的方法 '" + std::string(m->name) + "' 以相同参数签名重复定义", m->line,
            std::move(note));
        continue;
      }
      set.push_back(std::move(sig));
    }
    // 重载集合分配后端唯一符号名：单个方法保持 "Class.method"（与既往后端
    // 具名约定一致），多个重载追加 "$<声明序号>"（LLVM 标识符允许 '$'）。
    for (auto& [mname, set] : info.methods) {
      if (set.size() == 1) {
        set[0].name = info.name + "." + mname;
        continue;
      }
      for (std::size_t i = 0; i < set.size(); ++i) {
        set[i].name = info.name + "." + mname + "$" + std::to_string(i);
      }
    }
    if (!symtab_.add_class(std::move(info))) {
      err(diag_code::sem_redefined, "'" + std::string(name) + "' 重复定义", s->line, first_sym_note(name));
    }
  }

  // ---- 第二遍：检查函数体 ----
  void check_bodies(const Program& prog) {
    // 先校验类/结构字段的类型标注（符号表已完整，可解析前向声明的类型）
    for (const Stmt* s : prog.stmts) {
      if (s->kind == NodeKind::ClassDecl || s->kind == NodeKind::StructDecl) {
        validate_fields(s);
      }
    }
    for (const Stmt* s : prog.stmts) {
      if (s->kind == NodeKind::FuncDecl) {
        const auto* f = static_cast<const FuncDecl*>(s);
        check_function(f->params, f->ret_type, f->body, "", f->line);
      } else if (s->kind == NodeKind::ClassDecl) {
        const auto* c = static_cast<const ClassDecl*>(s);
        for (const FuncDecl* m : c->methods) {
          check_function(m->params, m->ret_type, m->body, std::string(c->name), m->line);
        }
      }
    }
  }

  // 字段声明类型标注校验：未知类型报 E0003（每个字段只报一次）
  void validate_fields(const Stmt* s) {
    const std::span<const ClassField> fields = s->kind == NodeKind::ClassDecl
                                                   ? static_cast<const ClassDecl*>(s)->fields
                                                   : static_cast<const StructDecl*>(s)->fields;
    for (const ClassField& f : fields) {
      if (f.init != nullptr || f.type.empty()) {
        continue;
      }
      TypeTag t = TypeTag::TYPE_UNKNOWN;
      std::string struct_name;
      if (!type_name_tag(f.type, t, struct_name)) {
        err(diag_code::sem_unknown_type, "未知类型 '" + std::string(f.type) + "'", s->line);
      }
    }
  }

  void check_function(std::span<const Param> params, std::string_view ret_type, std::span<const Stmt* const> body,
                      const std::string& class_ctx, int line) {
    // 每个函数独立的作用域栈（保留栈底模块作用域：函数体可引用顶层变量）
    while (env_.depth() > 1) {
      env_.pop_scope();
    }
    env_.push_scope();
    loop_depth_ = 0;
    in_function_ = true;
    env_.clear_leaked(); // 作用域泄漏登记按函数隔离：清空上个函数的残留
    // 返回类型标注：未知类型在此报 E0003（无标注 / var → 通配）
    ret_info_ = VarInfo{};
    std::string ret_struct;
    ret_info_.type = resolve_ann(ret_type, ret_struct, line);
    ret_info_.struct_name = ret_struct;

    // 参数声明；类方法补隐式 self（源码已显式声明时校正其类型）
    const bool has_self = !params.empty() && params[0].name == "self";
    if (!class_ctx.empty() && !has_self) {
      env_.declare("self", VarInfo{TypeTag::TYPE_STRUCT, false, class_ctx});
    }
    for (const Param& p : params) {
      VarInfo info;
      std::string struct_name;
      if (!p.type.empty()) {
        info.type = resolve_ann(p.type, struct_name, line);
        info.struct_name = struct_name;
      }
      if (!class_ctx.empty() && p.name == "self") {
        info = VarInfo{TypeTag::TYPE_STRUCT, false, class_ctx};
      }
      if (!env_.declare(p.name, info)) {
        err(diag_code::sem_redefined, "参数 '" + std::string(p.name) + "' 重复定义", line, first_def_note(p.name));
      }
    }

    env_.push_scope();
    for (const Stmt* st : body) {
      check_stmt(st);
    }
    env_.pop_scope();
    env_.pop_scope();
  }

  // ---- 语句检查 ----
  void check_stmt(const Stmt* s) {
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

  void check_block(std::span<const Stmt* const> stmts) {
    env_.push_scope();
    for (const Stmt* st : stmts) {
      check_stmt(st);
    }
    env_.pop_scope();
  }

  // 赋值目标的类型（用于「无隐式转换」校验）；无法确定时返回通配。
  VarInfo check_assign_target(const Expr* t) {
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
    err(diag_code::sem_undefined, "不支持的赋值目标: " + std::string(node_kind_name(t->kind)), t->line);
    return {};
  }

  // 类型不匹配诊断（无隐式转换：不做 int↔float、string↔int 等任何转换）
  void expect_type(const VarInfo& want, const VarInfo& got, std::string_view what, int line) {
    if (same_type(want, got)) {
      return;
    }
    err(diag_code::sem_type_mismatch,
        "类型不匹配：" + std::string(what) + "（期望 '" + type_text(want) + "'，实际 '" + type_text(got) + "'）", line);
  }

  // 赋值检查：= 直接校验；复合赋值按 target op value 推导结果类型再校验。
  void check_assign_types(const Assign* a, const VarInfo& target, const VarInfo& value) {
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

  // ---- 表达式检查 + 类型推断 ----
  // 推断规则与 codegen 的发射选择保持一致（本阶段 codegen 仍自行推断，
  // 此处结果供 TypeEnv 与后续步骤使用）。
  VarInfo check_expr(const Expr* e) {
    if (e == nullptr) {
      return {};
    }
    switch (e->kind) {
      case NodeKind::IntLit:
        return {TypeTag::TYPE_INT, false, ""};
      case NodeKind::FloatLit:
        return {TypeTag::TYPE_FLOAT, false, ""};
      case NodeKind::StrLit:
        return {TypeTag::TYPE_STRING, false, ""};
      case NodeKind::BoolLit:
        return {TypeTag::TYPE_BOOL, false, ""};
      case NodeKind::NilLit:
        return {};
      case NodeKind::Ident: {
        const auto* id = static_cast<const Ident*>(e);
        if (const VarInfo* vi = env_.lookup(id->name)) {
          return *vi;
        }
        // 类名 / 导入前缀作为命名空间使用（配合 Property 访问）
        if (symtab_.is_type(id->name)) {
          return {TypeTag::TYPE_UNKNOWN, false, std::string(id->name)};
        }
        if (imports_ != nullptr && imports_->lib_by_prefix(id->name) != nullptr) {
          return {};
        }
        err_undefined_var(id->name, e->line);
        return {};
      }
      case NodeKind::ListLit: {
        const auto* l = static_cast<const ListLit*>(e);
        // 元素类型由首个可推断元素决定，后续元素须与之相同（无隐式转换）；
        // 元素为类/结构时不记录（下标结果保持通配）。
        VarInfo first;
        bool have_first = false;
        for (const Expr* item : l->items) {
          VarInfo it = check_expr(item);
          if (it.type == TypeTag::TYPE_UNKNOWN || it.type == TypeTag::TYPE_STRUCT) {
            continue;
          }
          if (!have_first) {
            first = it;
            have_first = true;
          } else {
            expect_type(first, it, "列表元素", item->line);
          }
        }
        VarInfo list{TypeTag::TYPE_ARRAY, false, ""};
        if (have_first) {
          list.elem = first.type;
        }
        return list;
      }
      case NodeKind::DictLit: {
        const auto* d = static_cast<const DictLit*>(e);
        for (const DictEntry& entry : d->entries) {
          check_expr(entry.key);
          check_expr(entry.value);
        }
        return {TypeTag::TYPE_MAP, false, ""};
      }
      case NodeKind::Index: {
        const auto* ix = static_cast<const Index*>(e);
        VarInfo base = check_expr(ix->obj);
        check_expr(ix->index);
        // 列表下标结果按推断的元素类型返回；map / 未知对象保持通配
        if (base.type == TypeTag::TYPE_ARRAY && base.elem != TypeTag::TYPE_UNKNOWN) {
          return VarInfo{base.elem, false, ""};
        }
        return {};
      }
      case NodeKind::Property:
        return check_property(static_cast<const Property*>(e));
      case NodeKind::New: {
        const auto* n = static_cast<const New*>(e);
        for (const Expr* a : n->args) {
          check_expr(a);
        }
        if (symtab_.find_class(n->class_name) == nullptr) {
          err(diag_code::sem_undefined, "未定义的类 '" + std::string(n->class_name) + "'", e->line,
              help_of("先声明 `class " + std::string(n->class_name) + ":`（或检查拼写）"));
          return {};
        }
        return {TypeTag::TYPE_STRUCT, false, std::string(n->class_name)};
      }
      case NodeKind::Unary: {
        const auto* u = static_cast<const Unary*>(e);
        VarInfo op = check_expr(u->operand);
        if (u->op == "!") {
          return {TypeTag::TYPE_BOOL, false, ""};
        }
        return op; // "-" 保持操作数类型
      }
      case NodeKind::Binary: {
        const auto* b = static_cast<const Binary*>(e);
        VarInfo l = check_expr(b->left);
        VarInfo r = check_expr(b->right);
        check_binary_types(b->op, l, r, b->line);
        return infer_binary(b->op, l, r);
      }
      case NodeKind::Call:
        return check_call(static_cast<const Call*>(e));
      default:
        return {};
    }
  }

  static bool is_arith(std::string_view op) { return op == "+" || op == "-" || op == "*" || op == "/" || op == "%"; }
  static bool is_compare(std::string_view op) {
    return op == "==" || op == "!=" || op == "<" || op == ">" || op == "<=" || op == ">=";
  }

  // 运算符类型规则（无隐式转换）：两侧类型须一致；算术仅限 int/float，
  // 字符串拼接两侧须为 string，比较两侧类型须一致。任一侧类型未定则跳过。
  void check_binary_types(std::string_view op, const VarInfo& l, const VarInfo& r, int line) {
    if (l.type == TypeTag::TYPE_UNKNOWN || r.type == TypeTag::TYPE_UNKNOWN) {
      return;
    }
    if (op == "and" || op == "or" || op == "&&" || op == "||") {
      return; // 逻辑运算按真值处理，不做类型限制
    }
    if (op == "+" && (l.type == TypeTag::TYPE_STRING || r.type == TypeTag::TYPE_STRING)) {
      if (!same_type(l, r)) {
        err(diag_code::sem_type_mismatch,
            "字符串拼接要求两侧均为 string（实际 '" + type_text(l) + "' 与 '" + type_text(r) + "'）", line);
      }
      return;
    }
    if (is_compare(op)) {
      if (!same_type(l, r)) {
        err(diag_code::sem_type_mismatch,
            "比较两侧类型不一致（'" + type_text(l) + "' 与 '" + type_text(r) + "'），不支持隐式转换", line);
      }
      return;
    }
    if (is_arith(op) || op == "^") {
      const bool l_num = l.type == TypeTag::TYPE_INT || l.type == TypeTag::TYPE_FLOAT;
      const bool r_num = r.type == TypeTag::TYPE_INT || r.type == TypeTag::TYPE_FLOAT;
      if (!l_num || !r_num) {
        err(diag_code::sem_type_mismatch,
            "运算符 '" + std::string(op) + "' 不支持类型 '" + type_text(l) + "' 与 '" + type_text(r) + "'", line);
        return;
      }
      if (l.type != r.type) {
        err(diag_code::sem_type_mismatch,
            "运算符 '" + std::string(op) + "' 两侧类型不一致（'" + type_text(l) + "' 与 '" + type_text(r) +
                "'），不支持隐式转换",
            line);
        return;
      }
      if (op == "%" && l.type == TypeTag::TYPE_FLOAT) {
        err(diag_code::sem_type_mismatch, "取模运算符 '%' 仅支持 int 操作数", line);
      }
    }
  }

  static VarInfo infer_binary(std::string_view op, const VarInfo& l, const VarInfo& r) {
    if (op == "and" || op == "or" || op == "&&" || op == "||" || is_compare(op)) {
      return {TypeTag::TYPE_BOOL, false, ""};
    }
    if ((op == "+") && (l.type == TypeTag::TYPE_STRING || r.type == TypeTag::TYPE_STRING))
    // 字符串拼接：任一侧为字符串（与 codegen 的发射规则一致）
    {
      return {TypeTag::TYPE_STRING, false, ""};
    }

    if (is_arith(op) || op == "^") {
      // 两侧皆浮点 → F64 族；否则 I32 族
      if (l.type == TypeTag::TYPE_FLOAT && r.type == TypeTag::TYPE_FLOAT) {
        return {TypeTag::TYPE_FLOAT, false, ""};
      }
      return {TypeTag::TYPE_INT, false, ""};
    }
    return {};
  }

  VarInfo check_property(const Property* p) {
    // 命名空间形态：ClassName.member / lib.member
    if (p->obj != nullptr && p->obj->kind == NodeKind::Ident) {
      const auto* id = static_cast<const Ident*>(p->obj);
      // 导入库前缀：存在性由调用点（check_call）判定，此处直接放行
      if (imports_ != nullptr && imports_->lib_by_prefix(id->name) != nullptr) {
        return {};
      }
      if (const ClassInfo* cls = symtab_.find_class(id->name)) {
        return class_member(*cls, p->name, p->line);
      }
    }
    // 实例形态：obj.field —— obj 类型已知为类时检查字段
    VarInfo base = check_expr(p->obj);
    if (base.type == TypeTag::TYPE_STRUCT) {
      if (const ClassInfo* cls = symtab_.find_class(base.struct_name)) {
        return class_member(*cls, p->name, p->line);
      }
    }
    return {};
  }

  // 类成员访问：类常量按初始化表达式推断，实例字段按声明类型。
  VarInfo class_member(const ClassInfo& cls, std::string_view member, int line) {
    if (auto it = cls.consts.find(std::string(member)); it != cls.consts.end()) {
      return check_expr(it->second);
    }
    for (const ClassField& f : cls.fields) {
      if (f.name == member) {
        VarInfo info;
        std::string struct_name;
        // 字段声明类型已在校验阶段确认；此处静默解析（未知类型视为通配）
        if (!f.type.empty() && type_name_tag(f.type, info.type, struct_name)) {
          info.struct_name = struct_name;
        }
        return info;
      }
    }
    err(diag_code::sem_undefined, "类 '" + cls.name + "' 没有字段或常量 '" + std::string(member) + "'", line,
        help_of("检查成员名拼写；字段/常量需在 `" + cls.name + "` 内声明"));
    return {};
  }

  // 声明参数中显式 self 占首槽（codegen 约定），不计入调用实参
  static std::size_t self_offset(const FuncSig& sig) {
    return (!sig.params.empty() && sig.params[0].name == "self") ? 1 : 0;
  }

  // 参数标注 → 类型（未知标注/空标注视为通配，不报错）
  VarInfo param_info(const Param& p) {
    VarInfo info;
    std::string struct_name;
    if (!p.type.empty() && type_name_tag(p.type, info.type, struct_name)) {
      info.struct_name = struct_name;
    }
    return info;
  }

  // 实参与候选签名的参数类型是否全部兼容
  bool args_compatible(const FuncSig& sig, const std::vector<VarInfo>& args) {
    const std::size_t off = self_offset(sig);
    if (sig.params.size() < off || args.size() != sig.params.size() - off) {
      return false;
    }
    for (std::size_t i = 0; i < args.size(); ++i) {
      if (!same_type(param_info(sig.params[i + off]), args[i])) {
        return false;
      }
    }
    return true;
  }

  // 已确定签名后的实参类型校验（逐参数报错，带参数序号）
  void check_arg_types(const std::string& callee, const FuncSig& sig, const std::vector<VarInfo>& args, int line) {
    const std::size_t off = self_offset(sig);
    for (std::size_t i = 0; i < args.size() && i + off < sig.params.size(); ++i) {
      expect_type(param_info(sig.params[i + off]), args[i],
                  "调用 '" + callee + "' 的第 " + std::to_string(i + 1) + " 个参数", line);
    }
  }

  // 方法重载解析：先按实参个数过滤，再按参数类型过滤，
  // 零匹配时报 E0006/E0007，多匹配时报 E0008（歧义）。
  const FuncSig* resolve_method(const Call* call, const ClassInfo& cls, std::string_view mname,
                                const std::vector<VarInfo>& args, int line) {
    const std::string label = cls.name + "." + std::string(mname);
    auto it = cls.methods.find(std::string(mname));
    if (it == cls.methods.end()) {
      err(diag_code::sem_undefined, "类 '" + cls.name + "' 没有方法 '" + std::string(mname) + "'", line,
          help_of("检查方法名拼写；方法需在 `" + cls.name + "` 内声明"));
      return nullptr;
    }
    const auto& overloads = it->second;
    std::vector<const FuncSig*> arity_ok;
    for (const FuncSig& sig : overloads) {
      const std::size_t off = self_offset(sig);
      if (sig.params.size() >= off && args.size() == sig.params.size() - off) {
        arity_ok.push_back(&sig);
      }
    }
    if (arity_ok.empty()) {
      err(diag_code::sem_arity, "调用 '" + label + "' 没有接受 " + std::to_string(args.size()) + " 个参数的重载", line);
      return nullptr;
    }
    std::vector<const FuncSig*> type_ok;
    for (const FuncSig* sig : arity_ok) {
      if (args_compatible(*sig, args)) {
        type_ok.push_back(sig);
      }
    }
    if (type_ok.empty()) {
      err(diag_code::sem_type_mismatch, "调用 '" + label + "' 的实参类型不匹配任何重载", line);
      return nullptr;
    }
    if (type_ok.size() > 1) {
      err(diag_code::sem_ambiguous, "调用 '" + label + "' 匹配到多个重载，存在歧义", line);
      return nullptr;
    }
    // 记录解析出的后端符号名，供 codegen 直接复用（重载需唯一具名函数）
    if (analysis_ != nullptr) {
      analysis_->method_symbols[call] = type_ok.front()->name;
    }
    return type_ok.front();
  }

  VarInfo check_call(const Call* c) {
    std::vector<VarInfo> args;
    args.reserve(c->args.size());
    for (const Expr* a : c->args) {
      args.push_back(check_expr(a));
    }
    // 普通函数调用：f(args)
    if (c->callee->kind == NodeKind::Ident) {
      const auto* id = static_cast<const Ident*>(c->callee);
      if (const FuncSig* sig = symtab_.find_func(id->name)) {
        check_arity(std::string(id->name), args.size(), sig->params.size(), c->line);
        // extern 为 FFI 边界（int 常作通用指针），只查个数不查类型
        if (!sig->is_extern) {
          check_arg_types(std::string(id->name), *sig, args, c->line);
        }
        return sig_ret_info(*sig);
      }
      if (const BuiltinInfo* b = find_builtin(id->name)) {
        if (b->arity != kBuiltinArityUnchecked) {
          check_arity(std::string(id->name), args.size(), static_cast<std::size_t>(b->arity), c->line);
        }
        return {b->ret, false, ""};
      }
      // 未在本编译单元声明的函数不报错：vmake 多文件工程允许跨文件调用，
      // 符号在链接期解析（跨 TU 的函数存在性检查待引入模块级编译后再落地）。
      return {};
    }
    // 命名空间调用：lib.f(args) / 方法调用：o.m(args)
    if (c->callee->kind == NodeKind::Property) {
      const auto* prop = static_cast<const Property*>(c->callee);
      // 导入库命名空间：前缀命中 → 符号存在性 + 参数个数
      if (prop->obj != nullptr && prop->obj->kind == NodeKind::Ident && imports_ != nullptr) {
        const auto* id = static_cast<const Ident*>(prop->obj);
        if (const ImportedLib* lib = imports_->lib_by_prefix(id->name)) {
          const ImportedSymbol* sym = ImportTable::find_symbol(*lib, prop->name);
          if (sym == nullptr) {
            err(diag_code::sem_undefined, "库 '" + lib->name + "' 未声明符号 '" + std::string(prop->name) + "'",
                c->line, help_of("检查 `.vxh` 声明文件中是否有该符号，或核对符号拼写"));
            return {};
          }
          check_arity(lib->name + "." + sym->vx_name, args.size(), sym->params.size(), c->line);
          VarInfo info;
          std::string struct_name;
          // 导入符号的返回类型来自 .vxh，未知类型静默放行（不在此处报 E0003）
          if (type_name_tag(sym->ret_type, info.type, struct_name)) {
            info.struct_name = struct_name;
          }
          return info;
        }
      }
      // 方法调用：接收者类型为已知类时按参数类型做重载解析
      VarInfo base = check_expr(prop->obj);
      if (base.type == TypeTag::TYPE_STRUCT) {
        if (const ClassInfo* cls = symtab_.find_class(base.struct_name)) {
          if (const FuncSig* sig = resolve_method(c, *cls, prop->name, args, c->line)) {
            return sig_ret_info(*sig);
          }
        }
      }
      return {};
    }
    return {};
  }

  // 函数/方法返回类型 → 推断结果。外部签名（.vxh）类型可能超出本阶段
  // 已知集合，未知时不报错（extern 声明本身的标注检查在后续步骤覆盖）。
  VarInfo sig_ret_info(const FuncSig& sig) {
    VarInfo info;
    std::string struct_name;
    if (type_name_tag(sig.ret_type, info.type, struct_name)) {
      info.struct_name = struct_name;
    }
    return info;
  }

  void check_arity(const std::string& callee, std::size_t got, std::size_t expect, int line) {
    if (got != expect) {
      err(diag_code::sem_arity,
          "调用 '" + callee + "' 需要 " + std::to_string(expect) + " 个参数，实际传入 " + std::to_string(got) + " 个",
          line);
    }
  }

  // ---- 状态 ----
  std::string_view filename_;
  DiagBag& bag_;
  const ImportTable* imports_ = nullptr;
  Analysis* analysis_ = nullptr;
  SymbolTable symtab_;
  TypeEnv env_;
  int loop_depth_ = 0;
  bool in_function_ = false;
  VarInfo ret_info_; // 当前函数/方法的返回类型（void 表示显式 void）
  bool ok_ = true;
};

} // namespace

// ---- Scope / TypeEnv / SymbolTable ----

bool Scope::declare(std::string_view name, VarInfo info) {
  std::string key(name);
  return vars_.emplace(std::move(key), info).second;
}

const VarInfo* Scope::lookup(std::string_view name) const {
  auto it = vars_.find(std::string(name));
  return it == vars_.end() ? nullptr : &it->second;
}

void Scope::refine(std::string_view name, const VarInfo& info) {
  auto it = vars_.find(std::string(name));
  if (it == vars_.end() || it->second.type != TypeTag::TYPE_UNKNOWN) {
    return; // 未声明或类型已确定（标注/已推断）：不覆盖
  }
  it->second.type = info.type;
  it->second.struct_name = info.struct_name;
  it->second.elem = info.elem;
}

void TypeEnv::push_scope() {
  scopes_.emplace_back();
}
void TypeEnv::pop_scope() {
  if (scopes_.empty()) {
    return;
  }
  for (const auto& [name, info] : scopes_.back().vars()) {
    leaked_[name] = info.decl_line; // 登记离开作用域的变量（E0012 判定用）
  }
  scopes_.pop_back();
}

bool TypeEnv::declare(std::string_view name, VarInfo info) {
  return scopes_.empty() ? false : scopes_.back().declare(name, std::move(info));
}

const VarInfo* TypeEnv::lookup(std::string_view name) const {
  for (const auto& scope : std::views::reverse(scopes_)) {
    if (const VarInfo* vi = scope.lookup(name)) {
      return vi;
    }
  }
  return nullptr;
}

void TypeEnv::refine(std::string_view name, VarInfo info) {
  // 只细化最近一层中类型未定的同名绑定（内层遮蔽外层，不越层修改）
  for (auto& scope : std::views::reverse(scopes_)) {
    if (scope.lookup(name) != nullptr) {
      scope.refine(name, info);
      return;
    }
  }
}

bool SymbolTable::add_func(FuncSig sig) {
  // 函数名与类名共用命名空间（new X 与 f(X) 不允许歧义）
  if (classes_.contains(sig.name)) {
    return false;
  }
  std::string key = sig.name;
  return funcs_.emplace(std::move(key), std::move(sig)).second;
}

bool SymbolTable::add_class(ClassInfo info) {
  if (funcs_.contains(info.name)) {
    return false;
  }
  std::string key = info.name;
  return classes_.emplace(std::move(key), std::move(info)).second;
}

const FuncSig* SymbolTable::find_func(std::string_view name) const {
  auto it = funcs_.find(std::string(name));
  return it == funcs_.end() ? nullptr : &it->second;
}

const ClassInfo* SymbolTable::find_class(std::string_view name) const {
  auto it = classes_.find(std::string(name));
  return it == classes_.end() ? nullptr : &it->second;
}

bool SymbolTable::is_type(std::string_view name) const {
  return classes_.contains(std::string(name));
}

bool check_module(const Program& prog, std::string_view filename, DiagBag& bag, const ImportTable* imports,
                  Analysis* out) {
  return TypeChecker(filename, bag, imports, out).run(prog);
}

} // namespace vx::compiler
