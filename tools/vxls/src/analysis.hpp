// analysis.hpp — vxls 文档分析层：复用 vx_compiler 前端产出 LSP 所需信息。
// 职责：按文档文本运行 lexer+parser，转换诊断为 LSP Diagnostic，
// 抽取文档符号（函数/类/结构/导入），并提供补全与悬停信息。
#pragma once

#include "vx/compiler/diag.hpp"

#include <filesystem>
#include <map>
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
  int kind = 0;             // LSP SymbolKind
  LspRange range;           // 整体范围（行级）
  LspRange selection_range; // 名称范围
};

// 补全项
struct LspCompletion {
  std::string label;
  int kind = 0; // LSP CompletionItemKind
  std::string detail;
  std::string documentation; // Markdown 文档（空 = 无）
  std::string insert_text;   // 插入文本（空 = 用 label）
  std::string filter_text;   // 过滤文本（空 = 用 label）
  std::string sort_text;     // 排序键（空 = 编辑器自行排序）
  bool is_snippet = false;   // true 时 insert_text 为 LSP snippet（$1 占位）
};

// 内嵌提示（textDocument/inlayHint）
struct LspInlayHint {
  LspPosition position;
  std::string label;   // 展示文本（如 ": int" / "a:"）
  int kind = 1;        // 1=Type 2=Parameter
  std::string tooltip; // 悬停说明（空 = 无）
  bool padding_left = false;
  bool padding_right = false;
};

// 一次完整分析的结果（字符串均已自有，无 arena 依赖）
struct Analysis {
  std::vector<vx::compiler::Diagnostic> diags;
  std::vector<LspSymbol> symbols;
  bool parse_ok = false;
  int line_count = 0;
};

// 运行词法+语法分析，填充诊断/符号/补全。text 为文档全文。
Analysis analyze(std::string_view text);

// 位置换算：诊断行列（1 起始，Unicode 码点）→ LSP 位置（0 起始，UTF-16）
LspPosition diag_pos_to_lsp(std::string_view text, int line, int col);

// LSP 位置（0 起始，UTF-16）→ 字节偏移（用于增量编辑应用）
std::size_t lsp_pos_to_offset(std::string_view text, LspPosition pos);

// 诊断 → LSP 诊断
LspDiagnostic to_lsp_diagnostic(const vx::compiler::Diagnostic& d, std::string_view text);

// 悬停信息：给定 0 起始位置，返回富化的 Markdown 文本（空 = 无结果）。
// 内容按声明种类展开：
//   - 函数/extern：签名 + 参数表 + 返回类型 + 声明上方 # 文档注释
//   - class/struct：签名 + 字段与方法清单
//   - var/mut：类型标注 + 初始化表达式摘要
//   - 内置运行时符号：固定说明文档
//   - 导入库符号（vec.sum_to 形态）：ABI 符号与 .vxh 来源
// headers_root 为空时跳过导入库符号解析（其余 hover 不受影响）。
// include_dirs 为 -I 头文件搜索目录（优先于 headers_root，查找规则同 vxc）。
std::string hover_at(std::string_view text, int line, int character, const std::filesystem::path& headers_root = {},
                     const std::vector<std::filesystem::path>& include_dirs = {});

// 跳转定义：给定位置，返回目标声明的范围（找不到返回空范围 start.line<0）
LspRange definition_at(std::string_view text, int line, int character);

// 光标处标识符的范围（用于 prepareRename）；未命中返回 start.line<0
LspRange ident_range_at(std::string_view text, int line, int character);

// 光标处标识符名称（用于 prepareRename placeholder）；未命中返回空
std::string ident_name_at(std::string_view text, int line, int character);

// 文档内所有同名标识符位置（用于 documentHighlight）
std::vector<LspRange> document_highlights(std::string_view text, int line, int character);

// 签名帮助
struct LspParameterInformation {
  std::string label;
};
struct LspSignatureInformation {
  std::string label;
  std::vector<LspParameterInformation> parameters;
  std::string documentation;
};
struct LspSignatureHelp {
  std::vector<LspSignatureInformation> signatures;
  int active_signature = 0;
  int active_parameter = 0;
};

// 位置补全：按光标上下文给出候选（0 起始位置）。
// 上下文判定（基于光标前行文本，parse 失败时自动修补/降级）：
//   - `库前缀.` 或 `变量.` → 成员补全（导入库符号 / 类字段与方法）；
//     接收者无法解析时回退为全部类成员
//   - `var x:` / 参数注解 / `->` 后 → 类型补全（基础类型 + class/struct 名）
//   - 其余 → 作用域补全：当前函数可见的局部变量/参数/循环变量 + 顶层
//     函数/extern/类/全局变量 + 内置运行时 + 关键字，按 prefix 过滤并排序
// snippet_support 为 true 时可调用项给出 `name(${1:a})` 形式的 snippet 插入。
// headers_root 用于解析 @imp 导入库符号（空 = 跳过）。
// include_dirs 为 -I 头文件搜索目录（优先于 headers_root，查找规则同 vxc）。
std::vector<LspCompletion> completion_at(std::string_view text, int line, int character,
                                         const std::filesystem::path& headers_root = {},
                                         const std::vector<std::filesystem::path>& include_dirs = {},
                                         bool snippet_support = false);

// 签名帮助：光标在调用内时返回函数签名与活动参数
LspSignatureHelp signature_help_at(std::string_view text, int line, int character,
                                   const std::filesystem::path& headers_root = {},
                                   const std::vector<std::filesystem::path>& include_dirs = {});

// 内嵌提示：类型推断 + 调用参数名。
//   - 类型：`var x = <expr>` 未标注类型时，按初始化表达式推断（字面量 / new /
//     本文件与其它打开文档的顶层函数返回值 / 导入库符号返回值 / 内置运行时）
//     并在变量名后给出 `: T`
//   - 参数：函数调用实参前给出形参名（`f(1, 2)` → `f(a: 1, b: 2)`），
//     解析顺序为当前文档 → 其它打开文档 → 内置运行时；`库前缀.符号` 形态
//     经 @imp/@cppimp 导入表解析 .vxh 签名（需 headers_root）
// docs 为全部已打开文档（uri → 全文），uri 用于跳过当前文档自身。
// headers_root 为空时跳过导入库解析（其余提示不受影响）。
std::vector<LspInlayHint> inlay_hints(std::string_view text, const std::filesystem::path& headers_root = {},
                                      const std::vector<std::filesystem::path>& include_dirs = {},
                                      const std::map<std::string, std::string>& docs = {}, const std::string& uri = {});

// vx-tidy 静态检查结果 → LSP 诊断（经 vxtidy 动态库）。
// VXTIDY900（语法错误透传）会被跳过：该类问题由前端诊断负责，避免重复。
std::vector<LspDiagnostic> tidy_diagnostics(std::string_view text, std::string_view filename);

// ---- 跨文件语义（workspace 级别） ----

struct LspLocation {
  std::string uri;
  LspRange range;
};

struct LspWorkspaceSymbol {
  std::string name;
  int kind = 0;
  std::string uri;
  LspRange range;
};

struct LspTextEdit {
  LspRange range;
  std::string new_text;
};

std::vector<LspWorkspaceSymbol> workspace_symbols(const std::string& query,
                                                  const std::map<std::string, std::string>& docs);

LspLocation definition_at_cross(const std::string& uri, std::string_view text, int line, int character,
                                const std::map<std::string, std::string>& docs);

std::vector<LspLocation> references_at(const std::string& uri, std::string_view text, int line, int character,
                                       const std::map<std::string, std::string>& docs, bool include_declaration = true);

std::map<std::string, std::vector<LspTextEdit>> rename_at(const std::string& uri, std::string_view text, int line,
                                                          int character, const std::string& new_name,
                                                          const std::map<std::string, std::string>& docs);

} // namespace vxls
