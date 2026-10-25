// transport.cpp — vmake-lsp 传输层实现：Content-Length 帧的读写。
#include "transport.hpp"

#include <cstdio>
#include <cstdlib>
#include <string>

namespace vmake_lsp {

namespace {

// 从 stdin 读一行（含 \n）；EOF 返回 false
bool read_line(std::string &line) {
  line.clear();
  int c;
  while ((c = std::fgetc(stdin)) != EOF) {
    line += static_cast<char>(c);
    if (c == '\n') return true;
  }
  return !line.empty();
}

void write_frame(const std::string &body) {
  std::string head = "Content-Length: " + std::to_string(body.size()) + "\r\n\r\n";
  std::fwrite(head.data(), 1, head.size(), stdout);
  std::fwrite(body.data(), 1, body.size(), stdout);
  std::fflush(stdout);
}

} // namespace

std::optional<LspMessage> read_message() {
  // 头部：Content-Length 必需；其余头忽略
  std::size_t content_length = 0;
  bool have_length = false;
  while (true) {
    std::string line;
    if (!read_line(line)) return std::nullopt; // EOF
    if (line == "\r\n" || line == "\n") {
      if (have_length) break;
      continue; // 空行但尚无长度：继续等头部
    }
    // 头部形如 "Content-Length: 123\r\n"
    const std::size_t colon = line.find(':');
    if (colon == std::string::npos) continue;
    std::string key = line.substr(0, colon);
    std::string value = line.substr(colon + 1);
    while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) {
      value.erase(value.begin());
    }
    while (!value.empty() && (value.back() == '\r' || value.back() == '\n')) {
      value.pop_back();
    }
    if (key == "Content-Length") {
      content_length = static_cast<std::size_t>(std::strtoull(value.c_str(), nullptr, 10));
      have_length = content_length > 0;
    }
  }
  if (!have_length) return std::nullopt;

  std::string body(content_length, '\0');
  std::size_t got = 0;
  while (got < content_length) {
    const std::size_t n =
        std::fread(body.data() + got, 1, content_length - got, stdin);
    if (n == 0) return std::nullopt; // EOF 截断
    got += n;
  }

  auto parsed = json::parse(body);
  if (!parsed) return std::nullopt;

  LspMessage msg;
  const json::Value &root = *parsed;
  if (const json::Value *id = root.find("id")) {
    msg.has_id = true;
    if (id->type() == json::Value::Type::Number) {
      msg.id = id->as_number();
    } else {
      msg.id_text = std::string(id->as_string());
    }
  }
  msg.method = std::string(root.str_or("method"));
  if (const json::Value *p = root.find("params")) {
    msg.params = *p;
  }
  msg.is_response = msg.method.empty() && msg.has_id;
  return msg;
}

void send_message(const LspMessage &msg) {
  std::vector<std::pair<std::string, json::Value>> members;
  if (msg.has_id) {
    if (!msg.id_text.empty()) {
      members.emplace_back("id", json::Value::string(msg.id_text));
    } else {
      members.emplace_back("id", json::Value::number(msg.id));
    }
  }
  members.emplace_back("jsonrpc", json::Value::string("2.0"));
  if (!msg.method.empty()) {
    members.emplace_back("method", json::Value::string(msg.method));
  }
  if (!msg.params.is_null()) {
    members.emplace_back("params", msg.params);
  }
  write_frame(json::Value::object(std::move(members)).dump());
}

void send_response(const LspMessage &req, json::Value result) {
  std::vector<std::pair<std::string, json::Value>> members;
  if (!req.id_text.empty()) {
    members.emplace_back("id", json::Value::string(req.id_text));
  } else {
    members.emplace_back("id", json::Value::number(req.id));
  }
  members.emplace_back("jsonrpc", json::Value::string("2.0"));
  members.emplace_back("result", std::move(result));
  write_frame(json::Value::object(std::move(members)).dump());
}

void send_error(const LspMessage &req, int code, const std::string &message) {
  std::vector<std::pair<std::string, json::Value>> err;
  err.emplace_back("code", json::Value::number(code));
  err.emplace_back("message", json::Value::string(message));
  std::vector<std::pair<std::string, json::Value>> members;
  if (!req.id_text.empty()) {
    members.emplace_back("id", json::Value::string(req.id_text));
  } else {
    members.emplace_back("id", json::Value::number(req.id));
  }
  members.emplace_back("jsonrpc", json::Value::string("2.0"));
  members.emplace_back("error", json::Value::object(std::move(err)));
  write_frame(json::Value::object(std::move(members)).dump());
}

} // namespace vmake_lsp
