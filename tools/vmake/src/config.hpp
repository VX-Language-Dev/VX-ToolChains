// config.hpp — VMakeSet.vmake 解析与目标模型（Python 风格函数调用）。
// 支持三类目标：
//   exe(...)       可执行文件
//   dylib(...)     动态链接库
//   staticlib(...) 静态库（ar 归档，无链接阶段）
// 三类目标的 sources 字段均接受 .vx（由 vxc 编译）、.c / .cpp（由 clang
// -c 编译）混合；C/C++ 源的编译期参数独立于链接期参数。字段一览：
//   name / sources / output / deps / libs / lib_dirs（通用）
//   ccflags / cxxflags / incs / std       （clang -c 编译 C/C++ 源时生效）
//   cflags / defines / ldflags             （链接期 clang 生效；staticlib 禁
//                                           ldflags，仍允许 cflags/defines
//                                           供其 .c/.cpp 源的 clang -c 编译）
//   tests                                  （测试源文件，仅 .vx）
#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace vmake {

enum class TargetKind { Exe, Dylib, StaticLib };

// "exe" / "dylib" / "staticlib"（用于输出与默认产物名）
const char* kind_name(TargetKind kind);

// ---- 源文件分类（config 解析与 build 分流共用）----
// .vx → vxc；.c → clang -c（C）；.cpp/.cc/.cxx → clang -c（C++）
inline bool is_vx_source(std::string_view s) {
  return s.size() >= 3 && s.compare(s.size() - 3, 3, ".vx") == 0;
}
inline bool is_c_source(std::string_view s) {
  return s.size() >= 2 && s.back() == 'c' && s[s.size() - 2] == '.';
}
inline bool is_cxx_source(std::string_view s) {
  constexpr std::string_view exts[] = {".cpp", ".cc", ".cxx"};
  for (std::string_view e : exts) {
    if (s.size() >= e.size() && s.compare(s.size() - e.size(), e.size(), e) == 0)
      return true;
  }
  return false;
}

struct Target {
  std::string name;
  TargetKind kind = TargetKind::Exe;
  // 相对 VMakeSet.vmake 所在目录；支持 .vx / .c / .cpp 混合
  std::vector<std::string> sources;
  std::optional<std::string> output; // 相对 -B 构建目录；空则用默认名
  std::vector<std::string> deps;     // 依赖的目标名（链接期并入其产物）
  std::vector<std::string> libs;     // 外部库：裸名 → -l<name>；含路径/后缀 → --lib <path>
  std::vector<std::string> lib_dirs; // 库搜索目录（-L<dir>，置于 -l 之前）

  // ---- clang -c 编译 C/C++ 源的编译期参数（入 .c/.cpp 编译签名）----
  std::vector<std::string> ccflags;  // 额外 C 编译参数（clang -c .c）
  std::vector<std::string> cxxflags; // 额外 C++ 编译参数（clang -c .cpp）
  std::vector<std::string> incs;     // clang 头文件搜索目录（-I<dir>）
  std::optional<std::string> std_;   // C/C++ 标准（如 "c17" / "c++20"），映射为 -std=<v>

  // ---- 链接期 clang 参数（.ll/.c/.cpp 与 vxrt 一并编译链接时生效，入链接签名）----
  // staticlib 无链接阶段但有 .c/.cpp 源时，cflags/defines 也透传给 clang -c；
  // ldflags 仍禁止（无链接阶段）。
  std::vector<std::string> cflags;  // 额外编译/链接参数（透传 clang）
  std::vector<std::string> defines; // 宏定义 → -D<name>[=<value>]
  std::vector<std::string> ldflags; // 链接器参数（置于 clang 命令末尾）

  std::vector<std::string> tests; // 测试源文件（.vx）→ 构建 <name>-test
  std::size_t line = 0;           // 目标声明起始行（诊断）
};

// 目标是否含 .c/.cpp 源（决定是否需要 clang 与 C++ 运行时库）
inline bool has_native_source(const Target& t) {
  for (const std::string& s : t.sources) {
    if (is_c_source(s) || is_cxx_source(s))
      return true;
  }
  return false;
}

struct Manifest {
  std::vector<Target> targets;
};

struct ParseError {
  std::size_t line = 0;
  std::string message;
};

// 解析 VMakeSet.vmake 文本（注释、exe()/dylib() 调用、字符串、字符串数组）。
// 失败时返回 nullopt，errors 含首个错误（带 1 起始行号）。
std::optional<Manifest> parse_manifest(std::string_view text, std::vector<ParseError>& errors);

} // namespace vmake
