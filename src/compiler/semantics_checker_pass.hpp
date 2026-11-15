// semantics_checker_pass.hpp — 类型名解析 + 两遍扫描之声明收集（内部实现头，仅供 semantics.cpp 使用）
#pragma once

#include "semantics_checker.hpp"

namespace vx::compiler {

namespace {

// ---- 类型名解析 ----
// 返回 false = 未知类型（调用方报 E0003）。
inline bool TypeChecker::type_name_tag(std::string_view name, TypeTag& out, std::string& struct_name) const {
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
  if (name == "string" || name == "str") {
    out = TypeTag::TYPE_STRING;
    return true;
  }
  if (name == "ptr") {
    out = TypeTag::TYPE_POINTER;
    return true;
  }
  if (name == "void") {
    out = TypeTag::TYPE_VOID;
    return true;
  }
  // 枚举底层类型为 int：枚举名可作类型标注，与 int 同型
  if (symtab_.find_enum(name) != nullptr) {
    out = TypeTag::TYPE_INT;
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
inline TypeTag TypeChecker::resolve_ann(std::string_view ann, std::string& struct_name, int line) {
  TypeTag t = TypeTag::TYPE_UNKNOWN;
  if (!type_name_tag(ann, t, struct_name)) {
    err(diag_code::sem_unknown_type, "未知类型 '" + std::string(ann) + "'", line);
  }
  return t;
}

// ---- 第一遍：收集顶层声明 ----
inline void TypeChecker::collect_decls(const Program& prog) {
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
      case NodeKind::EnumDecl:
        collect_enum(s);
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

inline void TypeChecker::collect_class(const Stmt* s) {
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

// import 模块声明并入符号表：仅补齐本单元未声明的类/结构、枚举与函数。
// 模块函数体不参与本次编译，符号由 vlnk 合并各 .vxobj 时解析。
inline void TypeChecker::merge_modules() {
  if (imports_ == nullptr) {
    return;
  }
  for (const ImportedModule& mod : imports_->modules) {
    module_prefixes_.insert(mod.prefix);
    for (const ModuleEnum& me : mod.enums) {
      if (symtab_.find_enum(me.name) != nullptr || symtab_.find_class(me.name) != nullptr ||
          symtab_.find_func(me.name) != nullptr) {
        continue; // 本单元同名声明优先
      }
      EnumInfo info;
      info.name = me.name;
      info.decl_line = me.line;
      for (const auto& [mname, value] : me.members)
        info.members.emplace(mname, value);
      symtab_.add_enum(std::move(info));
    }
    for (const ModuleClass& mc : mod.classes) {
      if (symtab_.find_class(mc.name) != nullptr || symtab_.find_enum(mc.name) != nullptr ||
          symtab_.find_func(mc.name) != nullptr) {
        continue;
      }
      ClassInfo info;
      info.name = mc.name;
      info.decl_line = mc.line;
      for (const ModuleField& f : mc.fields) {
        info.fields.push_back(ClassField{intern(f.name), intern(f.type), nullptr});
      }
      for (const ModuleMethod& mm : mc.methods) {
        std::vector<Param> params;
        params.reserve(mm.params.size());
        for (const ModuleParam& p : mm.params)
          params.push_back(Param{intern(p.name), intern(p.type)});
        FuncSig sig;
        sig.name = mc.name + "." + mm.name;
        sig.params = std::move(params);
        sig.ret_type = mm.ret_type;
        sig.is_method = true;
        sig.decl_line = mc.line;
        info.methods[mm.name].push_back(std::move(sig));
      }
      symtab_.add_class(std::move(info));
    }
    for (const ModuleFunc& mf : mod.funcs) {
      if (symtab_.find_func(mf.name) != nullptr || symtab_.find_class(mf.name) != nullptr ||
          symtab_.find_enum(mf.name) != nullptr) {
        continue;
      }
      std::vector<Param> params;
      params.reserve(mf.params.size());
      for (const ModuleParam& p : mf.params)
        params.push_back(Param{intern(p.name), intern(p.type)});
      FuncSig sig;
      sig.name = mf.name;
      sig.params = std::move(params);
      sig.ret_type = mf.ret_type;
      sig.is_extern = mf.is_extern;
      sig.decl_line = mf.line;
      symtab_.add_func(std::move(sig));
    }
  }
}

// 模块声明摘要里的字符串需在本检查器生命周期内保持稳定
inline std::string_view TypeChecker::intern(const std::string& s) {
  intern_pool_.push_back(s);
  return intern_pool_.back();
}

inline void TypeChecker::collect_enum(const Stmt* s) {
  const auto* e = static_cast<const EnumDecl*>(s);
  EnumInfo info;
  info.name = std::string(e->name);
  info.decl_line = e->line;
  for (const EnumMember& m : e->members) {
    const std::string mname(m.name);
    if (!info.members.emplace(mname, m.value).second) {
      err(diag_code::sem_redefined, "枚举 '" + info.name + "' 的成员 '" + mname + "' 重复定义", m.line);
    }
  }
  if (!symtab_.add_enum(std::move(info))) {
    err(diag_code::sem_redefined, "'" + std::string(e->name) + "' 重复定义", e->line, first_sym_note(e->name));
  }
}

// ---- 第二遍：检查函数体 ----
inline void TypeChecker::check_bodies(const Program& prog) {
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
inline void TypeChecker::validate_fields(const Stmt* s) {
  const std::span<const ClassField> fields = s->kind == NodeKind::ClassDecl ? static_cast<const ClassDecl*>(s)->fields
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

inline void TypeChecker::check_function(std::span<const Param> params, std::string_view ret_type,
                                        std::span<const Stmt* const> body, const std::string& class_ctx, int line) {
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

} // namespace

} // namespace vx::compiler
