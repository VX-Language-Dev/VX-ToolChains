// vxobj.hpp — VXOBJ v4 二进制序列化器（vxc003）。
// 将 ModuleIR 序列化为磁盘上的 .vxobj 文件，供下游 vlnk 链接器消费。
// 线格式对齐 compiler/vxobj.vx 的自举序列化器，保证与既有 .vxobj 可互操作。
#pragma once

#include "vx/compiler/typeir.hpp"

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace vx::compiler::obj {

// VXOBJ 文件头常量
inline constexpr std::string_view VXOBJ_MAGIC = "VXOBJ";
inline constexpr std::uint32_t VXOBJ_VERSION = 4;
inline constexpr std::string_view SECTION_TYPEIR = "TypeIR";
// 导入库依赖段（@imp / @cppimp）：链接期需并入的实现文件与 C++ 头文件。
// 仅在存在导入库时写出；无导入时 section_count 仍为 1。
inline constexpr std::string_view SECTION_DEPS = "Deps";

// 将 ModuleIR 序列化为 VXOBJ v4 字节流。
// target 为平台三元组字符串（如 "x86_64-apple-darwin"），写入文件头供链接器校验。
// 返回空字符串表示序列化失败（调用方应先检查 ModuleIR 有效性）。
std::string serialize(const ir::ModuleIR &mod, std::string_view target);

// 解析 VXOBJ v4 文件头：返回 {magic_ok, version, flags, target, section_count}。
// 输入为文件原始字节；magic 不匹配或 version 不支持时 magic_ok 为 false。
struct Header {
  bool magic_ok = false;
  std::uint32_t version = 0;
  std::uint32_t flags = 0;
  std::string target;
  std::uint32_t section_count = 0;
};
Header parse_header(std::string_view data);

} // namespace vx::compiler::obj