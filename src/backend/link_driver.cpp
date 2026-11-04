// link_driver.cpp — 后端链接驱动：llc (LLVM IR → .o) + 系统 cc/c++ (.c/.cpp → .o) + lld (.o → 可执行文件)。
// 找不到任何分段工具时，自动回退 clang 驱动（维持旧行为）。
// 内置 vxrt 运行时优先以预编译 libvxrt.a 链接；找不到时改为内嵌源码现编。
#include "vx/backend/link_driver.hpp"

#include "vxrt_source.h"

#include "vx/compiler/vxabi.hpp"
#include "vx/util/platform.hpp"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
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

std::string read_text(const fs::path& p) {
  std::ifstream in(p, std::ios::binary);
  if (!in)
    return {};
  return {(std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>()};
}

// ---- 子进程执行：fork + execvp，stdout/stderr 重定向到日志文件 ----
// 返回 exit code；非 0 时 err_msg 包含日志末尾（供诊断）
int run_subprocess(const std::string& exe, std::vector<std::string>& args, const fs::path& log_file,
                   std::string* err_msg = nullptr) {
  std::vector<char*> argv;
  argv.reserve(args.size() + 1);
  for (std::string& a : args)
    argv.push_back(a.data());
  argv.push_back(nullptr);

  const pid_t pid = ::fork();
  if (pid < 0) {
    if (err_msg)
      *err_msg = "fork 失败";
    return -1;
  }
  if (pid == 0) {
    const int fd = ::open(log_file.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd >= 0) {
      ::dup2(fd, STDOUT_FILENO);
      ::dup2(fd, STDERR_FILENO);
      ::close(fd);
    }
    ::execvp(exe.c_str(), argv.data());
    ssize_t ignored = ::write(STDERR_FILENO, "execvp failed\n", 14);
    (void)ignored;
    ::_exit(127);
  }
  int status = 0;
  while (::waitpid(pid, &status, 0) < 0) {
  }
  if (!WIFEXITED(status))
    return -1;
  const int code = WEXITSTATUS(status);
  if (code != 0 && err_msg)
    *err_msg = read_text(log_file);
  return code;
}

// ---- 附加内存源：写入临时目录后参与编译 ----
struct MemorySource {
  std::string name;
  std::string content;
};

// RAII 守护 mkdtemp 创建的临时目录
struct TempDirGuard {
  fs::path path;
  ~TempDirGuard() {
    if (path.empty())
      return;
    std::error_code ec;
    fs::remove_all(path, ec);
  }
};

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

std::string find_vxrt_lib() {
  if (const char* env = std::getenv("VX_VXRT"); env != nullptr && env[0] != '\0' && ::access(env, R_OK) == 0)
    return env;
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
  for (const fs::path& p : cands) {
    std::error_code ec;
    if (fs::is_regular_file(p, ec) && !ec)
      return p.string();
  }
  return {};
}

// 查找 PATH 中的 ld.lld。可用时优先用 lld 链接（实测约快 20%）。
// 设置 VX_NO_LLD=1 可强制回退到平台默认链接器。
std::string find_lld() {
  if (const char* off = std::getenv("VX_NO_LLD"); off != nullptr && off[0] != '\0')
    return {};
  return vx::util::find_in_path({"ld.lld"});
}

// ---- 工具查找 ----
std::string find_tool_or_env(std::initializer_list<std::string_view> env_vars,
                             std::initializer_list<std::string_view> candidates) {
  for (std::string_view v : env_vars) {
    if (const char* env = std::getenv(std::string(v).c_str());
        env != nullptr && env[0] != '\0' && ::access(env, X_OK) == 0)
      return env;
  }
  return vx::util::find_in_path(candidates);
}

std::string find_llc() {
  return find_tool_or_env({"VX_LLC"}, {"llc"});
}
std::string find_cc() {
  return find_tool_or_env({"VX_CC"}, {"cc", "gcc", "clang"});
}
std::string find_cxx() {
  return find_tool_or_env({"VX_CXX"}, {"c++", "g++", "clang++"});
}

// 链接器选择：macOS 用 ld64.lld / ld，Linux 用 ld.lld / ld.gold / ld.bfd
std::string find_linker() {
  if (const char* env = std::getenv("VX_LINKER"); env != nullptr && env[0] != '\0' && ::access(env, X_OK) == 0)
    return env;
#if defined(__APPLE__)
  return vx::util::find_in_path({"ld64.lld", "ld"});
#else
  return vx::util::find_in_path({"ld.lld", "ld.gold", "ld.bfd", "ld"});
#endif
}

// ---- 旧版 clang 驱动实现（作为回退） ----
LinkResult link_via_clang(std::string_view ll_ir, std::string_view exe_path, std::string_view target,
                          const std::vector<std::string>& extra_files, const std::vector<MemorySource>& extra_sources,
                          bool shared, const std::vector<std::string>& cflags,
                          const std::vector<std::string>& ldflags) {
  LinkResult r;
  const std::string clang = find_clang();
  if (clang.empty()) {
    r.error = "未找到 clang / llc / lld 工具链。可设置 VX_CLANG 或安装 clang/LLVM。";
    return r;
  }
  std::string tmpl = (fs::temp_directory_path() / "vlnk_XXXXXX").string();
  std::vector<char> tmpl_buf(tmpl.begin(), tmpl.end());
  tmpl_buf.push_back('\0');
  const char* dir = ::mkdtemp(tmpl_buf.data());
  if (dir == nullptr) {
    r.error = "无法创建临时目录";
    return r;
  }
  TempDirGuard guard{fs::path(dir)};
  const fs::path& tmp = guard.path;
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
  for (const MemorySource& src : extra_sources) {
    const fs::path p = tmp / src.name;
    std::ofstream(p, std::ios::binary) << src.content;
    args.push_back(p.string());
    r.command += " " + p.string();
  }
  for (const std::string& f : extra_files) {
    args.push_back(f);
    r.command += " " + f;
  }
  for (const std::string& f : cflags) {
    args.push_back(f);
    r.command += " " + f;
  }
  args.emplace_back("-Wno-override-module");
  r.command += " -Wno-override-module";
  args.emplace_back("-lm");
  r.command += " -lm";
  if (target.empty() && !find_lld().empty()) {
    args.emplace_back("-fuse-ld=lld");
    r.command += " -fuse-ld=lld";
  }
  if (shared) {
    args.emplace_back("-shared");
    args.emplace_back("-fPIC");
    r.command += " -shared -fPIC";
  }
  bool any_cxx = false;
  for (const MemorySource& src : extra_sources)
    if (src.name.ends_with(".cpp") || src.name.ends_with(".cc") || src.name.ends_with(".cxx"))
      any_cxx = true;
  for (const std::string& f : extra_files)
    if (f.ends_with(".cpp") || f.ends_with(".cc") || f.ends_with(".cxx"))
      any_cxx = true;
  if (any_cxx) {
    const std::string cxx_lib = vx::util::host_os() == "macosx" ? "-lc++" : "-lstdc++";
    args.push_back(cxx_lib);
    r.command += " " + cxx_lib;
  }
  if (!target.empty()) {
    args.emplace_back("-target");
    args.emplace_back(target);
    r.command += " -target " + std::string(target);
  }
  args.emplace_back("-o");
  args.emplace_back(exe_path);
  r.command += " -o " + std::string(exe_path);
  for (const std::string& f : ldflags) {
    args.push_back(f);
    r.command += " " + f;
  }

  std::string err;
  const int code = run_subprocess(clang, args, log_file, &err);
  if (code != 0) {
    r.error = "clang 链接失败 (exit " + std::to_string(code) + "):\n" + (err.empty() ? "(无输出)" : err);
    return r;
  }
  if (!fs::exists(exe_path)) {
    r.error = "clang 报告成功但未生成可执行文件";
    return r;
  }
  r.ok = true;
  return r;
}

// ---- 新版分段链路：llc + cc/c++ + lld ----
LinkResult link_via_pipeline(std::string_view ll_ir, std::string_view exe_path, std::string_view target,
                             const std::vector<std::string>& extra_files,
                             const std::vector<MemorySource>& extra_sources, bool shared,
                             const std::vector<std::string>& cflags, const std::vector<std::string>& ldflags) {
  LinkResult r;
  const std::string llc = find_llc();
  const std::string cc = find_cc();
  const std::string cxx = find_cxx();
  const std::string linker = find_linker();
  if (llc.empty() || cc.empty() || linker.empty()) {
    r.error = "分段工具链不完整（缺 llc/cc/linker），应已在调用方回退 clang；这是内部错误";
    return r;
  }

  std::string tmpl = (fs::temp_directory_path() / "vlnk_XXXXXX").string();
  std::vector<char> tmpl_buf(tmpl.begin(), tmpl.end());
  tmpl_buf.push_back('\0');
  const char* dir = ::mkdtemp(tmpl_buf.data());
  if (dir == nullptr) {
    r.error = "无法创建临时目录";
    return r;
  }
  TempDirGuard guard{fs::path(dir)};
  const fs::path& tmp = guard.path;
  const std::string host = vx::backend::host_triple();
  const std::string triple = target.empty() ? host : std::string(target);

  std::vector<fs::path> obj_files;            // 所有 .o 文件（llc + cc + cxx 产出）
  std::vector<std::string> extra_a_files;     // 预编译 .a（libvxrt.a, 用户 --lib .a）
  std::vector<std::string> extra_dylib_files; // .dylib/.so（用户 --lib）

  // ---- 1. 写 .ll → llc → module.o ----
  const fs::path ll_file = tmp / "module.ll";
  const fs::path obj_vx = tmp / "module.o";
  {
    std::ofstream(ll_file, std::ios::binary) << std::string(ll_ir);
  }
  {
    std::vector<std::string> args = {llc,  "--filetype=obj", "--mtriple=" + triple,
                                     "-o", obj_vx.string(),  ll_file.string()};
    const fs::path log = tmp / "llc.log";
    std::string err;
    if (run_subprocess(llc, args, log, &err) != 0) {
      r.error = "llc 编译失败:\n" + err;
      return r;
    }
  }
  obj_files.push_back(obj_vx);
  r.command = llc + " --filetype=obj --mtriple=" + triple + " -o " + obj_vx.string() + " " + ll_file.string();

  // ---- 2. vxrt：优先预编译 .a，否则写源码用 cc 编 ----
  const std::string vxrt_lib = find_vxrt_lib();
  if (!vxrt_lib.empty()) {
    extra_a_files.push_back(vxrt_lib);
  } else {
    const fs::path rt_file = tmp / "vxrt.c";
    const fs::path rt_obj = tmp / "vxrt.o";
    std::ofstream(rt_file, std::ios::binary) << kVxrtSource;
    std::vector<std::string> args = {cc, "-c", "-o", rt_obj.string(), rt_file.string()};
    for (const auto& f : cflags) {
      // 粗粒度 cflag 过滤：-target/-arch/-shared/-fPIC 对单文件编译无意义；-W/-O/-I/-D 等有益
      if (f.starts_with("-W") || f.starts_with("-O") || f.starts_with("-I") || f.starts_with("-D") ||
          f.starts_with("-std"))
        args.push_back(f);
    }
    if (!target.empty()) {
      args.emplace_back("-target");
      args.emplace_back(target);
    }
    if (shared)
      args.emplace_back("-fPIC");
    const fs::path log = tmp / "cc_vxrt.log";
    std::string err;
    if (run_subprocess(cc, args, log, &err) != 0) {
      r.error = "cc 编译 vxrt.c 失败:\n" + err;
      return r;
    }
    obj_files.push_back(rt_obj);
    r.command += " && " + cc + " -c -o " + rt_obj.string() + " " + rt_file.string();
  }

  // ---- 3. 内存源（@cppimp shim.cpp 等）----
  for (const MemorySource& src : extra_sources) {
    const fs::path p = tmp / src.name;
    std::ofstream(p, std::ios::binary) << src.content;
    const fs::path out = tmp / (p.stem().string() + ".o");
    const bool is_cxx = src.name.ends_with(".cpp") || src.name.ends_with(".cc") || src.name.ends_with(".cxx");
    const std::string& compiler = is_cxx ? cxx : cc;
    if (compiler.empty()) {
      r.error = "需要 " + std::string(is_cxx ? "C++ 编译器" : "C 编译器") + " 但未找到";
      return r;
    }
    std::vector<std::string> args = {compiler, "-c", "-o", out.string(), p.string()};
    for (const auto& f : cflags)
      if (f.starts_with("-W") || f.starts_with("-O") || f.starts_with("-I") || f.starts_with("-D") ||
          f.starts_with("-std"))
        args.push_back(f);
    if (!target.empty()) {
      args.emplace_back("-target");
      args.emplace_back(target);
    }
    if (shared)
      args.emplace_back("-fPIC");
    const fs::path log = tmp / (std::string(is_cxx ? "cxx_" : "cc_") + p.stem().string() + ".log");
    std::string err;
    if (run_subprocess(compiler, args, log, &err) != 0) {
      r.error = compiler + " 编译 " + src.name + " 失败:\n" + err;
      return r;
    }
    obj_files.push_back(out);
    r.command += " && " + compiler + " -c -o " + out.string() + " " + p.string();
  }

  // ---- 4. 额外磁盘文件（@imp 导入库的 .c/.cpp）----
  for (const std::string& f : extra_files) {
    const bool is_cxx = f.ends_with(".cpp") || f.ends_with(".cc") || f.ends_with(".cxx");
    const bool is_lib_a = f.ends_with(".a");
    const bool is_lib_dylib = f.ends_with(".dylib") || f.ends_with(".so") || f.ends_with(".dylib.a");
    if (is_lib_a) {
      extra_a_files.push_back(f);
      continue;
    }
    if (is_lib_dylib) {
      extra_dylib_files.push_back(f);
      continue;
    }
    const std::string& compiler = is_cxx ? cxx : cc;
    if (compiler.empty()) {
      r.error = "需要 " + std::string(is_cxx ? "C++ 编译器" : "C 编译器") + " 但未找到";
      return r;
    }
    const fs::path in_path = f;
    const fs::path out = tmp / (in_path.stem().string() + ".o");
    std::vector<std::string> args = {compiler, "-c", "-o", out.string(), in_path.string()};
    for (const auto& cf : cflags)
      if (cf.starts_with("-W") || cf.starts_with("-O") || cf.starts_with("-I") || cf.starts_with("-D") ||
          cf.starts_with("-std"))
        args.push_back(cf);
    if (!target.empty()) {
      args.emplace_back("-target");
      args.emplace_back(target);
    }
    if (shared)
      args.emplace_back("-fPIC");
    const fs::path log = tmp / (std::string(is_cxx ? "cxx_" : "cc_") + in_path.stem().string() + ".log");
    std::string err;
    if (run_subprocess(compiler, args, log, &err) != 0) {
      r.error = compiler + " 编译 " + f + " 失败:\n" + err;
      return r;
    }
    obj_files.push_back(out);
    r.command += " && " + compiler + " -c -o " + out.string() + " " + f;
  }

  // ---- 5. lld 链接 ----
  std::vector<std::string> link_args = {linker};
#if defined(__APPLE__)
  // macOS：ld64.lld 直接用，需要 syslibroot 找 libSystem
  std::string arch = triple.substr(0, triple.find('-'));
  if (arch == "aarch64")
    arch = "arm64";
  link_args.push_back("-arch");
  link_args.push_back(arch);
  link_args.push_back("-platform_version");
  link_args.push_back("macos");
  link_args.push_back("11.0");
  link_args.push_back("0.0.0");
  // syslibroot：xcrun --show-sdk-path 拿 MacOSX.sdk 路径
  std::string sysroot;
  {
    FILE* f = ::popen("xcrun --show-sdk-path 2>/dev/null", "r");
    if (f) {
      char buf[512] = {};
      if (::fgets(buf, sizeof(buf), f)) {
        sysroot = buf;
        while (!sysroot.empty() && (sysroot.back() == '\n' || sysroot.back() == ' ' || sysroot.back() == '\r'))
          sysroot.pop_back();
      }
      ::pclose(f);
    }
  }
  if (!sysroot.empty()) {
    link_args.push_back("-syslibroot");
    link_args.push_back(sysroot);
  }
  if (shared) {
    link_args.push_back("-dylib");
    link_args.push_back("-install_name");
    link_args.push_back("@rpath/" + fs::path(exe_path).filename().string());
  } else {
    // 可执行文件默认加 @loader_path，让 @rpath 能找到同目录的 dylib（与 clang 默认行为一致）
    link_args.push_back("-rpath");
    link_args.push_back("@loader_path");
  }
#else
  // Linux ld.lld：--shared 产生 .so
  if (shared)
    link_args.push_back("--shared");
#endif
  for (const auto& obj : obj_files)
    link_args.push_back(obj.string());
  for (const auto& a : extra_a_files)
    link_args.push_back(a);
  for (const auto& d : extra_dylib_files)
    link_args.push_back(d);
  // 系统库
#if defined(__APPLE__)
  // macOS 全部系统库（libc/libm/pthread/...）合并进 libSystem
  link_args.push_back("-lSystem");
#else
  // Linux 显式链接 libc + libm
  link_args.push_back("-lc");
  link_args.push_back("-lm");
#endif
  bool any_cxx = false;
  for (const MemorySource& src : extra_sources)
    if (src.name.ends_with(".cpp") || src.name.ends_with(".cc") || src.name.ends_with(".cxx"))
      any_cxx = true;
  for (const std::string& f : extra_files)
    if (f.ends_with(".cpp") || f.ends_with(".cc") || f.ends_with(".cxx"))
      any_cxx = true;
  if (any_cxx) {
#if defined(__APPLE__)
    link_args.push_back("-lc++");
#else
    link_args.push_back("-lstdc++");
#endif
  }
  link_args.push_back("-o");
  link_args.push_back(std::string(exe_path));
  for (const auto& f : ldflags)
    link_args.push_back(f);

  const fs::path link_log = tmp / "lld.log";
  std::string link_err;
  r.command += " && " + linker;
  bool first_arg = true;
  for (const auto& a : link_args) {
    if (first_arg) {
      first_arg = false;
      continue;
    } // link_args[0] 就是 linker，跳过避免重复
    r.command += " " + a;
  }
  if (run_subprocess(linker, link_args, link_log, &link_err) != 0) {
    r.error = "链接失败 (" + linker + "):\n" + link_err;
    return r;
  }
  if (!fs::exists(exe_path)) {
    r.error = "链接成功但未生成产物";
    return r;
  }
  r.ok = true;
  return r;
}

// ---- 核心入口：先尝试分段链路，失败回退 clang ----
LinkResult link_impl(std::string_view ll_ir, std::string_view exe_path, std::string_view target,
                     const std::vector<std::string>& extra_files, const std::vector<MemorySource>& extra_sources,
                     bool shared = false, const std::vector<std::string>& cflags = {},
                     const std::vector<std::string>& ldflags = {}) {
  // 分段链路先决条件：llc + cc + linker 都在；任一缺失则回退 clang 驱动
  if (!find_llc().empty() && !find_cc().empty() && !find_linker().empty()) {
    auto r = link_via_pipeline(ll_ir, exe_path, target, extra_files, extra_sources, shared, cflags, ldflags);
    if (r.ok)
      return r;
    // 失败则回退 clang（保留 r.error 便于诊断，clang 回退信息会附在 LinkResult.error）
    if (!find_clang().empty()) {
      auto fallback = link_via_clang(ll_ir, exe_path, target, extra_files, extra_sources, shared, cflags, ldflags);
      if (fallback.ok) {
        fallback.error = "(分段链路失败已自动回退 clang: " + r.error + ")";
        return fallback;
      }
    }
    return r;
  }
  // 直接走 clang
  return link_via_clang(ll_ir, exe_path, target, extra_files, extra_sources, shared, cflags, ldflags);
}

} // namespace

std::string host_triple() {
  return vx::util::host_triple();
}

std::string find_clang() {
  if (const char* env = std::getenv("VX_CLANG"); env != nullptr && env[0] != '\0' && ::access(env, X_OK) == 0)
    return env;
  return vx::util::find_in_path({"clang"});
}

std::string find_lld() {
  if (const char* off = std::getenv("VX_NO_LLD"); off != nullptr && off[0] != '\0')
    return {};
  return vx::util::find_in_path({"ld.lld"});
}

LinkResult link_executable(std::string_view ll_ir, std::string_view exe_path, std::string_view target) {
  return link_impl(ll_ir, exe_path, target, {}, {});
}

std::string generate_cpp_shim(const vx::compiler::ir::ModuleIR& mod) {
  std::string out;
  out += "// 由 vlnk 自动生成：@cppimp 的 C++ ABI → VX ABI shim\n";
  out += "#include <cstdint>\n";
  std::vector<std::string> headers;
  for (const vx::compiler::ir::LibDep& d : mod.libs) {
    if (!d.from_cpp || d.cpp_header.empty())
      continue;
    bool seen = false;
    for (const std::string& h : headers)
      seen = seen || h == d.cpp_header;
    if (!seen)
      headers.push_back(d.cpp_header);
  }
  for (const std::string& h : headers)
    out += "#include \"" + h + "\"\n";
  out += "\nextern \"C\" {\n";
  for (const vx::compiler::ir::LinkEntry& e : mod.linkage) {
    if (!e.from_cpp)
      continue;
    const std::string sym = e.abi_symbol.empty() ? e.name : e.abi_symbol;
    const std::string_view ret = vx::compiler::abi::c_type_of(e.ret_type);
    out += std::string(ret) + " " + sym + "(";
    std::string call_args;
    for (std::size_t i = 0; i < e.params.size(); ++i) {
      if (i != 0) {
        out += ", ";
        call_args += ", ";
      }
      const std::string pname = e.params[i].name.empty() ? ("a" + std::to_string(i)) : e.params[i].name;
      out += std::string(vx::compiler::abi::c_type_of(e.params[i].type)) + " " + pname;
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

LinkResult link_module(std::string_view ll_ir, const vx::compiler::ir::ModuleIR& mod, std::string_view exe_path,
                       std::string_view target, bool shared, const std::vector<std::string>& native_libs,
                       const std::vector<std::string>& cflags, const std::vector<std::string>& ldflags) {
  std::vector<std::string> extra_files;
  for (const vx::compiler::ir::LibDep& d : mod.libs) {
    if (d.impl_file.empty())
      continue;
    std::error_code ec;
    if (fs::exists(d.impl_file, ec))
      extra_files.push_back(d.impl_file);
  }
  for (const std::string& lib : native_libs)
    extra_files.push_back(lib);
  std::vector<MemorySource> extra_sources;
  bool any_cpp = false;
  for (const vx::compiler::ir::LibDep& d : mod.libs) {
    if (d.from_cpp && !d.cpp_header.empty())
      any_cpp = true;
  }
  if (any_cpp)
    extra_sources.push_back({"vxshim.cpp", generate_cpp_shim(mod)});
  return link_impl(ll_ir, exe_path, target, extra_files, extra_sources, shared, cflags, ldflags);
}

} // namespace vx::backend
