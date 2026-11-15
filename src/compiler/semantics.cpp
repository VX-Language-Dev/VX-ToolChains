// semantics.cpp — 独立语义分析：符号表 + 类型环境 + 检查器。
// 两遍扫描：第一遍收集顶层声明（函数/extern/类/结构/顶层变量，允许前向引用），
// 第二遍逐函数检查语句与表达式。诊断聚合输出（不首错即停）。
// 检查范围与 codegen 解耦进度见 semantics.hpp 注释。
// TypeChecker 实现拆分于 semantics_*.hpp；本文件为主入口
// （Scope/TypeEnv/SymbolTable 的公开实现 + check_module 入口）。
#include "vx/compiler/semantics.hpp"

#include "semantics_builtin.hpp"
#include "semantics_checker.hpp"
#include "semantics_checker_expr.hpp"
#include "semantics_checker_pass.hpp"
#include "semantics_checker_stmt.hpp"
#include "semantics_types.hpp"

#include <cstddef>
#include <ranges>
#include <string>
#include <utility>

namespace vx::compiler {

using ir::TypeTag;

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
  // 函数名与类名/枚举名共用命名空间（new X 与 f(X) 不允许歧义）
  if (classes_.contains(sig.name) || enums_.contains(sig.name)) {
    return false;
  }
  std::string key = sig.name;
  return funcs_.emplace(std::move(key), std::move(sig)).second;
}

bool SymbolTable::add_class(ClassInfo info) {
  if (funcs_.contains(info.name) || enums_.contains(info.name)) {
    return false;
  }
  std::string key = info.name;
  return classes_.emplace(std::move(key), std::move(info)).second;
}

bool SymbolTable::add_enum(EnumInfo info) {
  if (funcs_.contains(info.name) || classes_.contains(info.name)) {
    return false;
  }
  std::string key = info.name;
  return enums_.emplace(std::move(key), std::move(info)).second;
}

const FuncSig* SymbolTable::find_func(std::string_view name) const {
  auto it = funcs_.find(std::string(name));
  return it == funcs_.end() ? nullptr : &it->second;
}

const ClassInfo* SymbolTable::find_class(std::string_view name) const {
  auto it = classes_.find(std::string(name));
  return it == classes_.end() ? nullptr : &it->second;
}

const EnumInfo* SymbolTable::find_enum(std::string_view name) const {
  auto it = enums_.find(std::string(name));
  return it == enums_.end() ? nullptr : &it->second;
}

bool SymbolTable::is_type(std::string_view name) const {
  return classes_.contains(std::string(name)) || enums_.contains(std::string(name));
}

bool check_module(const Program& prog, std::string_view filename, DiagBag& bag, const ImportTable* imports,
                  Analysis* out) {
  return TypeChecker(filename, bag, imports, out).run(prog);
}

} // namespace vx::compiler
