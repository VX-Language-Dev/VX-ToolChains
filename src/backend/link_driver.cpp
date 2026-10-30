// link_driver.cpp — 后端链接驱动实现（fork/execvp 调用 host clang）。
// 内置 vxrt 运行时优先以预编译 libvxrt.a 链接；找不到时改为内嵌源码现编
// （内嵌源码由 CMake 从 src/runtime/vxrt.c 生成，二者始终同源）。
// out/out_int/out_bool/out_float/vx_str_concat/系统能力（argv、文件 I/O）
// 由此提供，malloc/free/puts/exit 解析到 libc。
#include "vx/backend/link_driver.hpp"

#include "vxrt_source.h"

#include "vx/compiler/vxabi.hpp"
#include "vx/util/platform.hpp"

#include <cstdlib>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>

#if defined(__APPLE__)
#include <mach-o/dyld.h>
#endif

namespace fs = std::filesystem;

namespace vx::backend {
namespace {


std::string read_text(const fs::path &p) {
  std::ifstream in(p, std::ios::binary);
  if (!in) return {};
  return std::string((std::istreambuf_iterator<char>(in)),
                     std::istreambuf_iterator<char>());
}

// 附加内存源：写入临时目录后参与编译（如 @cppimp 的 shim.cpp）
struct MemorySource {
  std::string name;
  std::string content;
};

// 当前进程可执行文件所在目录（用于查找与安装位置同级的 libvxrt.a）
fs::path executable_dir() {
#if defined(__APPLE__)
  std::uint32_t size = 0;
  ::_NSGetExecutablePath(nullptr, &size);
  if (size == 0)
    return {};
  std::string buf(size, '\0');
  if (::_NSGetExecutablePath(buf.data(), &size) != 0)
    return {};
  buf.resize(size > 0 ? size - 1 : 0);
  return fs::path(buf).parent_path();
#elif defined(__linux__)
  std::vector<char> buf(4096);
  const ssize_t n = ::readlink("/proc/self/exe", buf.data(), buf.size() - 1);
  if (n <= 0)
    return {};
  buf[static_cast<std::size_t>(n)] = '\0';
  return fs::path(buf.data()).parent_path();
#else
  return {};
#endif
}

// 查找预编译的 vxrt 运行时库（.a），避免每次重新编译 C 源。
// 查找顺序：VX_VXRT > vlnk 可执行文件同级/上级/../lib > 当前目录。
static std::string find_vxrt_lib() {
  if (const char *env = std::getenv("VX_VXRT")) {
    if (env[0] != '\0' && ::access(env, R_OK) == 0) return env;
  }
  std::vector<fs::path> cands;
  const fs::path exe_dir = executable_dir();
  if (!exe_dir.empty()) {
    cands.push_back(exe_dir / "libvxrt.a");
    cands.push_back(exe_dir.parent_path() / "libvxrt.a");
    cands.push_back(exe_dir.parent_path() / "lib" / "libvxrt.a");
  }
  cands.push_back(fs::current_path() / "libvxrt.a");
  cands.push_back(fs::current_path() / "libvx_runtime.a");
  cands.push_back(fs::current_path() / "build" / "libvxrt.a");
  for (const fs::path &p : cands) {
    std::error_code ec;
    if (fs::is_regular_file(p, ec) && !ec) return p.string();
  }
  return {};
}

// 查找 PATH 中的 ld.lld。可用时优先用 lld 链接（实测约快 20%）。
// 设置 VX_NO_LLD=1 可强制回退到平台默认链接器。
std::string find_lld() {
  if (const char *off = std::getenv("VX_NO_LLD"); off && off[0] != '\0')
    return {};
  const char *path = std::getenv("PATH");
  if (!path)
    return {};
  const std::string dirs(path);
  std::size_t start = 0;
  while (start <= dirs.size()) {
    const std::size_t end = dirs.find(':', start);
    const std::string dir =
        end == std::string::npos ? dirs.substr(start) : dirs.substr(start, end - start);
    if (!dir.empty()) {
      const std::string cand = dir + "/ld.lld";
      if (::access(cand.c_str(), X_OK) == 0) return cand;
    }
    if (end == std::string::npos) break;
    start = end + 1;
  }
  return {};
}

// 核心链接实现：.ll + vxrt.c + 附加磁盘文件 + 附加内存源 → 可执行文件/动态库。
LinkResult link_impl(std::string_view ll_ir, std::string_view exe_path,
                     std::string_view target,
                     const std::vector<std::string> &extra_files,
                     const std::vector<MemorySource> &extra_sources,
                     bool shared = false,
                     const std::vector<std::string> &cflags = {},
                     const std::vector<std::string> &ldflags = {}) {
  LinkResult r;
  const std::string clang = find_clang();
  if (clang.empty()) {
    r.error = "未找到 clang 可执行文件（可设置环境变量 VX_CLANG 指定路径）";
    return r;
  }

  // 临时工作目录
  std::string tmpl = (fs::temp_directory_path() / "vlnk_XXXXXX").string();
  std::vector<char> tmpl_buf(tmpl.begin(), tmpl.end());
  tmpl_buf.push_back('\0');
  const char *dir = ::mkdtemp(tmpl_buf.data());
  if (dir == nullptr) {
    r.error = "无法创建临时目录";
    return r;
  }
  const fs::path tmp(dir);
  const fs::path ll_file = tmp / "module.ll";
  const fs::path log_file = tmp / "clang.log";

  const std::string vxrt_lib = find_vxrt_lib();

  {
    std::ofstream(ll_file, std::ios::binary) << std::string(ll_ir);
    if (vxrt_lib.empty()) {
      const fs::path rt_file = tmp / "vxrt.c";
      std::ofstream(rt_file, std::ios::binary) << kVxrtSource;
    }
  }

  // 单次调用 clang：.ll 与所有 .c/.cpp/.a 一并编译并链接为可执行文件。
  std::vector<std::string> args = {clang, ll_file.string()};
  r.command = clang + " " + ll_file.string();
  if (vxrt_lib.empty()) {
    const fs::path rt_file = tmp / "vxrt.c";
    args.push_back(rt_file.string());
    r.command += " " + rt_file.string();
  } else {
    args.push_back(vxrt_lib);
    r.command += " " + vxrt_lib;
  }
  // 附加内存源（如 C++ shim）
  for (const MemorySource &src : extra_sources) {
    const fs::path p = tmp / src.name;
    std::ofstream(p, std::ios::binary) << src.content;
    args.push_back(p.string());
    r.command += " " + p.string();
  }
  // 附加磁盘文件（导入库的实现文件）
  for (const std::string &f : extra_files) {
    args.push_back(f);
    r.command += " " + f;
  }
  // 额外 clang 参数（--cflag）：紧随输入文件，作用于 .ll/.c/.cpp 的编译与链接
  for (const std::string &f : cflags) {
    args.push_back(f);
    r.command += " " + f;
  }
  args.push_back("-Wno-override-module");
  r.command += " -Wno-override-module";
  // libm：vx_pow_f64 依赖 pow()；Linux 必须显式 -lm，macOS 该选项无害
  args.push_back("-lm");
  r.command += " -lm";
  // 本机链接且存在 ld.lld 时优先 lld；交叉编译（显式 -target）保持默认链接器。
  if (target.empty() && !find_lld().empty()) {
    args.push_back("-fuse-ld=lld");
    r.command += " -fuse-ld=lld";
  }
  // 动态库：位置无关代码 + 共享链接（macOS .dylib / Linux .so）
  if (shared) {
    args.push_back("-shared");
    args.push_back("-fPIC");
    r.command += " -shared -fPIC";
  }
  // C++ 源（@cppimp shim / 实现）：clang 驱动不会自动补 C++ 标准库，
  // 显式链接之，否则 std::string 等符号未定义。
  bool any_cxx = false;
  for (const MemorySource &src : extra_sources) {
    if (src.name.ends_with(".cpp") || src.name.ends_with(".cc") ||
        src.name.ends_with(".cxx")) {
      any_cxx = true;
    }
  }
  for (const std::string &f : extra_files) {
    if (f.ends_with(".cpp") || f.ends_with(".cc") || f.ends_with(".cxx")) {
      any_cxx = true;
    }
  }
  if (any_cxx) {
    const std::string cxx_lib =
        vx::util::host_os() == "macosx" ? "-lc++" : "-lstdc++";
    args.push_back(cxx_lib);
    r.command += " " + cxx_lib;
  }
  // 显式指定目标平台 → 交叉编译（.ll 头部的 triple 会以 -target 为准）
  if (!target.empty()) {
    args.push_back("-target");
    args.emplace_back(target);
    r.command += " -target " + std::string(target);
  }
  args.push_back("-o");
  args.emplace_back(exe_path);
  r.command += " -o " + std::string(exe_path);
  // 链接器参数（--ldflag）：置于命令末尾（如 -Wl,... / -static）
  for (const std::string &f : ldflags) {
    args.push_back(f);
    r.command += " " + f;
  }

  std::vector<char *> argv;
  argv.reserve(args.size() + 1);
  for (std::string &a : args) argv.push_back(a.data());
  argv.push_back(nullptr);

  const pid_t pid = ::fork();
  if (pid < 0) {
    fs::remove_all(tmp);
    r.error = "fork 失败";
    return r;
  }
  if (pid == 0) {
    const int fd = ::open(log_file.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd >= 0) {
      ::dup2(fd, STDOUT_FILENO);
      ::dup2(fd, STDERR_FILENO);
      ::close(fd);
    }
    ::execvp(clang.c_str(), argv.data());
    ::_exit(127); // exec 失败
  }

  int status = 0;
  while (::waitpid(pid, &status, 0) < 0) {
    // 被信号中断则重试
  }

  const bool exited = WIFEXITED(status);
  const int code = exited ? WEXITSTATUS(status) : -1;
  if (!exited || code != 0) {
    std::string log = read_text(log_file);
    if (log.empty()) log = "(无输出)";
    r.error = "clang 链接失败 (exit " + std::to_string(code) + "):\n" + log;
    fs::remove_all(tmp);
    return r;
  }

  if (!fs::exists(exe_path)) {
    r.error = "clang 报告成功但未生成可执行文件";
    fs::remove_all(tmp);
    return r;
  }

  fs::remove_all(tmp);
  r.ok = true;
  return r;
}

} // namespace

std::string host_triple() { return vx::util::host_triple(); }

std::string find_clang() {
  if (const char *env = std::getenv("VX_CLANG")) {
    if (env[0] != '\0' && ::access(env, X_OK) == 0) return env;
  }
  const char *path = std::getenv("PATH");
  if (!path) return {};
  std::string dirs(path);
  std::size_t start = 0;
  while (start <= dirs.size()) {
    const std::size_t end = dirs.find(':', start);
    const std::string dir =
        end == std::string::npos ? dirs.substr(start) : dirs.substr(start, end - start);
    if (!dir.empty()) {
      const std::string cand = dir + "/clang";
      if (::access(cand.c_str(), X_OK) == 0) return cand;
    }
    if (end == std::string::npos) break;
    start = end + 1;
  }
  return {};
}

LinkResult link_executable(std::string_view ll_ir, std::string_view exe_path,
                           std::string_view target) {
  return link_impl(ll_ir, exe_path, target, {}, {});
}

std::string generate_cpp_shim(const vx::compiler::ir::ModuleIR &mod) {
  std::string out;
  out += "// 由 vlnk 自动生成：@cppimp 的 C++ ABI → VX ABI shim\n";
  out += "#include <cstdint>\n";
  // 头文件（绝对路径，去重）
  std::vector<std::string> headers;
  for (const vx::compiler::ir::LibDep &d : mod.libs) {
    if (!d.from_cpp || d.cpp_header.empty()) continue;
    bool seen = false;
    for (const std::string &h : headers) seen = seen || h == d.cpp_header;
    if (!seen) headers.push_back(d.cpp_header);
  }
  for (const std::string &h : headers) out += "#include \"" + h + "\"\n";
  out += "\nextern \"C\" {\n";
  for (const vx::compiler::ir::LinkEntry &e : mod.linkage) {
    if (!e.from_cpp) continue;
    const std::string sym =
        e.abi_symbol.empty() ? e.name : e.abi_symbol;
    const std::string_view ret = vx::compiler::abi::c_type_of(e.ret_type);
    out += std::string(ret) + " " + sym + "(";
    std::string call_args;
    for (std::size_t i = 0; i < e.params.size(); ++i) {
      if (i) {
        out += ", ";
        call_args += ", ";
      }
      const std::string pname =
          e.params[i].name.empty() ? ("a" + std::to_string(i)) : e.params[i].name;
      out += std::string(vx::compiler::abi::c_type_of(e.params[i].type)) + " " +
             pname;
      call_args += pname;
    }
    out += ") {\n";
    if (ret == "void") {
      out += "  " + e.name + "(" + call_args + ");\n";
    } else {
      out += "  return " + e.name + "(" + call_args + ");\n";
    }
    out += "}\n";
  }
  out += "}\n";
  return out;
}

LinkResult link_module(std::string_view ll_ir,
                       const vx::compiler::ir::ModuleIR &mod,
                       std::string_view exe_path, std::string_view target,
                       bool shared,
                       const std::vector<std::string> &native_libs,
                       const std::vector<std::string> &cflags,
                       const std::vector<std::string> &ldflags) {
  std::vector<std::string> extra_files;
  for (const vx::compiler::ir::LibDep &d : mod.libs) {
    if (d.impl_file.empty()) continue;
    std::error_code ec;
    if (fs::exists(d.impl_file, ec)) extra_files.push_back(d.impl_file);
  }
  // 额外原生库（--lib，如其他目标构建的 dylib）：与导入库实现文件一同传给 clang
  for (const std::string &lib : native_libs) extra_files.push_back(lib);
  std::vector<MemorySource> extra_sources;
  bool any_cpp = false;
  for (const vx::compiler::ir::LibDep &d : mod.libs) {
    if (d.from_cpp && !d.cpp_header.empty()) any_cpp = true;
  }
  if (any_cpp) extra_sources.push_back({"vxshim.cpp", generate_cpp_shim(mod)});
  return link_impl(ll_ir, exe_path, target, extra_files, extra_sources, shared,
                   cflags, ldflags);
}

} // namespace vx::backend
