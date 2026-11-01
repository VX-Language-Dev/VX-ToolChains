// platform.cpp — 宿主平台自动检测实现（编译期宏，无运行时开销）。
// 三元组形式对齐 LLVM / clang 的规范 triple，供 .ll 头与 clang -target 使用。
// 注：代码中的 _WIN32 分支仅为编译期探测兜底（Windows 须在 MSYS2/WSL 等
// POSIX 环境下构建运行），不代表支持 Windows 原生（PE/MSVC）产物。
#include "vx/util/platform.hpp"

#include <cstdlib>

#include <unistd.h>

namespace vx::util {

std::string host_arch() {
#if defined(__aarch64__) || defined(__arm64__) || defined(_M_ARM64)
  return "arm64";
#elif defined(__x86_64__) || defined(__amd64__) || defined(_M_X64) || defined(_M_AMD64)
  return "x86_64";
#elif defined(__arm__)
  return "arm";
#elif defined(__i386__) || defined(_M_IX86)
  return "i386";
#elif defined(__riscv)
  return "riscv64";
#else
  return "unknown";
#endif
}

std::string host_os() {
#if defined(__APPLE__)
  return "macosx";
#elif defined(__linux__)
  return "linux";
#elif defined(_WIN32) || defined(_WIN64)
  return "windows";
#elif defined(__unix__)
  return "unix";
#else
  return "unknown";
#endif
}

std::string host_triple() {
  const std::string arch = host_arch();
  const std::string os = host_os();
  if (os == "macosx") {
    return arch + "-apple-macosx";
  }
  if (os == "linux") {
    // Linux 上 clang 接受的规范 arch 名是 aarch64 而非 arm64
    const std::string linux_arch = (arch == "arm64") ? "aarch64" : arch;
    return linux_arch + "-unknown-linux-gnu";
  }
  if (os == "windows") {
    return arch + "-pc-windows-msvc";
  }
  return arch + "-unknown-" + os;
}

std::string host_name() {
  const std::string arch = host_arch();
  const std::string os = host_os();
  if (os == "macosx")
    return "macOS " + arch;
  if (os == "linux")
    return "Linux " + arch;
  if (os == "windows")
    return "Windows " + arch;
  return "Unknown " + arch;
}

std::string find_in_path(std::initializer_list<std::string_view> candidates) {
  const char* path = std::getenv("PATH");
  if (path == nullptr)
    return {};
  const std::string dirs(path);
  std::size_t start = 0;
  while (start <= dirs.size()) {
    const std::size_t end = dirs.find(':', start);
    const std::string dir = end == std::string::npos ? dirs.substr(start) : dirs.substr(start, end - start);
    if (!dir.empty()) {
      for (const std::string_view c : candidates) {
        const std::string cand = dir + "/" + std::string(c);
        if (::access(cand.c_str(), X_OK) == 0)
          return cand;
      }
    }
    if (end == std::string::npos)
      break;
    start = end + 1;
  }
  return {};
}

} // namespace vx::util
