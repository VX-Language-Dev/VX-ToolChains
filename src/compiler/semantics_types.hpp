// semantics_types.hpp — 类型关系辅助函数（内部实现头，仅供 semantics.cpp 使用）
#pragma once

#include "vx/compiler/semantics.hpp"

#include <span>
#include <string>
#include <string_view>

namespace vx::compiler {

namespace {

using ir::TypeTag;

// TypeTag 显示名（诊断文案用）
std::string_view tag_name(TypeTag t) {
  switch (t) {
    case TypeTag::TYPE_VOID:
      return "void";
    case TypeTag::TYPE_INT:
      return "int";
    case TypeTag::TYPE_FLOAT:
      return "float";
    case TypeTag::TYPE_BOOL:
      return "bool";
    case TypeTag::TYPE_STRING:
      return "string";
    case TypeTag::TYPE_POINTER:
      return "ptr";
    case TypeTag::TYPE_STRUCT:
      return "struct";
    case TypeTag::TYPE_ARRAY:
      return "list";
    case TypeTag::TYPE_MAP:
      return "map";
    case TypeTag::TYPE_FUNC:
      return "func";
    default:
      return "var";
  }
}

// 完整类型名（类/结构带名字）
std::string type_text(const VarInfo& v) {
  if (v.type == TypeTag::TYPE_STRUCT && !v.struct_name.empty()) {
    return v.struct_name;
  }
  return std::string(tag_name(v.type));
}

// 类型兼容判定（无隐式转换）：类型未定（var / 无法推断）视为通配，
// 与任何类型兼容；其余必须完全相同（struct 还要求类名一致）。
bool same_type(const VarInfo& a, const VarInfo& b) {
  if (a.type == TypeTag::TYPE_UNKNOWN || b.type == TypeTag::TYPE_UNKNOWN) {
    return true;
  }
  if (a.type != b.type) {
    return false;
  }
  if (a.type == TypeTag::TYPE_STRUCT) {
    return a.struct_name == b.struct_name;
  }
  return true;
}

// 参数签名是否相同（重载集合内判定重复声明用）：
// 参数个数 + 逐个类型标注文本一致即视为同一签名。
bool params_identical(std::span<const Param> a, std::span<const Param> b) {
  if (a.size() != b.size()) {
    return false;
  }
  for (std::size_t i = 0; i < a.size(); ++i) {
    if (a[i].type != b[i].type) {
      return false;
    }
  }
  return true;
}

} // namespace

} // namespace vx::compiler
