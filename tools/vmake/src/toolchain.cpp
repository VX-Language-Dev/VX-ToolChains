// toolchain.cpp — 工具链定位实现（POSIX：access(X_OK) + PATH 扫描）。
#include "toolchain.hpp"

#include <cstdlib>
#include <string>
#include <vector>

#include <unistd.h>

namespace vmake {
namespace {

namespace fs = std::filesystem;

bool is_executable(const fs::path &p) {
  std::error_code ec;
  if (!fs::is_regular_file(p, ec) || ec)
    return false;
  return ::access(p.c_str(), X_OK) == 0;
}

// 在目录列表中按候选名顺序查找可执行文件
std::string find_in_dirs(const std::vector<std::string> &names,
                         const std::vector<fs::path> &dirs) {
  for (const fs::path &dir : dirs) {
    for (const std::string &name : names) {
      const fs::path cand = dir / name;
      if (is_executable(cand))
        return cand.string();
    }
  }
  return {};
}

// vmake 可执行文件所在目录及至多 3 层上级（覆盖 build/tools/vmake/vmake 布局）
std::vector<fs::path> exe_relative_dirs(const fs::path &argv0) {
  std::vector<fs::path> dirs;
  if (!argv0.has_parent_path())
    return dirs;
  std::error_code ec;
  fs::path dir = fs::absolute(argv0, ec).parent_path();
  if (ec || dir.empty())
    return dirs;
  for (int i = 0; i <= 3; ++i) {
    dirs.push_back(dir);
    if (!dir.has_parent_path())
      break;
    const fs::path parent = dir.parent_path();
    if (parent == dir)
      break;
    dir = parent;
  }
  return dirs;
}

std::vector<fs::path> path_dirs() {
  std::vector<fs::path> dirs;
  const char *path = std::getenv("PATH");
  if (!path)
    return dirs;
  const std::string text(path);
  std::size_t start = 0;
  while (start <= text.size()) {
    const std::size_t end = text.find(':', start);
    const std::string dir = end == std::string::npos
                                ? text.substr(start)
                                : text.substr(start, end - start);
    if (!dir.empty())
      dirs.emplace_back(dir);
    if (end == std::string::npos)
      break;
    start = end + 1;
  }
  return dirs;
}

// 单个工具查找：环境变量优先；显式指向不可执行文件时报错而非静默回退。
bool locate(const char *env_name, const std::vector<std::string> &names,
            const std::vector<fs::path> &dirs, const char *label,
            std::string &out, std::string &error) {
  if (const char *env = std::getenv(env_name); env && env[0] != '\0') {
    if (!is_executable(env)) {
      error = std::string(env_name) + " 指向的文件不可执行: " + env;
      return false;
    }
    out = env;
    return true;
  }
  out = find_in_dirs(names, dirs);
  if (out.empty()) {
    error = std::string("未找到 ") + label + "（可用环境变量 " + env_name +
            " 指定）";
    return false;
  }
  return true;
}

} // namespace

std::optional<Toolchain> find_toolchain(const fs::path &argv0,
                                        std::string &error) {
  std::vector<fs::path> dirs = exe_relative_dirs(argv0);
  const std::vector<fs::path> path = path_dirs();
  dirs.insert(dirs.end(), path.begin(), path.end());

  Toolchain tc;
  if (!locate("VX_VXC", {"vxc", "vxc002"}, dirs, "VX 编译器 (vxc)", tc.vxc,
              error)) {
    return std::nullopt;
  }
  if (!locate("VX_VLNK", {"vlnk"}, dirs, "VX 链接器 (vlnk)", tc.vlnk, error)) {
    return std::nullopt;
  }
  return tc;
}

} // namespace vmake
