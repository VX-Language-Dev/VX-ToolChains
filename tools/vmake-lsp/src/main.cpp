// main.cpp — vmake-lsp：VMakeSet.vmake 语言服务器（LSP over stdio）。
// 支持 initialize/shutdown/exit 生命周期、textDocument 同步（open/change/save/close）、
// 诊断（推送 publishDiagnostics + 拉取 textDocument/diagnostic）、
// documentSymbol、hover、definition、completion、formatting、foldingRange。
//
// 命令行参数：
//   --stdio    标准 LSP 传输（默认行为，显式声明以便编辑器配置兼容）
//   -V, --version   版本信息
//   -h, --help      帮助
#include "json.hpp"
#include "parser.hpp"
#include "transport.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace vmake_lsp {

using json::Value;

constexpr const char *kVersion = "0.3.0";

// 打开的文档集合（uri → 全文）
std::map<std::string, std::string> g_docs;

// LSP 生命周期状态（规范要求：initialize 之前的请求以 -32002 拒绝，
// shutdown 之后不再接受请求，未 shutdown 就 exit 时退出码为 1）
bool g_initialized = false;
bool g_shutdown = false;

ParseResult analyze_doc(const std::string &uri) {
  auto it = g_docs.find(uri);
  if (it == g_docs.end()) return ParseResult{};
  return parse_vmake(it->second);
}

Value range_value(const Range &r) {
  return Value::object(
      {{"start",
        Value::object({{"line", Value::number(r.start.line)},
                       {"character", Value::number(r.start.character)}})},
       {"end",
        Value::object({{"line", Value::number(r.end.line)},
                       {"character", Value::number(r.end.character)}})}});
}

Value diag_value(const Diagnostic &d) {
  return Value::object(
      {{"range", range_value(d.range)},
       {"severity", Value::number(d.severity)},
       {"code", Value::string("VMAKE")},
       {"source", Value::string("vmake-lsp")},
       {"message", Value::string(d.message)}});
}

// 诊断数组（推送与拉取共用）
std::vector<Value> diagnostic_items(const ParseResult &r) {
  std::vector<Value> diags;
  for (const auto &d : r.diagnostics) diags.push_back(diag_value(d));
  return diags;
}

// 发布诊断通知（textDocument/publishDiagnostics）
void publish_diagnostics(const std::string &uri) {
  ParseResult r = analyze_doc(uri);
  LspMessage n;
  n.method = "textDocument/publishDiagnostics";
  n.params = Value::object({{"uri", Value::string(uri)},
                            {"diagnostics", Value::array(diagnostic_items(r))}});
  send_message(n);
}

// 拉取式诊断（textDocument/diagnostic，LSP 3.17）
void handle_pull_diagnostics(const LspMessage &req) {
  const Value *td = req.params.find("textDocument");
  if (td == nullptr) {
    send_response(req, Value::object({{"kind", Value::string("full")},
                                      {"items", Value::array({})}}));
    return;
  }
  const std::string uri(td->str_or("uri"));
  ParseResult r = analyze_doc(uri);
  send_response(req, Value::object({{"kind", Value::string("full")},
                                    {"items", Value::array(diagnostic_items(r))}}));
}

void handle_initialize(const LspMessage &req) {
  Value caps = Value::object(
      {{"textDocumentSync",
        Value::object({{"openClose", Value::boolean(true)},
                       {"change", Value::number(1)}, // Full：didChange 全量替换
                       {"save", Value::object({})}})},
       // 拉取式诊断（textDocument/diagnostic），同时保留推送式 publishDiagnostics
       {"diagnosticProvider",
        Value::object({{"identifier", Value::string("vmake")},
                       {"interFileDependencies", Value::boolean(false)},
                       {"workspaceDiagnostics", Value::boolean(false)}})},
       {"documentSymbolProvider", Value::boolean(true)},
       {"hoverProvider", Value::boolean(true)},
       {"definitionProvider", Value::boolean(true)},
       {"formattingProvider", Value::boolean(true)},
       {"foldingRangeProvider", Value::boolean(true)},
       {"completionProvider",
        Value::object({{"triggerCharacters",
                        Value::array({Value::string("."),
                                      Value::string("="),
                                      Value::string("("),
                                      Value::string(","),
                                      Value::string("\""),
                                      Value::string("'")})},
                       {"resolveProvider", Value::boolean(false)}})}});
  Value result = Value::object(
      {{"capabilities", caps},
       {"serverInfo",
        Value::object({{"name", Value::string("vmake-lsp")},
                       {"version", Value::string(kVersion)}})}});
  send_response(req, result);
  g_initialized = true;
}

void handle_did_open(const Value &params) {
  const Value *td = params.find("textDocument");
  if (td == nullptr) return;
  const std::string uri(td->str_or("uri"));
  g_docs[uri] = std::string(td->str_or("text"));
  publish_diagnostics(uri);
}

void handle_did_change(const Value &params) {
  const Value *td = params.find("textDocument");
  const Value *changes = params.find("contentChanges");
  if (td == nullptr || changes == nullptr) return;
  const std::string uri(td->str_or("uri"));
  const auto &arr = changes->as_array();
  if (!arr.empty()) {
    g_docs[uri] = std::string(arr.back().str_or("text"));
  }
  publish_diagnostics(uri);
}

void handle_did_close(const Value &params) {
  const Value *td = params.find("textDocument");
  if (td == nullptr) return;
  const std::string uri(td->str_or("uri"));
  g_docs.erase(uri);
  publish_diagnostics(uri); // 推送空诊断以清除编辑器中的残留
}

// 保存后重新校验（textDocument/didSave）
void handle_did_save(const Value &params) {
  const Value *td = params.find("textDocument");
  if (td == nullptr) return;
  publish_diagnostics(std::string(td->str_or("uri")));
}

bool doc_and_pos(const Value &params, std::string &uri, int &line,
                 int &character) {
  const Value *td = params.find("textDocument");
  const Value *pos = params.find("position");
  if (td == nullptr || pos == nullptr) return false;
  uri = std::string(td->str_or("uri"));
  line = pos->int_or("line", 0);
  character = pos->int_or("character", 0);
  return true;
}

// 将文本按行拆分（不含换行符）
static std::vector<std::string> split_lines(std::string_view text) {
  std::vector<std::string> lines;
  std::size_t start = 0;
  while (start < text.size()) {
    std::size_t nl = text.find('\n', start);
    if (nl == std::string::npos) {
      lines.emplace_back(text.substr(start));
      break;
    }
    lines.emplace_back(text.substr(start, nl - start));
    start = nl + 1;
  }
  return lines;
}

// 判断给定位置是否落在范围内（start 含、end 不含）
static bool pos_in_range(int line, int character, const Range &r) {
  if (line < r.start.line || line > r.end.line) return false;
  if (line == r.start.line && character < r.start.character) return false;
  if (line == r.end.line && character >= r.end.character) return false;
  return true;
}

// 在目标列表中查找命中位置所在的源字符串索引
static std::size_t find_source_index(const VTarget &t, int line,
                                     int character) {
  for (std::size_t j = 0; j < t.source_ranges.size(); ++j) {
    if (pos_in_range(line, character, t.source_ranges[j])) return j;
  }
  return 0;
}

static std::size_t find_dep_index(const VTarget &t, int line, int character) {
  for (std::size_t j = 0; j < t.dep_ranges.size(); ++j) {
    if (pos_in_range(line, character, t.dep_ranges[j])) return j;
  }
  return 0;
}

void handle_document_symbol(const LspMessage &req) {
  const Value *td = req.params.find("textDocument");
  if (td == nullptr) {
    send_response(req, Value::array({}));
    return;
  }
  const std::string uri(td->str_or("uri"));
  ParseResult r = analyze_doc(uri);
  std::vector<Value> syms;
  for (const auto &t : r.targets) {
    std::string detail = kind_name(t.kind);
    detail += " · ";
    detail += std::to_string(t.sources.size());
    detail += t.sources.size() == 1 ? " source" : " sources";
    if (t.output) {
      detail += " → output=";
      detail += *t.output;
    }
    syms.push_back(Value::object(
        {{"name", Value::string(t.name)},
         {"detail", Value::string(detail)},
         {"kind", Value::number(12)}, // SymbolKind.Function
         {"range", range_value(t.call_range)},
         {"selectionRange", range_value(t.name_range)}}));
  }
  send_response(req, Value::array(syms));
}

void handle_hover(const LspMessage &req) {
  std::string uri;
  int line = 0, character = 0;
  if (!doc_and_pos(req.params, uri, line, character)) {
    send_response(req, Value{});
    return;
  }
  auto it = g_docs.find(uri);
  if (it == g_docs.end()) {
    send_response(req, Value{});
    return;
  }
  ParseResult r = analyze_doc(uri);
  Hit hit = hit_test(r.targets, line, character);
  if (hit.part == Hit::Part::None) {
    send_response(req, Value{});
    return;
  }
  const auto &t = r.targets[hit.target_index];
  std::string md;
  switch (hit.part) {
  case Hit::Part::Kind:
    md = "### `" + std::string(kind_name(t.kind)) + "` 目标类型\n";
    md += "声明一个 ";
    md += (t.kind == TargetKind::Exe ? "可执行文件" :
           t.kind == TargetKind::Dylib ? "动态链接库" : "静态库（ar 归档）");
    md += " 目标。\n";
    break;
  case Hit::Part::Name:
    md = "### 目标 `" + t.name + "`\n";
    md += "- 类型：`" + std::string(kind_name(t.kind)) + "`\n";
    md += "- 源文件：";
    for (std::size_t i = 0; i < t.sources.size(); ++i) {
      if (i) md += ", ";
      md += "`" + t.sources[i] + "`";
    }
    md += "\n";
    if (t.output) md += "- 输出：`" + *t.output + "`\n";
    if (!t.deps.empty()) {
      md += "- 依赖：";
      for (std::size_t i = 0; i < t.deps.size(); ++i) {
        if (i) md += ", ";
        md += "`" + t.deps[i] + "`";
      }
      md += "\n";
    }
    if (!t.libs.empty()) {
      md += "- 外部库：";
      for (std::size_t i = 0; i < t.libs.size(); ++i) {
        if (i) md += ", ";
        md += "`" + t.libs[i] + "`";
      }
      md += "\n";
    }
    if (!t.lib_dirs.empty()) {
      md += "- 库搜索目录：";
      for (std::size_t i = 0; i < t.lib_dirs.size(); ++i) {
        if (i) md += ", ";
        md += "`" + t.lib_dirs[i] + "`";
      }
      md += "\n";
    }
    break;
  case Hit::Part::Sources:
    md = "### `sources`\n";
    md += "目标 `" + t.name + "` 的源文件列表（相对 VMakeSet.vmake 所在目录）。\n";
    break;
  case Hit::Part::SourceElem: {
    std::size_t idx = find_source_index(t, line, character);
    md = "### 源文件\n";
    md += "`" + t.sources[idx] + "`\n";
    break;
  }
  case Hit::Part::Output:
    md = "### `output`\n";
    md += "目标 `" + t.name + "` 的输出路径（相对构建目录）：`";
    if (t.output) md += *t.output;
    md += "`\n";
    break;
  case Hit::Part::Deps:
    md = "### `deps`\n";
    md += "目标 `" + t.name + "` 依赖的目标名列表；被依赖方必须是 dylib 或 staticlib 目标，"
          "构建时先于依赖方构建，链接期以 `--lib` 并入，产物变化时依赖方自动重链。\n";
    break;
  case Hit::Part::DepElem: {
    std::size_t idx = find_dep_index(t, line, character);
    md = "### 依赖目标\n";
    md += "`" + (idx < t.deps.size() ? t.deps[idx] : std::string()) + "`\n";
    break;
  }
  case Hit::Part::Libs:
    md = "### `libs`\n";
    md += "目标 `" + t.name + "` 的外部库列表；裸名 → `-l<name>`，路径或带后缀 → `--lib <path>`。\n";
    break;
  case Hit::Part::LibElem: {
    std::size_t idx = 0;
    for (; idx < t.lib_ranges.size(); ++idx) {
      if (pos_in_range(line, character, t.lib_ranges[idx])) break;
    }
    md = "### 外部库\n";
    md += "`" + (idx < t.libs.size() ? t.libs[idx] : std::string()) + "`\n";
    break;
  }
  case Hit::Part::LibDirs:
    md = "### `lib_dirs`\n";
    md += "目标 `" + t.name + "` 的库搜索目录列表；每个目录以 `-L<dir>` 传给链接器。\n";
    break;
  case Hit::Part::LibDirElem: {
    std::size_t idx = 0;
    for (; idx < t.lib_dir_ranges.size(); ++idx) {
      if (pos_in_range(line, character, t.lib_dir_ranges[idx])) break;
    }
    md = "### 库搜索目录\n";
    md += "`" + (idx < t.lib_dirs.size() ? t.lib_dirs[idx] : std::string()) + "`\n";
    break;
  }
  case Hit::Part::ExtraField: {
    const ArrayField &f = t.extra_arrays[hit.extra_index];
    md = "### `" + f.key + "`\n";
    if (f.key == "cflags") {
      md += "目标 `" + t.name + "` 的额外 clang 参数；链接期与 .ll/.c/.cpp 一并"
            "传给 clang，并纳入增量链接签名（变化仅重链，不重编）。\n";
    } else if (f.key == "defines") {
      md += "目标 `" + t.name + "` 的宏定义；每项转为 `-D<name>[=<value>]` "
            "传给链接期的 clang。\n";
    } else if (f.key == "ldflags") {
      md += "目标 `" + t.name + "` 的链接器参数；置于链接期 clang 命令末尾"
            "（如 `-Wl,-rpath,...`）。\n";
    } else {
      md += "目标 `" + t.name + "` 的测试源文件（.vx）；`vmake test` 将其与目标"
            "源文件一并构建为 `" + t.name + "-test` 并运行。\n";
    }
    break;
  }
  case Hit::Part::ExtraElem: {
    const ArrayField &f = t.extra_arrays[hit.extra_index];
    const std::string v =
        hit.elem_index < f.values.size() ? f.values[hit.elem_index]
                                         : std::string();
    if (f.key == "tests") {
      md = "### 测试文件\n`" + v + "`\n";
    } else if (f.key == "defines") {
      md = "### 宏定义\n`" + v + "` → `-D" + v + "`\n";
    } else {
      md = "### `" + f.key + " 参数\n`" + v + "`\n";
    }
    break;
  }
  default:
    send_response(req, Value{});
    return;
  }
  send_response(req, Value::object(
               {{"contents",
                 Value::object({{"kind", Value::string("markdown")},
                                {"value", Value::string(md)}})}}));
}

// 将「清单 URI 所在目录 + 相对源路径」拼成源文件 URI（如 file:///tmp/src/main.vx）。
// 仅支持 file: 方案；目标文件不存在时返回空，避免跳进不存在的缓冲区。
std::optional<std::string> resolve_source_uri(const std::string &manifest_uri,
                                              const std::string &rel) {
  static const std::string kScheme = "file://";
  if (!manifest_uri.starts_with(kScheme)) return std::nullopt;
  std::string path = manifest_uri.substr(kScheme.size());
  if (const std::size_t q = path.find_first_of("?#"); q != std::string::npos) {
    path.erase(q); // 防御：去掉 query/fragment
  }
  const std::size_t slash = path.find_last_of('/');
  if (slash == std::string::npos) return std::nullopt;
  const std::string full = path.substr(0, slash + 1) + rel;
  std::error_code ec;
  if (!std::filesystem::is_regular_file(full, ec)) return std::nullopt;
  return kScheme + full;
}

void handle_definition(const LspMessage &req) {
  std::string uri;
  int line = 0, character = 0;
  if (!doc_and_pos(req.params, uri, line, character)) {
    send_response(req, Value{});
    return;
  }
  auto it = g_docs.find(uri);
  if (it == g_docs.end()) {
    send_response(req, Value{});
    return;
  }
  ParseResult r = analyze_doc(uri);
  Hit hit = hit_test(r.targets, line, character);
  if (hit.part == Hit::Part::None) {
    send_response(req, Value{});
    return;
  }
  const auto &t = r.targets[hit.target_index];
  Range r_;
  switch (hit.part) {
  case Hit::Part::Name:
    r_ = t.name_range;
    break;
  case Hit::Part::Kind:
    r_ = t.kind_range;
    break;
  case Hit::Part::SourceElem: {
    // 源文件 → 跳到磁盘上的 .vx 文件（相对清单所在目录）
    const std::size_t idx = find_source_index(t, line, character);
    if (std::optional<std::string> src_uri = resolve_source_uri(uri, t.sources[idx])) {
      send_response(req, Value::object(
                             {{"uri", Value::string(*src_uri)},
                              {"range",
                               range_value(Range{Position{0, 0}, Position{0, 0}})}}));
      return;
    }
    send_response(req, Value{}); // 文件不存在 → 不提供跳转
    return;
  }
  case Hit::Part::Output:
    r_ = t.output_range;
    break;
  case Hit::Part::DepElem: {
    // 依赖名 → 跳到同文档中被依赖目标的声明
    const std::size_t idx = find_dep_index(t, line, character);
    if (idx < t.deps.size()) {
      for (const auto &other : r.targets) {
        if (other.name == t.deps[idx]) {
          send_response(req, Value::object(
                                 {{"uri", Value::string(uri)},
                                  {"range", range_value(other.name_range)}}));
          return;
        }
      }
    }
    send_response(req, Value{}); // 未找到被依赖目标 → 不提供跳转
    return;
  }
  case Hit::Part::ExtraElem: {
    // tests 元素 → 跳到磁盘上的 .vx 测试文件；其余字段不提供跳转
    const ArrayField &f = t.extra_arrays[hit.extra_index];
    if (f.key == "tests" && hit.elem_index < f.values.size()) {
      if (std::optional<std::string> src_uri =
              resolve_source_uri(uri, f.values[hit.elem_index])) {
        send_response(req, Value::object(
                               {{"uri", Value::string(*src_uri)},
                                {"range", range_value(
                                              Range{Position{0, 0}, Position{0, 0}})}}));
        return;
      }
    }
    send_response(req, Value{});
    return;
  }
  default:
    send_response(req, Value{});
    return;
  }
  send_response(req, Value::object({{"uri", Value::string(uri)},
                                    {"range", range_value(r_)}}));
}

// 补全项构造（可选 insertText / 片段格式）
Value make_completion(const std::string &label, int kind,
                      const std::string &detail,
                      const std::string &insert_text = {},
                      bool snippet = false) {
  std::vector<std::pair<std::string, Value>> members;
  members.emplace_back("label", Value::string(label));
  members.emplace_back("kind", Value::number(kind));
  members.emplace_back("detail", Value::string(detail));
  if (!insert_text.empty()) {
    members.emplace_back("insertText", Value::string(insert_text));
    if (snippet) members.emplace_back("insertTextFormat", Value::number(2));
  }
  return Value::object(std::move(members));
}

// 补全上下文：由光标前的文本扫描得出
enum class Ctx {
  None,
  Statement,  // 语句位置：exe / dylib
  ArgName,    // 调用参数位置：字段名
  ArgValue,   // '=' 之后：字段值
};

static bool is_ident_start(char ch) {
  return (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') || ch == '_';
}

static bool is_ident_char(char ch) {
  return is_ident_start(ch) || (ch >= '0' && ch <= '9') || ch == '-';
}

// 光标前的扫描结果
struct ScanCtx {
  int depth = 0;          // 圆括号嵌套深度（0 表示在调用之外）
  char last_sig = '\0';   // 最后一个非空白字符
  std::string last_ident; // 最后一个标识符
  std::string field;      // 最后一个 '=' 左侧的标识符
  bool in_string = false; // 光标是否落在字符串字面量内
};

// 扫描光标之前的文本：统计括号深度、跟踪最后的关键字符，跳过注释与字符串
static ScanCtx scan_before(const std::string &text, int line, int character) {
  auto lines = split_lines(text);
  ScanCtx sc;
  for (int l = 0; l <= line && l < (int)lines.size(); ++l) {
    const std::string &ln = lines[l];
    int end = (l == line) ? std::min(character, (int)ln.size())
                          : (int)ln.size();
    char quote = '\0';
    for (int i = 0; i < end; ++i) {
      char c = ln[i];
      if (quote != '\0') { // 字符串内部
        if (c == '\\' && quote == '"') { ++i; continue; }
        if (c == quote) quote = '\0';
        continue;
      }
      if (c == '#') { // 注释直到行尾
        i = end;
        continue;
      }
      if (c == '"' || c == '\'') {
        quote = c;
        sc.last_sig = c;
        sc.last_ident.clear();
        continue;
      }
      if (c == '(') {
        ++sc.depth;
        sc.last_sig = c;
        sc.last_ident.clear();
        continue;
      }
      if (c == ')') {
        if (sc.depth > 0) --sc.depth;
        sc.last_sig = c;
        sc.last_ident.clear();
        continue;
      }
      if (c == ' ' || c == '\t' || c == '\r') continue;
      if (c == ',') {
        sc.last_sig = c;
        sc.last_ident.clear();
        sc.field.clear();
        continue;
      }
      if (c == '=') {
        sc.last_sig = c;
        sc.field = sc.last_ident;
        sc.last_ident.clear();
        continue;
      }
      if (is_ident_start(c)) {
        int j = i;
        while (j < end && is_ident_char(ln[j])) ++j;
        sc.last_ident = ln.substr(i, j - i);
        sc.last_sig = sc.last_ident.back();
        i = j - 1;
        continue;
      }
      sc.last_sig = c;
      sc.last_ident.clear();
    }
    sc.in_string = quote != '\0';
  }
  return sc;
}

void handle_completion(const LspMessage &req) {
  std::string uri;
  int line = 0, character = 0;
  if (!doc_and_pos(req.params, uri, line, character)) {
    send_response(req, Value::array({}));
    return;
  }
  auto it = g_docs.find(uri);
  if (it == g_docs.end()) {
    send_response(req, Value::array({}));
    return;
  }
  const std::string &text = it->second;

  ScanCtx sc = scan_before(text, line, character);
  if (sc.in_string) { // 字符串字面量内部不提供补全
    send_response(req, Value::array({}));
    return;
  }

  std::vector<Value> items;
  if (sc.depth == 0) {
    // 语句位置：目标类型
    items.push_back(make_completion("exe", 3, "声明可执行文件目标", "exe"));
    items.push_back(make_completion("dylib", 3, "声明动态链接库目标", "dylib"));
    items.push_back(make_completion("staticlib", 3, "声明静态库目标（ar 归档）", "staticlib"));
  } else if (sc.last_sig == '=') {
    // 字段值：按字段名给出片段
    if (sc.field == "name") {
      items.push_back(
          make_completion("\"\"", 1, "目标名称（字符串）", "\"$0\"", true));
    } else if (sc.field == "sources") {
      items.push_back(make_completion("[\"\"]", 1, "源文件列表", "[\"$0\"]", true));
    } else if (sc.field == "output") {
      items.push_back(
          make_completion("\"\"", 1, "输出路径（字符串）", "\"$0\"", true));
    } else if (sc.field == "deps") {
      items.push_back(
          make_completion("[\"\"]", 1, "依赖目标名列表（dylib/staticlib）", "[\"$0\"]", true));
    } else if (sc.field == "libs") {
      items.push_back(
          make_completion("[\"\"]", 1, "外部库列表", "[\"$0\"]", true));
    } else if (sc.field == "lib_dirs") {
      items.push_back(
          make_completion("[\"\"]", 1, "库搜索目录列表", "[\"$0\"]", true));
    } else if (sc.field == "cflags") {
      items.push_back(
          make_completion("[\"\"]", 1, "额外 clang 参数列表", "[\"$0\"]", true));
    } else if (sc.field == "defines") {
      items.push_back(
          make_completion("[\"\"]", 1, "宏定义列表（→ -D...）", "[\"$0\"]", true));
    } else if (sc.field == "ldflags") {
      items.push_back(
          make_completion("[\"\"]", 1, "链接器参数列表", "[\"$0\"]", true));
    } else if (sc.field == "tests") {
      items.push_back(
          make_completion("[\"\"]", 1, "测试源文件列表（.vx）", "[\"$0\"]", true));
    }
  } else {
    // 参数位置：关键字字段名（首个位置参数为目标名）
    if (sc.last_sig == '(') {
      items.push_back(
          make_completion("\"\"", 1, "目标名称（字符串）", "\"$0\"", true));
    }
    items.push_back(make_completion("name", 5, "目标名称", "name"));
    items.push_back(make_completion("sources", 5, "源文件列表", "sources"));
    items.push_back(make_completion("output", 5, "输出路径", "output"));
    items.push_back(
        make_completion("deps", 5, "依赖目标名列表（dylib/staticlib）", "deps"));
    items.push_back(make_completion("libs", 5, "外部库列表", "libs"));
    items.push_back(make_completion("lib_dirs", 5, "库搜索目录列表", "lib_dirs"));
    items.push_back(make_completion("cflags", 5, "额外 clang 参数列表", "cflags"));
    items.push_back(make_completion("defines", 5, "宏定义列表（→ -D...）", "defines"));
    items.push_back(make_completion("ldflags", 5, "链接器参数列表", "ldflags"));
    items.push_back(make_completion("tests", 5, "测试源文件列表（.vx）", "tests"));
  }

  send_response(req, Value::array(items));
}

// ---- 格式化与折叠 ----

// 计算 (line, character) 在全文中的字节偏移
static std::size_t offset_of(const std::vector<std::string> &lines, int line,
                             int character) {
  std::size_t off = 0;
  for (int i = 0; i < line && i < (int)lines.size(); ++i) {
    off += lines[i].size() + 1; // +1 为换行符
  }
  if (line >= 0 && line < (int)lines.size()) {
    int c = character;
    if (c < 0) c = 0;
    if (c > (int)lines[line].size()) c = (int)lines[line].size();
    off += (std::size_t)c;
  }
  return off;
}

// 取范围覆盖的源文本
static std::string slice_range(const std::string &text, const Range &r) {
  auto lines = split_lines(text);
  std::size_t begin = offset_of(lines, r.start.line, r.start.character);
  std::size_t end = offset_of(lines, r.end.line, r.end.character);
  if (end < begin) end = begin;
  if (begin > text.size()) begin = text.size();
  if (end > text.size()) end = text.size();
  return text.substr(begin, end - begin);
}

// 范围内是否含注释（字符串内的 '#' 不算）
static bool range_has_comment(const std::string &text, const Range &r) {
  std::string s = slice_range(text, r);
  char quote = '\0';
  for (std::size_t i = 0; i < s.size(); ++i) {
    char c = s[i];
    if (quote != '\0') {
      if (c == '\\' && quote == '"') { ++i; continue; }
      if (c == quote) quote = '\0';
      continue;
    }
    if (c == '"' || c == '\'') { quote = c; continue; }
    if (c == '#') return true;
  }
  return false;
}

// 字符串值重新编码为双引号字面量
static std::string quote_string(std::string_view s) {
  std::string out = "\"";
  for (char c : s) {
    switch (c) {
    case '"': out += "\\\""; break;
    case '\\': out += "\\\\"; break;
    case '\n': out += "\\n"; break;
    case '\t': out += "\\t"; break;
    case '\r': out += "\\r"; break;
    default: out += c; break;
    }
  }
  out += '"';
  return out;
}

// 目标声明的规范单行形式
static std::string format_target(const VTarget &t) {
  std::string out = kind_name(t.kind);
  out += '(';
  out += quote_string(t.name);
  out += ", sources=[";
  for (std::size_t i = 0; i < t.sources.size(); ++i) {
    if (i) out += ", ";
    out += quote_string(t.sources[i]);
  }
  out += ']';
  if (t.output) {
    out += ", output=";
    out += quote_string(*t.output);
  }
  if (t.has_deps && !t.deps.empty()) {
    out += ", deps=[";
    for (std::size_t i = 0; i < t.deps.size(); ++i) {
      if (i) out += ", ";
      out += quote_string(t.deps[i]);
    }
    out += ']';
  }
  if (t.has_libs && !t.libs.empty()) {
    out += ", libs=[";
    for (std::size_t i = 0; i < t.libs.size(); ++i) {
      if (i) out += ", ";
      out += quote_string(t.libs[i]);
    }
    out += ']';
  }
  if (t.has_lib_dirs && !t.lib_dirs.empty()) {
    out += ", lib_dirs=[";
    for (std::size_t i = 0; i < t.lib_dirs.size(); ++i) {
      if (i) out += ", ";
      out += quote_string(t.lib_dirs[i]);
    }
    out += ']';
  }
  // cflags / defines / ldflags / tests：按声明顺序输出
  for (const ArrayField &f : t.extra_arrays) {
    out += ", ";
    out += f.key;
    out += "=[";
    for (std::size_t i = 0; i < f.values.size(); ++i) {
      if (i) out += ", ";
      out += quote_string(f.values[i]);
    }
    out += ']';
  }
  out += ')';
  return out;
}

void handle_formatting(const LspMessage &req) {
  const Value *td = req.params.find("textDocument");
  if (td == nullptr) {
    send_response(req, Value::array({}));
    return;
  }
  const std::string uri(td->str_or("uri"));
  auto it = g_docs.find(uri);
  if (it == g_docs.end()) {
    send_response(req, Value::array({}));
    return;
  }
  ParseResult r = analyze_doc(uri);
  if (!r.diagnostics.empty()) {
    // 存在错误时不重排，避免破坏用户正在编辑的内容
    send_response(req, Value::array({}));
    return;
  }
  const std::string &text = it->second;
  std::vector<Value> edits;
  for (const auto &t : r.targets) {
    if (range_has_comment(text, t.call_range)) continue; // 含注释的目标保持原样
    std::string formatted = format_target(t);
    if (slice_range(text, t.call_range) == formatted) continue;
    edits.push_back(Value::object(
        {{"range", range_value(t.call_range)},
         {"newText", Value::string(formatted)}}));
  }
  send_response(req, Value::array(edits));
}

void handle_folding_range(const LspMessage &req) {
  const Value *td = req.params.find("textDocument");
  if (td == nullptr) {
    send_response(req, Value::array({}));
    return;
  }
  const std::string uri(td->str_or("uri"));
  ParseResult r = analyze_doc(uri);
  std::vector<Value> ranges;
  auto add_fold = [&](const Range &rg) {
    if (rg.end.line <= rg.start.line) return; // 单行无需折叠
    ranges.push_back(Value::object(
        {{"startLine", Value::number(rg.start.line)},
         {"startCharacter", Value::number(rg.start.character)},
         {"endLine", Value::number(rg.end.line)},
         {"endCharacter", Value::number(rg.end.character)},
         {"kind", Value::string("region")}}));
  };
  for (const auto &t : r.targets) {
    add_fold(t.call_range);
    add_fold(t.sources_range);
    for (const auto &f : t.extra_arrays)
      add_fold(f.range);
  }
  send_response(req, Value::array(ranges));
}

} // namespace vmake_lsp

int main(int argc, char **argv) {
  for (int i = 1; i < argc; ++i) {
    const std::string_view a = argv[i];
    if (a == "-h" || a == "--help") {
      std::fputs(
          "vmake-lsp — VMakeSet.vmake 语言服务器（LSP over stdio）\n"
          "Usage: vmake-lsp [options]\n"
          "Options:\n"
          "  --stdio          标准 LSP 传输（默认，供编辑器配置兼容）\n"
          "  -V, --version    打印版本\n"
          "  -h, --help       显示本帮助\n",
          stdout);
      return 0;
    }
    if (a == "-V" || a == "--version") {
      std::printf("vmake-lsp %s\n", vmake_lsp::kVersion);
      return 0;
    }
    if (a == "--stdio") {
      continue;
    }
    if (a.starts_with("-")) {
      std::fprintf(stderr, "error: unknown option: %s (try --help)\n",
                   argv[i]);
      return 2;
    }
  }

  while (true) {
    auto msg = vmake_lsp::read_message();
    if (!msg) break; // EOF 或帧错误 → 退出
    const vmake_lsp::LspMessage &m = *msg;

    // 生命周期：initialize 之前的请求按规范以 -32002 拒绝
    if (!vmake_lsp::g_initialized) {
      if (m.method == "initialize") {
        vmake_lsp::handle_initialize(m);
        continue;
      }
      if (m.has_id) {
        vmake_lsp::send_error(m, -32002, "Server not initialized");
        continue;
      }
      if (m.method == "initialized" || m.method == "exit") {
        if (m.method == "exit") break;
        continue;
      }
      continue; // 其余通知一律忽略
    }

    // 生命周期：shutdown 之后不再接受请求
    if (vmake_lsp::g_shutdown) {
      if (m.has_id) {
        vmake_lsp::send_error(m, -32600, "服务器已 shutdown，不再接受请求");
        continue;
      }
      if (m.method == "exit") break;
      continue;
    }

    if (m.method == "initialized") {
      // 通知，无需响应
    } else if (m.method == "shutdown") {
      vmake_lsp::g_shutdown = true;
      vmake_lsp::send_response(m, vmake_lsp::json::Value{});
    } else if (m.method == "exit") {
      break;
    } else if (m.method == "textDocument/didOpen") {
      vmake_lsp::handle_did_open(m.params);
    } else if (m.method == "textDocument/didChange") {
      vmake_lsp::handle_did_change(m.params);
    } else if (m.method == "textDocument/didClose") {
      vmake_lsp::handle_did_close(m.params);
    } else if (m.method == "textDocument/didSave") {
      vmake_lsp::handle_did_save(m.params);
    } else if (m.method == "textDocument/documentSymbol") {
      vmake_lsp::handle_document_symbol(m);
    } else if (m.method == "textDocument/hover") {
      vmake_lsp::handle_hover(m);
    } else if (m.method == "textDocument/definition") {
      vmake_lsp::handle_definition(m);
    } else if (m.method == "textDocument/completion") {
      vmake_lsp::handle_completion(m);
    } else if (m.method == "textDocument/diagnostic") {
      vmake_lsp::handle_pull_diagnostics(m);
    } else if (m.method == "textDocument/formatting") {
      vmake_lsp::handle_formatting(m);
    } else if (m.method == "textDocument/foldingRange") {
      vmake_lsp::handle_folding_range(m);
    } else if (m.method == "$/cancelRequest" || m.method == "$/setTrace") {
      // 忽略
    } else if (m.has_id) {
      vmake_lsp::send_error(m, -32601, "方法不支持: " + m.method);
    }
  }
  // 规范：未收到 shutdown 就退出时返回非零
  return vmake_lsp::g_shutdown ? 0 : 1;
}
