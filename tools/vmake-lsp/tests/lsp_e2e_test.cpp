// vmake_lsp_e2e_test — vmake-lsp 端到端测试：以真实子进程走完整 LSP 会话。
// 覆盖：initialize 能力协商、didOpen/didChange 诊断推送、documentSymbol、hover、
// definition、completion、formatting、foldingRange、shutdown/exit。
#include "json.hpp"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>
#include <vector>

#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>

#ifndef VMAKE_LSP_BIN_PATH
#error "VMAKE_LSP_BIN_PATH 未定义（由 CMake 注入）"
#endif

namespace {

int g_failures = 0;

#define CHECK(cond)                                                            \
  do {                                                                         \
    if (!(cond)) {                                                             \
      std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);     \
      ++g_failures;                                                            \
    }                                                                          \
  } while (0)

namespace json = vx::json;

// ---- LSP 客户端（子进程 + Content-Length 帧） ----
class Client {
public:
  ~Client() { stop(); }

  bool start() {
    int in_pipe[2];
    int out_pipe[2];
    if (pipe(in_pipe) != 0) return false;
    if (pipe(out_pipe) != 0) {
      close(in_pipe[0]);
      close(in_pipe[1]);
      return false;
    }
    pid_ = fork();
    if (pid_ < 0) return false;
    if (pid_ == 0) {
      dup2(in_pipe[0], STDIN_FILENO);
      dup2(out_pipe[1], STDOUT_FILENO);
      close(in_pipe[0]);
      close(in_pipe[1]);
      close(out_pipe[0]);
      close(out_pipe[1]);
      execl(VMAKE_LSP_BIN_PATH, "vmake-lsp", "--stdio", (char *)nullptr);
      _exit(127);
    }
    close(in_pipe[0]);
    close(out_pipe[1]);
    in_fd_ = in_pipe[1];
    out_fd_ = out_pipe[0];
    return true;
  }

  void stop() {
    if (in_fd_ >= 0) {
      close(in_fd_);
      in_fd_ = -1;
    }
    if (out_fd_ >= 0) {
      close(out_fd_);
      out_fd_ = -1;
    }
    if (pid_ > 0) {
      int status = 0;
      waitpid(pid_, &status, 0);
      pid_ = -1;
    }
  }

  // 发送一条已构造好的 JSON 消息
  bool send_raw(const std::string &body) {
    const std::string header =
        "Content-Length: " + std::to_string(body.size()) + "\r\n\r\n";
    if (!write_all(header)) return false;
    return write_all(body);
  }

  // 发送通知（无 id）
  bool notify(const std::string &method, const json::Value &params) {
    json::Value msg = json::Value::object(
        {{"jsonrpc", json::Value::string("2.0")},
         {"method", json::Value::string(method)},
         {"params", params}});
    return send_raw(msg.dump());
  }

  // 发送请求并等待对应 id 的响应；期间收到的通知放入 notifications
  json::Value request(const std::string &method, const json::Value &params,
                      std::vector<json::Value> &notifications) {
    const int id = next_id_++;
    json::Value msg = json::Value::object(
        {{"jsonrpc", json::Value::string("2.0")},
         {"id", json::Value::number(id)},
         {"method", json::Value::string(method)},
         {"params", params}});
    if (!send_raw(msg.dump())) return json::Value();
    for (;;) {
      json::Value v;
      if (!read_value(v)) return json::Value();
      const json::Value *rid = v.find("id");
      if (rid == nullptr) {
        notifications.push_back(v);
        continue;
      }
      if ((int)rid->as_number() == id) return v;
    }
  }

  // 读取下一条消息（通知或响应）
  bool read_value(json::Value &out) {
    std::size_t length = 0;
    bool found = false;
    for (;;) {
      std::string header;
      if (!read_line(header)) return false;
      if (header.empty()) break; // 空行表示头部结束
      std::string lower = header;
      for (char &c : lower) {
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
      }
      const std::string key = "content-length:";
      const std::size_t pos = lower.find(key);
      if (pos != std::string::npos) {
        length = (std::size_t)std::atoll(lower.c_str() + pos + key.size());
        found = true;
      }
    }
    if (!found) return false;
    std::string body;
    body.resize(length);
    std::size_t got = 0;
    while (got < length) {
      ssize_t n = ::read(out_fd_, body.data() + got, length - got);
      if (n <= 0) return false;
      got += (std::size_t)n;
    }
    auto parsed = json::parse(body);
    if (!parsed) return false;
    out = *parsed;
    return true;
  }

  // 等待子进程退出并返回退出码
  int wait_exit() {
    if (in_fd_ >= 0) {
      close(in_fd_);
      in_fd_ = -1;
    }
    if (out_fd_ >= 0) {
      close(out_fd_);
      out_fd_ = -1;
    }
    if (pid_ <= 0) return -1;
    int status = 0;
    if (waitpid(pid_, &status, 0) < 0) return -1;
    pid_ = -1;
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
  }

private:
  bool write_all(const std::string &s) {
    std::size_t done = 0;
    while (done < s.size()) {
      ssize_t n = ::write(in_fd_, s.data() + done, s.size() - done);
      if (n <= 0) return false;
      done += (std::size_t)n;
    }
    return true;
  }

  // 读一行（不含换行符）
  bool read_line(std::string &out) {
    out.clear();
    char c = 0;
    for (;;) {
      ssize_t n = ::read(out_fd_, &c, 1);
      if (n <= 0) return false;
      if (c == '\n') {
        if (!out.empty() && out.back() == '\r') out.pop_back();
        return true;
      }
      out += c;
    }
  }

  pid_t pid_ = -1;
  int in_fd_ = -1;
  int out_fd_ = -1;
  int next_id_ = 1;
};

// ---- 测试辅助 ----
json::Value pos_params(const std::string &uri, int line, int character) {
  return json::Value::object(
      {{"textDocument", json::Value::object({{"uri", json::Value::string(uri)}})},
       {"position",
        json::Value::object({{"line", json::Value::number(line)},
                              {"character", json::Value::number(character)}})}});
}

json::Value doc_params(const std::string &uri) {
  return json::Value::object(
      {{"textDocument", json::Value::object({{"uri", json::Value::string(uri)}})}});
}

// 数组元素个数
std::size_t array_size(const json::Value *v) {
  if (v == nullptr || v->type() != json::Value::Type::Array) return 0;
  return v->as_array().size();
}

// 结果是否为 null（如「无定义」）
bool result_is_null(const json::Value &v) {
  const json::Value *r = v.find("result");
  return r != nullptr && r->is_null();
}

// 建立临时项目目录：<dir>/VMakeSet.vmake 与 <dir>/src/main.vx
std::string make_project() {
  char tmpl[] = "/tmp/vmake-lsp-e2e-XXXXXX";
  char *dir = mkdtemp(tmpl);
  if (dir == nullptr) return {};
  const std::string root(dir);
  std::string src_dir = root + "/src";
  if (mkdir(src_dir.c_str(), 0755) != 0 && errno != EEXIST) return {};
  const std::string src_file = src_dir + "/main.vx";
  std::FILE *f = std::fopen(src_file.c_str(), "w");
  if (f == nullptr) return {};
  std::fputs("// e2e fixture\n", f);
  std::fclose(f);
  return root;
}

void test_session() {
  Client client;
  if (!client.start()) {
    std::fprintf(stderr, "无法启动 vmake-lsp 子进程\n");
    ++g_failures;
    return;
  }

  const std::string project = make_project();
  if (project.empty()) {
    std::fprintf(stderr, "无法创建临时项目目录\n");
    ++g_failures;
    return;
  }
  const std::string uri = "file://" + project + "/VMakeSet.vmake";
  std::vector<json::Value> notifications;

  // 1. initialize：能力声明
  json::Value init = client.request(
      "initialize",
      json::Value::object({{"capabilities", json::Value::object({})}}), notifications);
  const json::Value *caps = init.find("result") ? init.find("result")->find("capabilities") : nullptr;
  CHECK(caps != nullptr);
  if (caps != nullptr) {
    CHECK(caps->find("documentSymbolProvider") != nullptr);
    CHECK(caps->find("hoverProvider") != nullptr);
    CHECK(caps->find("definitionProvider") != nullptr);
    CHECK(caps->find("completionProvider") != nullptr);
    CHECK(caps->find("formattingProvider") != nullptr);
    CHECK(caps->find("foldingRangeProvider") != nullptr);
  }
  client.notify("initialized", json::Value::object({}));

  // 2. didOpen：合法文件 → 无诊断
  const std::string good =
      "exe(\"app\", sources=[\n  \"src/main.vx\",\n  \"src/util.vx\",\n])\n"
      "\n"
      "dylib(\"calc\", sources=[\"src/calc.vx\"], output=\"libcalc.dylib\")\n";
  client.notify("textDocument/didOpen",
                json::Value::object({{"textDocument",
                                      json::Value::object(
                                          {{"uri", json::Value::string(uri)},
                                           {"languageId", json::Value::string("vmake")},
                                           {"version", json::Value::number(1)},
                                           {"text", json::Value::string(good)}})}}));
  json::Value diag_msg;
  CHECK(client.read_value(diag_msg));
  CHECK(diag_msg.find("method") != nullptr);
  if (diag_msg.find("method") != nullptr) {
    CHECK(diag_msg.find("method")->as_string() == "textDocument/publishDiagnostics");
  }
  const json::Value *diags =
      diag_msg.find("params") ? diag_msg.find("params")->find("diagnostics") : nullptr;
  CHECK(array_size(diags) == 0);

  // 2b. 拉取式诊断（textDocument/diagnostic）：合法文件 → 空列表
  json::Value pull =
      client.request("textDocument/diagnostic", doc_params(uri), notifications);
  const json::Value *pull_items =
      pull.find("result") ? pull.find("result")->find("items") : nullptr;
  CHECK(pull_items != nullptr);
  CHECK(array_size(pull_items) == 0);

  // 3. documentSymbol：两个目标
  json::Value syms = client.request("textDocument/documentSymbol", doc_params(uri), notifications);
  const json::Value *sym_list = syms.find("result");
  CHECK(array_size(sym_list) == 2);
  if (array_size(sym_list) == 2) {
    CHECK(sym_list->as_array()[0].find("name")->as_string() == "app");
    CHECK(sym_list->as_array()[1].find("name")->as_string() == "calc");
  }

  // 4. hover：目标名
  json::Value hov =
      client.request("textDocument/hover", pos_params(uri, 5, 8), notifications);
  const json::Value *contents = hov.find("result") ? hov.find("result")->find("contents") : nullptr;
  CHECK(contents != nullptr);
  if (contents != nullptr) {
    const json::Value *value = contents->find("value");
    CHECK(value != nullptr);
    if (value != nullptr) {
      CHECK(value->as_string().find("calc") != std::string::npos);
    }
  }

  // 5. definition：跳到目标名
  json::Value def =
      client.request("textDocument/definition", pos_params(uri, 5, 9), notifications);
  const json::Value *def_range = def.find("result") ? def.find("result")->find("range") : nullptr;
  CHECK(def_range != nullptr);
  if (def_range != nullptr) {
    const json::Value *start = def_range->find("start");
    CHECK(start != nullptr);
    if (start != nullptr) {
      CHECK((int)start->int_or("line", -1) == 5);
      CHECK((int)start->int_or("character", -1) == 7);
    }
  }

  // 5b. definition：源文件元素 → 磁盘上的 .vx 文件（<清单目录>/src/main.vx）
  json::Value def_src =
      client.request("textDocument/definition", pos_params(uri, 1, 4), notifications);
  const json::Value *src_uri =
      def_src.find("result") ? def_src.find("result")->find("uri") : nullptr;
  CHECK(src_uri != nullptr);
  if (src_uri != nullptr) {
    CHECK(src_uri->as_string() == "file://" + project + "/src/main.vx");
  }

  // 5c. 磁盘上不存在的源文件 → null（不跳转）
  json::Value def_missing =
      client.request("textDocument/definition", pos_params(uri, 2, 4), notifications);
  CHECK(result_is_null(def_missing));

  // 6. completion：字段值位置给出 sources 片段
  json::Value comp =
      client.request("textDocument/completion", pos_params(uri, 5, 22), notifications);
  const json::Value *items = comp.find("result");
  CHECK(array_size(items) == 1);
  if (array_size(items) == 1) {
    const json::Value &it = items->as_array()[0];
    CHECK(it.find("label")->as_string() == "[\"\"]");
    CHECK(it.find("insertText") != nullptr);
    if (it.find("insertText")) {
      CHECK(it.find("insertText")->as_string() == "[\"$0\"]");
    }
  }

  // 7. formatting：规范化为单行
  client.notify("textDocument/didChange",
                json::Value::object({{"textDocument",
                                      json::Value::object(
                                          {{"uri", json::Value::string(uri)},
                                           {"version", json::Value::number(2)}})},
                                     {"contentChanges",
                                      json::Value::array({json::Value::object(
                                          {{"text", json::Value::string(
                                                        "exe( \"app\" ,sources=[\"a.vx\"] )\n")}})})}}));
  json::Value changed;
  CHECK(client.read_value(changed)); // 诊断通知
  json::Value fmt = client.request(
      "textDocument/formatting",
      json::Value::object({{"textDocument",
                            json::Value::object({{"uri", json::Value::string(uri)}})},
                           {"options", json::Value::object({{"tabSize", json::Value::number(2)}})}}),
      notifications);
  const json::Value *edits = fmt.find("result");
  CHECK(array_size(edits) == 1);
  if (array_size(edits) == 1) {
    const json::Value &e = edits->as_array()[0];
    CHECK(e.find("newText") != nullptr);
    if (e.find("newText")) {
      CHECK(e.find("newText")->as_string() == "exe(\"app\", sources=[\"a.vx\"])");
    }
  }

  // 8. foldingRange：多行目标可折叠
  client.notify("textDocument/didChange",
                json::Value::object({{"textDocument",
                                      json::Value::object(
                                          {{"uri", json::Value::string(uri)},
                                           {"version", json::Value::number(3)}})},
                                     {"contentChanges",
                                      json::Value::array({json::Value::object(
                                          {{"text", json::Value::string(good)}})})}}));
  json::Value changed2;
  CHECK(client.read_value(changed2));
  json::Value folds =
      client.request("textDocument/foldingRange", doc_params(uri), notifications);
  CHECK(array_size(folds.find("result")) >= 1);

  // 9. didChange：非法文件 → 出现诊断
  client.notify("textDocument/didChange",
                json::Value::object({{"textDocument",
                                      json::Value::object(
                                          {{"uri", json::Value::string(uri)},
                                           {"version", json::Value::number(4)}})},
                                     {"contentChanges",
                                      json::Value::array({json::Value::object(
                                          {{"text", json::Value::string(
                                                        "exe(\"a\", sources=[\"a.txt\"])\n")}})})}}));
  json::Value bad_diag;
  CHECK(client.read_value(bad_diag));
  const json::Value *bad_list =
      bad_diag.find("params") ? bad_diag.find("params")->find("diagnostics") : nullptr;
  CHECK(array_size(bad_list) == 1);
  if (array_size(bad_list) == 1) {
    CHECK(bad_list->as_array()[0].find("message")->as_string().find(".vx") != std::string::npos);
  }

  // 9b. didSave → 重新推送诊断
  client.notify("textDocument/didSave",
                json::Value::object({{"textDocument",
                                      json::Value::object(
                                          {{"uri", json::Value::string(uri)}})}}));
  json::Value save_diag;
  CHECK(client.read_value(save_diag));
  const json::Value *save_list =
      save_diag.find("params") ? save_diag.find("params")->find("diagnostics") : nullptr;
  CHECK(array_size(save_list) == 1);

  // 9c. 拉取式诊断：同一错误也能取到
  json::Value pull_bad =
      client.request("textDocument/diagnostic", doc_params(uri), notifications);
  const json::Value *pull_bad_items =
      pull_bad.find("result") ? pull_bad.find("result")->find("items") : nullptr;
  CHECK(array_size(pull_bad_items) == 1);

  // 10. shutdown / exit
  json::Value sd = client.request("shutdown", json::Value::object({}), notifications);
  CHECK(sd.find("result") != nullptr);
  client.notify("exit", json::Value::object({}));
  CHECK(client.wait_exit() == 0);
}

// 生命周期：initialize 前的请求、shutdown 后的请求、exit 的退出码
void test_lifecycle() {
  std::vector<json::Value> notifications;

  // 1) 未 initialize 就发请求 → -32002；未 shutdown 就 exit → 退出码 1
  {
    Client c;
    if (!c.start()) {
      std::fprintf(stderr, "无法启动 vmake-lsp 子进程\n");
      ++g_failures;
      return;
    }
    json::Value r = c.request("textDocument/documentSymbol",
                              doc_params("file:///x/VMakeSet.vmake"), notifications);
    const json::Value *err = r.find("error");
    CHECK(err != nullptr);
    if (err != nullptr) CHECK((int)err->int_or("code", 0) == -32002);
    c.notify("exit", json::Value::object({}));
    CHECK(c.wait_exit() == 1);
  }

  // 2) 正常 initialize → shutdown → 此后请求被拒（-32600）→ exit 退出码 0
  {
    Client c;
    if (!c.start()) {
      std::fprintf(stderr, "无法启动 vmake-lsp 子进程\n");
      ++g_failures;
      return;
    }
    json::Value init = c.request(
        "initialize",
        json::Value::object({{"capabilities", json::Value::object({})}}),
        notifications);
    CHECK(init.find("result") != nullptr);
    c.notify("initialized", json::Value::object({}));
    json::Value sd = c.request("shutdown", json::Value::object({}), notifications);
    CHECK(sd.find("result") != nullptr);
    json::Value after = c.request("textDocument/documentSymbol",
                                  doc_params("file:///x/VMakeSet.vmake"),
                                  notifications);
    const json::Value *err = after.find("error");
    CHECK(err != nullptr);
    if (err != nullptr) CHECK((int)err->int_or("code", 0) == -32600);
    c.notify("exit", json::Value::object({}));
    CHECK(c.wait_exit() == 0);
  }
}

} // namespace

int main() {
  test_session();
  test_lifecycle();

  if (g_failures == 0) {
    std::fprintf(stdout, "vmake-lsp e2e tests: all passed\n");
    return 0;
  }
  std::fprintf(stderr, "vmake-lsp e2e tests: %d failures\n", g_failures);
  return 1;
}
