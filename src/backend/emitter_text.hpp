// emitter_text.hpp — IR 文本工具函数（内部实现头，仅供 llvm_emitter.cpp 使用）
#pragma once

#include "vx/backend/llvm_emitter.hpp"

#include <cstdint>
#include <string>
#include <string_view>

namespace vx::backend {

namespace {

namespace ir = vx::compiler::ir;

std::string escape_bytes(std::string_view s) {
  std::string out;
  out.reserve(s.size() + 1);
  for (unsigned char ch : s) {
    if (ch == '"') {
      out += "\\22";
    } else if (ch == '\\') {
      out += "\\5C";
    } else if (ch >= 0x20 && ch < 0x7F) {
      out += static_cast<char>(ch);
    } else {
      char buf[8];
      std::snprintf(buf, sizeof(buf), "\\%02X", ch);
      out += buf;
    }
  }
  out += "\\00";
  return out;
}

const char* op_name(std::uint8_t op) {
  switch (op) {
    case ir::OP_CONST_INT:
      return "OP_CONST_INT";
    case ir::OP_CONST_FLOAT:
      return "OP_CONST_FLOAT";
    case ir::OP_CONST_BOOL:
      return "OP_CONST_BOOL";
    case ir::OP_CONST_STRING:
      return "OP_CONST_STRING";
    case ir::OP_CONST_NIL:
      return "OP_CONST_NIL";
    case ir::OP_LOAD_VAR:
      return "OP_LOAD_VAR";
    case ir::OP_STORE_VAR:
      return "OP_STORE_VAR";
    case ir::OP_I32_ADD:
      return "OP_I32_ADD";
    case ir::OP_I32_SUB:
      return "OP_I32_SUB";
    case ir::OP_I32_MUL:
      return "OP_I32_MUL";
    case ir::OP_I32_DIV:
      return "OP_I32_DIV";
    case ir::OP_I32_MOD:
      return "OP_I32_MOD";
    case ir::OP_F64_ADD:
      return "OP_F64_ADD";
    case ir::OP_F64_SUB:
      return "OP_F64_SUB";
    case ir::OP_F64_MUL:
      return "OP_F64_MUL";
    case ir::OP_F64_DIV:
      return "OP_F64_DIV";
    case ir::OP_I64_ADD:
      return "OP_I64_ADD";
    case ir::OP_I32_EQ:
      return "OP_I32_EQ";
    case ir::OP_I32_NEQ:
      return "OP_I32_NEQ";
    case ir::OP_I32_LT:
      return "OP_I32_LT";
    case ir::OP_I32_GT:
      return "OP_I32_GT";
    case ir::OP_I32_LE:
      return "OP_I32_LE";
    case ir::OP_I32_GE:
      return "OP_I32_GE";
    case ir::OP_F64_EQ:
      return "OP_F64_EQ";
    case ir::OP_F64_NE:
      return "OP_F64_NE";
    case ir::OP_F64_LT:
      return "OP_F64_LT";
    case ir::OP_F64_GT:
      return "OP_F64_GT";
    case ir::OP_F64_LE:
      return "OP_F64_LE";
    case ir::OP_F64_GE:
      return "OP_F64_GE";
    case ir::OP_I32_NEG:
      return "OP_I32_NEG";
    case ir::OP_F64_NEG:
      return "OP_F64_NEG";
    case ir::OP_BOOL_NOT:
      return "OP_BOOL_NOT";
    case ir::OP_I32_AND:
      return "OP_I32_AND";
    case ir::OP_I32_OR:
      return "OP_I32_OR";
    case ir::OP_BOOL_AND:
      return "OP_BOOL_AND";
    case ir::OP_BOOL_OR:
      return "OP_BOOL_OR";
    case ir::OP_JUMP:
      return "OP_JUMP";
    case ir::OP_JUMP_IF_FALSE:
      return "OP_JUMP_IF_FALSE";
    case ir::OP_JUMP_IF_TRUE:
      return "OP_JUMP_IF_TRUE";
    case ir::OP_STR_EQ:
      return "OP_STR_EQ";
    case ir::OP_NEQ_INT:
      return "OP_NEQ_INT";
    case ir::OP_CALL:
      return "OP_CALL";
    case ir::OP_RETURN:
      return "OP_RETURN";
    case ir::OP_RETURN_VOID:
      return "OP_RETURN_VOID";
    case ir::OP_STRUCT_NEW:
      return "OP_STRUCT_NEW";
    case ir::OP_FIELD_LOAD:
      return "OP_FIELD_LOAD";
    case ir::OP_FIELD_STORE:
      return "OP_FIELD_STORE";
    case ir::OP_ARRAY_NEW:
      return "OP_ARRAY_NEW";
    case ir::OP_INDEX_GET:
      return "OP_INDEX_GET";
    case ir::OP_INDEX_SET:
      return "OP_INDEX_SET";
    case ir::OP_MAP_NEW:
      return "OP_MAP_NEW";
    case ir::OP_DUP:
      return "OP_DUP";
    case ir::OP_POP:
      return "OP_POP";
    case ir::OP_STR_CONCAT:
      return "OP_STR_CONCAT";
    case ir::OP_DEREF:
      return "OP_DEREF";
    case ir::OP_DEREF_STORE:
      return "OP_DEREF_STORE";
    case ir::OP_CONST_FUNC:
      return "OP_CONST_FUNC";
    case ir::OP_CAST_INT:
      return "OP_CAST_INT";
    case ir::OP_CAST_FLOAT:
      return "OP_CAST_FLOAT";
    case ir::OP_CAST_BOOL:
      return "OP_CAST_BOOL";
    case ir::OP_CAST_STR:
      return "OP_CAST_STR";
    case ir::OP_HALT:
      return "OP_HALT";
    case ir::OP_NOOP:
      return "OP_NOOP";
    default:
      return "unknown";
  }
}

} // namespace

} // namespace vx::backend
