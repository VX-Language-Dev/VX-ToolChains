// analysis.hpp — vxls 文档分析层：复用 vx_compiler 前端产出 LSP 所需信息。
// 职责：按文档文本运行 lexer+parser，转换诊断为 LSP Diagnostic，
// 抽取文档符号（函数/类/结构/导入），并提供补全与悬停信息。
#pragma once

#include "vx/compiler/diag.hpp"

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace vxls {

// LSP 位置：0 起始（LSP 约定），character 按 UTF-16 码元计。
struct LspPosition {
  int line = 0;
  int character = 0;
};

// LSP 范围（含 start 不含 end）
struct LspRange {
  LspPosition start;
  LspPosition end;
};

// 一条 LSP 诊断
struct LspDiagnostic {
  LspRange range;
  int severity = 1; // 1=Error 2=Warning 3=Info 4=Hint
  std::string code;
  std::string message;
  std::string source = "vxls"; // 发布来源："vxls" / "vx-tidy"
};

// 文档符号（DocumentSymbol 平铺结构，供 textDocument/documentSymbol）
struct LspSymbol {
  std::string name;
  std::string detail;
  int kind = 0; // LSP SymbolKind
  LspRange range;         // 整体范围（行级）
  LspRange selection_range; // 名称范围
};

// 补全项
struct LspCompletion {
  std::string label;
  int kind = 0; // LSP CompletionItemKind
  std::string detail;
};

// 一次完整分析的结果（字符串均已自有，无 arena 依赖）
struct Analysis {
  std::vector<vx::compiler::Diagnostic> diags;
  std::vector<LspSymbol> symbols;
  std::vector<LspCompletion> completions;
  bool parse_ok = false;
  int line_count = 0;
};

// 运行词法+语法分析，填充诊断/符号/补全。text 为文档全文。
Analysis analyze(std::string_view text);

// 位置换算：诊断行列（1 起始，Unicode 码点）→ LSP 位置（0 起始，UTF-16）
LspPosition diag_pos_to_lsp(std::string_view text, int line, int col);

// 诊断 → LSP 诊断
LspDiagnostic to_lsp_diagnostic(const vx::compiler::Diagnostic &d,
                                std::string_view text);

// 悬停信息：给定 0 起始位置，返回富化的 Markdown 文本（空 = 无结果）。
// 内容按声明种类展开：
//   - 函数/extern：签名 + 参数表 + 返回类型 + 声明上方 # 文档注释
//   - class/struct：签名 + 字段与方法清单
//   - var/mut：类型标注 + 初始化表达式摘要
//   - 内置运行时符号：固定说明文档
//   - 导入库符号（vec.sum_to 形态）：ABI 符号与 .vxh 来源
// headers_root 为空时跳过导入库符号解析（其余 hover 不受影响）。
std::string hover_at(std::string_view text, int line, int character,
                     const std::filesystem::path &headers_root = {});

// 跳转定义：给定位置，返回目标声明的范围（找不到返回空范围 start.line<0）
LspRange definition_at(std::string_view text, int line, int character);

// vx-tidy 静态检查结果 → LSP 诊断（经 vxtidy 动态库）。
// VXTIDY900（语法错误透传）会被跳过：该类问题由前端诊断负责，避免重复。
std::vector<LspDiagnostic> tidy_diagnostics(std::string_view text,
                                            std::string_view filename);

} // namespace vxls
