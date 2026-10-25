// parser.hpp — VMakeSet.vmake 轻量解析器（带行/列位置，供 LSP 使用）。
// 语法（Python 风格函数调用）：
//   exe("hello", sources=["src/main.vx", "src/util.vx"])
//   dylib("mylib", sources=["lib.vx"], output="libmylib.dylib")
// 支持注释、尾随逗号、括号内换行、\uXXXX 转义。
#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace vmake_lsp {

struct Position {
  int line = 0;      // 0-based
  int character = 0; // 0-based，行内字节偏移（ASCII 场景下等同于字符偏移）
};

struct Range {
  Position start;
  Position end; // 不含 end
};

enum class TargetKind { Exe, Dylib };

// 单个目标声明及其各部分的位置（用于 hover / definition / completion）
struct VTarget {
  TargetKind kind = TargetKind::Exe;
  Range kind_range;      // "exe" / "dylib" 关键字范围
  std::string name;
  Range name_range;      // name 字符串值范围（不含引号）
  std::vector<std::string> sources;
  Range sources_range;   // 整个 [...] 数组范围
  Range sources_kw_range; // "sources" 字段名范围
  std::vector<Range> source_ranges; // 每个源字符串值范围
  std::optional<std::string> output;
  Range output_range;    // output 字符串值范围
  Range output_kw_range; // "output" 字段名范围
  Range call_range;      // 整个 exe(...) / dylib(...) 调用范围
};

// 一条诊断
struct Diagnostic {
  Range range;
  int severity = 1; // 1=Error
  std::string message;
};

// 解析结果
struct ParseResult {
  std::vector<VTarget> targets;
  std::vector<Diagnostic> diagnostics;
  bool ok = false;
};

// 目标类型关键字文本（"exe" / "dylib"）。
const char *kind_name(TargetKind kind);

// 解析 VMakeSet.vmake 文本，返回目标列表与诊断。
ParseResult parse_vmake(std::string_view text);

// 根据 0-based 行/列查找命中目标（用于 hover/definition/补全上下文）。
// 返回命中的目标索引及命中部分（kind/name/sources/output/arg_sep/none）。
struct Hit {
  std::size_t target_index = 0;
  enum class Part { None, Kind, Name, Sources, Output, SourceElem };
  Part part = Part::None;
};

Hit hit_test(const std::vector<VTarget> &targets, int line, int character);

} // namespace vmake_lsp
