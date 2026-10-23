// vxobj_reader.hpp — VXOBJ v4 反序列化器（vxobj.cpp writer 的逆操作）。
// 将磁盘上的 .vxobj 字节流还原为 ModuleIR，供后端 vlnk 消费。
#pragma once

#include "vx/compiler/typeir.hpp"

#include <deque>
#include <string>
#include <string_view>

namespace vx::backend {

// 读取结果。注意：module 内 Param::name/type 的 string_view 指向本结构体的
// pool（std::deque，push_back 不重分配既有元素），因此本结构体只可移动、
// 不可拷贝（拷贝会使 string_view 悬垂）。
struct VxobjReadResult {
  bool ok = false;
  std::string error;
  std::string target;             // 文件头中的平台三元组
  vx::compiler::ir::ModuleIR module;
  std::deque<std::string> pool;   // 支撑 Param 的 string_view
};

// 解析 VXOBJ v4 字节流；失败时 ok=false 且 error 含中文原因。
VxobjReadResult read_vxobj(std::string_view data);

} // namespace vx::backend
