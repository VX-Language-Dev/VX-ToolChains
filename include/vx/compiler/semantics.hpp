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
// 检查范围（本步）：未定义符号、重复定义、未知类型标注、不可变变量赋值、
// break/continue/return 位置、调用目标存在性与参数个数、new 的类存在性、
// 字段/方法/类常量访问。完整类型系统与控制流分析在后续步骤落地。
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
};

// 单层作用域：变量名 → VarInfo。
class Scope {
public:
  // 声明变量；同名重复声明返回 false（不覆盖）。
  bool declare(std::string_view name, VarInfo info);
  // 本层查找；未命中返回 nullptr。
  const VarInfo* lookup(std::string_view name) const;

private:
  std::unordered_map<std::string, VarInfo> vars_;
};

// 作用域栈：类型环境。查找由内向外；声明只写当前层。
class TypeEnv {
public:
  void push_scope();
  void pop_scope();
  std::size_t depth() const { return scopes_.size(); }

  // 当前层声明；false = 本层已有同名变量。
  bool declare(std::string_view name, VarInfo info);
  // 由内向外查找变量。
  const VarInfo* lookup(std::string_view name) const;

private:
  std::vector<Scope> scopes_;
};

// 函数签名（方法以 "Class.method" 具名入函数表，另在 ClassInfo.methods 索引）。
struct FuncSig {
  std::string name;
  std::vector<Param> params;
  std::string ret_type; // "" 表示无返回类型标注
  bool is_extern = false;
  bool is_method = false;
};

// 类/结构布局：实例字段（不含类常量）、类常量（名字 → 初始化表达式）、方法表。
struct ClassInfo {
  std::string name;
  std::vector<ClassField> fields;
  std::unordered_map<std::string, const Expr*> consts;
  std::unordered_map<std::string, FuncSig> methods;
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

// 语义分析入口：遍历 Program，构建符号表与类型环境并检查。
// 诊断推入 bag；存在 Error 级诊断时返回 false。
// imports 为已解析导入表（可空；空则跳过导入前缀符号检查）。
bool check_module(const Program& prog, std::string_view filename, DiagBag& bag, const ImportTable* imports = nullptr);

} // namespace vx::compiler
