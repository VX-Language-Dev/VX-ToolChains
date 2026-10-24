// build.hpp — 构建计划与编排：按依赖顺序调用 vxc（每源文件）→
// vlnk（合并链接）。 VMake 本身不生成任何机器码；它只是把 VMakeLists.toml
// 翻译成正确的工具链调用。
#pragma once

#include "config.hpp"

#include <filesystem>
#include <string>
#include <utility>
#include <vector>

namespace vmake {

struct BuildOptions {
  std::filesystem::path build_dir;    // -B（绝对路径，已创建）
  std::filesystem::path manifest_dir; // VMakeLists.toml 所在目录
  std::filesystem::path manifest_path;
  std::string vxc;
  std::string vlnk;
  std::vector<std::string> selected; // 空 = 全部
  std::vector<std::pair<std::string, std::string>> child_env;
  bool verbose = false;
  bool color = false;
};

// 目标构建顺序：动态库先于可执行文件（组内保持声明顺序），并按 selected 过滤。
std::vector<const Target *>
plan_order(const Manifest &manifest, const std::vector<std::string> &selected);

// 默认产物名：exe → <name>[.exe]；dylib → lib<name>.dylib / lib<name>.so /
// <name>.dll
std::string default_output_name(TargetKind kind, std::string_view name,
                                std::string_view host_os);

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
