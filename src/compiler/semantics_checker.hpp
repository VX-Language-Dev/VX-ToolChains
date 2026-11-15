// semantics_checker.hpp — TypeChecker 类声明（内部实现头，仅供 semantics.cpp 使用）
#pragma once

#include "semantics_builtin.hpp"
#include "semantics_types.hpp"
#include "vx/compiler/semantics.hpp"

#include <deque>
#include <span>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

namespace vx::compiler {

namespace {

using ir::TypeTag;

class TypeChecker {
public:
  TypeChecker(std::string_view filename, DiagBag& bag, const ImportTable* imports, Analysis* analysis)
      : filename_(filename), bag_(bag), imports_(imports), analysis_(analysis) {}

  bool run(const Program& prog) {
    env_.push_scope(); // 模块级作用域（顶层变量）
    collect_decls(prog);
    merge_modules();
    check_bodies(prog);
    return ok_;
  }

private:
  // ---- 诊断 ----
  void err(std::string_view code, std::string msg, int line, std::string note = "");

  // note/help 组合：rustc 风格的补充说明（help 为修复建议）
  static std::string help_of(std::string_view help) { return "help: " + std::string(help); }

  // 重复定义的 note：指向同作用域内首次定义的位置
  std::string first_def_note(std::string_view name) const;

  // 函数/类重复定义的 note：指向符号表中首次声明的位置
  std::string first_sym_note(std::string_view name) const;

  // 未定义变量的诊断：区分 E0010（从未声明）与 E0012（曾在内层作用域声明）
  void err_undefined_var(std::string_view name, int line);

  // ---- 类型名解析 ----
  // 返回 false = 未知类型（调用方报 E0003）。
  // ---- 类型名解析 ----
  // 返回 false = 未知类型（调用方报 E0003）。
  bool type_name_tag(std::string_view name, TypeTag& out, std::string& struct_name) const;

  // 参数/返回类型标注解析；未知标注报 E0003。
  TypeTag resolve_ann(std::string_view ann, std::string& struct_name, int line);

  // ---- 第一遍：收集顶层声明 ----
  void collect_decls(const Program& prog);

  void collect_class(const Stmt* s);

  // import 模块声明并入符号表：仅补齐本单元未声明的类/结构、枚举与函数。
  // 模块函数体不参与本次编译，符号由 vlnk 合并各 .vxobj 时解析。
  // import 模块声明并入符号表：仅补齐本单元未声明的类/结构、枚举与函数。
  // 模块函数体不参与本次编译，符号由 vlnk 合并各 .vxobj 时解析。
  void merge_modules();

  // 模块声明摘要里的字符串需在本检查器生命周期内保持稳定
  std::string_view intern(const std::string& s);

  void collect_enum(const Stmt* s);

  // ---- 第二遍：检查函数体 ----
  void check_bodies(const Program& prog);

  // 字段声明类型标注校验：未知类型报 E0003（每个字段只报一次）
  void validate_fields(const Stmt* s);

  void check_function(std::span<const Param> params, std::string_view ret_type, std::span<const Stmt* const> body,
                      const std::string& class_ctx, int line);

  // ---- 语句检查 ----
  void check_stmt(const Stmt* s);

  void check_block(std::span<const Stmt* const> stmts);

  // 赋值目标的类型（用于「无隐式转换」校验）；无法确定时返回通配。
  VarInfo check_assign_target(const Expr* t);

  // 类型不匹配诊断（无隐式转换：不做 int↔float、string↔int 等任何转换）
  void expect_type(const VarInfo& want, const VarInfo& got, std::string_view what, int line);

  // 赋值检查：= 直接校验；复合赋值按 target op value 推导结果类型再校验。
  void check_assign_types(const Assign* a, const VarInfo& target, const VarInfo& value);

  // ---- 表达式检查 + 类型推断 ----
  // 推断规则与 codegen 的发射选择保持一致（本阶段 codegen 仍自行推断，
  // 此处结果供 TypeEnv 与后续步骤使用）。
  // ---- 表达式检查 + 类型推断 ----
  // 推断规则与 codegen 的发射选择保持一致（本阶段 codegen 仍自行推断，
  // 此处结果供 TypeEnv 与后续步骤使用）。
  VarInfo check_expr(const Expr* e);

  static bool is_arith(std::string_view op) { return op == "+" || op == "-" || op == "*" || op == "/" || op == "%"; }
  static bool is_compare(std::string_view op) {
    return op == "==" || op == "!=" || op == "<" || op == ">" || op == "<=" || op == ">=";
  }

  // 运算符类型规则（无隐式转换）：两侧类型须一致；算术仅限 int/float，
  // 字符串拼接两侧须为 string，比较两侧类型须一致。任一侧类型未定则跳过。
  // 运算符类型规则（无隐式转换）：两侧类型须一致；算术仅限 int/float，
  // 字符串拼接两侧须为 string，比较两侧类型须一致。任一侧类型未定则跳过。
  void check_binary_types(std::string_view op, const VarInfo& l, const VarInfo& r, int line);

  static VarInfo infer_binary(std::string_view op, const VarInfo& l, const VarInfo& r);

  // 在导入模块摘要中定位类（用于构造实参数校验）
  const ModuleClass* find_module_class(const std::string& prefix, std::string_view name) const;

  VarInfo check_property(const Property* p);

  // 类成员访问：类常量按初始化表达式推断，实例字段按声明类型。
  VarInfo class_member(const ClassInfo& cls, std::string_view member, int line);

  // 声明参数中显式 self 占首槽（codegen 约定），不计入调用实参
  static std::size_t self_offset(const FuncSig& sig) {
    return (!sig.params.empty() && sig.params[0].name == "self") ? 1 : 0;
  }

  // 参数标注 → 类型（未知标注/空标注视为通配，不报错）
  VarInfo param_info(const Param& p);

  // 实参与候选签名的参数类型是否全部兼容
  bool args_compatible(const FuncSig& sig, const std::vector<VarInfo>& args);

  // 已确定签名后的实参类型校验（逐参数报错，带参数序号）
  void check_arg_types(const std::string& callee, const FuncSig& sig, const std::vector<VarInfo>& args, int line);

  // 方法重载解析：先按实参个数过滤，再按参数类型过滤，
  // 零匹配时报 E0006/E0007，多匹配时报 E0008（歧义）。
  // 方法重载解析：先按实参个数过滤，再按参数类型过滤，
  // 零匹配时报 E0006/E0007，多匹配时报 E0008（歧义）。
  const FuncSig* resolve_method(const Call* call, const ClassInfo& cls, std::string_view mname,
                                const std::vector<VarInfo>& args, int line);

  VarInfo check_call(const Call* c);

  // 函数/方法返回类型 → 推断结果。外部签名（.vxh）类型可能超出本阶段
  // 已知集合，未知时不报错（extern 声明本身的标注检查在后续步骤覆盖）。
  // 函数/方法返回类型 → 推断结果。外部签名（.vxh）类型可能超出本阶段
  // 已知集合，未知时不报错（extern 声明本身的标注检查在后续步骤覆盖）。
  VarInfo sig_ret_info(const FuncSig& sig);

  void check_arity(const std::string& callee, std::size_t got, std::size_t expect, int line);

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
  // 导入模块的访问前缀（import std.error → "error"）与声明字符串池
  std::unordered_set<std::string> module_prefixes_;
  std::deque<std::string> intern_pool_;
  bool ok_ = true;
};

} // namespace

} // namespace vx::compiler
