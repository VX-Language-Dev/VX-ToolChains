// config.hpp — VMakeSet.vmake 解析与目标模型（Python 风格函数调用）。
// 支持三类目标：
//   exe(...)       可执行文件
//   dylib(...)     动态链接库
//   staticlib(...) 静态库（ar 归档，无链接阶段）
// 字段：name（位置参数或 name=）、sources（必填，字符串数组）、output（可选，
// 构建目录相对路径）、deps（可选，依赖的目标名数组；exe/dylib 可依赖 dylib
// 或 staticlib；staticlib 本身不允许 deps）、libs（可选，外部库名或路径数组）、
// lib_dirs（可选，库搜索目录数组）、cflags / defines / ldflags（可选，链接期
// clang 参数与宏定义；staticlib 不允许）、tests（可选，测试源文件 .vx 数组，
// 构建为 <name>-test 可执行文件，由 vmake test 运行）。
#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace vmake {

enum class TargetKind { Exe, Dylib, StaticLib };

// "exe" / "dylib" / "staticlib"（用于输出与默认产物名）
const char *kind_name(TargetKind kind);

struct Target {
  std::string name;
  TargetKind kind = TargetKind::Exe;
  std::vector<std::string> sources;  // 相对 VMakeSet.vmake 所在目录
  std::optional<std::string> output; // 相对 -B 构建目录；空则用默认名
  std::vector<std::string> deps;     // 依赖的目标名（链接期并入其产物）
  std::vector<std::string> libs;     // 外部库：裸名 → -l<name>；含路径/后缀 → --lib <path>
  std::vector<std::string> lib_dirs; // 库搜索目录（-L<dir>，置于 -l 之前）
  // 链接期 clang 参数（.ll/.c/.cpp 与 vxrt 一并编译链接时生效，入链接签名）
  std::vector<std::string> cflags;  // 额外编译/链接参数（透传 clang）
  std::vector<std::string> defines; // 宏定义 → -D<name>[=<value>]
  std::vector<std::string> ldflags; // 链接器参数（置于 clang 命令末尾）
  std::vector<std::string> tests;   // 测试源文件（.vx）→ 构建 <name>-test
  std::size_t line = 0;              // 目标声明起始行（诊断）
};

struct Manifest {
  std::vector<Target> targets;
};

struct ParseError {
  std::size_t line = 0;
  std::string message;
};

// 解析 VMakeSet.vmake 文本（注释、exe()/dylib() 调用、字符串、字符串数组）。
// 失败时返回 nullopt，errors 含首个错误（带 1 起始行号）。
std::optional<Manifest> parse_manifest(std::string_view text,
                                       std::vector<ParseError> &errors);

} // namespace vmake
