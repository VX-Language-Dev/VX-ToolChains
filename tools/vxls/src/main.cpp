// main.cpp — vxls：VX 语言服务器（LSP over stdio）。
// 支持 initialize/shutdown/exit、textDocument 同步与诊断发布、
// documentSymbol、hover、definition、completion。
//
// 命令行参数：
//   --stdio         标准 LSP 传输（默认行为，显式声明以便编辑器配置兼容）
//   --no-tidy       不启用 vx-tidy 静态检查（默认启用，诊断合并发布）
//   --only-tidy     仅运行 vx-tidy 后退出，不启动 LSP；参数为待检文件
//   --format=text|json  配合 --only-tidy 的输出格式（默认 text）
//   -V, --version   版本信息
//   -h, --help      帮助
#include "analysis.hpp"
#include "json.hpp"
#include "tidy.hpp"
#include "transport.hpp"
#include "vx/compiler/importer.hpp"
#include "vx/util/fs.hpp"

#include <cstdio>
#include <cstdlib>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

using vxls::json::Value;
using vxls::Analysis;
using vxls::LspMessage;

constexpr const char *kVersion = "0.2.0";

// 打开的文档集合（uri → 全文）
std::map<std::string, std::string> g_docs;

// 全局选项
bool g_tidy = true;              // 默认启用 vx-tidy 集成（--no-tidy 关闭）
fs::path g_headers_root;         // @imp 库根（hover 解析导入符号用）

// 对单个文档跑分析
Analysis analyze_doc(const std::string &uri) {
  auto it = g_docs.find(uri);
  if (it == g_docs.end()) return vxls::analyze("");
  return vxls::analyze(it->second);
}

Value range_value(const vxls::LspRange &r) {
  return Value::object(
      {{"start",
        Value::object({{"line", Value::number(r.start.line)},
                       {"character", Value::number(r.start.character)}})},
       {"end",
        Value::object({{"line", Value::number(r.end.line)},
                       {"character", Value::number(r.end.character)}})}});
}

Value diag_value(const vxls::LspDiagnostic &d) {
  return Value::object(
      {{"range", range_value(d.range)},
       {"severity", Value::number(d.severity)},
       {"code", Value::string(d.code)},
       {"source", Value::string(d.source)},
       {"message", Value::string(d.message)}});
}

// 发布诊断通知（textDocument/publishDiagnostics）
// 诊断源：前端诊断（lexer/parser）+ 可选 vx-tidy 静态检查（默认启用）
void publish_diagnostics(const std::string &uri) {
  Analysis a = analyze_doc(uri);
  const std::string text = g_docs.count(uri) ? g_docs[uri] : std::string();
  std::vector<vxls::LspDiagnostic> all;
  for (const auto &d : a.diags) {
    all.push_back(vxls::to_lsp_diagnostic(d, text));
  }
  if (g_tidy) {
    auto tidy_diags = vxls::tidy_diagnostics(text, uri);
    all.insert(all.end(), tidy_diags.begin(), tidy_diags.end());
  }
  std::vector<Value> diags;
  for (const auto &d : all) diags.push_back(diag_value(d));
  LspMessage n;
  n.method = "textDocument/publishDiagnostics";
  n.params = Value::object(
      {{"uri", Value::string(uri)}, {"diagnostics", Value::array(diags)}});
  vxls::send_message(n);
}

void handle_initialize(const LspMessage &req) {
  Value caps = Value::object(
      {{"textDocumentSync",
        Value::number(1)}, // Full：didChange 全量替换
       {"hoverProvider", Value::boolean(true)},
       {"definitionProvider", Value::boolean(true)},
       {"documentSymbolProvider", Value::boolean(true)},
       {"completionProvider",
        Value::object({{"triggerCharacters", Value::array({Value::string(".")})},
                       {"resolveProvider", Value::boolean(false)}})}});
  Value result = Value::object(
      {{"capabilities", caps},
       {"serverInfo",
        Value::object({{"name", Value::string("vxls")},
                       {"version", Value::string(kVersion)}})}});
  vxls::send_response(req, result);
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
  // Full 同步：取最后一个变更的全文
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
  publish_diagnostics(uri); // 清空诊断
}

// 取 params.textDocument.uri 与 params.position.{line,character}
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

void handle_document_symbol(const LspMessage &req) {
  const Value *td = req.params.find("textDocument");
  if (td == nullptr) {
    vxls::send_response(req, Value::array({}));
    return;
  }
  const std::string uri(td->str_or("uri"));
  Analysis a = analyze_doc(uri);
  std::vector<Value> syms;
  for (const auto &s : a.symbols) {
    syms.push_back(Value::object(
        {{"name", Value::string(s.name)},
         {"detail", Value::string(s.detail)},
         {"kind", Value::number(s.kind)},
         {"range", range_value(s.range)},
         {"selectionRange", range_value(s.selection_range)}}));
  }
  vxls::send_response(req, Value::array(syms));
}

void handle_hover(const LspMessage &req) {
  std::string uri;
  int line = 0, character = 0;
  if (!doc_and_pos(req.params, uri, line, character)) {
    vxls::send_response(req, Value{});
    return;
  }
  auto it = g_docs.find(uri);
  if (it == g_docs.end()) {
    vxls::send_response(req, Value{});
    return;
  }
  // hover_at 返回富化 Markdown（参数表/成员/文档注释/内置/导入库 ABI）
  const std::string md =
      vxls::hover_at(it->second, line, character, g_headers_root);
  if (md.empty()) {
    vxls::send_response(req, Value{});
    return;
  }
  vxls::send_response(
      req, Value::object(
               {{"contents",
                 Value::object({{"kind", Value::string("markdown")},
                                {"value", Value::string(md)}})}}));
}

void handle_definition(const LspMessage &req) {
  std::string uri;
  int line = 0, character = 0;
  if (!doc_and_pos(req.params, uri, line, character)) {
    vxls::send_response(req, Value{});
    return;
  }
  auto it = g_docs.find(uri);
  if (it == g_docs.end()) {
    vxls::send_response(req, Value{});
    return;
  }
  const vxls::LspRange r = vxls::definition_at(it->second, line, character);
  if (r.start.line < 0) {
    vxls::send_response(req, Value{});
    return;
  }
  vxls::send_response(
      req, Value::object({{"uri", Value::string(uri)},
                          {"range", range_value(r)}}));
}

void handle_completion(const LspMessage &req) {
  std::string uri;
  int line = 0, character = 0;
  if (!doc_and_pos(req.params, uri, line, character)) {
    vxls::send_response(req, Value::array({}));
    return;
  }
  Analysis a = analyze_doc(uri);
  std::vector<Value> items;
  for (const auto &c : a.completions) {
    items.push_back(Value::object({{"label", Value::string(c.label)},
                                   {"kind", Value::number(c.kind)},
                                   {"detail", Value::string(c.detail)}}));
  }
  vxls::send_response(req, Value::array(items));
}

void print_help() {
  std::fputs(
      "vxls — VX 语言服务器（LSP over stdio）\n"
      "Usage: vxls [options] [--only-tidy <file.vx>...]\n"
      "Options:\n"
      "  --stdio          标准 LSP 传输（默认，供编辑器配置兼容）\n"
      "  --no-tidy        不启用 vx-tidy 静态检查（默认启用并合并发布诊断）\n"
      "  --only-tidy      仅运行 vx-tidy 静态检查后退出，不启动 LSP\n"
      "  --format=text|json  --only-tidy 的输出格式（默认 text）\n"
      "  -V, --version    打印版本\n"
      "  -h, --help       显示本帮助\n",
      stdout);
}

// --only-tidy：对给定文件运行 vx-tidy 并打印（退出码 0=无 error，1=有 error）
int run_only_tidy(const std::vector<std::string> &files,
                  const std::string &format) {
  if (files.empty()) {
    std::fputs("error: --only-tidy 需要至少一个 .vx 文件\n", stderr);
    return 2;
  }
  bool any_error = false;
  bool first_json = true;
  if (format == "json") std::fputs("[", stdout);
  for (const std::string &f : files) {
    auto src = vx::util::read_file(f);
    if (!src) {
      std::fprintf(stderr, "error: cannot read %s: %s\n", f.c_str(),
                   src.error().c_str());
      any_error = true;
      continue;
    }
    const auto findings = vxtidy::check(*src, f);
    for (const auto &fd : findings) {
      if (fd.severity == 1) any_error = true;
    }
    if (format == "json") {
      std::string arr = vxtidy::format_json(findings, f);
      if (arr.size() > 2) {
        if (!first_json) std::fputs(",", stdout);
        std::fputs(arr.substr(1, arr.size() - 2).c_str(), stdout);
        first_json = false;
      }
    } else {
      for (const auto &fd : findings) {
        std::puts(vxtidy::format_text(fd, f).c_str());
      }
    }
  }
  if (format == "json") std::fputs("]\n", stdout);
  return any_error ? 1 : 0;
}

} // namespace

int main(int argc, char **argv) {
  std::vector<std::string> tidy_files;
  std::string format = "text";
  bool only_tidy = false;

  for (int i = 1; i < argc; ++i) {
    const std::string_view a = argv[i];
    if (a == "-h" || a == "--help") {
      print_help();
      return 0;
    }
    if (a == "-V" || a == "--version") {
      std::printf("vxls %s\n", kVersion);
      return 0;
    }
    if (a == "--stdio") {
      continue; // 默认行为，显式声明兼容编辑器配置
    }
    if (a == "--no-tidy") {
      g_tidy = false;
      continue;
    }
    if (a == "--only-tidy") {
      only_tidy = true;
      continue;
    }
    if (a == "--format" || a.starts_with("--format=")) {
      if (a == "--format") {
        if (i + 1 >= argc) {
          std::fputs("error: --format requires an argument\n", stderr);
          return 2;
        }
        format = argv[++i];
      } else {
        format = std::string(a.substr(9));
      }
      if (format != "text" && format != "json") {
        std::fputs("error: invalid --format value (text|json)\n", stderr);
        return 2;
      }
      continue;
    }
    if (a.starts_with("-")) {
      std::fprintf(stderr, "error: unknown option: %s (try --help)\n",
                   argv[i]);
      return 2;
    }
    tidy_files.emplace_back(a); // 位置参数仅 --only-tidy 时使用
  }

  // headers 根：VX_HEADERS 环境变量 或 可执行文件同级 headers/（导入库 hover）
  if (const char *env = std::getenv("VX_HEADERS")) {
    g_headers_root = env;
  } else {
    const fs::path exe_dir = vx::compiler::executable_dir(argv[0]);
    g_headers_root = exe_dir.empty() ? fs::path("headers") : exe_dir / "headers";
  }

  if (only_tidy) {
    return run_only_tidy(tidy_files, format);
  }
  if (!tidy_files.empty()) {
    std::fputs("error: 非 --only-tidy 模式不接受位置参数\n", stderr);
    return 2;
  }

  while (true) {
    auto msg = vxls::read_message();
    if (!msg) break; // EOF 或帧错误 → 退出
    const LspMessage &m = *msg;

    if (m.method == "initialize") {
      handle_initialize(m);
    } else if (m.method == "initialized") {
      // 通知，无需响应
    } else if (m.method == "shutdown") {
      vxls::send_response(m, Value{});
    } else if (m.method == "exit") {
      break;
    } else if (m.method == "textDocument/didOpen") {
      handle_did_open(m.params);
    } else if (m.method == "textDocument/didChange") {
      handle_did_change(m.params);
    } else if (m.method == "textDocument/didClose") {
      handle_did_close(m.params);
    } else if (m.method == "textDocument/documentSymbol") {
      handle_document_symbol(m);
    } else if (m.method == "textDocument/hover") {
      handle_hover(m);
    } else if (m.method == "textDocument/definition") {
      handle_definition(m);
    } else if (m.method == "textDocument/completion") {
      handle_completion(m);
    } else if (m.method == "$/cancelRequest" || m.method == "$/setTrace") {
      // 忽略
    } else if (m.has_id) {
      vxls::send_error(m, -32601, "方法不支持: " + m.method);
    }
  }
  return 0;
}
