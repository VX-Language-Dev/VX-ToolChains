// semantics_checker_expr.hpp — 表达式检查 + 类型推断 + 调用解析（内部实现头，仅供 semantics.cpp 使用）
#pragma once

#include "semantics_checker.hpp"

namespace vx::compiler {

namespace {

// ---- 表达式检查 + 类型推断 ----
// 推断规则与 codegen 的发射选择保持一致（本阶段 codegen 仍自行推断，
// 此处结果供 TypeEnv 与后续步骤使用）。
inline VarInfo TypeChecker::check_expr(const Expr* e) {
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
    case NodeKind::Deref: {
      const auto* d = static_cast<const Deref*>(e);
      VarInfo base = check_expr(d->ptr);
      if (base.type != TypeTag::TYPE_UNKNOWN && base.type != TypeTag::TYPE_POINTER) {
        err(diag_code::sem_type_mismatch, "指针解引用要求 ptr 类型（实际 '" + type_text(base) + "'）", e->line);
      }
      return {TypeTag::TYPE_INT, false, ""};
    }
    case NodeKind::FuncRef:
      // 函数值：符号在链接期解析，语义层不做存在性检查（支持跨文件 &fn）
      return {TypeTag::TYPE_POINTER, false, ""};
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
      const ClassInfo* cls = symtab_.find_class(n->class_name);
      if (cls == nullptr) {
        err(diag_code::sem_undefined, "未定义的类 '" + std::string(n->class_name) + "'", e->line,
            help_of("先声明 `class " + std::string(n->class_name) + ":`（或检查拼写）"));
        return {};
      }
      // 构造器与调用保持一致：new C(args) 对应 C.new(self, args)。
      // 无实参的 new C() 是裸分配（构造函数内部同样使用该形式），不检查构造器。
      const auto mit = cls->methods.find("new");
      if (!n->args.empty() && (mit == cls->methods.end() || mit->second.empty())) {
        err(diag_code::sem_undefined, "类 '" + std::string(n->class_name) + "' 未声明构造方法 new，不能传参构造",
            e->line, help_of("在类内添加 `func new(...)`，或去掉构造实参"));
      } else if (!n->args.empty()) {
        bool arity_ok = false;
        for (const FuncSig& sig : mit->second) {
          arity_ok = arity_ok || sig.params.size() == n->args.size();
        }
        if (!arity_ok) {
          check_arity(std::string(n->class_name) + ".new", n->args.size(), mit->second.front().params.size(), e->line);
        }
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

// 运算符类型规则（无隐式转换）：两侧类型须一致；算术仅限 int/float，
// 字符串拼接两侧须为 string，比较两侧类型须一致。任一侧类型未定则跳过。
inline void TypeChecker::check_binary_types(std::string_view op, const VarInfo& l, const VarInfo& r, int line) {
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

inline VarInfo TypeChecker::infer_binary(std::string_view op, const VarInfo& l, const VarInfo& r) {
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

// 在导入模块摘要中定位类（用于构造实参数校验）
inline const ModuleClass* TypeChecker::find_module_class(const std::string& prefix, std::string_view name) const {
  if (imports_ == nullptr) {
    return nullptr;
  }
  if (const ImportedModule* mod = imports_->module_by_prefix(prefix)) {
    for (const ModuleClass& c : mod->classes) {
      if (c.name == name)
        return &c;
    }
  }
  return nullptr;
}

inline VarInfo TypeChecker::check_property(const Property* p) {
  // 命名空间形态：ClassName.member / lib.member
  if (p->obj != nullptr && p->obj->kind == NodeKind::Ident) {
    const auto* id = static_cast<const Ident*>(p->obj);
    // 导入库前缀：存在性由调用点（check_call）判定，此处直接放行
    if (imports_ != nullptr && imports_->lib_by_prefix(id->name) != nullptr) {
      return {};
    }
    // 模块命名空间：import std.error → error.Error / error.fn
    if (module_prefixes_.contains(std::string(id->name))) {
      if (const ClassInfo* cls = symtab_.find_class(p->name)) {
        return {TypeTag::TYPE_STRUCT, false, cls->name};
      }
      if (const FuncSig* sig = symtab_.find_func(p->name)) {
        return sig_ret_info(*sig);
      }
      if (symtab_.find_enum(p->name) != nullptr) {
        return {TypeTag::TYPE_INT, false, ""};
      }
      err(diag_code::sem_undefined, "模块 '" + std::string(id->name) + "' 没有符号 '" + std::string(p->name) + "'",
          p->line, help_of("检查被导入模块是否声明了该类型/函数，或核对拼写"));
      return {};
    }
    if (const ClassInfo* cls = symtab_.find_class(id->name)) {
      return class_member(*cls, p->name, p->line);
    }
    // 枚举成员：EnumName.MEMBER → 底层 int 常量
    if (const EnumInfo* en = symtab_.find_enum(id->name)) {
      if (en->members.contains(std::string(p->name))) {
        return {TypeTag::TYPE_INT, false, ""};
      }
      err(diag_code::sem_undefined, "枚举 '" + en->name + "' 没有成员 '" + std::string(p->name) + "'", p->line,
          help_of("检查成员名拼写；成员需在 `enum " + en->name + "` 内声明"));
      return {};
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
inline VarInfo TypeChecker::class_member(const ClassInfo& cls, std::string_view member, int line) {
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

// 参数标注 → 类型（未知标注/空标注视为通配，不报错）
inline VarInfo TypeChecker::param_info(const Param& p) {
  VarInfo info;
  std::string struct_name;
  if (!p.type.empty() && type_name_tag(p.type, info.type, struct_name)) {
    info.struct_name = struct_name;
  }
  return info;
}

// 实参与候选签名的参数类型是否全部兼容
inline bool TypeChecker::args_compatible(const FuncSig& sig, const std::vector<VarInfo>& args) {
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
inline void TypeChecker::check_arg_types(const std::string& callee, const FuncSig& sig,
                                         const std::vector<VarInfo>& args, int line) {
  const std::size_t off = self_offset(sig);
  for (std::size_t i = 0; i < args.size() && i + off < sig.params.size(); ++i) {
    expect_type(param_info(sig.params[i + off]), args[i],
                "调用 '" + callee + "' 的第 " + std::to_string(i + 1) + " 个参数", line);
  }
}

// 方法重载解析：先按实参个数过滤，再按参数类型过滤，
// 零匹配时报 E0006/E0007，多匹配时报 E0008（歧义）。
inline const FuncSig* TypeChecker::resolve_method(const Call* call, const ClassInfo& cls, std::string_view mname,
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

inline VarInfo TypeChecker::check_call(const Call* c) {
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
          err(diag_code::sem_undefined, "库 '" + lib->name + "' 未声明符号 '" + std::string(prop->name) + "'", c->line,
              help_of("检查 `.vxh` 声明文件中是否有该符号，或核对符号拼写"));
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
    // 模块命名空间调用：error.Error(args) 构造 / error.fn(args) 函数调用
    if (prop->obj != nullptr && prop->obj->kind == NodeKind::Ident &&
        module_prefixes_.contains(std::string(static_cast<const Ident*>(prop->obj)->name))) {
      const std::string prefix(static_cast<const Ident*>(prop->obj)->name);
      if (const ClassInfo* cls = symtab_.find_class(prop->name)) {
        const ModuleClass* mc = find_module_class(prefix, prop->name);
        const std::size_t arity = mc != nullptr && mc->has_ctor ? mc->ctor_arity : 0;
        check_arity(prefix + "." + std::string(prop->name), args.size(), arity, c->line);
        return {TypeTag::TYPE_STRUCT, false, cls->name};
      }
      if (const FuncSig* sig = symtab_.find_func(prop->name)) {
        if (!sig->is_extern) {
          check_arg_types(std::string(prop->name), *sig, args, c->line);
        } else {
          check_arity(std::string(prop->name), args.size(), sig->params.size(), c->line);
        }
        return sig_ret_info(*sig);
      }
      err(diag_code::sem_undefined, "模块 '" + prefix + "' 没有符号 '" + std::string(prop->name) + "'", c->line,
          help_of("检查被导入模块是否声明了该类型/函数，或核对拼写"));
      return {};
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
inline VarInfo TypeChecker::sig_ret_info(const FuncSig& sig) {
  VarInfo info;
  std::string struct_name;
  if (type_name_tag(sig.ret_type, info.type, struct_name)) {
    info.struct_name = struct_name;
  }
  return info;
}

inline void TypeChecker::check_arity(const std::string& callee, std::size_t got, std::size_t expect, int line) {
  if (got != expect) {
    err(diag_code::sem_arity,
        "调用 '" + callee + "' 需要 " + std::to_string(expect) + " 个参数，实际传入 " + std::to_string(got) + " 个",
        line);
  }
}

} // namespace

} // namespace vx::compiler
