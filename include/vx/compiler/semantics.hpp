// semantics.hpp — 独立语义分析层：符号表 + 类型环境 + 检查器。
// 目标（fix6 阶段一第 1 步）：把符号解析与类型环境从 codegen 中解耦，
// 形成独立的语义分析阶段；codegen 后续只消费已验证的 AST。
//
// 结构：
//   - Scope        单层作用域：变量名 → VarInfo（TypeTag + 可变性）
//   - TypeEnv      Scope 栈：由内向外查找；声明只落当前层（内层可遮蔽外层）
//   - SymbolTable  模块级符号表：函数表 + extern 表 + 类/结构布局表（字段/常量/方法）
//   - check_module 两遍扫描：先收集全部顶层声明（允许前向引用），
//                  再逐函数检查语句与表达式。
//
// 检查范围（本步）：未定义符号、重复定义、未知类型标注、
// break/continue/return 位置、调用目标存在性与参数个数、new 的类存在性、
// 字段/方法/类常量访问；以及类型检查：变量初始化与赋值、函数/方法实参、
// 返回值、字段赋值均按「无隐式转换」策略校验（var 与未定类型视为通配）；
// 方法按参数类型做重载解析（零匹配 / 多匹配分别报错）。
// 不可变变量赋值检查与控制流分析在后续步骤落地。
#pragma once

#include "vx/compiler/ast.hpp"
#include "vx/compiler/diag.hpp"
#include "vx/compiler/importer.hpp"
#include "vx/compiler/parser.hpp"
#include "vx/compiler/typeir.hpp"

#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace vx::compiler {

// 类型环境条目：变量类型 + 可变性标记。
struct VarInfo {
  ir::TypeTag type = ir::TYPE_UNKNOWN;
  bool is_mut = false;
  // STRUCT 类型对应的类/结构名（其余类型为空）。
  std::string struct_name;
  // 列表字面量推断出的元素类型（TYPE_ARRAY 时有效，其余为 TYPE_UNKNOWN）。
  // 仅记录标量元素；元素为类/结构时保持 UNKNOWN（下标结果不再假设类型）。
  ir::TypeTag elem = ir::TYPE_UNKNOWN;
  // 声明所在行（1 起始；0 = 未知），用于 E0011 note 指向首次定义。
  int decl_line = 0;
};

// 单层作用域：变量名 → VarInfo。
class Scope {
public:
  // 声明变量；同名重复声明返回 false（不覆盖）。
  bool declare(std::string_view name, VarInfo info);
  // 本层查找；未命中返回 nullptr。
  const VarInfo* lookup(std::string_view name) const;

  // 本层全部绑定（供 pop_scope 时登记作用域泄漏）。
  const std::unordered_map<std::string, VarInfo>& vars() const { return vars_; }

  // 本层类型细化：仅当已有绑定的类型未定（var）时更新为推断类型。
  void refine(std::string_view name, const VarInfo& info);

private:
  std::unordered_map<std::string, VarInfo> vars_;
};

// 作用域栈：类型环境。查找由内向外；声明只写当前层。
class TypeEnv {
public:
  void push_scope();
  void pop_scope();
  std::size_t depth() const { return scopes_.size(); }

  // 已离开作用域的变量登记（名字 → 声明行）：用于把 E0010 区分为
  // E0012 作用域泄漏（变量曾在更内层作用域声明，只是当前不可见）。
  const std::unordered_map<std::string, int>& leaked() const { return leaked_; }
  // 清空登记（每个函数检查开始时调用，避免跨函数残留误报）。
  void clear_leaked() { leaked_.clear(); }

  // 当前层声明；false = 本层已有同名变量。
  bool declare(std::string_view name, VarInfo info);
  // 由内向外查找变量。
  const VarInfo* lookup(std::string_view name) const;

  // 类型细化：把最近一层中「类型尚未确定（var）」的绑定更新为推断类型。
  // 已标注或已推断出类型的绑定不覆盖（标注优先）。
  void refine(std::string_view name, VarInfo info);

private:
  std::vector<Scope> scopes_;
  std::unordered_map<std::string, int> leaked_;
};

// 函数签名（方法以 "Class.method" 具名入函数表，另在 ClassInfo.methods 索引）。
struct FuncSig {
  std::string name;
  std::vector<Param> params;
  std::string ret_type; // "" 表示无返回类型标注
  bool is_extern = false;
  bool is_method = false;
  int decl_line = 0; // 声明行（E0011 note 指向首次定义用）
};

// 类/结构布局：实例字段（不含类常量）、类常量（名字 → 初始化表达式）、
// 方法重载集合（方法名 → 按参数签名的多个实现；同签名的重复声明在注册时拒绝）。
struct ClassInfo {
  std::string name;
  std::vector<ClassField> fields;
  std::unordered_map<std::string, const Expr*> consts;
  std::unordered_map<std::string, std::vector<FuncSig>> methods;
  int decl_line = 0; // 声明行（E0011 note 指向首次定义用）
};

// 模块级符号表：函数表 + 类/结构布局表。
class SymbolTable {
public:
  // 注册；同名冲突（函数/extern/类互相之间，或同类方法）返回 false。
  bool add_func(FuncSig sig);
  bool add_class(ClassInfo info);

  const FuncSig* find_func(std::string_view name) const;
  const ClassInfo* find_class(std::string_view name) const;
  // 类型名是否已知（内置标量由 type_name_tag 处理，这里查类/结构）。
  bool is_type(std::string_view name) const;

private:
  std::unordered_map<std::string, FuncSig> funcs_;
  std::unordered_map<std::string, ClassInfo> classes_;
};

// 语义分析产物：供 codegen 消费的解析结果，避免下游重复推断。
// 当前仅记录方法调用解析出的后端符号名（重载方法带 `$<序号>` 后缀；
// 非重载方法即 "Class.method"，与未做语义分析时的发射结果一致）。
struct Analysis {
  std::unordered_map<const Call*, std::string> method_symbols;
};

// 语义分析入口：遍历 Program，构建符号表与类型环境并检查。
// 诊断推入 bag；存在 Error 级诊断时返回 false。
// imports 为已解析导入表（可空；空则跳过导入前缀符号检查）。
// out 非空时收集可供 codegen 复用的解析结果。
bool check_module(const Program& prog, std::string_view filename, DiagBag& bag, const ImportTable* imports = nullptr,
                  Analysis* out = nullptr);

} // namespace vx::compiler
