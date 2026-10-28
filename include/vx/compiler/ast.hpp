// ast.hpp — VX 抽象语法树：类型化节点，全部在 vx::util::Arena 上分配。
// 节点文本（标识符/字面量原文）为 string_view，指向源码或词法阶段 arena 内存。
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace vx::compiler {

enum class NodeKind : std::uint8_t {
  // 表达式
  IntLit, FloatLit, StrLit, BoolLit, NilLit, Ident, ListLit, DictLit,
  Index, Property, Call, New, Unary, Binary,
  // 语句
  ExprStmt, VarDecl, Assign, FuncDecl, ExternDecl, Import, Export,
  Return, Break, Continue, If, While, For, ClassDecl, StructDecl,
  ImpDecl, CppImpDecl, // @imp / @cppimp 导入注解
};

struct Node {
  NodeKind kind;
  int line; // 1 起始行号
};

struct Expr : Node {
  explicit Expr(NodeKind k, int l) : Node{k, l} {}
};

struct Stmt : Node {
  explicit Stmt(NodeKind k, int l) : Node{k, l} {}
};

// ===== 表达式 =====
struct IntLit : Expr {
  std::string_view text;
  IntLit(std::string_view t, int l) : Expr{NodeKind::IntLit, l}, text(t) {}
};
struct FloatLit : Expr {
  std::string_view text;
  FloatLit(std::string_view t, int l) : Expr{NodeKind::FloatLit, l}, text(t) {}
};
struct StrLit : Expr { // value 为解码后的字符串内容
  std::string_view value;
  StrLit(std::string_view v, int l) : Expr{NodeKind::StrLit, l}, value(v) {}
};
struct BoolLit : Expr {
  bool value;
  BoolLit(bool v, int l) : Expr{NodeKind::BoolLit, l}, value(v) {}
};
struct NilLit : Expr {
  explicit NilLit(int l) : Expr{NodeKind::NilLit, l} {}
};
struct Ident : Expr {
  std::string_view name;
  Ident(std::string_view n, int l) : Expr{NodeKind::Ident, l}, name(n) {}
};
struct ListLit : Expr {
  std::span<const Expr *const> items;
  ListLit(std::span<const Expr *const> i, int l)
      : Expr{NodeKind::ListLit, l}, items(i) {}
};
struct DictEntry {
  const Expr *key;
  const Expr *value;
};
struct DictLit : Expr {
  std::span<const DictEntry> entries;
  DictLit(std::span<const DictEntry> e, int l)
      : Expr{NodeKind::DictLit, l}, entries(e) {}
};
struct Index : Expr { // obj[index]
  const Expr *obj;
  const Expr *index;
  Index(const Expr *o, const Expr *i, int l)
      : Expr{NodeKind::Index, l}, obj(o), index(i) {}
};
struct Property : Expr { // obj.name
  const Expr *obj;
  std::string_view name;
  Property(const Expr *o, std::string_view n, int l)
      : Expr{NodeKind::Property, l}, obj(o), name(n) {}
};
struct Call : Expr { // callee(args)；方法调用即 callee 为 Property
  const Expr *callee;
  std::span<const Expr *const> args;
  Call(const Expr *c, std::span<const Expr *const> a, int l)
      : Expr{NodeKind::Call, l}, callee(c), args(a) {}
};
struct New : Expr { // new ClassName(args?)
  std::string_view class_name;
  std::span<const Expr *const> args;
  New(std::string_view c, std::span<const Expr *const> a, int l)
      : Expr{NodeKind::New, l}, class_name(c), args(a) {}
};
struct Unary : Expr { // op ∈ {"-", "!"}
  std::string_view op;
  const Expr *operand;
  Unary(std::string_view o, const Expr *e, int l)
      : Expr{NodeKind::Unary, l}, op(o), operand(e) {}
};
struct Binary : Expr { // op 为运算符原文（and/or 用关键字文本）
  std::string_view op;
  const Expr *left;
  const Expr *right;
  Binary(std::string_view o, const Expr *a, const Expr *b, int l)
      : Expr{NodeKind::Binary, l}, op(o), left(a), right(b) {}
};

// ===== 语句 =====
struct ExprStmt : Stmt {
  const Expr *expr;
  ExprStmt(const Expr *e, int l) : Stmt{NodeKind::ExprStmt, l}, expr(e) {}
};
struct VarDecl : Stmt { // var/mut name [: type] = expr
  std::string_view name;
  std::string_view type_ann; // 空表示未标注
  const Expr *init;          // 可空
  bool is_mut;
  VarDecl(std::string_view n, std::string_view t, const Expr *i, bool m, int l)
      : Stmt{NodeKind::VarDecl, l}, name(n), type_ann(t), init(i), is_mut(m) {}
};
struct Assign : Stmt { // target op expr；op ∈ {"=","+=","-=","*=","/="}
  const Expr *target;
  std::string_view op;
  const Expr *value;
  Assign(const Expr *t, std::string_view o, const Expr *v, int l)
      : Stmt{NodeKind::Assign, l}, target(t), op(o), value(v) {}
};
struct Param {
  std::string_view name;
  std::string_view type; // 空表示 var
};
struct FuncDecl : Stmt {
  std::string_view name;
  std::span<const Param> params;
  std::string_view ret_type; // 空表示 void
  std::span<const Stmt *const> body;
  FuncDecl(std::string_view n, std::span<const Param> p, std::string_view r,
           std::span<const Stmt *const> b, int l)
      : Stmt{NodeKind::FuncDecl, l}, name(n), params(p), ret_type(r), body(b) {}
};
struct ExternDecl : Stmt { // extern [func] name(params) [-> type]
  std::string_view name;
  std::span<const Param> params;
  std::string_view ret_type;
  ExternDecl(std::string_view n, std::span<const Param> p, std::string_view r,
             int l)
      : Stmt{NodeKind::ExternDecl, l}, name(n), params(p), ret_type(r) {}
};
struct Import : Stmt { // import a.b.c [as alias]
  std::string_view path;
  Import(std::string_view p, int l) : Stmt{NodeKind::Import, l}, path(p) {}
};
struct Export : Stmt { // export name
  std::string_view name;
  Export(std::string_view n, int l) : Stmt{NodeKind::Export, l}, name(n) {}
};
struct ImpDecl : Stmt { // @imp(库引用) [as alias]
  std::string_view lib;   // 库引用："math" | "collections/vec" | "/sub/lib" | "./mylib"
  std::string_view alias; // 空表示取库名末段作为访问前缀
  ImpDecl(std::string_view l_, std::string_view a, int ln)
      : Stmt{NodeKind::ImpDecl, ln}, lib(l_), alias(a) {}
};
struct CppImpDecl : Stmt { // @cppimp(头文件路径) [as alias]
  std::string_view header; // C++ 头文件路径
  std::string_view alias;
  CppImpDecl(std::string_view h, std::string_view a, int ln)
      : Stmt{NodeKind::CppImpDecl, ln}, header(h), alias(a) {}
};
struct Return : Stmt {
  const Expr *value; // 可空
  Return(const Expr *v, int l) : Stmt{NodeKind::Return, l}, value(v) {}
};
struct Break : Stmt {
  explicit Break(int l) : Stmt{NodeKind::Break, l} {}
};
struct Continue : Stmt {
  explicit Continue(int l) : Stmt{NodeKind::Continue, l} {}
};
struct ElifArm {
  const Expr *cond;
  std::span<const Stmt *const> body;
};
struct If : Stmt {
  const Expr *cond;
  std::span<const Stmt *const> then_body;
  std::span<const ElifArm> elifs;
  std::span<const Stmt *const> else_body; // 空表示无 else
  If(const Expr *c, std::span<const Stmt *const> t,
     std::span<const ElifArm> e, std::span<const Stmt *const> s, int l)
      : Stmt{NodeKind::If, l}, cond(c), then_body(t), elifs(e), else_body(s) {}
};
struct While : Stmt {
  const Expr *cond;
  std::span<const Stmt *const> body;
  While(const Expr *c, std::span<const Stmt *const> b, int l)
      : Stmt{NodeKind::While, l}, cond(c), body(b) {}
};
struct For : Stmt { // for name in expr: body
  std::string_view var;
  const Expr *iterable;
  std::span<const Stmt *const> body;
  For(std::string_view v, const Expr *i, std::span<const Stmt *const> b, int l)
      : Stmt{NodeKind::For, l}, var(v), iterable(i), body(b) {}
};
struct ClassField {
  std::string_view name;
  std::string_view type;
  // 非空 = 类常量字段（NAME = expr）：类级共享，不占实例布局槽位
  const Expr *init = nullptr;
};
struct ClassDecl : Stmt { // class Name: 字段与方法
  std::string_view name;
  std::span<const ClassField> fields;
  std::span<const FuncDecl *const> methods;
  ClassDecl(std::string_view n, std::span<const ClassField> f,
            std::span<const FuncDecl *const> m, int l)
      : Stmt{NodeKind::ClassDecl, l}, name(n), fields(f), methods(m) {}
};
struct StructDecl : Stmt { // struct Name: 字段
  std::string_view name;
  std::span<const ClassField> fields;
  StructDecl(std::string_view n, std::span<const ClassField> f, int l)
      : Stmt{NodeKind::StructDecl, l}, name(n), fields(f) {}
};

// 节点种类名（诊断/调试用）
std::string_view node_kind_name(NodeKind kind);

} // namespace vx::compiler
