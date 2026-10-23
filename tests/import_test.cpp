// import_test — @imp / @cppimp 导入：VX ABI 命名、headers 解析、
// 导入表装载、codegen 的 ABI 调用与 linkage、emitter 的外部符号声明。
// 库文件位于 <源码根>/headers（经 CMake 的 VX_SOURCE_DIR 宏定位）。
#include "vx/backend/llvm_emitter.hpp"
#include "vx/compiler/codegen.hpp"
#include "vx/compiler/diag.hpp"
#include "vx/compiler/importer.hpp"
#include "vx/compiler/lexer.hpp"
#include "vx/compiler/parser.hpp"
#include "vx/compiler/vxabi.hpp"
#include "vx/util/util.hpp"

#include <cstdio>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

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
namespace fs = std::filesystem;
namespace ir = vx::compiler::ir;
namespace abi = vx::compiler::abi;
namespace backend = vx::backend;

// 库查找根：<源码根>/headers
fs::path headers_root() { return fs::path(VX_SOURCE_DIR) / "headers"; }

// 以 headers 根为上下文装载导入表；失败时打印诊断并返回 nullopt。
std::optional<ImportTable> load_imports(const Program &prog, DiagBag &bag) {
  static Arena dummy;
  (void)dummy;
  ImportContext ctx{headers_root(), headers_root()};
  return build_import_table(prog, ctx, "<test>", bag);
}

// 前端流水线（含导入表）：tokenize → parse → build_import_table → codegen
std::optional<ir::ModuleIR> gen_import(const std::string &src,
                                       const std::string &file = "<test>") {
  static Arena arena;
  DiagBag bag;
  auto toks = tokenize(src, file, arena, bag);
  auto prog = parse(toks, file, arena, bag);
  if (!prog) {
    std::fprintf(stderr, "FAIL parse:\n%s", render_all(bag, src, false).c_str());
    ++g_failures;
    return std::nullopt;
  }
  auto imports = load_imports(*prog, bag);
  if (!imports) {
    std::fprintf(stderr, "FAIL import:\n%s",
                 render_all(bag, src, false).c_str());
    ++g_failures;
    return std::nullopt;
  }
  auto mod = generate_module(*prog, file, bag, &*imports);
  if (!mod) {
    std::fprintf(stderr, "FAIL codegen:\n%s",
                 render_all(bag, src, false).c_str());
    ++g_failures;
    return std::nullopt;
  }
  return std::move(*mod);
}

bool ends_with(std::string_view s, std::string_view suf) {
  return s.size() >= suf.size() && s.substr(s.size() - suf.size()) == suf;
}

// ==================== VX ABI 纯函数 ====================

void test_abi() {
  CHECK(abi::VX_ABI_VERSION == 1);
  CHECK(abi::sanitize("a/b.c") == "a_b_c");
  CHECK(abi::sanitize("ok_Name1") == "ok_Name1");
  CHECK(abi::lib_name_of("math") == "math");
  CHECK(abi::lib_name_of("collections/vec") == "vec");
  CHECK(abi::lib_name_of("/sub/lib") == "lib");
  CHECK(abi::lib_name_of("./mylib") == "mylib");
  CHECK(abi::lib_name_of("a/b/") == "b");
  CHECK(abi::symbol("math", "add") == "_vx_math_add");
  // symbol 的 lib 参数是「库名（末段）」；路径形式先经 lib_name_of 归一
  CHECK(abi::symbol(abi::lib_name_of("collections/vec"), "sum_to") ==
        "_vx_vec_sum_to");
  // 类型映射
  CHECK(abi::c_type_of("int") == "int32_t");
  CHECK(abi::c_type_of("float") == "double");
  CHECK(abi::c_type_of("bool") == "int32_t");
  CHECK(abi::c_type_of("string") == "const char*");
  CHECK(abi::c_type_of("void") == "void");
  CHECK(abi::c_type_of("未知名") == "int32_t"); // 默认回退整型
  CHECK(abi::llvm_type_of("float") == "double");
  CHECK(abi::llvm_type_of("string") == "ptr");
  CHECK(abi::llvm_type_of("int") == "i32");
}

// ==================== headers 解析 ====================

void test_build_import_table() {
  static Arena arena;
  const std::string src =
      "@imp(math)\n"
      "@imp(/collections/vec)\n"
      "@cppimp(cpp/greet.hpp)\n";
  DiagBag bag;
  auto toks = tokenize(src, "<test>", arena, bag);
  auto prog = parse(toks, "<test>", arena, bag);
  CHECK(prog.has_value());
  if (!prog) return;
  auto imports = load_imports(*prog, bag);
  CHECK(imports.has_value());
  if (!imports) return;
  CHECK(imports->libs.size() == 3);
  if (imports->libs.size() != 3) return;

  // @imp(math)：根目录库，直接文件名引用
  const ImportedLib &math = imports->libs[0];
  CHECK(math.name == "math");
  CHECK(math.from_cpp == false);
  CHECK(ends_with(math.decl_file, "headers/math.vxh"));
  CHECK(ends_with(math.impl_file, "headers/math.c"));
  CHECK(math.symbols.size() == 2);
  if (math.symbols.size() == 2) {
    CHECK(math.symbols[0].vx_name == "add");
    CHECK(math.symbols[0].abi_symbol == "_vx_math_add");
    CHECK(math.symbols[0].params.size() == 2);
    CHECK(math.symbols[0].params[0].type == "int");
    CHECK(math.symbols[0].ret_type == "int");
    CHECK(math.symbols[1].abi_symbol == "_vx_math_mul");
  }

  // @imp(/collections/vec)：子目录库，库名取末段 vec
  const ImportedLib &vec = imports->libs[1];
  CHECK(vec.name == "vec");
  CHECK(ends_with(vec.decl_file, "headers/collections/vec.vxh"));
  CHECK(ends_with(vec.impl_file, "headers/collections/vec.c"));
  CHECK(ImportTable::find_symbol(vec, "sum_to") != nullptr);
  CHECK(ImportTable::find_symbol(vec, "nope") == nullptr);

  // @cppimp(cpp/greet.hpp)：C++ 头文件 + 伴生 .vxh + .cpp 实现
  const ImportedLib &greet = imports->libs[2];
  CHECK(greet.name == "greet");
  CHECK(greet.from_cpp == true);
  CHECK(ends_with(greet.cpp_header, "headers/cpp/greet.hpp"));
  CHECK(ends_with(greet.decl_file, "headers/cpp/greet.vxh"));
  CHECK(ends_with(greet.impl_file, "headers/cpp/greet.cpp"));
  CHECK(greet.symbols.size() == 1);
  if (greet.symbols.size() == 1) {
    CHECK(greet.symbols[0].abi_symbol == "_vx_greet_greet");
  }

  // 前缀查找：库名 / 别名
  CHECK(imports->lib_by_prefix("math") != nullptr);
  CHECK(imports->lib_by_prefix("vec") != nullptr);
  CHECK(imports->lib_by_prefix("greet") != nullptr);
  CHECK(imports->lib_by_prefix("missing") == nullptr);
}

// 整套库（int/float/bool/string/void + 别名 + 子目录 + C++）装载断言
void test_full_lib_set() {
  static Arena arena;
  const std::string src =
      "@imp(math)\n"
      "@imp(collections/vec)\n"
      "@imp(text)\n"
      "@imp(fmath)\n"
      "@imp(math) as m\n"
      "@cppimp(cpp/greet.hpp)\n";
  DiagBag bag;
  auto toks = tokenize(src, "<test>", arena, bag);
  auto prog = parse(toks, "<test>", arena, bag);
  CHECK(prog.has_value());
  if (!prog) return;
  auto imports = load_imports(*prog, bag);
  CHECK(imports.has_value());
  if (!imports) return;
  // 别名引用去重：5 个独立库（math 只装载一次）
  CHECK(imports->libs.size() == 5);
  CHECK(imports->aliases.size() == 1);
  if (imports->aliases.size() == 1) {
    CHECK(imports->aliases[0].first == "m");
    CHECK(imports->aliases[0].second == "math");
  }
  // 别名前缀解析到 math 主条目
  const ImportedLib *m = imports->lib_by_prefix("m");
  CHECK(m != nullptr);
  if (m) CHECK(m->ref == "math" && m->name == "math");

  // text 库：string/bool/void 三类签名与 ABI 符号
  const ImportedLib *text = imports->lib_by_prefix("text");
  CHECK(text != nullptr);
  if (text && text->symbols.size() == 3) {
    CHECK(text->symbols[0].vx_name == "length");
    CHECK(text->symbols[0].abi_symbol == "_vx_text_length");
    CHECK(text->symbols[0].ret_type == "int");
    CHECK(text->symbols[0].params[0].type == "string");
    CHECK(text->symbols[1].vx_name == "is_empty");
    CHECK(text->symbols[1].ret_type == "bool");
    CHECK(text->symbols[2].vx_name == "shout");
    CHECK(text->symbols[2].ret_type.empty()); // void
  } else {
    CHECK(false);
  }

  // fmath 库：float 签名
  const ImportedLib *fm = imports->lib_by_prefix("fmath");
  CHECK(fm != nullptr);
  if (fm && fm->symbols.size() == 2) {
    CHECK(fm->symbols[0].ret_type == "float");
    CHECK(fm->symbols[0].params[0].type == "float");
    CHECK(fm->symbols[0].abi_symbol == "_vx_fmath_halve");
  } else {
    CHECK(false);
  }
}

void test_missing_lib_diagnostic() {
  static Arena arena;
  DiagBag bag;
  const std::string src = "@imp(nosuchlib)\nfunc main():\n    return\n";
  auto toks = tokenize(src, "<test>", arena, bag);
  auto prog = parse(toks, "<test>", arena, bag);
  CHECK(prog.has_value());
  if (!prog) return;
  auto imports = load_imports(*prog, bag);
  CHECK(!imports.has_value());
  CHECK(bag.has_error());
  if (bag.has_error()) {
    const auto &d = bag.items().back();
    CHECK(d.code == "E0002");
    CHECK(d.line == 1);
    CHECK(d.message.find("nosuchlib") != std::string::npos);
  }
}

// ==================== codegen：ABI 调用与 linkage ====================

void test_codegen_abi_call() {
  auto m = gen_import(
      "@imp(math)\n"
      "func main() -> int:\n"
      "    return math.add(2, 3)\n");
  CHECK(m.has_value());
  if (!m) return;
  // 导入库依赖写入 ModuleIR
  CHECK(m->libs.size() == 1);
  if (m->libs.size() == 1) {
    CHECK(m->libs[0].lib == "math");
    CHECK(m->libs[0].from_cpp == false);
    CHECK(ends_with(m->libs[0].impl_file, "headers/math.c"));
  }
  // linkage 记录导入符号（ABI 名 + 来源库）
  bool has_add = false;
  for (const ir::LinkEntry &e : m->linkage) {
    if (e.abi_symbol == "_vx_math_add") {
      has_add = true;
      CHECK(e.lib == "math");
      CHECK(e.name == "add");
      CHECK(e.params.size() == 2);
      CHECK(e.ret_type == "int");
    }
  }
  CHECK(has_add);
  // 调用发射为 ABI 符号名
  const auto &f = m->funcs[0];
  bool abi_call = false;
  for (const ir::Inst &inst : f.instructions) {
    if (inst.op == ir::OP_CALL && inst.args == "f;_vx_math_add") {
      abi_call = true;
    }
  }
  CHECK(abi_call);
}

void test_codegen_unknown_symbol() {
  static Arena arena;
  DiagBag bag;
  const std::string src =
      "@imp(math)\n"
      "func main():\n"
      "    math.nope(1)\n"
      "    return\n";
  auto toks = tokenize(src, "<test>", arena, bag);
  auto prog = parse(toks, "<test>", arena, bag);
  CHECK(prog.has_value());
  if (!prog) return;
  auto imports = load_imports(*prog, bag);
  CHECK(imports.has_value());
  if (!imports) return;
  auto mod = generate_module(*prog, "<test>", bag, &*imports);
  CHECK(!mod.has_value()); // 库未声明该符号 → codegen 失败
  CHECK(bag.has_error());
}

// ==================== emitter：外部符号声明 ====================

void test_emitter_declares_import_symbols() {
  auto m = gen_import(
      "@imp(math)\n"
      "@cppimp(cpp/greet.hpp)\n"
      "func main() -> int:\n"
      "    out(greet.greet(\"vx\"))\n"
      "    return math.add(1, 2)\n");
  CHECK(m.has_value());
  if (!m) return;
  auto r = backend::emit_llvm_ir(*m, "arm64-apple-macosx");
  CHECK(r.ok);
  if (!r.ok) {
    std::fprintf(stderr, "emit error: %s\n", r.error.c_str());
    return;
  }
  // 导入符号按 VX ABI 名声明，签名来自 .vxh
  CHECK(r.ir.find("declare i32 @_vx_math_add(i32, i32)") != std::string::npos);
  CHECK(r.ir.find("call i32 @_vx_math_add(") != std::string::npos);
  CHECK(r.ir.find("declare ptr @_vx_greet_greet(ptr)") != std::string::npos);
  CHECK(r.ir.find("call ptr @_vx_greet_greet(") != std::string::npos);
  // C++ 库依赖（供链接期生成 shim）被保留
  bool has_cpp = false;
  for (const ir::LibDep &d : m->libs) {
    if (d.from_cpp && ends_with(d.cpp_header, "headers/cpp/greet.hpp")) {
      has_cpp = true;
    }
  }
  CHECK(has_cpp);
}

} // namespace

int main() {
  test_abi();
  test_build_import_table();
  test_full_lib_set();
  test_missing_lib_diagnostic();
  test_codegen_abi_call();
  test_codegen_unknown_symbol();
  test_emitter_declares_import_symbols();

  if (g_failures == 0) {
    std::fprintf(stdout, "import tests: all passed\n");
    return 0;
  }
  std::fprintf(stderr, "import tests: %d failures\n", g_failures);
  return 1;
}
