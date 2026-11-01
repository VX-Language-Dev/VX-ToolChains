// format.hpp — vfmt 格式化核心：VX 源码 → 规范化源码。
#pragma once

#include <expected>
#include <string>
#include <string_view>
#include <vector>

namespace vfmt {

struct Options {
  int indent_width = 4; // 缩进宽度（空格数）
  int line_length = 88; // 行宽上限（超出仅报告，不强制折行）
  bool verify = true;   // 格式化前后做 AST 结构等价性自检
};

struct FormatResult {
  std::string text;            // 规范化后的源码
  std::vector<int> long_lines; // 超过行宽的行号（1 起始）
};

// 规范化格式化 VX 源码。
// 失败（词法/语法错误、格式化结果与原源码结构不一致）时返回错误描述。
std::expected<FormatResult, std::string> format(std::string_view src, const Options& opts = {});

} // namespace vfmt
