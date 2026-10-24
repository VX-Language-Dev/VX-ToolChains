// link_driver.cpp — 后端链接驱动实现（fork/execvp 调用 host clang）。
// 内置 vxrt 运行时以字符串常量随链接注入；out/out_int/out_bool/out_float/
// vx_str_concat 由此提供，malloc/free/puts/exit 解析到 libc。
#include "vx/backend/link_driver.hpp"

#include "vx/compiler/vxabi.hpp"
#include "vx/util/platform.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>

namespace fs = std::filesystem;

namespace vx::backend {
namespace {

// 内置运行时（VX 运行时符号契约：out/out_int/out_bool/out_float/vx_str_concat）
constexpr const char *kRuntimeSource = R"VXRT(/* vxrt — VX 后端内置最小运行时 */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

void out(const char *s) { fputs(s, stdout); }
void out_int(int v) { printf("%d", v); }
void out_bool(int v) { fputs(v ? "true" : "false", stdout); }
void out_float(double v) { printf("%g", v); }

const char *vx_str_concat(const char *a, const char *b) {
  size_t la = strlen(a), lb = strlen(b);
  char *r = (char *)malloc(la + lb + 1);
  if (!r) return a;
  memcpy(r, a, la);
  memcpy(r + la, b, lb);
  r[la + lb] = '\0';
  return r;
}

/* 整数快速幂（VX ^ 运算符）：负指数按 0 处理 */
int32_t vx_pow_i32(int32_t base, int32_t exp) {
  if (exp < 0) return 0;
  int32_t r = 1;
  while (exp > 0) {
    if (exp & 1) r *= base;
    base *= base;
    exp >>= 1;
  }
  return r;
}

/* P4: 动态数组（元素统一为 8 字节槽） */
void *vx_array_new(int64_t n) {
  if (n < 0) n = 0;
  int64_t *p = (int64_t *)malloc((n + 1) * 8);
  if (!p) return 0;
  p[0] = n;
  for (int64_t i = 1; i <= n; i++) p[i] = 0;
  return (void *)p;
}
int64_t vx_array_len(void *a) {
  if (!a) return 0;
  return ((int64_t *)a)[0];
}
int64_t vx_array_get(void *a, int64_t i) {
  if (!a) return 0;
  int64_t *p = (int64_t *)a;
  int64_t n = p[0];
  if (i < 0 || i >= n) return 0;
  return p[i + 1];
}
void vx_array_set(void *a, int64_t i, int64_t v) {
  if (!a) return;
  int64_t *p = (int64_t *)a;
  int64_t n = p[0];
  if (i < 0 || i >= n) return;
  p[i + 1] = v;
}

/* P4: 简单字符串/整数键值映射（开放寻址，容量 2n+1，n>=1） */
typedef struct { int64_t key; int64_t is_str; char *keystr; int64_t val; int used; } VXMapEntry;
typedef struct { int64_t cap; int64_t size; VXMapEntry *entries; } VXMap;
void *vx_map_new(int64_t n) {
  if (n < 1) n = 1;
  int64_t cap = n * 2 + 1;
  VXMap *m = (VXMap *)malloc(sizeof(VXMap));
  if (!m) return 0;
  m->cap = cap;
  m->size = 0;
  m->entries = (VXMapEntry *)calloc((size_t)cap, sizeof(VXMapEntry));
  if (!m->entries) { free(m); return 0; }
  return (void *)m;
}
static int64_t vx_map_hash(int64_t key, const char *ks, int64_t is_str) {
  uint64_t h;
  if (is_str && ks) {
    h = 5381;
    for (const char *p = ks; *p; p++) h = h * 33 + (unsigned char)*p;
  } else {
    h = (uint64_t)key * 0x9e3779b97f4a7c15ULL;
  }
  return (int64_t)h;
}
void vx_map_set(void *m0, int64_t key, int64_t is_str, const char *keystr, int64_t val) {
  if (!m0) return;
  VXMap *m = (VXMap *)m0;
  int64_t idx = vx_map_hash(key, keystr, is_str) % m->cap;
  if (idx < 0) idx += m->cap;
  for (int64_t i = 0; i < m->cap; i++) {
    int64_t j = (idx + i) % m->cap;
    VXMapEntry *e = &m->entries[j];
    if (!e->used) {
      e->used = 1; e->key = key; e->is_str = is_str; e->keystr = keystr ? strdup(keystr) : 0; e->val = val; m->size++; return;
    }
    int match = 0;
    if (e->is_str && is_str) {
      match = (e->keystr && keystr && strcmp(e->keystr, keystr) == 0);
    } else if (!e->is_str && !is_str) {
      match = (e->key == key);
    }
    if (match) { e->val = val; return; }
  }
}
int64_t vx_map_get(void *m0, int64_t key, int64_t is_str, const char *keystr) {
  if (!m0) return 0;
  VXMap *m = (VXMap *)m0;
  int64_t idx = vx_map_hash(key, keystr, is_str) % m->cap;
  if (idx < 0) idx += m->cap;
  for (int64_t i = 0; i < m->cap; i++) {
    int64_t j = (idx + i) % m->cap;
    VXMapEntry *e = &m->entries[j];
    if (!e->used) return 0;
    int match = 0;
    if (e->is_str && is_str) {
      match = (e->keystr && keystr && strcmp(e->keystr, keystr) == 0);
    } else if (!e->is_str && !is_str) {
      match = (e->key == key);
    }
    if (match) return e->val;
  }
  return 0;
}
)VXRT";

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

// 查找预编译的 vxrt 运行时库（.a），避免每次重新编译 C 源。
static std::string find_vxrt_lib() {
  if (const char *env = std::getenv("VX_VXRT")) {
    if (env[0] != '\\0' && ::access(env, R_OK) == 0) return env;
  }
  std::vector<fs::path> cands;
  cands.push_back(fs::current_path() / "libvxrt.a");
  cands.push_back(fs::current_path() / "libvx_runtime.a");
  cands.push_back(fs::current_path() / "build" / "libvxrt.a");
  for (const fs::path &p : cands) {
    std::error_code ec;
    if (fs::is_regular_file(p, ec) && !ec) return p.string();
  }
  return {};
}

// 核心链接实现：.ll + vxrt.c + 附加磁盘文件 + 附加内存源 → 可执行文件/动态库。
LinkResult link_impl(std::string_view ll_ir, std::string_view exe_path,
                     std::string_view target,
                     const std::vector<std::string> &extra_files,
                     const std::vector<MemorySource> &extra_sources,
                     bool shared = false) {
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
      std::ofstream(rt_file, std::ios::binary) << kRuntimeSource;
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
  args.push_back("-Wno-override-module");
  r.command += " -Wno-override-module";
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
                       bool shared) {
  std::vector<std::string> extra_files;
  for (const vx::compiler::ir::LibDep &d : mod.libs) {
    if (d.impl_file.empty()) continue;
    std::error_code ec;
    if (fs::exists(d.impl_file, ec)) extra_files.push_back(d.impl_file);
  }
  std::vector<MemorySource> extra_sources;
  bool any_cpp = false;
  for (const vx::compiler::ir::LibDep &d : mod.libs) {
    if (d.from_cpp && !d.cpp_header.empty()) any_cpp = true;
  }
  if (any_cpp) extra_sources.push_back({"vxshim.cpp", generate_cpp_shim(mod)});
  return link_impl(ll_ir, exe_path, target, extra_files, extra_sources, shared);
}

} // namespace vx::backend
