// tidy.cpp — vx-tidy 静态分析引擎实现。
// 检查在单文件 AST 上进行：
//   VXTIDY001  调用未声明的函数（非本文件 func/extern、非内置、非导入库）
//   VXTIDY002  声明后从未读取的局部变量
//   VXTIDY003  函数重复声明
//   VXTIDY004  局部变量遮蔽参数（同函数内同名）
//   VXTIDY005  非 void 函数缺少 return（显式返回类型但函数体无任何 return）
//   VXTIDY900  词法/语法错误（前端诊断透传）
#include "tidy.hpp"

#include "vx/compiler/ast.hpp"
#include "vx/compiler/diag.hpp"
#include "vx/compiler/lexer.hpp"
#include "vx/compiler/parser.hpp"
#include "vx/util/arena.hpp"

#include <algorithm>
#include <set>
#include <map>

namespace vxtidy {

using namespace vx::compiler; // NOLINT：测试性工具，直接使用编译器 AST

namespace {

// 内置运行时符号（llvm_emitter Builtin 表 + 常见 libc）
const std::set<std::string> kBuiltins = {
    "out", "out_int", "out_bool", "out_float", "puts", "exit",
    "malloc", "free", "vx_str_concat",
    // 系统能力：命令行参数 / 文件 I/O / 执行 shell 命令
    "sys_argv_count", "sys_argv_get", "file_read", "file_write",
    "file_exists", "system"};

// ---- 作用域符号表 ----
struct LocalInfo {
  int decl_line = 0;
  int decl_col = 0;
  bool is_param = false;
  bool used = false;
};

class Checker {
public:
  Checker(std::vector<Finding> &out) : out_(out) {}

  void run(const Program &prog) {
    // 第一遍：顶层声明（函数/extern/导入库）供调用解析
    for (const Stmt *s : prog.stmts) {
      switch (s->kind) {
      case NodeKind::FuncDecl: {
        const auto *f = static_cast<const FuncDecl *>(s);
        if (!funcs_.emplace(std::string(f->name), f->line).second) {
          out_.push_back({"VXTIDY003", 2, f->line, 0,
                          "函数 '" + std::string(f->name) + "' 重复声明"});
        }
        break;
      }
      case NodeKind::ExternDecl: {
        const auto *e = static_cast<const ExternDecl *>(s);
        externs_.insert(std::string(e->name));
        break;
      }
      case NodeKind::ImpDecl: {
        const auto *d = static_cast<const ImpDecl *>(s);
        libs_.insert(lib_alias(d->lib, d->alias));
        break;
      }
      case NodeKind::CppImpDecl: {
        const auto *d = static_cast<const CppImpDecl *>(s);
        libs_.insert(lib_alias(d->header, d->alias));
        break;
      }
      default:
        break;
      }
    }

    // 第二遍：逐函数检查
    for (const Stmt *s : prog.stmts) {
      if (s->kind == NodeKind::FuncDecl) {
        check_func(static_cast<const FuncDecl *>(s));
      }
    }
  }

private:
  std::vector<Finding> &out_;
  std::map<std::string, int> funcs_;   // 顶层函数名 → 首次声明行
  std::set<std::string> externs_;
  std::set<std::string> libs_;    // 导入库访问前缀
  std::map<std::string, LocalInfo> locals_; // 当前函数内局部符号（含参数）
  bool has_return_ = false;
  bool is_void_ = true;

  static std::string lib_alias(std::string_view ref, std::string_view alias) {
    if (!alias.empty()) return std::string(alias);
    // 库名 = 去前导 '/' 后取末段
    std::string_view r = ref;
    while (!r.empty() && r.front() == '/') r.remove_prefix(1);
    const std::size_t slash = r.rfind('/');
    return slash == std::string_view::npos ? std::string(r)
                                           : std::string(r.substr(slash + 1));
  }

  void error(const std::string &rule, int line, std::string msg, int sev = 1) {
    out_.push_back({rule, sev, line, 0, std::move(msg)});
  }

  void check_func(const FuncDecl *f) {
    locals_.clear();
    has_return_ = false;
    is_void_ = f->ret_type.empty() || f->ret_type == "void";

    // 参数入作用域
    for (const Param &p : f->params) {
      LocalInfo info;
      info.decl_line = f->line;
      info.is_param = true;
      locals_[std::string(p.name)] = info;
    }
    // 方法隐式 self
    if (f->name.find('.') != std::string_view::npos) {
      LocalInfo info;
      info.decl_line = f->line;
      info.is_param = true;
      locals_["self"] = info;
    }

    walk_block(f->body);

    // 未使用的局部变量（参数不算未使用）
    for (const auto &[name, info] : locals_) {
      if (!info.is_param && !info.used) {
        error("VXTIDY002", info.decl_line,
              "变量 '" + name + "' 声明后从未被读取", 2);
      }
    }
    // 非 void 函数无任何 return
    if (!is_void_ && !has_return_) {
      error("VXTIDY005", f->line,
            "非 void 函数 '" + std::string(f->name) + "' 缺少 return 语句", 2);
    }
  }

  void walk_block(const std::span<const Stmt *const> &body) {
    for (const Stmt *s : body) walk_stmt(s);
  }

  void walk_stmt(const Stmt *s) {
    if (s == nullptr) return;
    switch (s->kind) {
    case NodeKind::VarDecl: {
      const auto *v = static_cast<const VarDecl *>(s);
      const std::string name(v->name);
      auto it = locals_.find(name);
      if (it != locals_.end()) {
        if (it->second.is_param) {
          error("VXTIDY004", v->line,
                "变量 '" + name + "' 遮蔽了同名参数", 2);
        } else {
          error("VXTIDY003", v->line,
                "变量 '" + name + "' 在同一函数内重复声明", 2);
        }
        // 仍更新行号便于未使用定位
        it->second.decl_line = v->line;
        it->second.used = true; // 重复声明视为已处理，避免叠加报错
      } else {
        LocalInfo info;
        info.decl_line = v->line;
        locals_[name] = info;
      }
      if (v->init) walk_expr(v->init);
      break;
    }
    case NodeKind::Assign: {
      const auto *a = static_cast<const Assign *>(s);
      mark_use(a->target);   // 目标读旧值（复合赋值）；纯 = 也按使用处理偏保守
      walk_expr(a->value);
      break;
    }
    case NodeKind::ExprStmt: {
      const auto *e = static_cast<const ExprStmt *>(s);
      walk_expr(e->expr);
      break;
    }
    case NodeKind::Return: {
      const auto *r = static_cast<const Return *>(s);
      has_return_ = true;
      if (r->value) walk_expr(r->value);
      break;
    }
    case NodeKind::If: {
      const auto *i = static_cast<const If *>(s);
      walk_expr(i->cond);
      walk_block(i->then_body);
      for (const ElifArm &arm : i->elifs) {
        walk_expr(arm.cond);
        walk_block(arm.body);
      }
      walk_block(i->else_body);
      break;
    }
    case NodeKind::While: {
      const auto *w = static_cast<const While *>(s);
      walk_expr(w->cond);
      walk_block(w->body);
      break;
    }
    case NodeKind::For: {
      const auto *f = static_cast<const For *>(s);
      walk_expr(f->iterable);
      LocalInfo loop_var;
      loop_var.decl_line = f->line;
      const std::string vname(f->var);
      // 循环变量若与参数同名按遮蔽报，否则登记为局部
      if (!locals_.emplace(vname, loop_var).second) {
        if (locals_[vname].is_param) {
          error("VXTIDY004", f->line,
                "循环变量 '" + vname + "' 遮蔽了同名参数", 2);
        }
      }
      walk_block(f->body);
      break;
    }
    case NodeKind::FuncDecl: // 嵌套函数不支持，忽略
      break;
    default:
      // 其余语句类型含表达式时遍历
      break;
    }
  }

  // 遍历表达式：读到 Ident 标记使用；调用解析函数名
  void walk_expr(const Expr *e) {
    if (e == nullptr) return;
    switch (e->kind) {
    case NodeKind::Ident: {
      const auto *id = static_cast<const Ident *>(e);
      auto it = locals_.find(std::string(id->name));
      if (it != locals_.end()) it->second.used = true;
      break;
    }
    case NodeKind::Call: {
      const auto *c = static_cast<const Call *>(e);
      // 普通函数调用：callee 为 Ident 时校验声明
      if (c->callee->kind == NodeKind::Ident) {
        const auto *id = static_cast<const Ident *>(c->callee);
        const std::string name(id->name);
        const bool known = funcs_.count(name) || externs_.count(name) ||
                           kBuiltins.count(name) ||
                           locals_.count(name); // 本地变量调用（函数值，暂不判）
        if (!known && locals_.count(name) == 0) {
          error("VXTIDY001", c->line,
                "调用了未声明的函数 '" + name + "'");
        }
      }
      walk_expr(c->callee);
      for (const Expr *a : c->args) walk_expr(a);
      break;
    }
    case NodeKind::Property: {
      const auto *p = static_cast<const Property *>(e);
      // 导入库前缀：libs_ 命中则根标识符不必是局部量
      if (p->obj->kind == NodeKind::Ident) {
        const auto *id = static_cast<const Ident *>(p->obj);
        auto it = locals_.find(std::string(id->name));
        if (it != locals_.end()) {
          it->second.used = true;
        } else if (libs_.count(std::string(id->name)) == 0) {
          // 非导入前缀也非局部 → 访问未定义变量
          error("VXTIDY001", p->line,
                "标识符 '" + std::string(id->name) +
                    "' 未声明（既非局部变量也非导入库前缀）");
        }
      } else {
        walk_expr(p->obj);
      }
      break;
    }
    case NodeKind::Index: {
      const auto *ix = static_cast<const Index *>(e);
      walk_expr(ix->obj);
      walk_expr(ix->index);
      break;
    }
    case NodeKind::Unary: {
      const auto *u = static_cast<const Unary *>(e);
      walk_expr(u->operand);
      break;
    }
    case NodeKind::Binary: {
      const auto *b = static_cast<const Binary *>(e);
      walk_expr(b->left);
      walk_expr(b->right);
      break;
    }
    case NodeKind::New: {
      const auto *n = static_cast<const New *>(e);
      for (const Expr *a : n->args) walk_expr(a);
      break;
    }
    case NodeKind::ListLit: {
      const auto *l = static_cast<const ListLit *>(e);
      for (const Expr *it : l->items) walk_expr(it);
      break;
    }
    case NodeKind::DictLit: {
      const auto *d = static_cast<const DictLit *>(e);
      for (const DictEntry &en : d->entries) {
        walk_expr(en.key);
        walk_expr(en.value);
      }
      break;
    }
    default:
      break; // 字面量等无子节点
    }
  }

  // 赋值目标：标量 Ident 按读旧值计入使用；属性/下标按子表达式遍历
  void mark_use(const Expr *target) {
    if (target == nullptr) return;
    if (target->kind == NodeKind::Ident) {
      walk_expr(target);
      return;
    }
    walk_expr(target);
  }
};

} // namespace

const std::vector<RuleInfo> &all_rules() {
  static const std::vector<RuleInfo> rules = {
      {"VXTIDY001", 1, "undeclared",
       "调用/引用了未声明的函数或标识符（非本文件声明、非内置、非导入库）"},
      {"VXTIDY002", 2, "unused-variable",
       "声明后从未被读取的局部变量"},
      {"VXTIDY003", 2, "redeclared",
       "同一作用域内的重复声明（函数或局部变量）"},
      {"VXTIDY004", 2, "shadowed-variable",
       "局部变量或循环变量遮蔽了同名参数"},
      {"VXTIDY005", 2, "missing-return",
       "声明了非 void 返回类型但函数体内没有任何 return"},
      {"VXTIDY900", 1, "syntax-error",
       "词法/语法错误（前端诊断透传）"},
  };
  return rules;
}

std::vector<Finding> check(std::string_view text, std::string_view filename) {
  std::vector<Finding> out;
  vx::util::Arena arena;
  DiagBag bag;
  auto toks = tokenize(text, filename, arena, bag);
  auto prog = parse(toks, filename, arena, bag);
  // 前端错误透传为 VXTIDY900（仅 Error 级）
  for (const Diagnostic &d : bag.items()) {
    if (d.severity == Severity::Error) {
      out.push_back({"VXTIDY900", 1, d.line, d.col,
                     d.message + (d.code.empty() ? "" : " [" + d.code + "]")});
    }
  }
  // 前端有语法错误时不跑语义规则（部分 AST 会产生误导性发现），
  // 仅透传 VXTIDY900
  if (!prog || bag.has_error()) return out;

  Checker c(out);
  c.run(*prog);
  // 输出按行号排序（稳定）
  std::stable_sort(out.begin(), out.end(),
                   [](const Finding &a, const Finding &b) {
                     return a.line < b.line;
                   });
  return out;
}

std::string format_text(const Finding &f, std::string_view filename) {
  const char *sev =
      f.severity == 1 ? "error" : f.severity == 2 ? "warning" : "note";
  std::string line = std::string(filename) + ":" + std::to_string(f.line);
  if (f.col > 0) line += ":" + std::to_string(f.col);
  line += ": ";
  line += sev;
  line += ": ";
  line += f.message;
  line += " [";
  line += f.rule;
  line += "]";
  return line;
}

std::string format_json(const std::vector<Finding> &fs,
                        std::string_view filename) {
  // 手写 JSON（避免引入依赖）
  auto esc = [](std::string_view s) {
    std::string o;
    for (char c : s) {
      if (c == '"' || c == '\\') {
        o += '\\';
        o += c;
      } else if (c == '\n') {
        o += "\\n";
      } else {
        o += c;
      }
    }
    return o;
  };
  std::string out = "[";
  for (std::size_t i = 0; i < fs.size(); ++i) {
    const Finding &f = fs[i];
    if (i) out += ",";
    out += "{\"file\":\"" + esc(filename) + "\",";
    out += "\"line\":" + std::to_string(f.line) + ",";
    out += "\"col\":" + std::to_string(f.col) + ",";
    out += "\"severity\":" + std::to_string(f.severity) + ",";
    out += "\"rule\":\"" + esc(f.rule) + "\",";
    out += "\"message\":\"" + esc(f.message) + "\"}";
  }
  out += "]";
  return out;
}

} // namespace vxtidy
