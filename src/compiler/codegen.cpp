// codegen.cpp — 类型化 AST → TypeIR 虚拟指令序列。
// 发射语义逐条对齐自举 compiler/codegen.vx；自举缺失的结构
// （方法调用、列表/字典、下标/属性赋值、复合赋值）按 codegen.hpp 注释的约定补齐。
#include "vx/compiler/codegen.hpp"

#include "vx/compiler/parser.hpp"

#include <algorithm>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace vx::compiler {
namespace {

using ir::Inst;
using ir::ModuleIR;

class CodeGen {
public:
  CodeGen(std::string_view filename, DiagBag &bag, const ImportTable *imports)
      : filename_(filename), bag_(bag), imports_(imports) {}

  std::optional<ModuleIR> run(const Program &prog) {
    ModuleIR mod;

    // 第一遍：注册 class/struct 字段（字段表供后续按索引访问，当前用名字）
    for (const Stmt *s : prog.stmts) {
      if (s->kind == NodeKind::ClassDecl) {
        register_struct(static_cast<const ClassDecl *>(s));
        ir::StructLayout sl;
        const auto *c = static_cast<const ClassDecl *>(s);
        sl.name = std::string(c->name);
        for (const ClassField &f : c->fields) sl.fields.push_back({std::string(f.name), std::string(f.type)});
        mod.structs.push_back(std::move(sl));
      } else if (s->kind == NodeKind::StructDecl) {
        register_struct(static_cast<const StructDecl *>(s));
        ir::StructLayout sl;
        const auto *st = static_cast<const StructDecl *>(s);
        sl.name = std::string(st->name);
        for (const ClassField &f : st->fields) sl.fields.push_back({std::string(f.name), std::string(f.type)});
        mod.structs.push_back(std::move(sl));
      }
    }

    // 第二遍：生成函数 / 链接表 / 方法
    for (const Stmt *s : prog.stmts) {
      if (failed_) {
        break;
      }
      switch (s->kind) {
        case NodeKind::FuncDecl:
          gen_func(static_cast<const FuncDecl *>(s)->name,
                   static_cast<const FuncDecl *>(s)->params,
                   static_cast<const FuncDecl *>(s)->ret_type,
                   static_cast<const FuncDecl *>(s)->body, mod);
          break;
        case NodeKind::ExternDecl:
          gen_extern(static_cast<const ExternDecl *>(s), mod);
          break;
        case NodeKind::ImpDecl:
        case NodeKind::CppImpDecl:
          register_import(s, mod);
          break;
        case NodeKind::ClassDecl: {
          const auto *c = static_cast<const ClassDecl *>(s);
          for (const FuncDecl *m : c->methods) {
            gen_method(*c, *m, mod);
          }
          break;
        }
        case NodeKind::VarDecl:
          // 对齐自举：顶层变量不生成指令，仅记录
          mod.global_vars.emplace_back(
              static_cast<const VarDecl *>(s)->name);
          break;
        default:
          break; // Import/Export 等不产生代码
      }
    }

    if (failed_) {
      return std::nullopt;
    }
    return mod;
  }

private:
  // ---- 错误传播（fail-fast，首次错误优先）----
  void fail(std::string msg, int line) {
    if (!failed_) {
      bag_.add(make_diag(diag_code::sem_codegen, Severity::Error,
                         std::move(msg), std::string(filename_), line, 0));
      failed_ = true;
    }
  }

  // ---- 局部变量槽位 ----
  std::uint32_t alloc_local(std::string_view name) {
    std::string key(name);
    auto it = local_ids_.find(key);
    if (it != local_ids_.end()) {
      return it->second;
    }
    const std::uint32_t id = local_counter_++;
    local_ids_.emplace(std::move(key), id);
    return id;
  }

  std::string new_label() {
    return "L" + std::to_string(++label_counter_);
  }

  // ---- 指令发射 ----
  void emit(std::uint8_t op, std::string args = "") {
    insts_.push_back(Inst{op, std::move(args)});
  }

  void emit_label(std::string name) { emit(ir::OP_NOOP, std::move(name)); }

  // ---- 表达式 ----

  // 逻辑运算短路求值。结果规范化为 0/1（与原 BOOL_AND/OR 非短路语义等价），
  // 分支结果经临时槽位合流——SSA 发射器按线性栈模拟，汇合点两路径栈内容
  // 不同会产生悬垂值，用 alloca（LOAD/STORE_VAR）规避 phi 需求：
  //   a and b → a 假：0（b 不求值）；a 真：b ? 1 : 0
  //   a or  b → a 真：1（b 不求值）；a 假：b ? 1 : 0
  //
  // 序列（and）：每条路径净栈变化 0，汇合到 l_end 时统一 LOAD 结果：
  //   left → JIF_TRUE l_a（假→短路 0）
  //   [i0, STORE, JUMP l_end] | l_a: right → JIF_TRUE l_b（假→0）
  //   [i0, STORE, JUMP l_end] | l_b: [i1, STORE] → l_end: LOAD tmp
  void gen_short_circuit(const Binary *b) {
    const bool is_and = b->op == "and" || b->op == "&&";
    const std::string tmp = std::to_string(
        alloc_local("__sc_" + std::to_string(label_counter_)));
    const std::string l_check = new_label(); // 求值 right 的入口
    const std::string l_set = new_label();   // right 为真/假的直接结果
    const std::string l_end = new_label();

    gen_expr(b->left);
    if (is_and) {
      emit(ir::OP_JUMP_IF_TRUE, l_check); // left 假 → 短路 0（right 不求值）
      emit(ir::OP_CONST_INT, "i0");
      emit(ir::OP_STORE_VAR, tmp);
      emit(ir::OP_JUMP, l_end);
      emit_label(l_check);
      gen_expr(b->right);
      emit(ir::OP_JUMP_IF_TRUE, l_set); // right 真 → 结果 1
      emit(ir::OP_CONST_INT, "i0");
      emit(ir::OP_STORE_VAR, tmp);
      emit(ir::OP_JUMP, l_end);
      emit_label(l_set);
      emit(ir::OP_CONST_INT, "i1");
      emit(ir::OP_STORE_VAR, tmp);
    } else {
      emit(ir::OP_JUMP_IF_FALSE, l_check); // left 真 → 短路 1（right 不求值）
      emit(ir::OP_CONST_INT, "i1");
      emit(ir::OP_STORE_VAR, tmp);
      emit(ir::OP_JUMP, l_end);
      emit_label(l_check);
      gen_expr(b->right);
      emit(ir::OP_JUMP_IF_FALSE, l_set); // right 假 → 结果 0
      emit(ir::OP_CONST_INT, "i1");
      emit(ir::OP_STORE_VAR, tmp);
      emit(ir::OP_JUMP, l_end);
      emit_label(l_set);
      emit(ir::OP_CONST_INT, "i0");
      emit(ir::OP_STORE_VAR, tmp);
    }
    emit_label(l_end);
    emit(ir::OP_LOAD_VAR, tmp);
  }

  void gen_expr(const Expr *e) {
    if (e == nullptr || failed_) {
      return;
    }
    switch (e->kind) {
      case NodeKind::IntLit:
        emit(ir::OP_CONST_INT,
             "i" + std::string(static_cast<const IntLit *>(e)->text));
        break;
      case NodeKind::FloatLit:
        emit(ir::OP_CONST_FLOAT,
             "f" + std::string(static_cast<const FloatLit *>(e)->text));
        break;
      case NodeKind::StrLit:
        emit(ir::OP_CONST_STRING,
             "s" + std::string(static_cast<const StrLit *>(e)->value));
        break;
      case NodeKind::BoolLit:
        emit(ir::OP_CONST_BOOL, static_cast<const BoolLit *>(e)->value
                                    ? "btrue"
                                    : "bfalse");
        break;
      case NodeKind::NilLit:
        emit(ir::OP_CONST_NIL);
        break;
      case NodeKind::Ident:
        emit(ir::OP_LOAD_VAR,
             std::to_string(alloc_local(static_cast<const Ident *>(e)->name)));
        break;
      case NodeKind::ListLit: {
        const auto *l = static_cast<const ListLit *>(e);
        for (const Expr *item : l->items) {
          gen_expr(item);
        }
        emit(ir::OP_ARRAY_NEW, std::to_string(l->items.size()));
        break;
      }
      case NodeKind::DictLit: {
        const auto *d = static_cast<const DictLit *>(e);
        for (const DictEntry &entry : d->entries) {
          gen_expr(entry.key);
          gen_expr(entry.value);
        }
        emit(ir::OP_MAP_NEW, std::to_string(d->entries.size()));
        break;
      }
      case NodeKind::Index: {
        const auto *ix = static_cast<const Index *>(e);
        gen_expr(ix->obj);
        gen_expr(ix->index);
        // 元素类型推断（用于后端按槽位发射）
        std::string elem = "i32";
        if (ix->obj->kind == NodeKind::Ident) {
          auto it = var_kind_.find(std::string(static_cast<const Ident *>(ix->obj)->name));
          if (it != var_kind_.end()) {
            switch (it->second) {
              case kVarListFloat: elem = "f64"; break;
              case kVarListStr: elem = "ptr"; break;
              case kVarMap: elem = "map"; break;
              default: break;
            }
          }
        }
        emit(ir::OP_INDEX_GET, elem);
        break;
      }
      case NodeKind::Property: {
        const auto *p = static_cast<const Property *>(e);
        gen_expr(p->obj);
        emit(ir::OP_FIELD_LOAD, std::string(p->name));
        break;
      }
      case NodeKind::New: {
        const std::string cn = std::string(static_cast<const New *>(e)->class_name);
        std::size_t nfields = 0;
        auto it = struct_fields_.find(cn);
        if (it != struct_fields_.end()) nfields = it->second.size();
        const std::size_t bytes = (nfields == 0 ? 1 : nfields) * 8;
        emit(ir::OP_NEWZ, cn + ";" + std::to_string(bytes));
        break;
      }
      case NodeKind::Unary: {
        const auto *u = static_cast<const Unary *>(e);
        gen_expr(u->operand);
        if (u->op == "-") {
          // 浮点字面量取负 → F64 族（与二元运算规则一致）
          emit(is_float_expr(u->operand) ? ir::OP_F64_NEG : ir::OP_I32_NEG);
        } else if (u->op == "!") {
          emit(ir::OP_BOOL_NOT);
        } else {
          fail("unsupported unary operator: " + std::string(u->op), e->line);
        }
        break;
      }
      case NodeKind::Binary: {
        const auto *b = static_cast<const Binary *>(e);
        // 逻辑运算短路求值：右侧仅在需要时求值（见 gen_short_circuit）
        if (b->op == "and" || b->op == "&&" || b->op == "or" ||
            b->op == "||") {
          gen_short_circuit(b);
          break;
        }
        gen_expr(b->left);
        gen_expr(b->right);
        emit_binary_op(b->op, b->left, b->right, e->line);
        break;
      }
      case NodeKind::Call:
        gen_call(static_cast<const Call *>(e));
        break;
      default:
        fail("unsupported expression: " +
                 std::string(node_kind_name(e->kind)),
             e->line);
    }
  }

  // 操作数类型判断辅助
  static bool is_float_lit(const Expr *e) {
    return e && e->kind == NodeKind::FloatLit;
  }
  // 语法浮点性推断：FloatLit、浮点算术（+ - * /）、浮点取负、
  // 以及「初始化/赋值为浮点」的变量（var_kind_ 传播）结果为浮点；
  // 比较运算结果为整型布尔，不计入浮点。
  bool is_float_expr(const Expr *e) {
    if (!e) return false;
    if (e->kind == NodeKind::FloatLit) return true;
    if (e->kind == NodeKind::Ident) {
      auto it = var_kind_.find(std::string(static_cast<const Ident *>(e)->name));
      return it != var_kind_.end() && it->second == kVarFloat;
    }
    if (e->kind == NodeKind::Unary) {
      const auto *u = static_cast<const Unary *>(e);
      return u->op == "-" && is_float_expr(u->operand);
    }
    if (e->kind == NodeKind::Binary) {
      const auto *b = static_cast<const Binary *>(e);
      if (b->op != "+" && b->op != "-" && b->op != "*" && b->op != "/")
        return false;
      return is_float_expr(b->left) && is_float_expr(b->right);
    }
    return false;
  }
  // 字符串表达式：StrLit、字符串拼接结果、字符串变量（var_kind_ 传播）
  bool is_str_expr(const Expr *e) {
    if (!e) return false;
    if (e->kind == NodeKind::StrLit) return true;
    if (e->kind == NodeKind::Ident) {
      auto it = var_kind_.find(std::string(static_cast<const Ident *>(e)->name));
      return it != var_kind_.end() && it->second == kVarStr;
    }
    if (e->kind == NodeKind::Binary) {
      const auto *b = static_cast<const Binary *>(e);
      return b->op == "+" && (is_str_expr(b->left) || is_str_expr(b->right));
    }
    return false;
  }
  // 记录变量声明/赋值推断出的类型（浮点/字符串），供后续表达式选择运算族
  void note_var_kind(std::string_view name, const Expr *value) {
    if (value == nullptr) return;
    if (value->kind == NodeKind::ListLit) {
      const auto *l = static_cast<const ListLit *>(value);
      if (!l->items.empty() && is_float_expr(l->items[0])) {
        var_kind_[std::string(name)] = kVarListFloat;
      } else if (!l->items.empty() && is_str_expr(l->items[0])) {
        var_kind_[std::string(name)] = kVarListStr;
      } else {
        var_kind_[std::string(name)] = kVarListInt;
      }
      return;
    }
    if (value->kind == NodeKind::DictLit) {
      var_kind_[std::string(name)] = kVarMap;
      return;
    }
    if (is_float_expr(value)) {
      var_kind_[std::string(name)] = kVarFloat;
    } else if (is_str_expr(value)) {
      var_kind_[std::string(name)] = kVarStr;
    } else {
      var_kind_.erase(std::string(name));
    }
  }
  static bool is_str_lit(const Expr *e) {
    return e && e->kind == NodeKind::StrLit;
  }

  void emit_binary_op(std::string_view op, const Expr *left, const Expr *right,
                      int line) {
    // 字符串拼接："a" + "b" → OP_STR_CONCAT（含字符串变量参与）
    if (op == "+" && (is_str_expr(left) || is_str_expr(right))) {
      emit(ir::OP_STR_CONCAT);
      return;
    }
    // 字符串相等比较 → OP_STR_EQ（内容比较；!= 由 BOOL_NOT 取反）
    if ((op == "==" || op == "!=") &&
        (is_str_expr(left) || is_str_expr(right))) {
      emit(ir::OP_STR_EQ);
      if (op == "!=") {
        emit(ir::OP_BOOL_NOT);
      }
      return;
    }
    // 整数幂：a ^ b → 运行时 vx_pow_i32（两操作数已在栈上）
    if (op == "^") {
      emit(ir::OP_CALL, "f;vx_pow_i32");
      return;
    }
    // 浮点运算：两边皆浮点表达式 → F64 族；否则退化为 I32 族
    bool use_f64 = is_float_expr(left) && is_float_expr(right);
    if (use_f64) {
      if (op == "+") {
        emit(ir::OP_F64_ADD);
      } else if (op == "-") {
        emit(ir::OP_F64_SUB);
      } else if (op == "*") {
        emit(ir::OP_F64_MUL);
      } else if (op == "/") {
        emit(ir::OP_F64_DIV);
      } else if (op == "==") {
        emit(ir::OP_F64_EQ);
      } else if (op == "!=") {
        emit(ir::OP_F64_NE);
      } else if (op == "<") {
        emit(ir::OP_F64_LT);
      } else if (op == ">") {
        emit(ir::OP_F64_GT);
      } else if (op == "<=") {
        emit(ir::OP_F64_LE);
      } else if (op == ">=") {
        emit(ir::OP_F64_GE);
      } else {
        fail("unsupported binary operator on float: " + std::string(op), line);
      }
      return;
    }
    // 默认 I32 族
    if (op == "+") {
      emit(ir::OP_I32_ADD);
    } else if (op == "-") {
      emit(ir::OP_I32_SUB);
    } else if (op == "*") {
      emit(ir::OP_I32_MUL);
    } else if (op == "/") {
      emit(ir::OP_I32_DIV);
    } else if (op == "%") {
      emit(ir::OP_I32_MOD);
    } else if (op == "==") {
      emit(ir::OP_I32_EQ);
    } else if (op == "!=") {
      emit(ir::OP_I32_NEQ);
    } else if (op == "<") {
      emit(ir::OP_I32_LT);
    } else if (op == ">") {
      emit(ir::OP_I32_GT);
    } else if (op == "<=") {
      emit(ir::OP_I32_LE);
    } else if (op == ">=") {
      emit(ir::OP_I32_GE);
    } else if (op == "and" || op == "&&") {
      emit(ir::OP_BOOL_AND);
    } else if (op == "or" || op == "||") {
      emit(ir::OP_BOOL_OR);
    } else {
      fail("unsupported binary operator: " + std::string(op), line);
    }
  }

  // 调用约定：
  //   f(args)     → args 入栈后 OP_CALL "f;<name>"
  //   lib.f(args) → 导入库的命名空间调用：args 入栈（无接收者）后
  //                 OP_CALL "f;<VX ABI 符号名>"
  //   o.m(args)   → 接收者 o 入栈，args 入栈后 OP_CALL "m;<method>"
  void gen_call(const Call *c) {
    if (c->callee->kind == NodeKind::Ident) {
      for (const Expr *a : c->args) {
        gen_expr(a);
      }
      emit(ir::OP_CALL,
           "f;" + std::string(static_cast<const Ident *>(c->callee)->name));
      return;
    }
    if (c->callee->kind == NodeKind::Property) {
      const auto *prop = static_cast<const Property *>(c->callee);
      // 导入库命名空间：obj 为 Ident 且命中导入前缀 → ABI 调用（无接收者）
      if (imports_ != nullptr && prop->obj->kind == NodeKind::Ident) {
        const auto *id = static_cast<const Ident *>(prop->obj);
        if (const ImportedLib *lib = imports_->lib_by_prefix(id->name)) {
          const ImportedSymbol *sym = ImportTable::find_symbol(*lib, prop->name);
          if (sym == nullptr) {
            fail("库 '" + lib->name + "' 未声明符号 '" +
                     std::string(prop->name) + "'",
                 c->line);
            return;
          }
          for (const Expr *a : c->args) {
            gen_expr(a);
          }
          emit(ir::OP_CALL, "f;" + sym->abi_symbol);
          return;
        }
      }
      gen_expr(prop->obj);
      for (const Expr *a : c->args) {
        gen_expr(a);
      }
      emit(ir::OP_CALL, "m;" + std::string(prop->name));
      return;
    }
    fail("unsupported call target: " +
             std::string(node_kind_name(c->callee->kind)),
         c->line);
  }

  // ---- 语句 ----
  void gen_stmt(const Stmt *s) {
    if (s == nullptr || failed_) {
      return;
    }
    switch (s->kind) {
      case NodeKind::VarDecl: {
        const auto *v = static_cast<const VarDecl *>(s);
        if (v->init != nullptr) {
          gen_expr(v->init);
        } else {
          emit(ir::OP_CONST_NIL);
        }
        // 记录推断类型（浮点/字符串），后续运算按对应发射族
        note_var_kind(v->name, v->init);
        emit(ir::OP_STORE_VAR, std::to_string(alloc_local(v->name)));
        break;
      }
      case NodeKind::Assign:
        gen_assign(static_cast<const Assign *>(s));
        break;
      case NodeKind::Return: {
        const auto *r = static_cast<const Return *>(s);
        if (r->value != nullptr) {
          gen_expr(r->value);
          emit(ir::OP_RETURN);
        } else {
          emit(ir::OP_RETURN_VOID);
        }
        has_return_ = true;
        break;
      }
      case NodeKind::Break: {
        if (loop_stack_.empty()) {
          fail("break outside loop", s->line);
          return;
        }
        emit(ir::OP_JUMP, loop_stack_.back().second);
        break;
      }
      case NodeKind::Continue: {
        if (loop_stack_.empty()) {
          fail("continue outside loop", s->line);
          return;
        }
        emit(ir::OP_JUMP, loop_stack_.back().first);
        break;
      }
      case NodeKind::If:
        gen_if(static_cast<const If *>(s));
        break;
      case NodeKind::While:
        gen_while(static_cast<const While *>(s));
        break;
      case NodeKind::For:
        gen_for(static_cast<const For *>(s));
        break;
      case NodeKind::ExprStmt: {
        const auto *es = static_cast<const ExprStmt *>(s);
        gen_expr(es->expr);
        emit(ir::OP_POP);
        break;
      }
      default:
        fail("unsupported statement: " +
                 std::string(node_kind_name(s->kind)),
             s->line);
    }
  }

  // 赋值目标支持 Ident / Property / Index；复合赋值（+= 等）按目标形态展开：
  //   Ident    → LOAD, value, binop, STORE
  //   Property → obj, DUP, FIELD_LOAD, value, binop, FIELD_STORE
  //   Index    → obj/idx 存隐藏临时，LOAD obj, LOAD idx, INDEX_GET,
  //              value, binop, LOAD obj, LOAD idx, INDEX_SET
  void gen_assign(const Assign *a) {
    if (a->op != "=") {
      gen_compound_assign(a);
      return;
    }
    const Expr *t = a->target;
    switch (t->kind) {
      case NodeKind::Ident:
        gen_expr(a->value);
        note_var_kind(static_cast<const Ident *>(t)->name, a->value);
        emit(ir::OP_STORE_VAR,
             std::to_string(alloc_local(static_cast<const Ident *>(t)->name)));
        break;
      case NodeKind::Property: {
        const auto *p = static_cast<const Property *>(t);
        gen_expr(p->obj);
        gen_expr(a->value);
        emit(ir::OP_FIELD_STORE, std::string(p->name));
        break;
      }
      case NodeKind::Index: {
        const auto *ix = static_cast<const Index *>(t);
        gen_expr(ix->obj);
        gen_expr(ix->index);
        gen_expr(a->value);
        if (ix->obj->kind == NodeKind::Ident) {
          auto it = var_kind_.find(std::string(static_cast<const Ident *>(ix->obj)->name));
          if (it != var_kind_.end() && it->second == kVarMap) { emit(ir::OP_INDEX_SET, "map"); break; }
        }
        emit(ir::OP_INDEX_SET);
        break;
      }
      default:
        fail("unsupported assignment target: " +
                 std::string(node_kind_name(t->kind)),
             a->line);
    }
  }

  void gen_compound_assign(const Assign *a) {
    const char bin_op = a->op[0]; // "+=" → '+'，"-=" 等
    const Expr *t = a->target;
    switch (t->kind) {
      case NodeKind::Ident: {
        const auto *id = static_cast<const Ident *>(t);
        emit(ir::OP_LOAD_VAR, std::to_string(alloc_local(id->name)));
        gen_expr(a->value);
        emit_binary_op(std::string_view(&bin_op, 1), id, a->value, a->line);
        emit(ir::OP_STORE_VAR, std::to_string(alloc_local(id->name)));
        break;
      }
      case NodeKind::Property: {
        const auto *p = static_cast<const Property *>(t);
        gen_expr(p->obj);
        emit(ir::OP_DUP);
        emit(ir::OP_FIELD_LOAD, std::string(p->name));
        gen_expr(a->value);
        emit_binary_op(std::string_view(&bin_op, 1), p, a->value, a->line);
        emit(ir::OP_FIELD_STORE, std::string(p->name));
        break;
      }
      case NodeKind::Index: {
        const auto *ix = static_cast<const Index *>(t);
        const std::string obj_slot =
            std::to_string(alloc_local("__assign_obj_" +
                                       std::to_string(label_counter_)));
        const std::string idx_slot =
            std::to_string(alloc_local("__assign_idx_" +
                                       std::to_string(label_counter_)));
        gen_expr(ix->obj);
        emit(ir::OP_STORE_VAR, obj_slot);
        gen_expr(ix->index);
        emit(ir::OP_STORE_VAR, idx_slot);
        emit(ir::OP_LOAD_VAR, obj_slot);
        emit(ir::OP_LOAD_VAR, idx_slot);
        if (ix->obj->kind == NodeKind::Ident) {
          auto it = var_kind_.find(std::string(static_cast<const Ident *>(ix->obj)->name));
          if (it != var_kind_.end() && it->second == kVarMap) { emit(ir::OP_INDEX_GET, "map"); } else { emit(ir::OP_INDEX_GET, "i32"); }
        } else {
          emit(ir::OP_INDEX_GET, "i32");
        }
        gen_expr(a->value);
        emit_binary_op(std::string_view(&bin_op, 1), ix, a->value, a->line);
        emit(ir::OP_LOAD_VAR, obj_slot);
        emit(ir::OP_LOAD_VAR, idx_slot);
        if (ix->obj->kind == NodeKind::Ident) {
          auto it = var_kind_.find(std::string(static_cast<const Ident *>(ix->obj)->name));
          if (it != var_kind_.end() && it->second == kVarMap) { emit(ir::OP_INDEX_SET, "map"); } else { emit(ir::OP_INDEX_SET); }
        } else {
          emit(ir::OP_INDEX_SET);
        }
        break;
      }
      default:
        fail("unsupported assignment target: " +
                 std::string(node_kind_name(t->kind)),
             a->line);
    }
  }

  // if/elif/else 控制流（对齐自举 codegen.vx）：
  //   cond, JUMP_IF_FALSE else, then, JUMP end, [elif...], else, end:
  void gen_if(const If *s) {
    gen_expr(s->cond);
    const std::string else_label = new_label();
    const std::string end_label = new_label();
    emit(ir::OP_JUMP_IF_FALSE, else_label);
    gen_block(s->then_body);
    emit(ir::OP_JUMP, end_label);
    emit_label(else_label);
    for (const ElifArm &arm : s->elifs) {
      gen_expr(arm.cond);
      const std::string next_label = new_label();
      emit(ir::OP_JUMP_IF_FALSE, next_label);
      gen_block(arm.body);
      emit(ir::OP_JUMP, end_label);
      emit_label(next_label);
    }
    gen_block(s->else_body);
    emit_label(end_label);
  }

  void gen_while(const While *s) {
    const std::string start_label = new_label();
    const std::string end_label = new_label();
    loop_stack_.push_back({start_label, end_label});
    emit_label(start_label);
    gen_expr(s->cond);
    emit(ir::OP_JUMP_IF_FALSE, end_label);
    gen_block(s->body);
    emit(ir::OP_JUMP, start_label);
    emit_label(end_label);
    loop_stack_.pop_back();
  }

  // for-in 降级（对齐自举 codegen.vx）：
  //   隐藏变量 __for_iter_N（可迭代对象）、__for_idx_N（下标），
  //   循环上界 100000 防失控，元素经 INDEX_GET 取出。
  void gen_for(const For *s) {
    gen_expr(s->iterable);
    const std::string iter_slot =
        std::to_string(alloc_local("__for_iter_" +
                                   std::to_string(label_counter_)));
    emit(ir::OP_STORE_VAR, iter_slot);
    const std::string idx_slot =
        std::to_string(alloc_local("__for_idx_" +
                                   std::to_string(label_counter_)));
    emit(ir::OP_CONST_INT, "i0");
    emit(ir::OP_STORE_VAR, idx_slot);

    const std::string start_label = new_label();
    const std::string end_label = new_label();
    loop_stack_.push_back({start_label, end_label});

    emit_label(start_label);
    emit(ir::OP_LOAD_VAR, idx_slot);
    emit(ir::OP_CONST_INT, "i100000");
    emit(ir::OP_I32_LT);
    emit(ir::OP_JUMP_IF_FALSE, end_label);

    emit(ir::OP_LOAD_VAR, iter_slot);
    emit(ir::OP_LOAD_VAR, idx_slot);
    emit(ir::OP_INDEX_GET);
    emit(ir::OP_STORE_VAR, std::to_string(alloc_local(s->var)));

    gen_block(s->body);

    emit(ir::OP_LOAD_VAR, idx_slot);
    emit(ir::OP_CONST_INT, "i1");
    emit(ir::OP_I32_ADD);
    emit(ir::OP_STORE_VAR, idx_slot);
    emit(ir::OP_JUMP, start_label);
    emit_label(end_label);
    loop_stack_.pop_back();
  }

  void gen_block(std::span<const Stmt *const> stmts) {
    for (const Stmt *s : stmts) {
      gen_stmt(s);
    }
  }

  // ---- 函数 ----
  void begin_func(const std::string &name, std::vector<Param> params,
                  std::string_view ret_type) {
    cur_.name = name;
    cur_.id = 0;
    cur_.ret_type = std::string(ret_type);
    cur_.params = std::move(params);
    cur_.instructions.clear();
    cur_.local_count = 0;
    local_ids_.clear();
    local_counter_ = 0;
    var_kind_.clear();
    insts_.clear();
    has_return_ = false;
    label_counter_ = 0;
    loop_stack_.clear();
    for (const Param &p : cur_.params) {
      alloc_local(p.name);
    }
  }

  void end_func(ModuleIR &mod) {
    // 无显式 return 的非 void 函数补 i0 返回（对齐自举）
    if (!has_return_ && !cur_.ret_type.empty() && cur_.ret_type != "void") {
      emit(ir::OP_CONST_INT, "i0");
      emit(ir::OP_RETURN);
    }
    cur_.instructions = std::move(insts_);
    cur_.local_count = local_counter_;
    cur_.id = static_cast<std::uint32_t>(mod.funcs.size());
    mod.funcs.push_back(std::move(cur_));
    cur_ = ir::FuncIR{};
  }

  void gen_func(std::string_view name, std::span<const Param> params,
                std::string_view ret_type,
                std::span<const Stmt *const> body, ModuleIR &mod) {
    begin_func(std::string(name), {params.begin(), params.end()}, ret_type);
    gen_block(body);
    end_func(mod);
  }

  // 方法生成为 "<Class>.<method>"；隐式 self 占首槽位
  //（源码已显式声明 self 时不重复添加）。
  void gen_method(const ClassDecl &cls, const FuncDecl &m, ModuleIR &mod) {
    std::vector<Param> params;
    params.reserve(m.params.size() + 1);
    if (m.params.empty() || m.params[0].name != "self") {
      params.push_back(Param{"self", ""});
    }
    params.insert(params.end(), m.params.begin(), m.params.end());
    begin_func(std::string(cls.name) + "." + std::string(m.name),
               std::move(params), m.ret_type);
    gen_block(m.body);
    end_func(mod);
  }

  void gen_extern(const ExternDecl *e, ModuleIR &mod) {
    ir::LinkEntry entry;
    entry.name = std::string(e->name);
    entry.ret_type = std::string(e->ret_type);
    for (const Param &p : e->params) {
      entry.params.push_back(ir::AbiParam{std::string(p.name), std::string(p.type)});
    }
    mod.linkage.push_back(std::move(entry));
  }

  // @imp / @cppimp：把导入库的依赖与全部符号签名写入 ModuleIR
  //（符号以 VX ABI 名 _vx_<lib>_<name> 登记为 linkage 条目）。
  void register_import(const Stmt *s, ModuleIR &mod) {
    if (imports_ == nullptr) {
      return;
    }
    const std::string_view ref =
        s->kind == NodeKind::ImpDecl
            ? static_cast<const ImpDecl *>(s)->lib
            : static_cast<const CppImpDecl *>(s)->header;
    for (const ImportedLib &l : imports_->libs) {
      if (l.ref != ref) {
        continue;
      }
      // 同一库被多条注解引用（@imp(math) 与 @imp(math) as m）时只注册一次
      const bool dup = std::any_of(
          mod.libs.begin(), mod.libs.end(),
          [&](const ir::LibDep &d) { return d.ref == l.ref; });
      if (dup) {
        return;
      }
      mod.libs.push_back(ir::LibDep{l.name, l.ref, l.impl_file, l.from_cpp,
                                    l.cpp_header});
      for (const ImportedSymbol &sym : l.symbols) {
        ir::LinkEntry entry;
        entry.name = sym.vx_name;
        entry.params = sym.params;
        entry.ret_type = sym.ret_type;
        entry.abi_symbol = sym.abi_symbol;
        entry.lib = l.name;
        entry.from_cpp = l.from_cpp;
        mod.linkage.push_back(std::move(entry));
      }
      return;
    }
  }

  template <class Decl> void register_struct(const Decl *d) {
    struct_fields_[std::string(d->name)] = {d->fields.begin(),
                                            d->fields.end()};
  }

  // ---- 状态 ----
  std::string_view filename_; // 诊断定位用（可为空）
  DiagBag &bag_;              // 共享诊断收集器
  const ImportTable *imports_ = nullptr; // 导入表（可空）
  bool failed_ = false;
  std::unordered_map<std::string, std::vector<ClassField>> struct_fields_;

  ir::FuncIR cur_; // 当前函数
  std::unordered_map<std::string, std::uint32_t> local_ids_;
  std::uint32_t local_counter_ = 0;
  // 变量类型传播（函数级）：名称 → 推断类型，供 is_float_expr/is_str_expr
  // 选择运算族（LLVM 层槽位类型由首条写入决定，二者需一致）
  enum VarKind : int { kVarUnknown = 0, kVarFloat = 1, kVarStr = 2,
                       kVarListInt = 3, kVarListFloat = 4, kVarListStr = 5,
                       kVarMap = 6 };
  std::unordered_map<std::string, int> var_kind_;
  std::vector<Inst> insts_;
  bool has_return_ = false;
  int label_counter_ = 0;
  std::vector<std::pair<std::string, std::string>> loop_stack_; // start, end
};

} // namespace

std::optional<ir::ModuleIR> generate_module(const Program &prog,
                                           std::string_view filename,
                                           DiagBag &bag,
                                           const ImportTable *imports) {
  return CodeGen(filename, bag, imports).run(prog);
}

} // namespace vx::compiler
