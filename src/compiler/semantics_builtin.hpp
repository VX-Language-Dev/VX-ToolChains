// semantics_builtin.hpp — 内置运行时函数表（内部实现头，仅供 semantics.cpp 使用）
#pragma once

#include "vx/compiler/semantics.hpp"

#include <string_view>
#include <unordered_map>

namespace vx::compiler {

namespace {

using ir::TypeTag;

// 内置运行时函数（与后端 llvm_emitter 的 kBuiltinTable 对齐）：
// VX 源码可见调用名 → 参数个数。返回类型由 builtin_ret_tag 单独给出。
constexpr int kBuiltinArityUnchecked = -1; // 低层符号不做参数个数检查
struct BuiltinInfo {
  int arity;
  TypeTag ret;
};

const BuiltinInfo* find_builtin(std::string_view name) {
  static const std::unordered_map<std::string_view, BuiltinInfo> kBuiltins = {
      {"out", {1, TypeTag::TYPE_UNKNOWN}},
      {"out_int", {1, TypeTag::TYPE_UNKNOWN}},
      {"out_bool", {1, TypeTag::TYPE_UNKNOWN}},
      {"out_float", {1, TypeTag::TYPE_UNKNOWN}},
      {"puts", {1, TypeTag::TYPE_INT}},
      {"exit", {1, TypeTag::TYPE_UNKNOWN}},
      {"malloc", {1, TypeTag::TYPE_UNKNOWN}},
      {"free", {1, TypeTag::TYPE_UNKNOWN}},
      {"sys_argv_count", {0, TypeTag::TYPE_INT}},
      {"sys_argv_get", {1, TypeTag::TYPE_STRING}},
      {"file_read", {1, TypeTag::TYPE_STRING}},
      {"file_write", {2, TypeTag::TYPE_INT}},
      {"file_exists", {1, TypeTag::TYPE_BOOL}},
      {"strcmp", {2, TypeTag::TYPE_INT}},
      {"vx_str_concat", {2, TypeTag::TYPE_STRING}},
      // 显式类型转换（int(x) / float(x) / bool(x) / str(x)）
      {"int", {1, TypeTag::TYPE_INT}},
      {"float", {1, TypeTag::TYPE_FLOAT}},
      {"bool", {1, TypeTag::TYPE_BOOL}},
      {"str", {1, TypeTag::TYPE_STRING}},
      {"vx_pow_i32", {2, TypeTag::TYPE_INT}},
      {"vx_pow_f64", {2, TypeTag::TYPE_FLOAT}},
      {"vx_array_new", {kBuiltinArityUnchecked, TypeTag::TYPE_ARRAY}},
      {"vx_array_get", {kBuiltinArityUnchecked, TypeTag::TYPE_UNKNOWN}},
      {"vx_array_set", {kBuiltinArityUnchecked, TypeTag::TYPE_UNKNOWN}},
      {"vx_map_new", {kBuiltinArityUnchecked, TypeTag::TYPE_MAP}},
      {"vx_map_get", {kBuiltinArityUnchecked, TypeTag::TYPE_UNKNOWN}},
      {"vx_map_set", {kBuiltinArityUnchecked, TypeTag::TYPE_UNKNOWN}},
  };
  auto it = kBuiltins.find(name);
  return it == kBuiltins.end() ? nullptr : &it->second;
}

} // namespace

} // namespace vx::compiler
