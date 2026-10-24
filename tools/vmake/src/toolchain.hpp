// toolchain.hpp — 定位 VX 工具链（vxc 编译器 + vlnk 链接器）。
// 查找顺序：环境变量覆盖 → vmake 可执行文件附近（含 build/tools/vmake 布局）→
// PATH。
#pragma once

#include <filesystem>
#include <optional>
#include <string>

namespace vmake {

struct Toolchain {
  std::string vxc;  // 编译器可执行文件（vxc / vxc002）
  std::string vlnk; // 链接器可执行文件
};

// 查找工具链。argv0 为 vmake 自身路径（含 '/' 时用于推导同级/上级构建产物）。
// 失败时返回 nullopt，error 含具体缺失项与环境变量提示。
std::optional<Toolchain> find_toolchain(const std::filesystem::path &argv0,
                                        std::string &error);

} // namespace vmake
