// emitter_value.hpp — 值类型与内置运行时符号表（内部实现头，仅供 llvm_emitter.cpp 使用）
#pragma once

#include "vx/backend/llvm_emitter.hpp"

#include <cstdint>
#include <map>
#include <string_view>
#include <vector>

namespace vx::backend {

namespace {

// ---- 值类型 ----
enum class VT : std::int8_t { I32 = 0, I64 = 1, F64 = 2, PTR = 3 };

const char* vt_name(VT t) {
  switch (t) {
    case VT::I32:
      return "i32";
    case VT::I64:
      return "i64";
    case VT::F64:
      return "double";
    case VT::PTR:
      return "ptr";
  }
  return "i32";
}

VT vt_of(std::string_view ty) {
  if (ty == "float")
    return VT::F64;
  if (ty == "string" || ty == "str" || ty == "ptr")
    return VT::PTR;
  if (ty == "i64" || ty == "var")
    return VT::I64; // var = 64 位通用槽：可承载 int / float / ptr
  return VT::I32;   // int/bool/类名/空
}

// TypeIR 类型标签 → 后端值类型（用于 local_types 预分配）
VT vt_of_tag(std::uint8_t tag) {
  switch (tag) {
    case ir::TYPE_FLOAT:
      return VT::F64;
    case ir::TYPE_STRING:
    case ir::TYPE_STRUCT:
    case ir::TYPE_ARRAY:
    case ir::TYPE_MAP:
    case ir::TYPE_FUNC:
    case ir::TYPE_POINTER:
      return VT::PTR;
    default:
      return VT::I32; // int / bool / enum
  }
}

bool is_void(std::string_view ty) {
  return ty.empty() || ty == "void";
}

// 裸值 + 类型（val 为 LLVM 操作数文本，不含类型；常量形如 "5"/"null"，寄存器形如 "%t3"）
struct Value {
  std::string val;
  VT vt = VT::I32;
};

// ---- 内置运行时符号表 ----
struct Builtin {
  const char* sym;
  std::vector<VT> args;
  bool has_ret;
  VT ret;
  bool helper = false; // true = 后端生成 IR 实现（不产生外部 declare）
};

// ---- 线程 / 任务池内置 ----
// proc 库的线程 API 无 C/C++ 实现：后端按 VX ABI 符号识别后，直接发射
// pthread 调用与通用函数适配器 IR。任务以函数值（&fn）传入，符号在链接期
// 解析，因此支持跨文件；参数 0~4 个 int，返回值由 join_* 系列按类型取出。
enum class TaskCall : std::uint8_t {
  None,
  Spawn0,
  Spawn1,
  Spawn2,
  Spawn3,
  Spawn4,
  JoinInt,
  JoinFloat,
  JoinStr,
  JoinPtr,
  PoolStart,
  PoolSubmit0,
  PoolSubmit1,
  PoolSubmit2,
  PoolSubmit3,
  PoolSubmit4,
  PoolWait,
  PoolStop,
};

TaskCall task_call_of(std::string_view name) {
  if (name == "_vx_proc_spawn0")
    return TaskCall::Spawn0;
  if (name == "_vx_proc_spawn1")
    return TaskCall::Spawn1;
  if (name == "_vx_proc_spawn2")
    return TaskCall::Spawn2;
  if (name == "_vx_proc_spawn3")
    return TaskCall::Spawn3;
  if (name == "_vx_proc_spawn4")
    return TaskCall::Spawn4;
  if (name == "_vx_proc_join" || name == "_vx_proc_join_int")
    return TaskCall::JoinInt;
  if (name == "_vx_proc_join_float")
    return TaskCall::JoinFloat;
  if (name == "_vx_proc_join_str")
    return TaskCall::JoinStr;
  if (name == "_vx_proc_join_ptr")
    return TaskCall::JoinPtr;
  if (name == "_vx_proc_pool_start")
    return TaskCall::PoolStart;
  if (name == "_vx_proc_pool_submit0")
    return TaskCall::PoolSubmit0;
  if (name == "_vx_proc_pool_submit1")
    return TaskCall::PoolSubmit1;
  if (name == "_vx_proc_pool_submit2")
    return TaskCall::PoolSubmit2;
  if (name == "_vx_proc_pool_submit3")
    return TaskCall::PoolSubmit3;
  if (name == "_vx_proc_pool_submit4")
    return TaskCall::PoolSubmit4;
  if (name == "_vx_proc_pool_wait")
    return TaskCall::PoolWait;
  if (name == "_vx_proc_pool_stop")
    return TaskCall::PoolStop;
  return TaskCall::None;
}

// 任务调用对应的实参个数（第一个实参为函数值）
int task_arg_count(TaskCall tc) {
  switch (tc) {
    case TaskCall::Spawn0:
    case TaskCall::PoolSubmit0:
      return 0;
    case TaskCall::Spawn1:
    case TaskCall::PoolSubmit1:
      return 1;
    case TaskCall::Spawn2:
    case TaskCall::PoolSubmit2:
      return 2;
    case TaskCall::Spawn3:
    case TaskCall::PoolSubmit3:
      return 3;
    case TaskCall::Spawn4:
    case TaskCall::PoolSubmit4:
      return 4;
    default:
      return 0;
  }
}

// 键为 VX 源码中可见的调用名（_vx_proc_* 为例外：proc 库无 C 实现，
// 键取其 VX ABI 符号，命中后直连系统 libc / 生成线程 IR）。
// 注意：使用函数内静态避免静态初始化顺序 / 抛异常问题（throwing-static-init）。
static const std::map<std::string_view, Builtin>& builtin_table() {
  static const std::map<std::string_view, Builtin> t = {
      {"out", {"out", {VT::PTR}, false, VT::I32}},
      {"out_int", {"out_int", {VT::I32}, false, VT::I32}},
      {"out_bool", {"out_bool", {VT::I32}, false, VT::I32}},
      {"out_float", {"out_float", {VT::F64}, false, VT::I32}},
      {"puts", {"puts", {VT::PTR}, true, VT::I32}},
      {"exit", {"exit", {VT::I32}, false, VT::I32}},
      {"malloc", {"malloc", {VT::I64}, true, VT::PTR}},
      {"free", {"free", {VT::PTR}, false, VT::I32}},
      {"vx_array_new", {"vx_array_new", {VT::I64}, true, VT::PTR}},
      {"vx_array_get", {"vx_array_get", {VT::PTR, VT::I64}, true, VT::I64}},
      {"vx_array_set", {"vx_array_set", {VT::PTR, VT::I64, VT::I64}, false, VT::I32}},
      {"vx_map_new", {"vx_map_new", {VT::I64}, true, VT::PTR}},
      {"vx_map_get", {"vx_map_get", {VT::PTR, VT::I64, VT::I64, VT::PTR}, true, VT::I64}},
      {"vx_map_set", {"vx_map_set", {VT::PTR, VT::I64, VT::I64, VT::PTR, VT::I64}, false, VT::I32}},
      {"vx_str_concat", {"vx_str_concat", {VT::PTR, VT::PTR}, true, VT::PTR}},
      {"strcmp", {"strcmp", {VT::PTR, VT::PTR}, true, VT::I32}},
      {"vx_pow_i32", {"vx_pow_i32", {VT::I32, VT::I32}, true, VT::I32}},
      {"vx_pow_f64", {"vx_pow_f64", {VT::F64, VT::F64}, true, VT::F64}},
      // 显式转换 str(x) 的运行时格式化（实现见 vxrt.c）
      {"vx_str_from_int", {"vx_str_from_int", {VT::I32}, true, VT::PTR}},
      {"vx_str_from_float", {"vx_str_from_float", {VT::F64}, true, VT::PTR}},
      {"vx_str_from_bool", {"vx_str_from_bool", {VT::I32}, true, VT::PTR}},
      // 系统能力：命令行参数 / 文件 I/O（实现见 vxrt.c）
      {"sys_argv_count", {"vx_argv_count", {}, true, VT::I32}},
      {"sys_argv_get", {"vx_argv_get", {VT::I32}, true, VT::PTR}},
      {"file_read", {"vx_file_read", {VT::PTR}, true, VT::PTR}},
      {"file_write", {"vx_file_write", {VT::PTR, VT::PTR}, true, VT::I32}},
      {"file_exists", {"vx_file_exists", {VT::PTR}, true, VT::I32}},
      // 进程原语（headers/proc.vxh 声明，无 C 实现）：键为 VX ABI 符号，
      // 值为 libc 符号与签名；调用经 libc 直连，status 实参可传 nil / 0（NULL）
      {"_vx_proc_fork", {"fork", {}, true, VT::I32}},
      {"_vx_proc_getpid", {"getpid", {}, true, VT::I32}},
      {"_vx_proc_getppid", {"getppid", {}, true, VT::I32}},
      {"_vx_proc_waitpid", {"waitpid", {VT::I32, VT::PTR, VT::I32}, true, VT::I32}},
      {"_vx_proc_kill", {"kill", {VT::I32, VT::I32}, true, VT::I32}},
      {"_vx_proc_sleep", {"sleep", {VT::I32}, true, VT::I32}},
      // 同步原语（headers/proc.vxh 声明，无 C 实现）：直连 pthread，attr 形参
      // 传 nil（NULL）即默认属性；mutex/cond 对象内存由调用方提供
      {"_vx_proc_mutex_init", {"pthread_mutex_init", {VT::PTR, VT::PTR}, true, VT::I32}},
      {"_vx_proc_mutex_lock", {"pthread_mutex_lock", {VT::PTR}, true, VT::I32}},
      {"_vx_proc_mutex_trylock", {"pthread_mutex_trylock", {VT::PTR}, true, VT::I32}},
      {"_vx_proc_mutex_unlock", {"pthread_mutex_unlock", {VT::PTR}, true, VT::I32}},
      {"_vx_proc_mutex_destroy", {"pthread_mutex_destroy", {VT::PTR}, true, VT::I32}},
      {"_vx_proc_cond_init", {"pthread_cond_init", {VT::PTR, VT::PTR}, true, VT::I32}},
      {"_vx_proc_cond_wait", {"pthread_cond_wait", {VT::PTR, VT::PTR}, true, VT::I32}},
      {"_vx_proc_cond_signal", {"pthread_cond_signal", {VT::PTR}, true, VT::I32}},
      {"_vx_proc_cond_broadcast", {"pthread_cond_broadcast", {VT::PTR}, true, VT::I32}},
      {"_vx_proc_cond_destroy", {"pthread_cond_destroy", {VT::PTR}, true, VT::I32}},
      {"_vx_proc_usleep", {"usleep", {VT::I32}, true, VT::I32}},
      // 管道 / 输出捕获（headers/proc.vxh 声明）：helper=true 时由后端生成 IR
      {"_vx_proc_pipe", {"vx_proc_pipe_impl", {}, true, VT::PTR, true}},
      {"_vx_proc_read", {"vx_proc_read_impl", {VT::I32, VT::PTR, VT::I32}, true, VT::I32, true}},
      {"_vx_proc_write", {"vx_proc_write_impl", {VT::I32, VT::PTR, VT::I32}, true, VT::I32, true}},
      {"_vx_proc_read_str", {"vx_proc_read_str_impl", {VT::I32, VT::I32}, true, VT::PTR, true}},
      {"_vx_proc_write_str", {"vx_proc_write_str_impl", {VT::I32, VT::PTR}, true, VT::I32, true}},
      {"_vx_proc_capture", {"vx_proc_capture_impl", {VT::PTR}, true, VT::PTR, true}},
      {"_vx_proc_close", {"close", {VT::I32}, true, VT::I32}},
  };
  return t;
}

// 按 VX 源码中的调用名查找
const Builtin* lookup_builtin(std::string_view name) {
  const auto& t = builtin_table();
  auto it = t.find(name);
  return it == t.end() ? nullptr : &it->second;
}

// 按链接期实际符号查找（发射 declare 时使用）
const Builtin* lookup_builtin_by_sym(std::string_view sym) {
  for (const auto& [name, b] : builtin_table()) {
    if (sym == b.sym)
      return &b;
  }
  return nullptr;
}

// LLVM c"..." 字节转义

} // namespace

} // namespace vx::backend
