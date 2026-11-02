// transport.hpp — vxls 的 LSP 传输层门面：转发到共享实现 tools/lsp（vx::lsp）。
#pragma once

#include "lsp.hpp"

namespace vxls {

// 共享 JSON 实现位于 tools/json（vx::json），在本工具命名空间内以 json 引用
namespace json = ::vx::json;

using vx::lsp::LspMessage;
using vx::lsp::read_message;
using vx::lsp::send_error;
using vx::lsp::send_message;
using vx::lsp::send_response;

} // namespace vxls
