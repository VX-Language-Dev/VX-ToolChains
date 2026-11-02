// semantics.cpp — 独立语义分析：符号表 + 类型环境 + 检查器。
// 两遍扫描：第一遍收集顶层声明（函数/extern/类/结构/顶层变量，允许前向引用），
// 第二遍逐函数检查语句与表达式。诊断聚合输出（不首错即停）。
// 检查范围与 codegen 解耦进度见 semantics.hpp 注释。
#include "vx/compiler/semantics.hpp"

#include <cstddef>
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

class TypeChecker {
public:
  TypeChecker(std::string_view filename, DiagBag& bag, const ImportTable* imports)
      : filename_(filename), bag_(bag), imports_(imports) {}

  bool run(const Program& prog) {
    env_.push_scope(); // 模块级作用域（顶层变量）
    collect_decls(prog);
    check_bodies(prog);
    return ok_;
  }

private:
  // ---- 诊断 ----
  void err(std::string_view code, std::string msg, int line) {
    bag_.add(make_diag(code, Severity::Error, std::move(msg), std::string(filename_), line, 0));
    ok_ = false;
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
              std::string(f->name), {f->params.begin(), f->params.end()}, std::string(f->ret_type), false, false};
          if (!symtab_.add_func(std::move(sig))) {
            err(diag_code::sem_redefined, "'" + std::string(f->name) + "' 重复定义", f->line);
          }
          break;
        }
        case NodeKind::ExternDecl: {
          const auto* e = static_cast<const ExternDecl*>(s);
          FuncSig sig{
              std::string(e->name), {e->params.begin(), e->params.end()}, std::string(e->ret_type), true, false};
          if (!symtab_.add_func(std::move(sig))) {
            err(diag_code::sem_redefined, "'" + std::string(e->name) + "' 重复定义", e->line);
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
          if (!env_.declare(v->name, info)) {
            err(diag_code::sem_redefined, "顶层变量 '" + std::string(v->name) + "' 重复定义", v->line);
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
    for (const ClassField& f : fields) {
      if (f.init) {
        info.consts[std::string(f.name)] = f.init;
      } else {
        info.fields.push_back(f);
      }
    }
    for (const FuncDecl* m : methods) {
      FuncSig sig{info.name + "." + std::string(m->name),
                  {m->params.begin(), m->params.end()},
                  std::string(m->ret_type),
                  false,
                  true};
      if (!info.methods.emplace(std::string(m->name), std::move(sig)).second) {
        err(diag_code::sem_redefined, "类 '" + info.name + "' 的方法 '" + std::string(m->name) + "' 重复定义", m->line);
      }
    }
    if (!symtab_.add_class(std::move(info))) {
      err(diag_code::sem_redefined, "'" + std::string(name) + "' 重复定义", s->line);
    }
  }

  // ---- 第二遍：检查函数体 ----
  void check_bodies(const Program& prog) {
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

  void check_function(std::span<const Param> params, std::string_view ret_type, std::span<const Stmt* const> body,
                      const std::string& class_ctx, int line) {
    // 每个函数独立的作用域栈（保留栈底模块作用域：函数体可引用顶层变量）
    while (env_.depth() > 1) {
      env_.pop_scope();
    }
    env_.push_scope();
    loop_depth_ = 0;
    in_function_ = true;
    cur_ret_ = std::string(ret_type);

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
        err(diag_code::sem_redefined, "参数 '" + std::string(p.name) + "' 重复定义", line);
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
          // 标注优先；未标注时用初始化表达式推断
          if (info.type == TypeTag::TYPE_UNKNOWN && init.type != TypeTag::TYPE_UNKNOWN) {
            info = init;
            info.is_mut = v->is_mut;
          }
        }
        if (!env_.declare(v->name, info)) {
          err(diag_code::sem_redefined, "变量 '" + std::string(v->name) + "' 在同一作用域内重复定义", v->line);
        }
        break;
      }
      case NodeKind::Assign: {
        const auto* a = static_cast<const Assign*>(s);
        check_assign_target(a->target);
        check_expr(a->value);
        break;
      }
      case NodeKind::Return: {
        const auto* r = static_cast<const Return*>(s);
        if (!in_function_) {
          err(diag_code::sem_flow, "return 不能出现在函数外", r->line);
        }
        if (r->value != nullptr) {
          check_expr(r->value);
        }
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
        check_expr(f->iterable);
        env_.push_scope();
        env_.declare(f->var, VarInfo{TypeTag::TYPE_UNKNOWN, true, ""});
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

  void check_assign_target(const Expr* t) {
    if (t == nullptr) {
      return;
    }
    if (t->kind == NodeKind::Ident) {
      const auto* id = static_cast<const Ident*>(t);
      if (env_.lookup(id->name) == nullptr) {
        err(diag_code::sem_undefined, "未定义变量 '" + std::string(id->name) + "'", t->line);
      }
      return;
    }
    if (t->kind == NodeKind::Property) {
      // 属性赋值：字段存在性检查（复用表达式路径，但不取其推断结果）
      check_expr(t);
      return;
    }
    if (t->kind == NodeKind::Index) {
      const auto* ix = static_cast<const Index*>(t);
      check_expr(ix->obj);
      check_expr(ix->index);
      return;
    }
    err(diag_code::sem_undefined, "不支持的赋值目标: " + std::string(node_kind_name(t->kind)), t->line);
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
        err(diag_code::sem_undefined, "未定义变量 '" + std::string(id->name) + "'", e->line);
        return {};
      }
      case NodeKind::ListLit: {
        const auto* l = static_cast<const ListLit*>(e);
        for (const Expr* item : l->items) {
          check_expr(item);
        }
        return {TypeTag::TYPE_ARRAY, false, ""};
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
        check_expr(ix->obj);
        check_expr(ix->index);
        return {}; // 元素类型本阶段不追踪
      }
      case NodeKind::Property:
        return check_property(static_cast<const Property*>(e));
      case NodeKind::New: {
        const auto* n = static_cast<const New*>(e);
        for (const Expr* a : n->args) {
          check_expr(a);
        }
        if (symtab_.find_class(n->class_name) == nullptr) {
          err(diag_code::sem_undefined, "未定义的类 '" + std::string(n->class_name) + "'", e->line);
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

  VarInfo infer_binary(std::string_view op, const VarInfo& l, const VarInfo& r) {
    if (op == "and" || op == "or" || op == "&&" || op == "||" || is_compare(op)) {
      return {TypeTag::TYPE_BOOL, false, ""};
    }
    if (op == "+") {
      // 字符串拼接：任一侧为字符串（与 codegen 的发射规则一致）
      if (l.type == TypeTag::TYPE_STRING || r.type == TypeTag::TYPE_STRING) {
        return {TypeTag::TYPE_STRING, false, ""};
      }
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
        if (!f.type.empty()) {
          info.type = resolve_ann(f.type, struct_name, line);
          info.struct_name = struct_name;
        }
        return info;
      }
    }
    err(diag_code::sem_undefined, "类 '" + cls.name + "' 没有字段或常量 '" + std::string(member) + "'", line);
    return {};
  }

  VarInfo check_call(const Call* c) {
    for (const Expr* a : c->args) {
      check_expr(a);
    }
    // 普通函数调用：f(args)
    if (c->callee->kind == NodeKind::Ident) {
      const auto* id = static_cast<const Ident*>(c->callee);
      if (const FuncSig* sig = symtab_.find_func(id->name)) {
        check_arity(std::string(id->name), c->args.size(), sig->params.size(), c->line);
        return sig_ret_info(*sig);
      }
      if (const BuiltinInfo* b = find_builtin(id->name)) {
        if (b->arity != kBuiltinArityUnchecked) {
          check_arity(std::string(id->name), c->args.size(), static_cast<std::size_t>(b->arity), c->line);
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
                c->line);
            return {};
          }
          check_arity(lib->name + "." + sym->vx_name, c->args.size(), sym->params.size(), c->line);
          VarInfo info;
          std::string struct_name;
          // 导入符号的返回类型来自 .vxh，未知类型静默放行（不在此处报 E0003）
          if (type_name_tag(sym->ret_type, info.type, struct_name)) {
            info.struct_name = struct_name;
          }
          return info;
        }
      }
      // 方法调用：接收者类型为已知类时检查方法存在性与参数个数
      VarInfo base = check_expr(prop->obj);
      if (base.type == TypeTag::TYPE_STRUCT) {
        if (const ClassInfo* cls = symtab_.find_class(base.struct_name)) {
          auto it = cls->methods.find(std::string(prop->name));
          if (it == cls->methods.end()) {
            err(diag_code::sem_undefined, "类 '" + cls->name + "' 没有方法 '" + std::string(prop->name) + "'", c->line);
            return {};
          }
          const FuncSig& sig = it->second;
          // 声明参数含显式 self 时不计入调用实参（与 codegen 的接收者约定一致）
          const bool explicit_self = !sig.params.empty() && sig.params[0].name == "self";
          const std::size_t expect = sig.params.size() - (explicit_self ? 1 : 0);
          check_arity(cls->name + "." + sig.name, c->args.size(), expect, c->line);
          return sig_ret_info(sig);
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
  SymbolTable symtab_;
  TypeEnv env_;
  int loop_depth_ = 0;
  bool in_function_ = false;
  std::string cur_ret_;
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

void TypeEnv::push_scope() {
  scopes_.emplace_back();
}
void TypeEnv::pop_scope() {
  if (!scopes_.empty()) {
    scopes_.pop_back();
  }
}

bool TypeEnv::declare(std::string_view name, VarInfo info) {
  return scopes_.empty() ? false : scopes_.back().declare(name, std::move(info));
}

const VarInfo* TypeEnv::lookup(std::string_view name) const {
  for (auto it = scopes_.rbegin(); it != scopes_.rend(); ++it) {
    if (const VarInfo* vi = it->lookup(name)) {
      return vi;
    }
  }
  return nullptr;
}

bool SymbolTable::add_func(FuncSig sig) {
  // 函数名与类名共用命名空间（new X 与 f(X) 不允许歧义）
  if (classes_.count(sig.name) > 0) {
    return false;
  }
  std::string key = sig.name;
  return funcs_.emplace(std::move(key), std::move(sig)).second;
}

bool SymbolTable::add_class(ClassInfo info) {
  if (funcs_.count(info.name) > 0) {
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
  return classes_.count(std::string(name)) > 0;
}

bool check_module(const Program& prog, std::string_view filename, DiagBag& bag, const ImportTable* imports) {
  return TypeChecker(filename, bag, imports).run(prog);
}

} // namespace vx::compiler
