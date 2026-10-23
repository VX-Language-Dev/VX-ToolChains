// diag.hpp — Compiler 层统一诊断基础设施。
// 编译器最底层模块，先于 Lexer/Parser/Codegen 存在，供各阶段上报结构化诊断。
// 设计对齐原 Go 版 internal/diagnostics：严重级别、集中错误码、收集器与
// Cargo 风格渲染（`error[E2001]: ...` + `--> file:line:col` + 源码行 + `^` 指示）。
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace vx::compiler {

// 严重级别：数值越大越严重（排序时严重者在前）
enum class Severity : std::uint8_t { Info = 0, Warning = 1, Error = 2 };

// 级别名（渲染用）："info" / "warning" / "error"
std::string_view severity_name(Severity s);

// 级别权重：Info=0、Warning=1、Error=2
std::uint8_t severity_rank(Severity s);

// 集中错误码。E2xxx = 词法/语法（前端）；E0xxx 语义、E1xxx 所有权留待后续阶段。
namespace diag_code {
inline constexpr std::string_view sem_codegen = "E0001";                // 代码生成期语义错误
inline constexpr std::string_view sem_import = "E0002";                 // 库导入解析错误（@imp/@cppimp）
inline constexpr std::string_view lex_unknown_char = "E2001";           // 无法识别的字符
inline constexpr std::string_view lex_unterminated_string = "E2002";    // 字符串未闭合
inline constexpr std::string_view lex_incomplete_escape = "E2003";      // 转义序列不完整
inline constexpr std::string_view parse_expected_token = "E2010";       // 出现非期望 token
inline constexpr std::string_view parse_invalid_token_stream = "E2011"; // token 流不合法
} // namespace diag_code

// 一条诊断。位置按 1 起始计数；col=0 表示列未知（渲染时省略插入符）。
struct Diagnostic {
  std::string code;              // 错误码，可为空
  Severity severity = Severity::Info; // 默认最弱级别：宁可弱报，不可误报
  std::string message;           // 描述主体（不含文件/位置）
  std::string file;              // 源文件路径，可为空
  int line = 0;                  // 1 起始；0 表示未知
  int col = 0;                   // 1 起始；0 表示未知
  int end_line = 0;              // 可选 span 结束（含）
  int end_col = 0;
  std::string note;              // 可选补充说明
};

// 构造：仅起点位置
Diagnostic make_diag(std::string_view code, Severity sev, std::string message,
                     std::string file, int line, int col);

// 构造：带 span 结束位置（用于多字符高亮）
Diagnostic make_diag_span(std::string_view code, Severity sev,
                          std::string message, std::string file, int line,
                          int col, int end_line, int end_col);

// 诊断收集器：各阶段共用，聚合后可排序一次性输出。
class DiagBag {
public:
  void add(Diagnostic d);
  void add_all(const DiagBag &other);

  bool empty() const { return diags_.empty(); }
  std::size_t size() const { return diags_.size(); }

  // 是否含 Error 级诊断
  bool has_error() const;

  const std::vector<Diagnostic> &items() const { return diags_; }

  // 排序：file → line → col → severity（严重者在前）
  void sort();

  void clear() { diags_.clear(); }

private:
  std::vector<Diagnostic> diags_;
};

// 渲染单条诊断（Cargo 风格，末尾带换行）：
//   error[E2001]: 无法识别的字符 0x40
//     --> foo.vx:3:5
//      |
//    3 | var x = @
//      |         ^
// source 为空、或 line/col 未知时才省略源码摘录段；color=true 时上 ANSI 颜色。
std::string render_diagnostic(const Diagnostic &d, std::string_view source,
                              bool color);

// 渲染整个 bag（内部先按位置排序，不改动原 bag），并在末尾追加一行汇总：
//   error: 因 2 个错误中止编译（另有 1 个警告）
// source 为 bag 中诊断所对应文件的源码文本（单文件编译场景）。
std::string render_all(const DiagBag &bag, std::string_view source, bool color);

// 多文件渲染：source_of(file) 返回指定文件的源码文本，文件不存在时返回空。
// 适用于编译器/链接器/LSP 等多文件场景；每条诊断按自身的 file 字段取源码。
std::string render_all(
    const DiagBag &bag,
    std::function<std::string_view(std::string_view file)> source_of,
    bool color);

} // namespace vx::compiler
