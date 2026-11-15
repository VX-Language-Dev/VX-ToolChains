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
#include "vx/compiler/semantics.hpp"
#include "vx/compiler/vxobj.hpp"
#include "vx/util/util.hpp"

#include <array>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>
#include <string_view>
#include <utility>

#include <sys/wait.h>
#include <unistd.h>

namespace {

int g_failures = 0;

#define CHECK(cond)                                                                                                    \
  do {                                                                                                                 \
    if (!(cond)) {                                                                                                     \
      std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                             \
      ++g_failures;                                                                                                    \
    }                                                                                                                  \
  } while (0)

using namespace vx::compiler;
using vx::util::Arena;
namespace fs = std::filesystem;
namespace backend = vx::backend;

// 执行可执行文件，返回 (退出码, stdout)；退出码 -1 表示启动失败
std::pair<int, std::string> run_capture(const std::string& exe) {
  std::string cmd = "\"" + exe + "\" 2>/dev/null";
  FILE* p = ::popen(cmd.c_str(), "r");
  if (!p)
    return {-1, {}};
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
void run_case(const std::string& src, const std::string& expected, const std::string& tag) {
  const std::string exe = "/tmp/vx_import_e2e_" + tag + "_" + std::to_string(::getpid());
  static Arena arena;
  DiagBag bag;
  auto toks = tokenize(src, "<e2e>", arena, bag);
  auto prog = parse(toks, "<e2e>", arena, bag);
  CHECK(prog != std::nullopt);
  if (!prog)
    return;

  const fs::path headers = fs::path(VX_SOURCE_DIR) / "headers";
  ImportContext ctx{headers, headers, {}, {}};
  auto imports = build_import_table(*prog, ctx, "<e2e>", bag);
  CHECK(imports.has_value());
  if (!imports) {
    std::fprintf(stderr, "import error (%s):\n%s", tag.c_str(), render_all(bag, src, false).c_str());
    return;
  }

  // 语义分析（与 vxc 驱动一致）：产出 method_symbols 供 codegen 发射限定方法名，
  // 多类同名方法（如 init / dispose）依赖此步消歧。
  Analysis analysis;
  if (!check_module(*prog, "<e2e>", bag, &*imports, &analysis)) {
    std::fprintf(stderr, "semantics error (%s):\n%s", tag.c_str(), render_all(bag, src, false).c_str());
    return;
  }

  auto mod = generate_module(*prog, "<e2e>", bag, &*imports, &analysis);
  CHECK(mod.has_value());
  if (!mod) {
    std::fprintf(stderr, "codegen error (%s):\n%s", tag.c_str(), render_all(bag, src, false).c_str());
    return;
  }

  const std::string host = backend::host_triple();
  const std::string obj = obj::serialize(*mod, host);
  auto rt = backend::read_vxobj(obj);
  CHECK(rt.ok);
  if (!rt.ok)
    return;

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
  if (out != expected) {
    std::fprintf(stderr, "FAIL case '%s': output mismatch (len %zu vs %zu)\n--- expected ---\n%s\n--- actual ---\n%s\n",
                 tag.c_str(), out.size(), expected.size(), expected.c_str(), out.c_str());
    ++g_failures;
  }
  ::remove(exe.c_str());
}

// 多编译单元：模块源写入 <dir>/pkg/mathx.vx，主源写入 <dir>/main.vx；
// 两单元分别 编译→VXOBJ→读回，再合并为一个模块链接运行（等价 vlnk 多输入）。
void run_case_module(const std::string& mod_src, const std::string& main_src, const std::string& expected,
                     const std::string& tag) {
  const fs::path dir = fs::path("/tmp") / ("vx_mod_e2e_src_" + tag + "_" + std::to_string(::getpid()));
  std::error_code ec;
  fs::remove_all(dir, ec);
  fs::create_directories(dir / "pkg", ec);
  {
    std::ofstream mf(dir / "pkg" / "mathx.vx");
    mf << mod_src;
    std::ofstream mn(dir / "main.vx");
    mn << main_src;
  }

  const std::string host = backend::host_triple();
  const std::string sources[2] = {mod_src, main_src};
  const std::string names[2] = {"pkg/mathx.vx", "main.vx"};
  const fs::path dirs[2] = {dir / "pkg", dir};
  const fs::path headers = fs::path(VX_SOURCE_DIR) / "headers";
  static Arena arena;
  std::string objs[2];
  backend::VxobjReadResult rts[2];

  for (int i = 0; i < 2; ++i) {
    DiagBag bag;
    auto toks = tokenize(sources[i], names[i], arena, bag);
    auto prog = parse(toks, names[i], arena, bag);
    CHECK(prog.has_value());
    if (!prog)
      return;
    ImportContext ctx{headers, dirs[i], {}, {}};
    auto imports = build_import_table(*prog, ctx, names[i], bag);
    CHECK(imports.has_value());
    if (!imports)
      return;
    Analysis analysis;
    if (!check_module(*prog, names[i], bag, &*imports, &analysis)) {
      std::fprintf(stderr, "semantics error (%s/%d):\n%s", tag.c_str(), i, render_all(bag, sources[i], false).c_str());
      ++g_failures;
      return;
    }
    auto mod = generate_module(*prog, names[i], bag, &*imports, &analysis);
    CHECK(mod.has_value());
    if (!mod)
      return;
    objs[i] = obj::serialize(*mod, host);
    rts[i] = backend::read_vxobj(objs[i]);
    CHECK(rts[i].ok);
    if (!rts[i].ok)
      return;
  }

  // 合并（与 vlnk 多输入规则一致：函数名/结构名/链接符号去重）
  ir::ModuleIR merged;
  std::set<std::string> func_names;
  std::set<std::string> struct_names;
  std::set<std::string> link_keys;
  for (backend::VxobjReadResult& rt : rts) {
    for (ir::FuncIR& f : rt.module.funcs) {
      if (func_names.insert(f.name).second)
        merged.funcs.push_back(f);
    }
    for (ir::StructLayout& sl : rt.module.structs) {
      if (struct_names.insert(sl.name).second)
        merged.structs.push_back(sl);
    }
    for (ir::LinkEntry& e : rt.module.linkage) {
      const std::string key = e.abi_symbol.empty() ? e.name : e.abi_symbol;
      if (link_keys.insert(key).second)
        merged.linkage.push_back(e);
    }
  }

  auto em = backend::emit_llvm_ir(merged, host);
  CHECK(em.ok);
  if (!em.ok) {
    std::fprintf(stderr, "emit error (%s): %s\n", tag.c_str(), em.error.c_str());
    return;
  }
  const std::string exe = "/tmp/vx_mod_e2e_" + tag + "_" + std::to_string(::getpid());
  auto lr = backend::link_module(em.ir, merged, exe);
  CHECK(lr.ok);
  if (!lr.ok) {
    std::fprintf(stderr, "link error (%s): %s\n", tag.c_str(), lr.error.c_str());
    return;
  }
  auto [code, out] = run_capture(exe);
  CHECK(code == 0);
  if (out != expected) {
    std::fprintf(stderr, "FAIL case '%s': output mismatch (len %zu vs %zu)\n--- expected ---\n%s\n--- actual ---\n%s\n",
                 tag.c_str(), out.size(), expected.size(), expected.c_str(), out.c_str());
    ++g_failures;
  }
  ::remove(exe.c_str());
  fs::remove_all(dir, ec);
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

  // @imp(sys)：系统能力库，shell 命令执行经 VX ABI 调用（headers/sys.c）
  run_case("@imp(sys)\n"
           "func main() -> int:\n"
           "    out_int(sys.system(\"true\"))\n"
           "    out(\"\\n\")\n"
           "    out_bool(sys.system(\"false\") != 0)\n"
           "    out(\"\\n\")\n"
           "    return 0\n",
           "0\ntrue\n", "sys");

  // @imp(proc)：进程原语库（headers/proc.vxh 无 C 实现，emitter 直连 libc）
  // fork 子进程输出后退出，父进程 waitpid 回收（nil 与整型 0 两种 NULL 形参），
  // 并覆盖 getpid/getppid/sleep/kill(sig=0 存活探测)
  run_case("@imp(proc)\n"
           "func main() -> int:\n"
           "    var pid = proc.fork()\n"
           "    if pid == 0:\n"
           "        out(\"child\\n\")\n"
           "        exit(0)\n"
           "    var got = proc.waitpid(pid, nil, 0)\n"
           "    out_bool(got == pid)\n"
           "    out(\"\\n\")\n"
           "    out_bool(proc.waitpid(proc.getpid(), 0, 1) == -1)\n"
           "    out(\"\\n\")\n"
           "    out_int(proc.sleep(0))\n"
           "    out(\"\\n\")\n"
           "    out_bool(proc.kill(proc.getpid(), 0) == 0)\n"
           "    out(\"\\n\")\n"
           "    out_bool(proc.getppid() > 0)\n"
           "    out(\"\\n\")\n"
           "    return 0\n",
           "child\ntrue\ntrue\n0\ntrue\ntrue\n", "proc");

  // @imp(proc) 线程与任务池：任务以 &fn 传入（函数值），参数 0~4 个 int，
  // join_int 取回结果；1 个工作线程时队列 FIFO 顺序确定，可稳定断言
  run_case("@imp(proc)\n"
           "func task(x: int) -> int:\n"
           "    return x * 2\n"
           "func show(x: int) -> int:\n"
           "    out_int(x)\n"
           "    out(\"\\n\")\n"
           "    return x\n"
           "func main() -> int:\n"
           "    var h = proc.spawn1(&task, 21)\n"
           "    out_int(proc.join_int(h))\n"
           "    out(\"\\n\")\n"
           "    var n = proc.pool_start(1)\n"
           "    out_int(n)\n"
           "    out(\"\\n\")\n"
           "    proc.pool_submit1(&show, 1)\n"
           "    proc.pool_submit1(&show, 2)\n"
           "    proc.pool_submit1(&show, 3)\n"
           "    proc.pool_wait()\n"
           "    proc.pool_stop()\n"
           "    return 0\n",
           "42\n1\n1\n2\n3\n", "threadpool");

  // 管道数据回读 + 指针解引用 + 多参数任务 + 输出捕获（零 C 实现）
  run_case("@imp(proc)\n"
           "@imp(ptr)\n"
           "func mul(a: int, b: int) -> int:\n"
           "    return a * b\n"
           "func main() -> int:\n"
           "    var h = proc.spawn2(&mul, 6, 7)\n"
           "    out_int(proc.join_int(h))\n"
           "    out(\"\\n\")\n"
           "    var p = malloc(8)\n"
           "    *p = 11\n"
           "    out_int(*p)\n"
           "    out(\"\\n\")\n"
           "    var fds = proc.pipe()\n"
           "    var rfd = *fds\n"
           "    var wfd = *(ptr.add(fds, 4))\n"
           "    proc.write_str(wfd, \"pipe-ok\")\n"
           "    proc.close(wfd)\n"
           "    out(proc.read_str(rfd, 32))\n"
           "    out(\"\\n\")\n"
           "    proc.close(rfd)\n"
           "    out(proc.capture(\"echo cap\"))\n"
           "    return 0\n",
           "42\n11\npipe-ok\ncap\n", "pipeptr");

  // 同步原语（mutex / cond / usleep）：proc.vxh 只有声明，链接期直连 pthread。
  // 顺序执行即可验证符号解析与基本语义：加锁后 trylock 必失败，解锁后成功。
  run_case("@imp(proc)\n"
           "func main() -> int:\n"
           "    var mtx = malloc(64)\n"
           "    out_int(proc.mutex_init(mtx, nil))\n"
           "    out(\"\\n\")\n"
           "    out_int(proc.mutex_lock(mtx))\n"
           "    out(\"\\n\")\n"
           "    out_bool(proc.mutex_trylock(mtx) != 0)\n"
           "    out(\"\\n\")\n"
           "    out_int(proc.mutex_unlock(mtx))\n"
           "    out(\"\\n\")\n"
           "    out_int(proc.mutex_trylock(mtx))\n"
           "    out(\"\\n\")\n"
           "    out_int(proc.mutex_unlock(mtx))\n"
           "    out(\"\\n\")\n"
           "    out_int(proc.mutex_destroy(mtx))\n"
           "    out(\"\\n\")\n"
           "    free(mtx)\n"
           "    var cv = malloc(64)\n"
           "    out_int(proc.cond_init(cv, nil))\n"
           "    out(\"\\n\")\n"
           "    out_int(proc.cond_signal(cv))\n"
           "    out(\"\\n\")\n"
           "    out_int(proc.cond_broadcast(cv))\n"
           "    out(\"\\n\")\n"
           "    out_int(proc.cond_destroy(cv))\n"
           "    out(\"\\n\")\n"
           "    free(cv)\n"
           "    out_int(proc.usleep(1000))\n"
           "    out(\"\\n\")\n"
           "    return 0\n",
           "0\n0\ntrue\n0\n0\n0\n0\n0\n0\n0\n0\n0\n", "sync");

  // 多线程互斥：任务实参按 i64 透传，64 位指针可安全跨线程传递。
  // 两个线程各做 500 次「加锁 → 自增 → 解锁」，共享缓冲区前 64 字节为
  // pthread_mutex_t、偏移 64 处为计数器，最终必须为 1000（无丢失更新）。
  run_case("@imp(proc)\n"
           "@imp(ptr)\n"
           "func bump(shared: ptr) -> int:\n"
           "    var i = 0\n"
           "    var c = ptr.add(shared, 64)\n"
           "    while i < 500:\n"
           "        proc.mutex_lock(shared)\n"
           "        *c = *c + 1\n"
           "        proc.mutex_unlock(shared)\n"
           "        i = i + 1\n"
           "    return 0\n"
           "func main() -> int:\n"
           "    var shared = malloc(72)\n"
           "    proc.mutex_init(shared, nil)\n"
           "    var c = ptr.add(shared, 64)\n"
           "    *c = 0\n"
           "    var h1 = proc.spawn1(&bump, shared)\n"
           "    var h2 = proc.spawn1(&bump, shared)\n"
           "    proc.join_int(h1)\n"
           "    proc.join_int(h2)\n"
           "    out_int(*c)\n"
           "    out(\"\\n\")\n"
           "    out_int(proc.mutex_destroy(shared))\n"
           "    out(\"\\n\")\n"
           "    free(shared)\n"
           "    return 0\n",
           "1000\n0\n", "mutexptr");

  // 原子操作：两个线程各 1000 次无锁自增（atomic_fetch_add），最终必须为 2000；
  // 随后验证 atomic_cas 的返回语义（返回旧值，等于 expected 表示成功）
  run_case("@imp(proc)\n"
           "func inc(addr: ptr) -> int:\n"
           "    var i = 0\n"
           "    while i < 1000:\n"
           "        proc.atomic_fetch_add(addr, 1)\n"
           "        i = i + 1\n"
           "    return 0\n"
           "func main() -> int:\n"
           "    var counter = malloc(4)\n"
           "    proc.atomic_store(counter, 0)\n"
           "    var h1 = proc.spawn1(&inc, counter)\n"
           "    var h2 = proc.spawn1(&inc, counter)\n"
           "    proc.join_int(h1)\n"
           "    proc.join_int(h2)\n"
           "    out_int(proc.atomic_load(counter))\n"
           "    out(\"\\n\")\n"
           "    out_int(proc.atomic_cas(counter, 2000, 9999))\n"
           "    out(\"\\n\")\n"
           "    out_int(proc.atomic_load(counter))\n"
           "    out(\"\\n\")\n"
           "    proc.atomic_fetch_sub(counter, 9)\n"
           "    out_int(proc.atomic_load(counter))\n"
           "    out(\"\\n\")\n"
           "    proc.atomic_fence()\n"
           "    free(counter)\n"
           "    return 0\n",
           "2000\n2000\n9999\n9990\n", "atomic");

  // 跨线程等待-唤醒（cond_wait / cond_signal）：工作线程阻塞在条件变量上，
  // 主线程置标志并唤醒；工作线程完成后反向唤醒主线程。两种调度顺序均安全
  // （等待方都在持锁下循环复查标志，无虚假唤醒/丢失唤醒问题）。
  run_case("@imp(proc)\n"
           "@imp(ptr)\n"
           "func waiter(shared: ptr) -> int:\n"
           "    var cv = ptr.add(shared, 64)\n"
           "    var flag = ptr.add(shared, 128)\n"
           "    proc.mutex_lock(shared)\n"
           "    while *flag == 0:\n"
           "        proc.cond_wait(cv, shared)\n"
           "    *flag = 2\n"
           "    proc.cond_signal(cv)\n"
           "    proc.mutex_unlock(shared)\n"
           "    return 0\n"
           "func main() -> int:\n"
           "    var shared = malloc(132)\n"
           "    var cv = ptr.add(shared, 64)\n"
           "    var flag = ptr.add(shared, 128)\n"
           "    proc.mutex_init(shared, nil)\n"
           "    proc.cond_init(cv, nil)\n"
           "    *flag = 0\n"
           "    var h = proc.spawn1(&waiter, shared)\n"
           "    proc.mutex_lock(shared)\n"
           "    *flag = 1\n"
           "    proc.cond_signal(cv)\n"
           "    while *flag != 2:\n"
           "        proc.cond_wait(cv, shared)\n"
           "    proc.mutex_unlock(shared)\n"
           "    proc.join_int(h)\n"
           "    out_int(*flag)\n"
           "    out(\"\\n\")\n"
           "    proc.mutex_destroy(shared)\n"
           "    proc.cond_destroy(cv)\n"
           "    free(shared)\n"
           "    return 0\n",
           "2\n", "condwait");

  // std/sys/sync.vx 已可被当前工具链编译执行：读取库源码，拼接使用各
  // 同步原语的 main，走完整 编译→链接→运行 流程并断言输出。
  {
    const fs::path sync_path = fs::path(VX_SOURCE_DIR) / "std" / "sys" / "sync.vx";
    auto content = vx::util::read_file(sync_path.string());
    CHECK(content.has_value());
    if (content) {
      const std::string src = *content + R"VX(
func main() -> int:
    var m = new Mutex()
    m.init()
    m.lock()
    out_bool(m.try_lock())
    out("\n")
    m.unlock()
    out_bool(m.try_lock())
    out("\n")
    m.unlock()
    m.dispose()

    var s = new Semaphore()
    s.init(2)
    s.acquire()
    s.acquire()
    out_bool(s.try_acquire())
    out("\n")
    out_int(s.available())
    out("\n")
    s.release()
    out_bool(s.try_acquire())
    out("\n")
    s.dispose()

    var ch = new Channel()
    ch.init(4)
    ch.send(10)
    ch.send(20)
    ch.send(30)
    out_int(ch.recv())
    out("\n")
    out_int(ch.recv())
    out("\n")
    out_int(ch.recv())
    out("\n")
    out_bool(ch.try_recv() == -1)
    out("\n")
    ch.close()
    out_int(ch.recv())
    out("\n")
    out_bool(ch.is_closed())
    out("\n")
    ch.dispose()

    var b = new Barrier()
    b.init(1)
    b.wait()
    out("barrier\n")
    b.dispose()
    return 0
)VX";
      run_case(src, "false\ntrue\nfalse\n0\ntrue\n10\n20\n30\ntrue\n-1\ntrue\nbarrier\n", "synclib");
    }
  }

  // 类实例经 ptr 形参传入线程任务：生产者线程通过方法接收者（ptr）读写
  // 实例字段，join 后主线程读取——验证任务实参 64 位透传 + 接收者无地址截断
  run_case("@imp(proc)\n"
           "class Pipe:\n"
           "    data: ptr\n"
           "    func init():\n"
           "        self.data = malloc(4)\n"
           "        *self.data = 0\n"
           "func produce(p: ptr) -> int:\n"
           "    var i = 0\n"
           "    while i < 10:\n"
           "        *p.data = *p.data + (i + 1)\n"
           "        i = i + 1\n"
           "    return 0\n"
           "func main() -> int:\n"
           "    var p = new Pipe()\n"
           "    p.init()\n"
           "    var h = proc.spawn1(&produce, p)\n"
           "    proc.join_int(h)\n"
           "    out_int(*p.data)\n"
           "    out(\"\\n\")\n"
           "    p.data = nil\n"
           "    return 0\n",
           "55\n", "ptrclass");

  // Channel 跨线程生产者-消费者：拼接 std/sys/sync.vx，生产者线程发 0..9
  // （容量 4 < 10，send 阻塞），主线程 recv 阻塞取数求和；close 广播唤醒后
  // recv 返回 -1 哨兵结束。类实例经 ptr 形参传入任务，跨线程调用类方法。
  {
    const fs::path sync_path = fs::path(VX_SOURCE_DIR) / "std" / "sys" / "sync.vx";
    auto content = vx::util::read_file(sync_path.string());
    CHECK(content.has_value());
    if (content) {
      const std::string src = *content + R"VX(
func producer(ch: ptr) -> int:
    var i = 0
    while i < 10:
        ch.send(i)
        i = i + 1
    ch.close()
    return 0

func main() -> int:
    var ch = new Channel()
    ch.init(4)
    var h = proc.spawn1(&producer, ch)
    var total = 0
    var v = ch.recv()
    while v != -1:
        total = total + v
        v = ch.recv()
    out_int(total)
    out("\n")
    out_bool(ch.is_closed())
    out("\n")
    proc.join_int(h)
    ch.dispose()
    return 0
)VX";
      run_case(src, "45\ntrue\n", "channelipc");
    }
  }

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

  // text 库扩展：查找 / 前缀后缀 / 截取 / trim / 数值转换 / 行切分
  run_case("@imp(text)\n"
           "func main() -> int:\n"
           "    out_int(text.find(\"hello world\", \"world\"))\n"
           "    out(\"\\n\")\n"
           "    out_int(text.find(\"abc\", \"z\"))\n"
           "    out(\"\\n\")\n"
           "    out_bool(text.contains(\"abc\", \"bc\"))\n"
           "    out(\"\\n\")\n"
           "    out_bool(text.starts_with(\"hello\", \"he\"))\n"
           "    out(\"\\n\")\n"
           "    out_bool(text.ends_with(\"hello\", \"lo\"))\n"
           "    out(\"\\n\")\n"
           "    out(text.substr(\"abcdef\", 2, 3))\n"
           "    out(\"\\n\")\n"
           "    out(text.substr(\"abc\", 5, 9))\n"
           "    out(\"\\n\")\n"
           "    out(text.trim(\"  hi \\t\"))\n"
           "    out(\"\\n\")\n"
           "    out_int(text.char_at(\"abc\", 1))\n"
           "    out(\"\\n\")\n"
           "    out_int(text.char_at(\"abc\", 9))\n"
           "    out(\"\\n\")\n"
           "    out(text.from_char(65))\n"
           "    out(\"\\n\")\n"
           "    out_bool(text.is_int(\"-42\"))\n"
           "    out(\"\\n\")\n"
           "    out_bool(text.is_int(\"12a\"))\n"
           "    out(\"\\n\")\n"
           "    out_int(text.parse_int(\" -42\"))\n"
           "    out(\"\\n\")\n"
           "    out(text.format_int(123))\n"
           "    out(\"\\n\")\n"
           "    out_int(text.line_count(\"a\\nb\\nc\"))\n"
           "    out(\"\\n\")\n"
           "    out(text.line_at(\"a\\nb\\n\", 0))\n"
           "    out(\"|\")\n"
           "    out(text.line_at(\"a\\nb\\n\", 1))\n"
           "    out(\"|\")\n"
           "    return 0\n",
           "6\n-1\ntrue\ntrue\ntrue\ncde\n\nhi\n98\n-1\nA\ntrue\nfalse\n-42\n123\n3\na|b|", "textapi");

  // fs 库：路径运算 + 目录创建/列举/删除（目录名带 pid，避免并发冲突）
  {
    const std::string dir = "/tmp/vx_fs_e2e_" + std::to_string(::getpid());
    const std::string src = "@imp(fs)\n"
                            "@imp(text)\n"
                            "func main() -> int:\n"
                            "    var p: string = fs.path_join(\"/tmp\", \"vx_fs_e2e_" +
                            std::to_string(::getpid()) +
                            "\")\n"
                            "    out(fs.path_dir(p))\n"
                            "    out(\"|\")\n"
                            "    out(fs.path_base(p))\n"
                            "    out(\"\\n\")\n"
                            "    out(fs.path_dir(\"a.txt\"))\n"
                            "    out(\"|\")\n"
                            "    out(fs.path_base(\"a.txt\"))\n"
                            "    out(\"\\n\")\n"
                            "    out_int(fs.mkdir_p(p))\n"
                            "    out(\"|\")\n"
                            "    out_bool(fs.dir_exists(p))\n"
                            "    out(\"\\n\")\n"
                            "    var f: string = fs.path_join(p, \"a.txt\")\n"
                            "    file_write(f, \"abc\")\n"
                            "    out_int(fs.file_size(f))\n"
                            "    out(\"|\")\n"
                            "    out_bool(fs.is_file(f))\n"
                            "    out(\"\\n\")\n"
                            "    var listed: string = fs.list_dir(p)\n"
                            "    out(listed)\n"
                            "    out_int(text.line_count(listed))\n"
                            "    out(\"\\n\")\n"
                            "    out_int(fs.remove_file(f))\n"
                            "    out(\"|\")\n"
                            "    out_bool(fs.is_file(f))\n"
                            "    out(\"\\n\")\n"
                            "    return 0\n";
    std::string base = "vx_fs_e2e_" + std::to_string(::getpid());
    run_case(src, "/tmp|" + base + "\n.|a.txt\n0|true\n3|true\na.txt\n1\n0|false\n", "fsapi");
    std::error_code ec;
    fs::remove_all(dir, ec);
  }

  // 跨编译单元模块：import pkg.mathx 获取 枚举/类/函数 声明，
  // 主单元发射调用与构造，模块单元提供实现，合并后链接运行
  run_case_module("enum Level:\n"
                  "    Low\n"
                  "    High = 5\n"
                  "class Counter:\n"
                  "    n: int\n"
                  "    func new(start: int):\n"
                  "        self.n = start\n"
                  "    func bump() -> int:\n"
                  "        self.n = self.n + 1\n"
                  "        return self.n\n"
                  "func add(a: int, b: int) -> int:\n"
                  "    return a + b\n",
                  "import pkg.mathx\n"
                  "func main() -> int:\n"
                  "    out_int(mathx.add(1, 2))\n"
                  "    out(\"\\n\")\n"
                  "    var c: Counter = mathx.Counter(10)\n"
                  "    out_int(c.bump())\n"
                  "    out(\"\\n\")\n"
                  "    out_int(c.n)\n"
                  "    out(\"\\n\")\n"
                  "    out_int(Level.High)\n"
                  "    out(\"\\n\")\n"
                  "    return 0\n",
                  "3\n11\n11\n5\n", "module");

  if (g_failures == 0) {
    std::fprintf(stdout, "import e2e: all passed\n");
    return 0;
  }
  std::fprintf(stderr, "import e2e: %d failures\n", g_failures);
  return 1;
}
