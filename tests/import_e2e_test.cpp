// import_e2e_test — @imp / @cppimp 端到端：源码 → 导入表 → codegen → vxobj
// → reader → .ll → link_module（并入库实现 + 生成 C++ shim）→ 运行。
// 库文件位于 <源码根>/headers（经 CMake 的 VX_SOURCE_DIR 宏定位）。
// 找不到 clang 时返回 77（CTest SKIP）。
#include "vx/backend/link_driver.hpp"
#include "vx/backend/llvm_emitter.hpp"
#include "vx/backend/vxobj_reader.hpp"
#include "vx/compiler/codegen.hpp"
#include "vx/compiler/diag.hpp"
#include "vx/compiler/importer.hpp"
#include "vx/compiler/lexer.hpp"
#include "vx/compiler/parser.hpp"
#include "vx/compiler/vxobj.hpp"
#include "vx/util/util.hpp"

#include <array>
#include <cstdio>
#include <filesystem>
#include <string>
#include <string_view>
#include <utility>

#include <sys/wait.h>
#include <unistd.h>

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
namespace backend = vx::backend;

// 执行可执行文件，返回 (退出码, stdout)；退出码 -1 表示启动失败
std::pair<int, std::string> run_capture(const std::string &exe) {
  std::string cmd = "\"" + exe + "\" 2>/dev/null";
  FILE *p = ::popen(cmd.c_str(), "r");
  if (!p) return {-1, {}};
  std::string out;
  std::array<char, 256> buf{};
  std::size_t n = 0;
  while ((n = std::fread(buf.data(), 1, buf.size(), p)) > 0) {
    out.append(buf.data(), n);
  }
  const int rc = ::pclose(p);
  const int code = WIFEXITED(rc) ? WEXITSTATUS(rc) : -1;
  return {code, out};
}

// 全流程：源码（含 @imp/@cppimp）→ 运行，断言 stdout 等于 expected。
void run_case(const char *src, const std::string &expected,
              const std::string &tag) {
  const std::string exe = "/tmp/vx_import_e2e_" + tag + "_" +
                          std::to_string(::getpid());
  static Arena arena;
  DiagBag bag;
  auto toks = tokenize(src, "<e2e>", arena, bag);
  auto prog = parse(toks, "<e2e>", arena, bag);
  CHECK(prog != std::nullopt);
  if (!prog) return;

  const fs::path headers = fs::path(VX_SOURCE_DIR) / "headers";
  ImportContext ctx{headers, headers};
  auto imports = build_import_table(*prog, ctx, "<e2e>", bag);
  CHECK(imports.has_value());
  if (!imports) {
    std::fprintf(stderr, "import error (%s):\n%s", tag.c_str(),
                 render_all(bag, src, false).c_str());
    return;
  }

  auto mod = generate_module(*prog, "<e2e>", bag, &*imports);
  CHECK(mod.has_value());
  if (!mod) {
    std::fprintf(stderr, "codegen error (%s):\n%s", tag.c_str(),
                 render_all(bag, src, false).c_str());
    return;
  }

  const std::string host = backend::host_triple();
  const std::string obj = obj::serialize(*mod, host);
  auto rt = backend::read_vxobj(obj);
  CHECK(rt.ok);
  if (!rt.ok) return;

  auto em = backend::emit_llvm_ir(rt.module, host);
  CHECK(em.ok);
  if (!em.ok) {
    std::fprintf(stderr, "emit error (%s): %s\n", tag.c_str(), em.error.c_str());
    return;
  }

  // link_module：自动并入库实现文件，并在有 @cppimp 时生成 C++ shim
  auto lr = backend::link_module(em.ir, rt.module, exe);
  CHECK(lr.ok);
  if (!lr.ok) {
    std::fprintf(stderr, "link error (%s): %s\n", tag.c_str(), lr.error.c_str());
    return;
  }

  auto [code, out] = run_capture(exe);
  CHECK(code == 0);
  CHECK(out == expected);
  ::remove(exe.c_str());
}

} // namespace

int main() {
  if (backend::find_clang().empty()) {
    std::fprintf(stdout, "import e2e: skipped (clang not found)\n");
    return 77;
  }

  // @imp(math)：根目录库，A 库函数经 VX ABI 调用
  run_case("@imp(math)\n"
           "func main() -> int:\n"
           "    out_int(math.add(2, 3))\n"
           "    out(\"\\n\")\n"
           "    out_int(math.mul(4, 5))\n"
           "    out(\"\\n\")\n"
           "    return 0\n",
           "5\n20\n", "math");

  // @imp(collections/vec)：子目录库，库名取末段 vec
  run_case("@imp(collections/vec)\n"
           "func main() -> int:\n"
           "    out_int(vec.sum_to(10))\n"
           "    out(\"\\n\")\n"
           "    return 0\n",
           "55\n", "vec");

  // @cppimp：C++ 头文件 → 链接期生成 shim → VX ABI 调用
  run_case("@cppimp(cpp/greet.hpp)\n"
           "func main() -> int:\n"
           "    out(greet.greet(\"VX\"))\n"
           "    out(\"\\n\")\n"
           "    return 0\n",
           "Hello, VX!\n", "cpp");

  // 整套库：int/float/bool/string/void + 子目录 + 别名 + C++ 混合导入
  run_case("@imp(math)\n"
           "@imp(collections/vec)\n"
           "@imp(text)\n"
           "@imp(fmath)\n"
           "@imp(math) as m\n"
           "@cppimp(cpp/greet.hpp)\n"
           "func main() -> int:\n"
           "    out_int(math.add(2, 3))\n"
           "    out(\"\\n\")\n"
           "    out_int(vec.sum_to(10))\n"
           "    out(\"\\n\")\n"
           "    out_int(text.length(\"hello\"))\n"
           "    out(\"\\n\")\n"
           "    out_bool(text.is_empty(\"\"))\n"
           "    out(\"\\n\")\n"
           "    out_float(fmath.halve(9.0))\n"
           "    out(\"\\n\")\n"
           "    out_float(fmath.average(1.0, 4.5))\n"
           "    out(\"\\n\")\n"
           "    out_int(m.mul(6, 7))\n"
           "    out(\"\\n\")\n"
           "    text.shout(\"boom\")\n"
           "    out(greet.greet(\"VX\"))\n"
           "    out(\"\\n\")\n"
           "    return 0\n",
           "5\n55\n5\ntrue\n4.5\n2.75\n42\nboom\nHello, VX!\n", "alllibs");

  if (g_failures == 0) {
    std::fprintf(stdout, "import e2e: all passed\n");
    return 0;
  }
  std::fprintf(stderr, "import e2e: %d failures\n", g_failures);
  return 1;
}
