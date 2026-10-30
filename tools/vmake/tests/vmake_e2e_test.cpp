// vmake_e2e_test — VMake 端到端：VMakeSet.vmake → vxc × N → vlnk → 产物运行。
// 覆盖：dylib 先于 exe 的构建顺序、多源文件合并、Cargo 风格输出、增量 Fresh、
// 编译失败诊断。找不到 clang 时返回 77（CTest SKIP）。
#include "vx/backend/link_driver.hpp"

#include <cstdio>
#include <cstdlib>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>

#ifndef VMAKE_BIN_PATH
#error "VMAKE_BIN_PATH 未定义（由 CMake 注入）"
#endif
#ifndef VXC_BIN_PATH
#error "VXC_BIN_PATH 未定义（由 CMake 注入）"
#endif
#ifndef VLNK_BIN_PATH
#error "VLNK_BIN_PATH 未定义（由 CMake 注入）"
#endif

namespace {

namespace fs = std::filesystem;

int g_failures = 0;

#define CHECK(cond)                                                            \
  do {                                                                         \
    if (!(cond)) {                                                             \
      std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);     \
      ++g_failures;                                                            \
    }                                                                          \
  } while (0)

struct RunResult {
  int code = -1;
  std::string output;
};

RunResult
run_in_dir(const std::string &program, const std::vector<std::string> &args,
           const fs::path &cwd,
           const std::vector<std::pair<std::string, std::string>> &env = {}) {
  RunResult r;
  std::string tmpl =
      (fs::temp_directory_path() / "vmake_e2e_log_XXXXXX").string();
  std::vector<char> tmpl_buf(tmpl.begin(), tmpl.end());
  tmpl_buf.push_back('\0');
  const int fd = ::mkstemp(tmpl_buf.data());
  if (fd < 0)
    return r;
  const std::string log_path(tmpl_buf.data());

  std::vector<std::string> cmd{program};
  cmd.insert(cmd.end(), args.begin(), args.end());
  std::vector<char *> argv;
  argv.reserve(cmd.size() + 1);
  for (std::string &s : cmd)
    argv.push_back(s.data());
  argv.push_back(nullptr);

  const pid_t pid = ::fork();
  if (pid < 0) {
    ::close(fd);
    ::unlink(log_path.c_str());
    return r;
  }
  if (pid == 0) {
    if (::chdir(cwd.c_str()) != 0)
      ::_exit(126);
    ::dup2(fd, STDOUT_FILENO);
    ::dup2(fd, STDERR_FILENO);
    ::close(fd);
    for (const auto &[key, value] : env)
      ::setenv(key.c_str(), value.c_str(), 1);
    ::execv(cmd[0].c_str(), argv.data());
    ::_exit(127);
  }

  ::close(fd);
  int status = 0;
  while (::waitpid(pid, &status, 0) < 0) {
  }
  {
    std::ifstream in(log_path, std::ios::binary);
    if (in) {
      r.output.assign((std::istreambuf_iterator<char>(in)),
                      std::istreambuf_iterator<char>());
    }
  }
  ::unlink(log_path.c_str());
  r.code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
  return r;
}

bool write_text(const fs::path &path, std::string_view text) {
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  out.write(text.data(), static_cast<std::streamsize>(text.size()));
  return out.good();
}

bool contains(const std::string &text, std::string_view needle) {
  return text.find(needle) != std::string::npos;
}

// 仅更新 mtime（内容不变）：用于验证内容哈希增量不会误判重编
void bump_mtime(const fs::path &p) {
  std::error_code ec;
  const auto t = fs::last_write_time(p, ec);
  if (!ec)
    fs::last_write_time(p, t + std::chrono::seconds(2), ec);
}

std::vector<std::pair<std::string, std::string>> tool_env() {
  return {{"VX_VXC", VXC_BIN_PATH}, {"VX_VLNK", VLNK_BIN_PATH}};
}

} // namespace

int main() {
  if (vx::backend::find_clang().empty()) {
    std::fprintf(stdout, "vmake e2e: skipped (clang not found)\n");
    return 77;
  }

  const fs::path root =
      fs::temp_directory_path() / ("vmake_e2e_" + std::to_string(::getpid()));
  std::error_code ec;
  fs::remove_all(root, ec);
  fs::create_directories(root, ec);
  if (ec) {
    std::fprintf(stderr, "vmake e2e: cannot create temp dir\n");
    return 1;
  }

  // exe 声明在前，dylib 在后：验证 VMake 将动态库排到前面
  const std::string config = "exe(\"app\", sources=[\"main.vx\", \"util.vx\"])\n"
                             "\n"
                             "dylib(\"calc\", sources=[\"calc.vx\"])\n";
  CHECK(write_text(root / "VMakeSet.vmake", config));
  CHECK(write_text(root / "main.vx", "extern out_int(v: int)\n"
                                     "func main() -> int:\n"
                                     "    out_int(add(19, 23))\n"
                                     "    return 0\n"));
  CHECK(write_text(root / "util.vx", "func add(a: int, b: int) -> int:\n"
                                     "    return a + b\n"));
  CHECK(write_text(root / "calc.vx", "func triple(x: int) -> int:\n"
                                     "    return x * 3\n"));

  const std::string vmake = VMAKE_BIN_PATH;

  // ---- 首次构建（-j2 同时覆盖线程池路径） ----
  RunResult first =
      run_in_dir(vmake, {"build", "-B", "bdir", "-j2"}, root, tool_env());
  CHECK(first.code == 0);
  if (first.code != 0)
    std::fprintf(stderr, "%s\n", first.output.c_str());
  CHECK(contains(first.output, "vxc"));
  CHECK(contains(first.output, "vlnk"));
  CHECK(contains(first.output, "vmake-builder: build"));
  const std::size_t pos_calc = first.output.find("calc");
  const std::size_t pos_app = first.output.find("app");
  CHECK(pos_calc != std::string::npos && pos_app != std::string::npos &&
        pos_calc < pos_app);

  const fs::path bdir = root / "bdir";
  CHECK(fs::is_regular_file(bdir / "app", ec) && !ec);
#if defined(__APPLE__)
  CHECK(fs::is_regular_file(bdir / "libcalc.dylib", ec) && !ec);
#elif defined(_WIN32)
  CHECK(fs::is_regular_file(bdir / "calc.dll", ec) && !ec);
#else
  CHECK(fs::is_regular_file(bdir / "libcalc.so", ec) && !ec);
#endif

  if (fs::is_regular_file(bdir / "app", ec) && !ec) {
    RunResult run = run_in_dir((bdir / "app").string(), {}, root);
    CHECK(run.code == 0);
    CHECK(run.output == "42");
  }

  // ---- 第二次构建：全部 Fresh，不应再出现 Compiling ----
  RunResult second =
      run_in_dir(vmake, {"build", "-B", "bdir"}, root, tool_env());
  CHECK(second.code == 0);
  CHECK(!contains(second.output, "vxc"));
  CHECK(contains(second.output, "vmake-builder: build"));

  // ---- 仅更新 mtime（内容未变）：内容哈希命中，不应重编也不应重链 ----
  bump_mtime(root / "util.vx");
  RunResult touch_only =
      run_in_dir(vmake, {"build", "-B", "bdir"}, root, tool_env());
  CHECK(touch_only.code == 0);
  CHECK(!contains(touch_only.output, "vxc"));
  CHECK(!contains(touch_only.output, "vlnk"));

  // ---- 触达一个 exe 源文件：只重编 app，calc 保持 Fresh ----
  CHECK(write_text(root / "util.vx", "func add(a: int, b: int) -> int:\n"
                                     "    return a + b\n"
                                     "# touched\n"));
  RunResult third =
      run_in_dir(vmake, {"build", "-B", "bdir"}, root, tool_env());
  CHECK(third.code == 0);
  CHECK(contains(third.output, "vxc"));
  CHECK(!contains(third.output, "libcalc"));

  // ---- 导入头/实现变更：headers 依赖纳入增量签名，必须触发重编+重链 ----
  const fs::path hdr = root / "hdrproj";
  fs::create_directories(hdr / "headers", ec);
  CHECK(write_text(hdr / "VMakeSet.vmake",
                   "exe(\"hi\", sources=[\"main.vx\"])\n"));
  CHECK(write_text(hdr / "main.vx",
                   "@imp(math)\n"
                   "extern out_int(v: int)\n"
                   "func main() -> int:\n"
                   "    out_int(math.add(3, 4))\n"
                   "    return 0\n"));
  CHECK(write_text(hdr / "headers" / "math.vxh",
                   "extern func add(a: int, b: int) -> int\n"));
  CHECK(write_text(hdr / "headers" / "math.c",
                   "#include <stdint.h>\n"
                   "int32_t _vx_math_add(int32_t a, int32_t b) { return a + b; }\n"));
  RunResult hdr_first =
      run_in_dir(vmake, {"build", "-B", "bdir"}, hdr, tool_env());
  CHECK(hdr_first.code == 0);
  if (hdr_first.code != 0)
    std::fprintf(stderr, "%s\n", hdr_first.output.c_str());
  RunResult hdr_second =
      run_in_dir(vmake, {"build", "-B", "bdir"}, hdr, tool_env());
  CHECK(hdr_second.code == 0);
  CHECK(!contains(hdr_second.output, "vxc"));

  CHECK(write_text(hdr / "headers" / "math.c",
                   "#include <stdint.h>\n"
                   "/* changed */\n"
                   "int32_t _vx_math_add(int32_t a, int32_t b) { return a + b; }\n"));
  RunResult hdr_third =
      run_in_dir(vmake, {"build", "-B", "bdir"}, hdr, tool_env());
  CHECK(hdr_third.code == 0);
  CHECK(contains(hdr_third.output, "vxc"));
  CHECK(contains(hdr_third.output, "vlnk"));
  if (fs::is_regular_file(hdr / "bdir" / "hi", ec) && !ec) {
    RunResult run = run_in_dir((hdr / "bdir" / "hi").string(), {}, hdr);
    CHECK(run.code == 0);
    CHECK(run.output == "7");
  }

  // ---- -I 搜索目录：vmake -I 透传给 vxc；同参数二次构建 Fresh ----
  const fs::path incproj = root / "incproj";
  fs::create_directories(incproj / "mylibs", ec);
  CHECK(write_text(incproj / "VMakeSet.vmake", "exe(\"hi\", sources=[\"main.vx\"])\n"));
  CHECK(write_text(incproj / "main.vx",
                   "@imp(triplib)\n"
                   "extern out_int(v: int)\n"
                   "func main() -> int:\n"
                   "    out_int(triplib.triple(5))\n"
                   "    return 0\n"));
  CHECK(write_text(incproj / "mylibs" / "triplib.vxh",
                   "extern func triple(x: int) -> int\n"));
  CHECK(write_text(incproj / "mylibs" / "triplib.c",
                   "#include <stdint.h>\n"
                   "int32_t _vx_triplib_triple(int32_t x) { return x * 3; }\n"));
  RunResult inc_first = run_in_dir(vmake, {"build", "-B", "bdir", "-I", "mylibs"},
                                   incproj, tool_env());
  CHECK(inc_first.code == 0);
  if (inc_first.code != 0)
    std::fprintf(stderr, "%s\n", inc_first.output.c_str());
  if (fs::is_regular_file(incproj / "bdir" / "hi", ec) && !ec) {
    RunResult run = run_in_dir((incproj / "bdir" / "hi").string(), {}, incproj);
    CHECK(run.code == 0);
    CHECK(run.output == "15");
  }
  RunResult inc_second = run_in_dir(vmake, {"build", "-B", "bdir", "-I", "mylibs"},
                                    incproj, tool_env());
  CHECK(inc_second.code == 0);
  CHECK(!contains(inc_second.output, "vxc"));
  // 不带 -I：库不可见 → 编译失败
  RunResult inc_none =
      run_in_dir(vmake, {"build", "-B", "bdir2"}, incproj, tool_env());
  CHECK(inc_none.code != 0);
  CHECK(contains(inc_none.output, "triplib"));

  // ---- deps + dylib 链接：exe 依赖 dylib 目标，链接期 --lib 并入 ----
  const fs::path depproj = root / "depproj";
  fs::create_directories(depproj, ec);
  CHECK(write_text(depproj / "VMakeSet.vmake",
                   "exe(\"app\", sources=[\"main.vx\"], deps=[\"calc\"])\n"
                   "dylib(\"calc\", sources=[\"calc.vx\"])\n"));
  // app 通过 extern 声明引用 dylib 内的 VX 函数（符号 = 函数名）
  CHECK(write_text(depproj / "main.vx",
                   "extern func triple(x: int) -> int\n"
                   "extern out_int(v: int)\n"
                   "func main() -> int:\n"
                   "    out_int(triple(5))\n"
                   "    return 0\n"));
  CHECK(write_text(depproj / "calc.vx",
                   "func triple(x: int) -> int:\n"
                   "    return x * 3\n"));
  RunResult dep_first =
      run_in_dir(vmake, {"build", "-B", "bdir"}, depproj, tool_env());
  CHECK(dep_first.code == 0);
  if (dep_first.code != 0)
    std::fprintf(stderr, "%s\n", dep_first.output.c_str());
  // 构建顺序：dylib calc 先于 exe app
  {
    const std::size_t p_calc = dep_first.output.find("calc.vx");
    const std::size_t p_main = dep_first.output.find("main.vx");
    CHECK(p_calc != std::string::npos && p_main != std::string::npos &&
          p_calc < p_main);
  }
  if (fs::is_regular_file(depproj / "bdir" / "app", ec) && !ec) {
    RunResult run = run_in_dir((depproj / "bdir" / "app").string(), {}, depproj);
    CHECK(run.code == 0);
    CHECK(run.output == "15");
  }

  // 二次构建：全 Fresh
  RunResult dep_second =
      run_in_dir(vmake, {"build", "-B", "bdir"}, depproj, tool_env());
  CHECK(dep_second.code == 0);
  CHECK(!contains(dep_second.output, "vxc"));
  CHECK(!contains(dep_second.output, "vlnk"));

  // dylib 源变更 → calc 重编重链 + app 仅重链（对象 fresh）
  CHECK(write_text(depproj / "calc.vx",
                   "func triple(x: int) -> int:\n"
                   "    return x * 4\n"));
  RunResult dep_third =
      run_in_dir(vmake, {"build", "-B", "bdir"}, depproj, tool_env());
  CHECK(dep_third.code == 0);
  CHECK(contains(dep_third.output, "vxc calc.vx"));
  CHECK(contains(dep_third.output, "vlnk app"));
  CHECK(!contains(dep_third.output, "vxc main.vx")); // app 源未重编
  if (fs::is_regular_file(depproj / "bdir" / "app", ec) && !ec) {
    RunResult run = run_in_dir((depproj / "bdir" / "app").string(), {}, depproj);
    CHECK(run.code == 0);
    CHECK(run.output == "20");
  }

  // 只选 app：依赖 calc 被闭包展开自动纳入计划
  const fs::path dep_bdir2 = depproj / "bdir2";
  RunResult dep_selected =
      run_in_dir(vmake, {"build", "-B", "bdir2", "app"}, depproj, tool_env());
  CHECK(dep_selected.code == 0);
  CHECK(contains(dep_selected.output, "calc"));
  CHECK(fs::is_regular_file(dep_bdir2 / "app", ec) && !ec);

  // ---- defines：链接期 -D 透传给 clang（导入库 .c 条件编译） ----
  const fs::path defproj = root / "defproj";
  fs::create_directories(defproj / "headers", ec);
  CHECK(write_text(defproj / "VMakeSet.vmake",
                   "exe(\"hi\", sources=[\"main.vx\"],"
                   " defines=[\"VMAKE_E2E_DEFINE\"])\n"));
  CHECK(write_text(defproj / "main.vx",
                   "@imp(flag)\n"
                   "extern out_int(v: int)\n"
                   "func main() -> int:\n"
                   "    out_int(flag.flag_value())\n"
                   "    return 0\n"));
  CHECK(write_text(defproj / "headers" / "flag.vxh",
                   "extern func flag_value() -> int\n"));
  CHECK(write_text(defproj / "headers" / "flag.c",
                   "#include <stdint.h>\n"
                   "#ifdef VMAKE_E2E_DEFINE\n"
                   "int32_t _vx_flag_flag_value(void) { return 7; }\n"
                   "#endif\n"));
  RunResult def_first =
      run_in_dir(vmake, {"build", "-B", "bdir"}, defproj, tool_env());
  CHECK(def_first.code == 0);
  if (def_first.code != 0)
    std::fprintf(stderr, "%s\n", def_first.output.c_str());
  if (fs::is_regular_file(defproj / "bdir" / "hi", ec) && !ec) {
    RunResult run = run_in_dir((defproj / "bdir" / "hi").string(), {}, defproj);
    CHECK(run.code == 0);
    CHECK(run.output == "7");
  }
  // 去掉 define：.c 中符号被 #ifdef 排除 → 链接失败；对象签名不含 flags → 不重编
  CHECK(write_text(defproj / "VMakeSet.vmake",
                   "exe(\"hi\", sources=[\"main.vx\"])\n"));
  RunResult def_off =
      run_in_dir(vmake, {"build", "-B", "bdir"}, defproj, tool_env());
  CHECK(def_off.code != 0);
  CHECK(contains(def_off.output, "vlnk"));
  CHECK(!contains(def_off.output, "vxc"));
  // 恢复 define：链接签名变化 → 重链成功，对象保持 fresh
  CHECK(write_text(defproj / "VMakeSet.vmake",
                   "exe(\"hi\", sources=[\"main.vx\"],"
                   " defines=[\"VMAKE_E2E_DEFINE\"])\n"));
  RunResult def_restored =
      run_in_dir(vmake, {"build", "-B", "bdir"}, defproj, tool_env());
  CHECK(def_restored.code == 0);
  if (fs::is_regular_file(defproj / "bdir" / "hi", ec) && !ec) {
    RunResult run = run_in_dir((defproj / "bdir" / "hi").string(), {}, defproj);
    CHECK(run.code == 0);
    CHECK(run.output == "7");
  }

  // ---- --release / --debug：profile 变化仅触发重链，相同 profile 全 Fresh ----
  const fs::path relproj = root / "relproj";
  fs::create_directories(relproj, ec);
  CHECK(write_text(relproj / "VMakeSet.vmake",
                   "exe(\"hi\", sources=[\"main.vx\"])\n"));
  CHECK(write_text(relproj / "main.vx", "extern out_int(v: int)\n"
                                        "func main() -> int:\n"
                                        "    out_int(3)\n"
                                        "    return 0\n"));
  RunResult rel_first =
      run_in_dir(vmake, {"build", "-B", "bdir"}, relproj, tool_env());
  CHECK(rel_first.code == 0);
  if (rel_first.code != 0)
    std::fprintf(stderr, "%s\n", rel_first.output.c_str());
  RunResult rel_same =
      run_in_dir(vmake, {"build", "-B", "bdir"}, relproj, tool_env());
  CHECK(rel_same.code == 0);
  CHECK(!contains(rel_same.output, "vlnk"));
  RunResult rel_on = run_in_dir(vmake, {"build", "-B", "bdir", "--release"},
                                relproj, tool_env());
  CHECK(rel_on.code == 0);
  CHECK(contains(rel_on.output, "vlnk"));
  CHECK(!contains(rel_on.output, "vxc"));
  RunResult rel_again = run_in_dir(vmake, {"build", "-B", "bdir", "--release"},
                                   relproj, tool_env());
  CHECK(rel_again.code == 0);
  CHECK(!contains(rel_again.output, "vlnk"));
  RunResult rel_off = run_in_dir(vmake, {"build", "-B", "bdir", "--debug"},
                                 relproj, tool_env());
  CHECK(rel_off.code == 0);
  CHECK(contains(rel_off.output, "vlnk"));
  CHECK(!contains(rel_off.output, "vxc"));

  // ---- vmake run：构建后直接执行（参数经 -- 转发），二次 run 全 Fresh ----
  const fs::path runproj = root / "runproj";
  fs::create_directories(runproj, ec);
  CHECK(write_text(runproj / "VMakeSet.vmake",
                   "exe(\"app\", sources=[\"main.vx\"])\n"));
  CHECK(write_text(runproj / "main.vx", "func main() -> int:\n"
                                        "    out(sys_argv_get(1))\n"
                                        "    return 0\n"));
  RunResult run1 = run_in_dir(vmake, {"run", "-B", "bdir", "--", "arg1"},
                              runproj, tool_env());
  CHECK(run1.code == 0);
  if (run1.code != 0)
    std::fprintf(stderr, "%s\n", run1.output.c_str());
  CHECK(run1.output.ends_with("arg1"));
  RunResult run2 = run_in_dir(vmake, {"run", "-B", "bdir", "--", "arg2"},
                              runproj, tool_env());
  CHECK(run2.code == 0);
  CHECK(!contains(run2.output, "vxc"));
  CHECK(run2.output.ends_with("arg2"));

  // ---- vmake test：tests 派生测试目标（dylib 并入目标源；exe 仅测试源） ----
  const fs::path tproj = root / "tproj";
  fs::create_directories(tproj / "tests", ec);
  CHECK(write_text(tproj / "VMakeSet.vmake",
                   "exe(\"app\", sources=[\"app.vx\"], deps=[\"calc\"],\n"
                   "    tests=[\"tests/app_test.vx\"])\n"
                   "dylib(\"calc\", sources=[\"calc.vx\"],\n"
                   "      tests=[\"tests/calc_test.vx\"])\n"
                   "exe(\"solo\", sources=[\"solo.vx\"])\n"));
  CHECK(write_text(tproj / "app.vx",
                   "extern func triple(x: int) -> int\n"
                   "extern out_int(v: int)\n"
                   "func main() -> int:\n"
                   "    out_int(triple(1))\n"
                   "    return 0\n"));
  CHECK(write_text(tproj / "solo.vx", "func main() -> int:\n"
                                      "    return 0\n"));
  CHECK(write_text(tproj / "calc.vx", "func triple(x: int) -> int:\n"
                                      "    return x * 3\n"));
  // dylib calc → calc-test 源 = calc.vx + calc_test.vx（测试文件提供 main）
  CHECK(write_text(tproj / "tests" / "calc_test.vx",
                   "extern out_int(v: int)\n"
                   "func main() -> int:\n"
                   "    out_int(triple(5))\n"
                   "    return 0\n"));
  // exe app → app-test 源 = 仅 app_test.vx，经 deps=calc 引用被测代码
  CHECK(write_text(tproj / "tests" / "app_test.vx",
                   "extern func triple(x: int) -> int\n"
                   "extern out_int(v: int)\n"
                   "func main() -> int:\n"
                   "    out_int(triple(7))\n"
                   "    return 0\n"));
  RunResult test1 = run_in_dir(vmake, {"test", "-B", "bdir"}, tproj, tool_env());
  CHECK(test1.code == 0);
  if (test1.code != 0)
    std::fprintf(stderr, "%s\n", test1.output.c_str());
  CHECK(contains(test1.output, "calc-test ... ok"));
  CHECK(contains(test1.output, "app-test ... ok"));
  CHECK(contains(test1.output, "2 passed, 0 failed"));

  // 让 calc_test 返回非零 → 失败汇总与退出码 1
  CHECK(write_text(tproj / "tests" / "calc_test.vx",
                   "func main() -> int:\n"
                   "    return 1\n"));
  RunResult test2 = run_in_dir(vmake, {"test", "-B", "bdir"}, tproj, tool_env());
  CHECK(test2.code == 1);
  CHECK(contains(test2.output, "calc-test ... FAILED"));
  CHECK(contains(test2.output, "app-test ... ok"));
  CHECK(contains(test2.output, "1 passed, 1 failed"));

  // 显式点名无 tests 的目标 → 用法错误（退出码 2）
  RunResult test_named = run_in_dir(vmake, {"test", "-B", "bdir", "solo"},
                                    tproj, tool_env());
  CHECK(test_named.code == 2);
  CHECK(contains(test_named.output, "declares no tests"));

  // tproj 有两个 exe：vmake run 未点名 → 退出码 2
  RunResult run_multi = run_in_dir(vmake, {"run", "-B", "bdir"}, tproj,
                                   tool_env());
  CHECK(run_multi.code == 2);
  CHECK(contains(run_multi.output, "multiple exe targets"));
  // 点名 exe 目标可运行
  RunResult run_named = run_in_dir(vmake, {"run", "-B", "bdir", "app"}, tproj,
                                   tool_env());
  CHECK(run_named.code == 0);
  CHECK(run_named.output.ends_with("3"));

  // ---- 无 exe 目标：run 报错；无 tests 声明：test 提示并返回 0 ----
  const fs::path noproj = root / "noproj";
  fs::create_directories(noproj, ec);
  CHECK(write_text(noproj / "VMakeSet.vmake",
                   "staticlib(\"util\", sources=[\"util.vx\"])\n"));
  CHECK(write_text(noproj / "util.vx", "func id(x: int) -> int:\n"
                                       "    return x\n"));
  RunResult no_exe =
      run_in_dir(vmake, {"run", "-B", "bdir"}, noproj, tool_env());
  CHECK(no_exe.code == 2);
  CHECK(contains(no_exe.output, "no exe target"));
  RunResult no_tests =
      run_in_dir(vmake, {"test", "-B", "bdir"}, noproj, tool_env());
  CHECK(no_tests.code == 0);
  CHECK(contains(no_tests.output, "no tests declared"));

  // ---- 语法错误：失败诊断应包含 Cargo 风格错误行 ----
  CHECK(write_text(root / "main.vx", "func main(:\n    return 0\n"));
  RunResult bad = run_in_dir(vmake, {"build", "-B", "bdir"}, root, tool_env());
  CHECK(bad.code != 0);
  CHECK(contains(bad.output, "could not compile `app`"));

  // ---- 缺少 -B：使用错误（退出码 2） ----
  RunResult no_b = run_in_dir(vmake, {}, root, tool_env());
  CHECK(no_b.code == 2);
  CHECK(contains(no_b.output, "-B"));

  fs::remove_all(root, ec);

  if (g_failures == 0) {
    std::fprintf(stdout, "vmake e2e: all passed\n");
    return 0;
  }
  std::fprintf(stderr, "vmake e2e: %d failures\n", g_failures);
  return 1;
}
