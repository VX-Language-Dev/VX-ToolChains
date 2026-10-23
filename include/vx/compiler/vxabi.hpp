// vxabi.hpp — VX ABI 定义：跨库/跨语言边界的符号命名与类型映射契约。
//
// 符号命名：_vx_<lib>_<name>
//   lib  = 库名（库引用路径的末段，经 sanitize 净化为 C 标识符）
//   name = 库内的导出符号名
//   例：@imp(collections/vec) 的 push → _vx_vec_push
//
// 类型映射（VX 类型 → ABI/LLVM/C 类型）：
//   int    → i32    / int32_t
//   float  → double / double
//   bool   → i32    / int32_t
//   string → ptr    / const char*
//   void   → void   / void
//
// 该契约是前端（符号发射）、后端（IR declare/调用）与 @cppimp shim 生成
// （把 C++ ABI 转换为 VX ABI）三方共用的唯一事实来源。
#pragma once

#include <string>
#include <string_view>

namespace vx::compiler::abi {

// ABI 版本：符号命名/类型映射发生不兼容变更时递增
inline constexpr int VX_ABI_VERSION = 1;

// 净化：把非 [A-Za-z0-9_] 字符替换为 '_'，使结果可作为 C 标识符片段。
std::string sanitize(std::string_view s);

// 库引用 → 库名：取路径末段。
//   "math"              → "math"
//   "collections/vec"   → "vec"
//   "/sub/lib"          → "lib"
//   "./mylib"           → "mylib"
std::string_view lib_name_of(std::string_view ref);

// VX ABI 符号名：_vx_<sanitize(lib)>_<sanitize(name)>
std::string symbol(std::string_view lib, std::string_view name);

// VX 类型名 → ABI 的 C 类型名（C 声明 / shim 生成用）
std::string_view c_type_of(std::string_view vx_type);

// VX 类型名 → LLVM 类型名
std::string_view llvm_type_of(std::string_view vx_type);

} // namespace vx::compiler::abi
