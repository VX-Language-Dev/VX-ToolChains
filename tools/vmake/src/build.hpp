// build.hpp — 构建计划与编排：按依赖顺序调用 vxc（每源文件）→
// vlnk（合并链接）。 VMake 本身不生成任何机器码；它只是把 VMakeSet.vmake
// 翻译成正确的工具链调用。
#pragma once

#include "config.hpp"

#include <filesystem>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace vmake {

struct BuildOptions {
  std::filesystem::path build_dir;    // -B（绝对路径，已创建）
  std::filesystem::path manifest_dir; // VMakeSet.vmake 所在目录
  std::filesystem::path manifest_path;
  std::filesystem::path headers_root; // 导入库根（空 = 无），参与增量依赖签名
  std::vector<std::string> include_dirs; // -I 头文件搜索目录，透传给 vxc 并参与签名
  std::vector<std::string> lib_dirs_cli; // --lib-dir CLI：覆盖所有目标的 lib_dirs
  std::string target;                 // --target <triple>：交叉编译，透传 vxc+vlnk，入签名
  std::string profile = "debug";      // debug | release：追加链接期 clang 参数，入链接签名
  std::string vxc;
  std::string vlnk;
  std::vector<std::string> selected; // 空 = 全部
  std::vector<std::pair<std::string, std::string>> child_env;
  bool verbose = false;
  bool color = false;
  int jobs = 0; // 0 = 自动（CPU 核数）
};

// 目标构建顺序：动态库先于可执行文件（组内保持声明顺序），并按 selected 过滤。
std::vector<const Target *>
plan_order(const Manifest &manifest, const std::vector<std::string> &selected);

// 默认产物名：exe → <name>[.exe]；dylib → lib<name>.dylib / lib<name>.so /
// <name>.dll
std::string default_output_name(TargetKind kind, std::string_view name,
                                std::string_view host_os);

// 由原目标派生测试可执行目标：名字 <name>-test（kind=exe）。
// staticlib/dylib → 源 = 目标源 + 测试源；exe → 源 = 仅测试源（目标源含
// main，与测试入口在链接期冲突），经 deps / libs 引用被测代码；
// 继承 deps / libs / lib_dirs / cflags / defines / ldflags。
// 目标未声明 tests 时返回 std::nullopt。
std::optional<Target> test_target_of(const Target &target);

// 产物路径（-B 下）与中间 .vxobj 路径
std::filesystem::path artifact_path(const std::filesystem::path &build_dir,
                                    const Target &target);
std::filesystem::path object_path(const std::filesystem::path &build_dir,
                                  const Target &target,
                                  std::size_t source_index,
                                  std::string_view source);

// 执行构建；返回 0 成功，1 编译/链接失败（输出已打印），2 配置/环境错误。
int build_manifest(const Manifest &manifest, const BuildOptions &opts);

} // namespace vmake
