// parser_test — Compiler 层语法分析器测试（自带断言宏，不依赖第三方框架）。
#include "vx/compiler/parser.hpp"

#include "vx/util/util.hpp"

#include <cstdio>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace {

int g_failures = 0;

#define CHECK(cond)                                                            \
  do {                                                                         \
    if (!(cond)) {                                                             \
      std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);     \
      ++g_failures;                                                            \
    }                                                                          \
  } while (0)

using namespace vx::compiler;
using vx::util::Arena;

// 按 kind 判定后 static_cast（AST 无虚表）
template <class T>
const T *cast(NodeKind k, const Node *n) {
  return (n != nullptr && n->kind == k) ? static_cast<const T *>(n) : nullptr;
}

// 词法 + 语法一体（arena 静态，保证 AST string_view 在断言期间有效）
std::vector<const Stmt *> parse_src(std::string_view src) {
  static Arena arena;
  DiagBag bag;
  auto toks = tokenize(src, "<test>", arena, bag);
  auto prog = parse(toks, "<test>", arena, bag);
  if (!prog) {
    std::fprintf(stderr, "FAIL parse:\n%s", render_all(bag, src, false).c_str());
    ++g_failures;
    return {};
  }
  return {prog->stmts.begin(), prog->stmts.end()};
}

const Stmt *stmt_at(const std::vector<const Stmt *> &v, std::size_t i) {
  return i < v.size() ? v[i] : nullptr;
}

const Expr *init_of(const Stmt *s) {
  const auto *vd = cast<VarDecl>(NodeKind::VarDecl, s);
  return vd ? vd->init : nullptr;
}

void test_expr_precedence() {
  auto stmts = parse_src("var x = 1 + 2 * 3\n");
  CHECK(stmts.size() == 1);
  const auto *vd = cast<VarDecl>(NodeKind::VarDecl, stmt_at(stmts, 0));
  CHECK(vd != nullptr);
  if (!vd) {
    return;
  }
  CHECK(vd->name == "x");
  const auto *bin = cast<Binary>(NodeKind::Binary, vd->init);
  CHECK(bin && bin->op == "+");
  if (bin) {
    const auto *rhs = cast<Binary>(NodeKind::Binary, bin->right);
    CHECK(rhs && rhs->op == "*"); // 优先级爬升：2*3 先结合
  }
}

void test_unary_and_logic() {
  auto stmts =
      parse_src("var a = -1\nvar b = not true\nvar c = 1 and 2 or 3\n");
  CHECK(stmts.size() == 3);
  if (stmts.size() == 3) {
    const auto *u1 = cast<Unary>(NodeKind::Unary, init_of(stmts[0]));
    CHECK(u1 && u1->op == "-");
    const auto *u2 = cast<Unary>(NodeKind::Unary, init_of(stmts[1]));
    CHECK(u2 && u2->op == "!");
    const auto *top = cast<Binary>(NodeKind::Binary, init_of(stmts[2]));
    CHECK(top && top->op == "or"); // or 优先级最低
  }
}

void test_literals_containers() {
  auto stmts = parse_src("var l = [1, 2, 3]\nvar d = {\"k\": 1, \"b\": 2}\n"
                         "var e = []\n");
  CHECK(stmts.size() == 3);
  if (stmts.size() == 3) {
    const auto *ll = cast<ListLit>(NodeKind::ListLit, init_of(stmts[0]));
    CHECK(ll && ll->items.size() == 3);
    const auto *dl = cast<DictLit>(NodeKind::DictLit, init_of(stmts[1]));
    CHECK(dl && dl->entries.size() == 2);
    if (dl && dl->entries.size() == 2) {
      const auto *k = cast<StrLit>(NodeKind::StrLit, dl->entries[0].key);
      CHECK(k && k->value == "k");
    }
    const auto *el = cast<ListLit>(NodeKind::ListLit, init_of(stmts[2]));
    CHECK(el && el->items.empty());
  }
}

void test_postfix_chain() {
  auto stmts =
      parse_src("r = self.read_number()[0]\nSet.union(s, s2)\nl = Lexer.new(src)\n");
  CHECK(stmts.size() == 3);
  if (stmts.size() == 3) {
    const auto *as = cast<Assign>(NodeKind::Assign, stmts[0]);
    const auto *idx = as ? cast<Index>(NodeKind::Index, as->value) : nullptr;
    CHECK(idx != nullptr); // read_number() 后接 [0]
    const auto *es = cast<ExprStmt>(NodeKind::ExprStmt, stmts[1]);
    const auto *call = es ? cast<Call>(NodeKind::Call, es->expr) : nullptr;
    const auto *prop =
        call ? cast<Property>(NodeKind::Property, call->callee) : nullptr;
    CHECK(prop && prop->name == "union"); // 关键字成员名
    CHECK(call && call->args.size() == 2);
    const auto *as2 = cast<Assign>(NodeKind::Assign, stmts[2]);
    const auto *call2 =
        as2 ? cast<Call>(NodeKind::Call, as2->value) : nullptr;
    const auto *prop2 =
        call2 ? cast<Property>(NodeKind::Property, call2->callee) : nullptr;
    CHECK(prop2 && prop2->name == "new");
  }
}

void test_assign_targets() {
  auto stmts = parse_src("x += 1\nbuf[0] = 48\nself.pos = 0\n");
  CHECK(stmts.size() == 3);
  if (stmts.size() == 3) {
    const auto *a1 = cast<Assign>(NodeKind::Assign, stmts[0]);
    CHECK(a1 && a1->op == "+=");
    const auto *a2 = cast<Assign>(NodeKind::Assign, stmts[1]);
    CHECK(a2 && cast<Index>(NodeKind::Index, a2->target) != nullptr);
    const auto *a3 = cast<Assign>(NodeKind::Assign, stmts[2]);
    CHECK(a3 && cast<Property>(NodeKind::Property, a3->target) != nullptr);
  }
}

void test_control_flow() {
  auto stmts = parse_src("if a:\n    b = 1\nelif c:\n    d = 2\nelse:\n"
                         "    e = 3\nwhile x:\n    break\nfor i in xs:\n"
                         "    continue\n");
  CHECK(stmts.size() == 3);
  if (stmts.size() == 3) {
    const auto *ifn = cast<If>(NodeKind::If, stmts[0]);
    CHECK(ifn && ifn->then_body.size() == 1);
    CHECK(ifn && ifn->elifs.size() == 1);
    CHECK(ifn && ifn->else_body.size() == 1);
    const auto *wh = cast<While>(NodeKind::While, stmts[1]);
    CHECK(wh && wh->body.size() == 1 &&
          wh->body[0]->kind == NodeKind::Break);
    const auto *fo = cast<For>(NodeKind::For, stmts[2]);
    CHECK(fo && fo->var == "i" && fo->body.size() == 1 &&
          fo->body[0]->kind == NodeKind::Continue);
  }
}

void test_inline_body_with_semi() {
  // 单行体 + 分号多语句：两条语句都属于 if 体
  auto stmts = parse_src("if c:\n    a()\n    b()\nif d: x(); y()\n");
  CHECK(stmts.size() == 2);
  if (stmts.size() == 2) {
    const auto *if2 = cast<If>(NodeKind::If, stmts[1]);
    CHECK(if2 && if2->then_body.size() == 2);
  }
}

void test_decls() {
  auto stmts = parse_src("import std.error\nimport a.b.c as ab\n"
                         "export lexer\nextern puts(s: string)\n"
                         "extern add(a: int, b: int) -> int\n"
                         "func main(argc: int) -> var:\n    return 0\n"
                         "class Lexer:\n    src: str\n    pos: int\n"
                         "    func new(source:str) -> var:\n        return nil\n"
                         "struct Pair:\n    x: int\n    y: int\n");
  CHECK(stmts.size() == 8);
  if (stmts.size() != 8) {
    return;
  }
  const auto *imp = cast<Import>(NodeKind::Import, stmts[0]);
  CHECK(imp && imp->path == "std.error");
  const auto *imp2 = cast<Import>(NodeKind::Import, stmts[1]);
  CHECK(imp2 && imp2->path == "a.b.c");
  const auto *exp = cast<Export>(NodeKind::Export, stmts[2]);
  CHECK(exp && exp->name == "lexer");
  const auto *ext = cast<ExternDecl>(NodeKind::ExternDecl, stmts[3]);
  CHECK(ext && ext->name == "puts" && ext->params.size() == 1 &&
        ext->ret_type.empty());
  const auto *ext2 = cast<ExternDecl>(NodeKind::ExternDecl, stmts[4]);
  CHECK(ext2 && ext2->ret_type == "int");
  const auto *fn = cast<FuncDecl>(NodeKind::FuncDecl, stmts[5]);
  CHECK(fn && fn->name == "main" && fn->ret_type == "var" &&
        fn->body.size() == 1);
  const auto *cls = cast<ClassDecl>(NodeKind::ClassDecl, stmts[6]);
  CHECK(cls && cls->name == "Lexer" && cls->fields.size() == 2 &&
        cls->methods.size() == 1);
  if (cls && cls->methods.size() == 1) {
    CHECK(cls->methods[0]->name == "new"); // 关键字函数名
    CHECK(cls->fields[0].type == "str");
  }
  const auto *st = cast<StructDecl>(NodeKind::StructDecl, stmts[7]);
  CHECK(st && st->name == "Pair" && st->fields.size() == 2);
}

// @imp / @cppimp 导入注解：库引用（含子目录路径）与可选 as 别名
void test_import_directives() {
  auto stmts = parse_src(
      "@imp(math)\n"
      "@imp(collections/vec) as cvec\n"
      "@cppimp(cpp/greet.hpp)\n"
      "@cppimp(\"cpp/other.hpp\") as other\n");
  CHECK(stmts.size() == 4);
  if (stmts.size() != 4) {
    return;
  }
  const auto *d0 = cast<ImpDecl>(NodeKind::ImpDecl, stmts[0]);
  CHECK(d0 && d0->lib == "math" && d0->alias.empty());
  // 子目录引用拼接为 "collections/vec"；库名取末段由 importer 完成
  const auto *d1 = cast<ImpDecl>(NodeKind::ImpDecl, stmts[1]);
  CHECK(d1 && d1->lib == "collections/vec" && d1->alias == "cvec");
  const auto *d2 = cast<CppImpDecl>(NodeKind::CppImpDecl, stmts[2]);
  CHECK(d2 && d2->header == "cpp/greet.hpp" && d2->alias.empty());
  // 字符串形式引用（含引号）同样接受
  const auto *d3 = cast<CppImpDecl>(NodeKind::CppImpDecl, stmts[3]);
  CHECK(d3 && d3->header == "cpp/other.hpp" && d3->alias == "other");
}

void test_errors() {
  Arena arena;
  {
    DiagBag bag;
    auto toks = tokenize("var x = \n", "<test>", arena, bag);
    CHECK(!bag.has_error());
    auto prog = parse(toks, "<test>", arena, bag);
    CHECK(prog.has_value()); // fail-soft：返回部分 AST（此处 0 条语句）
    CHECK(bag.has_error());
    if (bag.has_error()) {
      const auto &d = bag.items().back();
      CHECK(d.code == "E2010");
      CHECK(d.line == 1);
      CHECK(!d.message.empty());
    }
  }
  {
    DiagBag bag;
    auto toks2 = tokenize("if x\n    y = 1\n", "<test>", arena, bag);
    CHECK(!bag.has_error());
    auto prog2 = parse(toks2, "<test>", arena, bag);
    CHECK(prog2.has_value()); // fail-soft：仍返回部分 AST
    CHECK(bag.has_error());
  }
  {
    // offending token 为多字符标识符：span 覆盖 token 整段
    DiagBag bag;
    auto toks3 = tokenize("if foo bar:\n    y = 1\n", "<test>", arena, bag);
    CHECK(!bag.has_error());
    auto prog3 = parse(toks3, "<test>", arena, bag);
    CHECK(prog3.has_value()); // fail-soft：仍返回部分 AST
    CHECK(bag.has_error());
    if (bag.has_error()) {
      const auto &d = bag.items().back();
      CHECK(d.code == "E2010");
      CHECK(d.line == 1);
      CHECK(d.col == 8);       // "bar" 起始列
      CHECK(d.end_line == 1);
      CHECK(d.end_col == 11);  // 覆盖 3 个码点
    }
  }
}

// 流水线：真实 VX 源码 read_file → tokenize → parse 零错误
void test_pipeline_vx_sources() {
  namespace fs = std::filesystem;

  fs::path root = fs::current_path();
  while (!root.empty() && !fs::exists(root / "compiler" / "lexer.vx")) {
    root = root.parent_path();
  }
  CHECK(!root.empty());
  if (root.empty()) {
    return;
  }

  std::size_t files = 0;
  std::size_t stmts_total = 0;
  for (const char *dir : {"compiler", "tests"}) {
    std::error_code ec;
    for (fs::recursive_directory_iterator it(root / dir, ec), end; it != end;
         it.increment(ec)) {
      if (ec) {
        break;
      }
      if (!it->is_regular_file() || it->path().extension() != ".vx") {
        continue;
      }
      if (it->path().filename().string().find(".decompiled.vx") !=
          std::string::npos) {
        continue; // 旧反编译器输出，非当前语法
      }
      auto src = vx::util::read_file(it->path());
      CHECK(src.has_value());
      if (!src) {
        continue;
      }
      Arena arena;
      DiagBag bag;
      auto toks = tokenize(*src, it->path().string(), arena, bag);
      auto prog = parse(toks, it->path().string(), arena, bag);
      CHECK(!bag.has_error());
      if (bag.has_error()) {
        std::fprintf(stderr, "FAIL %s\n%s\n", it->path().string().c_str(),
                     render_all(bag, *src, false).c_str());
        ++g_failures;
        continue;
      }
      CHECK(prog.has_value());
      CHECK(!prog->stmts.empty());
      stmts_total += prog->stmts.size();
      ++files;
    }
  }
  CHECK(files >= 10);
  CHECK(stmts_total > 100);

  // 抽查 compiler/lexer.vx 的结构
  auto src = vx::util::read_file(root / "compiler" / "lexer.vx");
  CHECK(src.has_value());
  if (src) {
    Arena arena;
    DiagBag bag;
    auto toks = tokenize(*src, "<lexer.vx>", arena, bag);
    auto prog = parse(toks, "<lexer.vx>", arena, bag);
    CHECK(!bag.has_error());
    if (prog) {
      const auto *imp =
          cast<Import>(NodeKind::Import, prog->stmts[0]);
      CHECK(imp && imp->path == "std.error");
      bool has_class = false;
      bool has_tokenize = false;
      for (const Stmt *s : prog->stmts) {
        if (const auto *c = cast<ClassDecl>(NodeKind::ClassDecl, s)) {
          has_class = c->name == "Lexer" && !c->methods.empty();
        }
        if (const auto *f = cast<FuncDecl>(NodeKind::FuncDecl, s)) {
          has_tokenize = f->name == "tokenize";
        }
      }
      CHECK(has_class);
      CHECK(has_tokenize);
    }
  }
}

} // namespace

int main() {
  test_expr_precedence();
  test_unary_and_logic();
  test_literals_containers();
  test_postfix_chain();
  test_assign_targets();
  test_control_flow();
  test_inline_body_with_semi();
  test_decls();
  test_import_directives();
  test_errors();
  test_pipeline_vx_sources();

  if (g_failures != 0) {
    std::fprintf(stderr, "%d 项检查失败\n", g_failures);
    return 1;
  }
  std::puts("Parser 测试全部通过");
  return 0;
}
