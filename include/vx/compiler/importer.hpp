// importer.hpp — 库导入解析：把源码中的 @imp / @cppimp 注解解析为实际库、
// 符号签名与链接期依赖。
//
// 库查找规则（@imp）：
//   - 库默认位于 vxc 可执行文件同级的 headers/ 目录及其子目录下；
//   - headers 根目录的库直接用文件名引用，如 @imp(math) → headers/math.vxh；
//   - 子目录下的库在库名前加 '/' + 子目录路径，如 @imp(/collections/vec)
//     → headers/collections/vec.vxh（前导 '/' 可省略，两者等价）；
//   - 自定义库用相对路径（以 './' 或 '../' 起始），相对被编译源文件所在目录。
//
// @cppimp 引用 C++ 头文件；VX 侧签名取自与该头文件同目录、同主名的
// .vxh 伴生声明文件（如 greet.hpp ↔ greet.vxh），链接期由后端生成
// extern "C" shim 把 C++ ABI 转换为 VX ABI。
#pragma once

#include "vx/compiler/ast.hpp"
#include "vx/compiler/diag.hpp"
#include "vx/compiler/parser.hpp"
#include "vx/compiler/typeir.hpp"

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace vx::compiler {

// 一个已解析的导入符号：VX 侧签名 + 计算出的 VX ABI 符号名。
struct ImportedSymbol {
  std::string vx_name;    // 库内原名（如 "add"）
  std::string abi_symbol; // _vx_<lib>_<name>
  std::vector<ir::AbiParam> params;
  std::string ret_type;
};

// 一个已导入的库。
struct ImportedLib {
  std::string ref;        // 原始库引用（"math" / "collections/vec" / "./mylib"）
  std::string name;       // 库名（末段）
  std::string alias;      // 访问前缀（默认 = name）
  std::string decl_file;  // 声明文件（.vxh）绝对路径
  std::string impl_file;  // 实现文件绝对路径（.c/.a/.dylib/.cpp），可空
  bool from_cpp = false;  // true = @cppimp
  std::string cpp_header; // from_cpp 时的 C++ 头文件绝对路径
  std::vector<ImportedSymbol> symbols;
};

// 导入表：源码内所有 @imp / @cppimp 解析结果的汇总。
struct ImportTable {
  std::vector<ImportedLib> libs;
  // 同一库被再次引用（如 @imp(math) 与 @imp(math) as m）时，
  // 额外别名记录为 (别名, 原始引用)，指向 libs 中的同一库。
  std::vector<std::pair<std::string, std::string>> aliases;

  // 按访问前缀（alias / name / 额外别名）查找库；未命中返回 nullptr。
  const ImportedLib *lib_by_prefix(std::string_view prefix) const;

  // 在某库内按库内名查找符号；未命中返回 nullptr。
  static const ImportedSymbol *find_symbol(const ImportedLib &lib,
                                           std::string_view name);
};

// 库解析上下文。
struct ImportContext {
  std::filesystem::path headers_root; // <exe_dir>/headers
  std::filesystem::path source_dir;   // 被编译源文件所在目录
};

// 由可执行文件路径推导其所在目录（用于定位同级 headers/）。
std::filesystem::path executable_dir(std::string_view argv0);

// 解析库引用 → 该库的基准路径（未加扩展名）。
// 以 '.' 起始 → 相对 source_dir；否则相对 headers_root（允许前导 '/'）。
std::filesystem::path resolve_ref_path(const ImportContext &ctx,
                                       std::string_view ref);

// 扫描程序中的所有 @imp / @cppimp，构建导入表。
// 出错时把诊断推入 bag（定位到 main_file 的注解行）并返回 std::nullopt。
std::optional<ImportTable> build_import_table(const Program &prog,
                                              const ImportContext &ctx,
                                              std::string_view main_file,
                                              DiagBag &bag);

} // namespace vx::compiler
