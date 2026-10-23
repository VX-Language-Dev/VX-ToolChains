// transport.hpp — LSP 消息传输：stdin/stdout 上的 Content-Length 帧协议。
#pragma once

#include "json.hpp"

#include <optional>
#include <string>

namespace vxls {

// 一条解码后的 LSP 消息
struct LspMessage {
  bool has_id = false;      // 请求/响应带 id；通知无 id
  double id = 0;            // 数字 id（LSP 允许 string id，此处简化为数字）
  std::string id_text;      // id 原文（string id 时非空）
  std::string method;       // 请求/通知方法名；响应为空
  json::Value params;       // params 对象（可为空值）
  bool is_response = false; // 服务端发出的响应（收到时通常不出现）
};

// 读一条消息（阻塞）；EOF/帧错误返回 std::nullopt
std::optional<LspMessage> read_message();

// 发送请求/通知（无 id 为通知）
void send_message(const LspMessage &msg);

// 发送响应：id 回显，result 为结果值
void send_response(const LspMessage &req, json::Value result);

// 发送错误响应（LSP ErrorCodes：-32601 方法不支持等）
void send_error(const LspMessage &req, int code, const std::string &message);

} // namespace vxls
