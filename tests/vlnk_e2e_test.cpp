// vlnk_e2e_test — 后端端到端：.vx 源码 → .vxobj → reader → .ll → link → 运行。
// 断言运行 stdout 等于 "hello vx"。找不到 clang 时返回 77（CTest SKIP）。
#include "vx/backend/link_driver.hpp"
#include "vx/backend/llvm_emitter.hpp"
#include "vx/backend/vxobj_reader.hpp"
#include "vx/compiler/codegen.hpp"
#include "vx/compiler/diag.hpp"
#include "vx/compiler/lexer.hpp"
#include "vx/compiler/parser.hpp"
#include "vx/compiler/vxobj.hpp"
#include "vx/util/util.hpp"

#include <array>
#include <cstdio>
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
namespace backend = vx::backend;

// 执行可执行文件，返回 (退出码, stdout) ；退出码 -1 表示启动失败
// extra_args 为追加的命令行参数（用于验证 argv 能力）
std::pair<int, std::string> run_capture(const std::string &exe,
                                       const std::string &extra_args = {}) {
  std::string cmd = "\"" + exe + "\"";
  if (!extra_args.empty()) cmd += " " + extra_args;
  cmd += " 2>/dev/null";
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

// 全流程：源码 → ModuleIR → VXOBJ → reader → .ll → link → 运行，
// 断言 stdout 等于 expected。tag 用于临时文件与诊断输出。
// explicit_target 为 true 时走 link_executable 的显式 -target 路径并校验命令行。
void run_case(const char *src, const std::string &expected,
              const std::string &tag, bool explicit_target = false,
              const std::string &extra_args = {}) {
  const std::string exe = "/tmp/vx_vlnk_e2e_" + tag + "_" +
                          std::to_string(::getpid());
  static Arena arena;
  DiagBag bag;
  auto toks = tokenize(src, "<e2e>", arena, bag);
  auto prog = parse(toks, "<e2e>", arena, bag);
  CHECK(prog != std::nullopt);
  if (!prog) return;
  auto mod = generate_module(*prog, "<e2e>", bag);
  CHECK(mod.has_value());
  if (!mod) return;

  const std::string host = backend::host_triple();
  // 序列化 → 反序列化（同时验证 reader）
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

  auto lr = explicit_target ? backend::link_executable(em.ir, exe, host)
                            : backend::link_executable(em.ir, exe);
  CHECK(lr.ok);
  if (!lr.ok) {
    std::fprintf(stderr, "link error (%s): %s\n", tag.c_str(), lr.error.c_str());
    return;
  }
  if (explicit_target)
    CHECK(lr.command.find("-target " + host) != std::string::npos);

  auto [code, out] = run_capture(exe, extra_args);
  CHECK(code == 0);
  CHECK(out == expected);
  ::remove(exe.c_str());
}

} // namespace

int main() {
  if (backend::find_clang().empty()) {
    std::fprintf(stdout, "vlnk e2e: skipped (clang not found)\n");
    return 77;
  }

  run_case("extern out(msg: string)\n"
           "func main() -> int:\n"
           "    out(\"hello vx\")\n"
           "    return 0\n",
           "hello vx", "str");

  // F64 族端到端：1.5 + 2.25 * 2.0 - 0.5 / 2.0 = 5.75
  run_case("extern out_float(v: float)\n"
           "func main() -> int:\n"
           "    out_float(1.5 + 2.25 * 2.0 - 0.5 / 2.0)\n"
           "    return 0\n",
           "5.75", "f64");

  // 显式 target 路径：target == host → clang 收到 -target <host triple>
  run_case("extern out(msg: string)\n"
           "func main() -> int:\n"
           "    out(\"hello vx\")\n"
           "    return 0\n",
           "hello vx", "tgt", true);

  // 短路求值：右侧带副作用（输出），短路时不应执行
  run_case("extern out(msg: string)\n"
           "extern out_bool(v: bool)\n"
           "func mark(t: string) -> int:\n"
           "    out(t)\n"
           "    return 1\n"
           "func main() -> int:\n"
           "    out_bool(0 and mark(\"A\"))\n" // 左假短路：不输出 A → false
           "    out_bool(1 or mark(\"B\"))\n"  // 左真短路：不输出 B → true
           "    out(\"\\n\")\n"
           "    out_bool(1 and mark(\"C\"))\n" // 求值 C → Ctrue
           "    out(\"\\n\")\n"
           "    return 0\n",
           "falsetrue\nCtrue\n", "sc");

  // 字符串内容比较（地址比较会得到相反结果）
  run_case("extern out(msg: string)\n"
           "func main() -> int:\n"
           "    if \"ab\" == \"ab\":\n"
           "        out(\"eq\")\n"
           "    else:\n"
           "        out(\"ne\")\n"
           "    if \"a\" != \"b\":\n"
           "        out(\"+ne\")\n"
           "    return 0\n",
           "eq+ne", "streq");

  // 整数幂：2^10=1024，5^0=1
  run_case("extern out_int(v: int)\n"
           "func main() -> int:\n"
           "    out_int(2 ^ 10)\n"
           "    out_int(5 ^ 0)\n"
           "    return 0\n",
           "10241", "pow");

  // 变量类型传播：浮点变量参与 F64 运算（旧版误发 I32 → 后端诊断）
  run_case("extern out_float(v: float)\n"
           "func main() -> int:\n"
           "    var x = 1.5\n"
           "    out_float(x * 2.0)\n"
           "    var y = 0.0\n"
           "    y = y + 4.5\n"
           "    out_float(y)\n"
           "    return 0\n",
           "34.5", "fprop");

  // 系统能力：文件 I/O（写入 → 读取 → 存在性检查）
  {
    const std::string path =
        "/tmp/vx_vlnk_fileio_" + std::to_string(::getpid()) + ".txt";
    const std::string src =
        "extern out(msg: string)\n"
        "extern out_int(v: int)\n"
        "func main() -> int:\n"
        "    var n = file_write(\"" + path + "\", \"hello file\")\n"
        "    out_int(n)\n"
        "    out(\"|\")\n"
        "    var content = file_read(\"" + path + "\")\n"
        "    out(content)\n"
        "    out(\"|\")\n"
        "    if file_exists(\"" + path + "\"):\n"
        "        out(\"exists\")\n"
        "    out(\"|\")\n"
        "    out_int(file_exists(\"/tmp/vx_vlnk_absent_" +
        std::to_string(::getpid()) + ".txt\"))\n"
        "    return 0\n";
    run_case(src.c_str(), "10|hello file|exists|0", "fileio");
    ::remove(path.c_str());
  }

  // 系统能力：命令行参数（argv 数量与取值）
  run_case("extern out(msg: string)\n"
           "extern out_int(v: int)\n"
           "func main() -> int:\n"
           "    out_int(sys_argv_count())\n"
           "    out(\"|\")\n"
           "    out(sys_argv_get(1))\n"
           "    out(\"|\")\n"
           "    out(sys_argv_get(99))\n" // 越界 → 空串
           "    return 0\n",
           "2|hello-arg|", "argv", false, "hello-arg");

  if (g_failures == 0) {
    std::fprintf(stdout, "vlnk e2e: all passed\n");
    return 0;
  }
  std::fprintf(stderr, "vlnk e2e: %d failures\n", g_failures);
  return 1;
}
