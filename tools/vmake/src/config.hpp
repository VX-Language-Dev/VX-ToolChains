// config.hpp — VMakeSet.vmake 解析与目标模型（Python 风格函数调用）。
// 当前仅支持从源文件声明两类目标：
//   exe(...)    可执行文件
//   dylib(...)  动态链接库
// 字段：name（位置参数或 name=）、sources（必填，字符串数组）、output（可选，构建目录相对路径）。
#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace vmake {

enum class TargetKind { Exe, Dylib };

// "exe" / "dylib"（用于输出与默认产物名）
const char *kind_name(TargetKind kind);

struct Target {
  std::string name;
  TargetKind kind = TargetKind::Exe;
  std::vector<std::string> sources;  // 相对 VMakeSet.vmake 所在目录
  std::optional<std::string> output; // 相对 -B 构建目录；空则用默认名
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
