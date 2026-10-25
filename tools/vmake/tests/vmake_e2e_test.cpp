// vmake_e2e_test — VMake 端到端：VMakeSet.vmake → vxc × N → vlnk → 产物运行。
// 覆盖：dylib 先于 exe 的构建顺序、多源文件合并、Cargo 风格输出、增量 Fresh、
// 编译失败诊断。找不到 clang 时返回 77（CTest SKIP）。
#include "vx/backend/link_driver.hpp"

#include <cstdio>
#include <cstdlib>
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

  // ---- 首次构建 ----
  RunResult first =
      run_in_dir(vmake, {"build", "-B", "bdir"}, root, tool_env());
  CHECK(first.code == 0);
  if (first.code != 0)
    std::fprintf(stderr, "%s\n", first.output.c_str());
  CHECK(contains(first.output, "Compiling calc (dylib)"));
  CHECK(contains(first.output, "Compiling app (exe)"));
  CHECK(contains(first.output, "Finished build in"));
  const std::size_t pos_calc = first.output.find("Compiling calc");
  const std::size_t pos_app = first.output.find("Compiling app");
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
  CHECK(!contains(second.output, "Compiling "));
  CHECK(contains(second.output, "Finished build in"));

  // ---- 触达一个 exe 源文件：只重编 app，calc 保持 Fresh ----
  CHECK(write_text(root / "util.vx", "func add(a: int, b: int) -> int:\n"
                                     "    return a + b\n"
                                     "# touched\n"));
  RunResult third =
      run_in_dir(vmake, {"build", "-B", "bdir"}, root, tool_env());
  CHECK(third.code == 0);
  CHECK(contains(third.output, "Compiling app (exe)"));
  CHECK(!contains(third.output, "Compiling calc"));

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
