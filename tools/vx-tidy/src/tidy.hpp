// tidy.hpp — vx-tidy 静态分析引擎：对 VX 源码运行 AST 级检查规则。
// 规则基于词法/语法分析产出的 AST 与作用域符号表，不依赖类型推断。
// 每条规则有稳定编号（VXTIDYxxx），供 --list-rules 与输出抑制使用。
#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace vxtidy {

// 一条检查发现
struct Finding {
  std::string rule;    // 规则编号，如 "VXTIDY001"
  int severity = 1;    // 1=Error 2=Warning 3=Note
  int line = 0;        // 1 起始
  int col = 0;         // 1 起始（0 = 未知）
  std::string message; // 中文描述
};

// 规则元数据（--list-rules 用）
struct RuleInfo {
  std::string rule;
  int severity;
  std::string name; // 规则短名
  std::string desc; // 说明
};

// 全部规则列表
const std::vector<RuleInfo> &all_rules();

// 运行静态分析；filename 仅用于发现的文件字段回填（Finding 不含文件，
// 由调用方打印时附加）。词法/语法错误同样以 VXTIDY900 收录（报文件错误）。
std::vector<Finding> check(std::string_view text, std::string_view filename);

// 文本格式渲染单条：file:line:col: severity: message [rule]
std::string format_text(const Finding &f, std::string_view filename);

// JSON 数组渲染（供 CI 消费）
std::string format_json(const std::vector<Finding> &fs, std::string_view filename);

} // namespace vxtidy
