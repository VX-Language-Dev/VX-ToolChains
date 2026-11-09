// llvm_emitter.cpp — TypeIR（虚拟栈机）→ LLVM IR 文本（SSA 直接发射）。
// 设计：发射期维护「编译期操作数栈」（Value{val, vt}），槽位类型由写入者推断；
// 条件分支自动开 fallthrough 基本块；标签处若非终止块则补 br，保证 IR 合法。
#include "vx/backend/llvm_emitter.hpp"

#include <algorithm>
#include <charconv>
#include <cstdint>
#include <cstdio>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace vx::backend {
namespace {

namespace ir = vx::compiler::ir;

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
  if (ty == "string" || ty == "ptr")
    return VT::PTR;
  if (ty == "i64")
    return VT::I64;
  return VT::I32; // int/bool/var/空
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
    case ir::OP_HALT:
      return "OP_HALT";
    case ir::OP_NOOP:
      return "OP_NOOP";
    default:
      return "unknown";
  }
}

class Emitter {
public:
  Emitter(const ir::ModuleIR& mod, std::string_view triple, bool emit_entry)
      : mod_(mod), triple_(triple), emit_entry_(emit_entry) {}

  EmitResult run() {
    for (const ir::FuncIR& f : mod_.funcs) {
      fn_by_name_[f.name] = &f;
    }
    // 外部符号表：key 为链接符号名（VX ABI 名优先，回退到声明名）
    for (const ir::LinkEntry& e : mod_.linkage) {
      const std::string key = e.abi_symbol.empty() ? e.name : e.abi_symbol;
      extern_by_sym_[key] = &e;
    }
    // P4：类/结构字段布局（字段名 -> 索引/类型；首个定义优先）
    for (const ir::StructLayout& sl : mod_.structs) {
      for (std::size_t i = 0; i < sl.fields.size(); ++i) {
        if (!field_index_.contains(sl.fields[i].name)) {
          field_index_[sl.fields[i].name] = i;
          field_type_[sl.fields[i].name] = vt_of(sl.fields[i].type);
        }
      }
    }
    std::string funcs_txt;
    for (const ir::FuncIR& f : mod_.funcs) {
      if (!emit_function(f))
        return {false, error_, {}};
      funcs_txt += func_block_;
    }
    // main 入口包装（动态库不需要进程入口）
    if (emit_entry_) {
      if (!emit_main_wrapper())
        return {false, error_, {}};
      funcs_txt += func_block_;
    }

    // 函数值适配器 / 线程 / 任务池 / 管道：按使用情况生成声明、全局状态与 IR
    const bool any_task = wants_threads_ || wants_pool_;
    if (any_task || wants_pipe_ || !func_refs_.empty()) {
      task_decls_ = build_task_decls(any_task, wants_pipe_);
      if (!func_refs_.empty()) {
        task_defs_ += build_adapters();
        if (!error_.empty())
          return {false, error_, {}};
      }
      if (wants_threads_) {
        task_globals_ += build_thread_globals();
        task_defs_ += build_thread_helpers();
      }
      if (wants_pool_) {
        task_globals_ += build_pool_globals();
        task_defs_ += build_pool_helpers();
      }
      if (wants_pipe_) {
        task_globals_ += build_pipe_globals();
        task_defs_ += build_pipe_helpers();
      }
    }

    // 组装输出：declare 去重（后端 helper 声明可能与 builtin/linkage 声明同名）
    std::set<std::string> decl_lines;
    {
      std::size_t pos = 0;
      while (pos < task_decls_.size()) {
        const std::size_t nl = task_decls_.find('\n', pos);
        const std::string line = task_decls_.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos);
        if (!line.empty())
          decl_lines.insert(line);
        if (nl == std::string::npos)
          break;
        pos = nl + 1;
      }
    }
    for (const std::string& sym : used_builtins_) {
      const Builtin* b = lookup_builtin_by_sym(sym);
      if (b == nullptr)
        continue;
      std::string line = "declare ";
      line += b->has_ret ? vt_name(b->ret) : "void";
      line += " @" + std::string(b->sym) + "(";
      for (std::size_t i = 0; i < b->args.size(); ++i) {
        if (i != 0U)
          line += ", ";
        line += vt_name(b->args[i]);
      }
      line += ")";
      decl_lines.insert(line);
    }
    // 来自 linkage 的外部符号声明（@imp / @cppimp / 普通 extern）
    for (const std::string& sym : used_externs_) {
      auto it = extern_by_sym_.find(sym);
      if (it == extern_by_sym_.end())
        continue;
      const ir::LinkEntry* e = it->second;
      const bool has_ret = !is_void(e->ret_type);
      std::string line = "declare ";
      line += has_ret ? vt_name(vt_of(e->ret_type)) : "void";
      line += " @" + sym + "(";
      for (std::size_t i = 0; i < e->params.size(); ++i) {
        if (i != 0U)
          line += ", ";
        line += vt_name(vt_of(e->params[i].type));
      }
      line += ")";
      decl_lines.insert(line);
    }

    std::string out;
    out += "; VXOBJ -> LLVM IR by vlnk\n";
    out += "target triple = \"" + std::string(triple_) + "\"\n\n";
    for (const std::string& line : decl_lines)
      out += line + "\n";
    if (!decl_lines.empty())
      out += "\n";
    for (const std::string& g : globals_)
      out += g + "\n";
    if (!globals_.empty())
      out += "\n";
    out += task_globals_;
    out += funcs_txt;
    out += task_defs_;

    EmitResult r;
    r.ok = true;
    r.ir = std::move(out);
    return r;
  }

private:
  bool fail(std::string msg) {
    if (error_.empty())
      error_ = std::move(msg);
    return false;
  }

  // ---- 函数值适配器 / 线程 / 任务池 / 管道：IR 生成 ----
  static std::string sanitize_sym(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
      const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
      out.push_back(ok ? c : '_');
    }
    return out;
  }

  // 通用适配器：i64(i64,i64,i64,i64) → 目标函数原生签名。
  // 函数值（&fn）取的是适配器地址，因此可跨文件、可存入变量、可传给任意接收 ptr 的 API。
  std::string build_adapters() {
    std::string s;
    for (const std::string& name : func_refs_) {
      const auto it = fn_by_name_.find(name);
      if (it == fn_by_name_.end()) {
        error_ = "函数值引用的函数不存在: " + name;
        return {};
      }
      const ir::FuncIR* f = it->second;
      if (f->params.size() > 4) {
        error_ = "函数值最多支持 4 个参数: " + name;
        return {};
      }
      const std::string ad = "vx_adapter_" + sanitize_sym(name);
      const std::string sym = symbol_of(f->name);
      std::string body;
      std::string args;
      for (std::size_t i = 0; i < f->params.size(); ++i) {
        const std::string n = std::to_string(i);
        const VT pv = vt_of(f->params[i].type);
        std::string a = "%a" + n;
        if (pv == VT::I32) {
          body += "  %p" + n + " = trunc i64 %a" + n + " to i32\n";
          a = "%p" + n;
        } else if (pv == VT::F64) {
          body += "  %p" + n + " = bitcast i64 %a" + n + " to double\n";
          a = "%p" + n;
        } else if (pv == VT::PTR) {
          body += "  %p" + n + " = inttoptr i64 %a" + n + " to ptr\n";
          a = "%p" + n;
        }
        if (i != 0U)
          args += ", ";
        args += std::string(vt_name(pv)) + " " + a;
      }
      s += "define i64 @" + ad + "(i64 %a0, i64 %a1, i64 %a2, i64 %a3) {\nentry:\n" + body;
      if (is_void(f->ret_type)) {
        s += "  call void @" + sym + "(" + args + ")\n  ret i64 0\n";
      } else {
        const VT rv = vt_of(f->ret_type);
        s += "  %r = call " + std::string(vt_name(rv)) + " @" + sym + "(" + args + ")\n";
        if (rv == VT::I32) {
          s += "  %r64 = zext i32 %r to i64\n  ret i64 %r64\n";
        } else if (rv == VT::F64) {
          s += "  %r64 = bitcast double %r to i64\n  ret i64 %r64\n";
        } else if (rv == VT::PTR) {
          s += "  %r64 = ptrtoint ptr %r to i64\n  ret i64 %r64\n";
        } else {
          s += "  ret i64 %r\n";
        }
      }
      s += "}\n\n";
    }
    return s;
  }

  static std::string build_task_decls(bool task_helpers, bool pipe_helpers) {
    std::string s = "declare ptr @malloc(i64)\ndeclare void @free(ptr)\n";
    if (task_helpers) {
      s += "declare i32 @pthread_create(ptr, ptr, ptr, ptr)\n"
           "declare i32 @pthread_join(ptr, ptr)\n"
           "declare i32 @pthread_mutex_init(ptr, ptr)\n"
           "declare i32 @pthread_mutex_destroy(ptr)\n"
           "declare i32 @pthread_mutex_lock(ptr)\n"
           "declare i32 @pthread_mutex_unlock(ptr)\n"
           "declare i32 @pthread_cond_init(ptr, ptr)\n"
           "declare i32 @pthread_cond_destroy(ptr)\n"
           "declare i32 @pthread_cond_wait(ptr, ptr)\n"
           "declare i32 @pthread_cond_signal(ptr)\n"
           "declare i32 @pthread_cond_broadcast(ptr)\n";
    }
    if (pipe_helpers) {
      s += "declare i32 @pipe(ptr)\n"
           "declare i64 @read(i32, ptr, i64)\n"
           "declare i64 @write(i32, ptr, i64)\n"
           "declare i32 @close(i32)\n"
           "declare i64 @strlen(ptr)\n"
           "declare ptr @popen(ptr, ptr)\n"
           "declare i32 @pclose(ptr)\n"
           "declare i64 @fread(ptr, i64, i64, ptr)\n"
           "declare ptr @realloc(ptr, i64)\n";
    }
    s += "\n";
    return s;
  }

  static std::string build_thread_globals() {
    return "@vx_thread_count = global i32 0\n"
           "@vx_thread_slots = global [1024 x ptr] zeroinitializer\n"
           "@vx_thread_results = global [1024 x i64] zeroinitializer\n\n";
  }

  std::string build_thread_helpers() const {
    std::string s;
    // 线程入口：参数包 {fn, a0..a3（i64）, idx} → 调用适配器，结果写入 i64 结果槽
    s += "define ptr @vx_thread_entry(ptr %mem) {\nentry:\n";
    s += "  %fnp = getelementptr i8, ptr %mem, i64 0\n"
         "  %fn = load ptr, ptr %fnp\n"
         "  %a0p = getelementptr i8, ptr %mem, i64 8\n"
         "  %a0 = load i64, ptr %a0p\n"
         "  %a1p = getelementptr i8, ptr %mem, i64 16\n"
         "  %a1 = load i64, ptr %a1p\n"
         "  %a2p = getelementptr i8, ptr %mem, i64 24\n"
         "  %a2 = load i64, ptr %a2p\n"
         "  %a3p = getelementptr i8, ptr %mem, i64 32\n"
         "  %a3 = load i64, ptr %a3p\n"
         "  %idxp = getelementptr i8, ptr %mem, i64 40\n"
         "  %idx = load i32, ptr %idxp\n"
         "  %r = call i64 %fn(i64 %a0, i64 %a1, i64 %a2, i64 %a3)\n"
         "  %i64 = zext i32 %idx to i64\n"
         "  %rp = getelementptr [1024 x i64], ptr @vx_thread_results, i64 0, i64 %i64\n"
         "  store i64 %r, ptr %rp\n"
         "  call void @free(ptr %mem)\n"
         "  ret ptr null\n"
         "}\n\n";
    // 启动线程：句柄 1 起，0 = 失败；实参按 i64 原样透传（ptr 不会被截断）
    s += "define i32 @vx_thread_spawn_impl(ptr %fn, i64 %a0, i64 %a1, i64 %a2, i64 %a3) {\nentry:\n";
    s += "  %idx = atomicrmw add ptr @vx_thread_count, i32 1 monotonic\n"
         "  %in = icmp ult i32 %idx, 1024\n"
         "  br i1 %in, label %ok, label %bad\n"
         "ok:\n"
         "  %mem = call ptr @malloc(i64 48)\n"
         "  %fnp = getelementptr i8, ptr %mem, i64 0\n"
         "  store ptr %fn, ptr %fnp\n"
         "  %a0p = getelementptr i8, ptr %mem, i64 8\n"
         "  store i64 %a0, ptr %a0p\n"
         "  %a1p = getelementptr i8, ptr %mem, i64 16\n"
         "  store i64 %a1, ptr %a1p\n"
         "  %a2p = getelementptr i8, ptr %mem, i64 24\n"
         "  store i64 %a2, ptr %a2p\n"
         "  %a3p = getelementptr i8, ptr %mem, i64 32\n"
         "  store i64 %a3, ptr %a3p\n"
         "  %idxp = getelementptr i8, ptr %mem, i64 40\n"
         "  store i32 %idx, ptr %idxp\n"
         "  %i64 = zext i32 %idx to i64\n"
         "  %slot = getelementptr [1024 x ptr], ptr @vx_thread_slots, i64 0, i64 %i64\n"
         "  %rv = call i32 @pthread_create(ptr %slot, ptr null, ptr @vx_thread_entry, ptr %mem)\n"
         "  %okc = icmp eq i32 %rv, 0\n"
         "  br i1 %okc, label %good, label %fail\n"
         "good:\n"
         "  %h = add i32 %idx, 1\n"
         "  ret i32 %h\n"
         "fail:\n"
         "  call void @free(ptr %mem)\n"
         "  ret i32 0\n"
         "bad:\n"
         "  ret i32 0\n"
         "}\n\n";
    // 回收线程：返回适配器原始 i64 结果；由 join_* 调用点按类型转换
    s += "define i64 @vx_thread_join_raw(i32 %handle) {\nentry:\n";
    s += "  %zero = icmp eq i32 %handle, 0\n"
         "  br i1 %zero, label %bad, label %cont\n"
         "cont:\n"
         "  %idx = sub i32 %handle, 1\n"
         "  %in = icmp ult i32 %idx, 1024\n"
         "  br i1 %in, label %look, label %bad\n"
         "look:\n"
         "  %i64 = zext i32 %idx to i64\n"
         "  %slot = getelementptr [1024 x ptr], ptr @vx_thread_slots, i64 0, i64 %i64\n"
         "  %t = load ptr, ptr %slot\n"
         "  %tn = icmp eq ptr %t, null\n"
         "  br i1 %tn, label %bad, label %do\n"
         "do:\n"
         "  %rv = call i32 @pthread_join(ptr %t, ptr null)\n"
         "  %ok = icmp eq i32 %rv, 0\n"
         "  br i1 %ok, label %good, label %bad\n"
         "good:\n"
         "  %rp = getelementptr [1024 x i64], ptr @vx_thread_results, i64 0, i64 %i64\n"
         "  %res = load i64, ptr %rp\n"
         "  ret i64 %res\n"
         "bad:\n"
         "  ret i64 -1\n"
         "}\n\n";
    return s;
  }

  static std::string build_pool_globals() {
    return "@vx_pool_lock = global [64 x i8] zeroinitializer, align 8\n"
           "@vx_pool_cond = global [48 x i8] zeroinitializer, align 8\n"
           "@vx_pool_idle = global [48 x i8] zeroinitializer, align 8\n"
           "@vx_pool_fns = global [1024 x ptr] zeroinitializer\n"
           "@vx_pool_a0 = global [1024 x i64] zeroinitializer\n"
           "@vx_pool_a1 = global [1024 x i64] zeroinitializer\n"
           "@vx_pool_a2 = global [1024 x i64] zeroinitializer\n"
           "@vx_pool_a3 = global [1024 x i64] zeroinitializer\n"
           "@vx_pool_head = global i32 0\n"
           "@vx_pool_tail = global i32 0\n"
           "@vx_pool_queued = global i32 0\n"
           "@vx_pool_pending = global i32 0\n"
           "@vx_pool_active = global i32 0\n"
           "@vx_pool_shutdown = global i32 0\n"
           "@vx_pool_started = global i32 0\n"
           "@vx_pool_nworkers = global i32 0\n"
           "@vx_pool_workers = global [64 x ptr] zeroinitializer\n\n";
  }

  std::string build_pool_helpers() const {
    std::string s;
    // 工作线程：加锁取任务 → 解锁调用函数值 → 回写计数并通知空闲
    s += "define ptr @vx_pool_worker(ptr %unused) {\nentry:\n  br label %lock\n";
    s += "lock:\n"
         "  %mp = getelementptr [64 x i8], ptr @vx_pool_lock, i64 0, i64 0\n"
         "  %lr = call i32 @pthread_mutex_lock(ptr %mp)\n"
         "  br label %check\n"
         "check:\n"
         "  %q = load i32, ptr @vx_pool_queued\n"
         "  %empty = icmp eq i32 %q, 0\n"
         "  %sd = load i32, ptr @vx_pool_shutdown\n"
         "  %sdb = icmp ne i32 %sd, 0\n"
         "  %quit = and i1 %empty, %sdb\n"
         "  br i1 %quit, label %done, label %try\n"
         "try:\n"
         "  br i1 %empty, label %wait, label %pop\n"
         "wait:\n"
         "  %cp = getelementptr [48 x i8], ptr @vx_pool_cond, i64 0, i64 0\n"
         "  %wr = call i32 @pthread_cond_wait(ptr %cp, ptr %mp)\n"
         "  br label %check\n"
         "pop:\n"
         "  %head = load i32, ptr @vx_pool_head\n"
         "  %hi32 = and i32 %head, 1023\n"
         "  %hi = zext i32 %hi32 to i64\n"
         "  %fnp = getelementptr [1024 x ptr], ptr @vx_pool_fns, i64 0, i64 %hi\n"
         "  %fn = load ptr, ptr %fnp\n"
         "  %a0p = getelementptr [1024 x i64], ptr @vx_pool_a0, i64 0, i64 %hi\n"
         "  %a0 = load i64, ptr %a0p\n"
         "  %a1p = getelementptr [1024 x i64], ptr @vx_pool_a1, i64 0, i64 %hi\n"
         "  %a1 = load i64, ptr %a1p\n"
         "  %a2p = getelementptr [1024 x i64], ptr @vx_pool_a2, i64 0, i64 %hi\n"
         "  %a2 = load i64, ptr %a2p\n"
         "  %a3p = getelementptr [1024 x i64], ptr @vx_pool_a3, i64 0, i64 %hi\n"
         "  %a3 = load i64, ptr %a3p\n"
         "  %h2 = add i32 %head, 1\n"
         "  store i32 %h2, ptr @vx_pool_head\n"
         "  %q2 = sub i32 %q, 1\n"
         "  store i32 %q2, ptr @vx_pool_queued\n"
         "  %ac = load i32, ptr @vx_pool_active\n"
         "  %ac2 = add i32 %ac, 1\n"
         "  store i32 %ac2, ptr @vx_pool_active\n"
         "  %ur = call i32 @pthread_mutex_unlock(ptr %mp)\n"
         "  %r = call i64 %fn(i64 %a0, i64 %a1, i64 %a2, i64 %a3)\n"
         "  %lr2 = call i32 @pthread_mutex_lock(ptr %mp)\n"
         "  %ac3 = load i32, ptr @vx_pool_active\n"
         "  %ac4 = sub i32 %ac3, 1\n"
         "  store i32 %ac4, ptr @vx_pool_active\n"
         "  %pd = load i32, ptr @vx_pool_pending\n"
         "  %pd2 = sub i32 %pd, 1\n"
         "  store i32 %pd2, ptr @vx_pool_pending\n"
         "  %pz = icmp eq i32 %pd2, 0\n"
         "  %az = icmp eq i32 %ac4, 0\n"
         "  %idle = and i1 %pz, %az\n"
         "  br i1 %idle, label %sig, label %cont\n"
         "sig:\n"
         "  %ipc = getelementptr [48 x i8], ptr @vx_pool_idle, i64 0, i64 0\n"
         "  %br0 = call i32 @pthread_cond_broadcast(ptr %ipc)\n"
         "  br label %cont\n"
         "cont:\n"
         "  %ur2 = call i32 @pthread_mutex_unlock(ptr %mp)\n"
         "  br label %lock\n"
         "done:\n"
         "  %ur3 = call i32 @pthread_mutex_unlock(ptr %mp)\n"
         "  ret ptr null\n"
         "}\n\n";
    // 启动任务池：已启动则直接返回线程数；线程数夹到 1..64
    s += "define i32 @vx_pool_start_impl(i32 %workers) {\nentry:\n";
    s += "  %st = load i32, ptr @vx_pool_started\n"
         "  %run = icmp ne i32 %st, 0\n"
         "  br i1 %run, label %ret, label %init\n"
         "init:\n"
         "  %w0 = icmp slt i32 %workers, 1\n"
         "  %w1 = select i1 %w0, i32 1, i32 %workers\n"
         "  %w7 = icmp sgt i32 %w1, 64\n"
         "  %n = select i1 %w7, i32 64, i32 %w1\n"
         "  %mp = getelementptr [64 x i8], ptr @vx_pool_lock, i64 0, i64 0\n"
         "  %mi = call i32 @pthread_mutex_init(ptr %mp, ptr null)\n"
         "  %cp = getelementptr [48 x i8], ptr @vx_pool_cond, i64 0, i64 0\n"
         "  %ci = call i32 @pthread_cond_init(ptr %cp, ptr null)\n"
         "  %ip = getelementptr [48 x i8], ptr @vx_pool_idle, i64 0, i64 0\n"
         "  %ii = call i32 @pthread_cond_init(ptr %ip, ptr null)\n"
         "  store i32 0, ptr @vx_pool_head\n"
         "  store i32 0, ptr @vx_pool_tail\n"
         "  store i32 0, ptr @vx_pool_queued\n"
         "  store i32 0, ptr @vx_pool_pending\n"
         "  store i32 0, ptr @vx_pool_active\n"
         "  store i32 0, ptr @vx_pool_shutdown\n"
         "  store i32 %n, ptr @vx_pool_nworkers\n"
         "  store i32 1, ptr @vx_pool_started\n"
         "  %ip_i = alloca i32\n"
         "  store i32 0, ptr %ip_i\n"
         "  br label %loop\n"
         "loop:\n"
         "  %i = load i32, ptr %ip_i\n"
         "  %done = icmp sge i32 %i, %n\n"
         "  br i1 %done, label %ret, label %spawn\n"
         "spawn:\n"
         "  %i64 = zext i32 %i to i64\n"
         "  %slot = getelementptr [64 x ptr], ptr @vx_pool_workers, i64 0, i64 %i64\n"
         "  %rv = call i32 @pthread_create(ptr %slot, ptr null, ptr @vx_pool_worker, ptr null)\n"
         "  %i2 = add i32 %i, 1\n"
         "  store i32 %i2, ptr %ip_i\n"
         "  br label %loop\n"
         "ret:\n"
         "  %nr = load i32, ptr @vx_pool_nworkers\n"
         "  ret i32 %nr\n"
         "}\n\n";
    // 提交任务：队列满返回 0（调用方可重试），成功返回 1
    s += "define i32 @vx_pool_submit_impl(ptr %fn, i64 %a0, i64 %a1, i64 %a2, i64 %a3) {\nentry:\n";
    s += "  %st = load i32, ptr @vx_pool_started\n"
         "  %run = icmp ne i32 %st, 0\n"
         "  br i1 %run, label %lock, label %bad\n"
         "lock:\n"
         "  %mp = getelementptr [64 x i8], ptr @vx_pool_lock, i64 0, i64 0\n"
         "  %lr = call i32 @pthread_mutex_lock(ptr %mp)\n"
         "  %q = load i32, ptr @vx_pool_queued\n"
         "  %full = icmp uge i32 %q, 1024\n"
         "  br i1 %full, label %unlock_bad, label %put\n"
         "put:\n"
         "  %tail = load i32, ptr @vx_pool_tail\n"
         "  %ti32 = and i32 %tail, 1023\n"
         "  %ti = zext i32 %ti32 to i64\n"
         "  %fnp = getelementptr [1024 x ptr], ptr @vx_pool_fns, i64 0, i64 %ti\n"
         "  store ptr %fn, ptr %fnp\n"
         "  %p0 = getelementptr [1024 x i64], ptr @vx_pool_a0, i64 0, i64 %ti\n"
         "  store i64 %a0, ptr %p0\n"
         "  %p1 = getelementptr [1024 x i64], ptr @vx_pool_a1, i64 0, i64 %ti\n"
         "  store i64 %a1, ptr %p1\n"
         "  %p2 = getelementptr [1024 x i64], ptr @vx_pool_a2, i64 0, i64 %ti\n"
         "  store i64 %a2, ptr %p2\n"
         "  %p3 = getelementptr [1024 x i64], ptr @vx_pool_a3, i64 0, i64 %ti\n"
         "  store i64 %a3, ptr %p3\n"
         "  %t2 = add i32 %tail, 1\n"
         "  store i32 %t2, ptr @vx_pool_tail\n"
         "  %q2 = add i32 %q, 1\n"
         "  store i32 %q2, ptr @vx_pool_queued\n"
         "  %pd = load i32, ptr @vx_pool_pending\n"
         "  %pd2 = add i32 %pd, 1\n"
         "  store i32 %pd2, ptr @vx_pool_pending\n"
         "  %cp = getelementptr [48 x i8], ptr @vx_pool_cond, i64 0, i64 0\n"
         "  %sg = call i32 @pthread_cond_signal(ptr %cp)\n"
         "  %ur = call i32 @pthread_mutex_unlock(ptr %mp)\n"
         "  ret i32 1\n"
         "unlock_bad:\n"
         "  %ur2 = call i32 @pthread_mutex_unlock(ptr %mp)\n"
         "  ret i32 0\n"
         "bad:\n"
         "  ret i32 0\n"
         "}\n\n";
    // 等待全部任务完成
    s += "define i32 @vx_pool_wait_impl() {\nentry:\n";
    s += "  %st = load i32, ptr @vx_pool_started\n"
         "  %run = icmp ne i32 %st, 0\n"
         "  br i1 %run, label %lock, label %ret\n"
         "lock:\n"
         "  %mp = getelementptr [64 x i8], ptr @vx_pool_lock, i64 0, i64 0\n"
         "  %lr = call i32 @pthread_mutex_lock(ptr %mp)\n"
         "  br label %check\n"
         "check:\n"
         "  %pd = load i32, ptr @vx_pool_pending\n"
         "  %z = icmp eq i32 %pd, 0\n"
         "  br i1 %z, label %out, label %wait\n"
         "wait:\n"
         "  %ip = getelementptr [48 x i8], ptr @vx_pool_idle, i64 0, i64 0\n"
         "  %wr = call i32 @pthread_cond_wait(ptr %ip, ptr %mp)\n"
         "  br label %check\n"
         "out:\n"
         "  %ur = call i32 @pthread_mutex_unlock(ptr %mp)\n"
         "  ret i32 0\n"
         "ret:\n"
         "  ret i32 0\n"
         "}\n\n";
    // 停止任务池：置关闭标志 → 通知 → 逐个 join → 销毁同步对象
    s += "define i32 @vx_pool_stop_impl() {\nentry:\n";
    s += "  %st = load i32, ptr @vx_pool_started\n"
         "  %run = icmp ne i32 %st, 0\n"
         "  br i1 %run, label %shut, label %ret\n"
         "shut:\n"
         "  %mp = getelementptr [64 x i8], ptr @vx_pool_lock, i64 0, i64 0\n"
         "  %lr = call i32 @pthread_mutex_lock(ptr %mp)\n"
         "  store i32 1, ptr @vx_pool_shutdown\n"
         "  %cp = getelementptr [48 x i8], ptr @vx_pool_cond, i64 0, i64 0\n"
         "  %sg = call i32 @pthread_cond_broadcast(ptr %cp)\n"
         "  %ur = call i32 @pthread_mutex_unlock(ptr %mp)\n"
         "  %ip_i = alloca i32\n"
         "  store i32 0, ptr %ip_i\n"
         "  br label %loop\n"
         "loop:\n"
         "  %i = load i32, ptr %ip_i\n"
         "  %n = load i32, ptr @vx_pool_nworkers\n"
         "  %done = icmp sge i32 %i, %n\n"
         "  br i1 %done, label %fin, label %join\n"
         "join:\n"
         "  %i64 = zext i32 %i to i64\n"
         "  %slot = getelementptr [64 x ptr], ptr @vx_pool_workers, i64 0, i64 %i64\n"
         "  %t = load ptr, ptr %slot\n"
         "  %tn = icmp eq ptr %t, null\n"
         "  br i1 %tn, label %next, label %dojoin\n"
         "dojoin:\n"
         "  %jr = call i32 @pthread_join(ptr %t, ptr null)\n"
         "  br label %next\n"
         "next:\n"
         "  %i2 = add i32 %i, 1\n"
         "  store i32 %i2, ptr %ip_i\n"
         "  br label %loop\n"
         "fin:\n"
         "  %mp2 = getelementptr [64 x i8], ptr @vx_pool_lock, i64 0, i64 0\n"
         "  %lr2 = call i32 @pthread_mutex_lock(ptr %mp2)\n"
         "  store i32 0, ptr @vx_pool_started\n"
         "  store i32 0, ptr @vx_pool_shutdown\n"
         "  store i32 0, ptr @vx_pool_nworkers\n"
         "  store i32 0, ptr @vx_pool_queued\n"
         "  store i32 0, ptr @vx_pool_pending\n"
         "  store i32 0, ptr @vx_pool_active\n"
         "  %ur2 = call i32 @pthread_mutex_unlock(ptr %mp2)\n"
         "  %md = call i32 @pthread_mutex_destroy(ptr %mp2)\n"
         "  %cd = call i32 @pthread_cond_destroy(ptr %cp)\n"
         "  %idc = getelementptr [48 x i8], ptr @vx_pool_idle, i64 0, i64 0\n"
         "  %idcd = call i32 @pthread_cond_destroy(ptr %idc)\n"
         "  ret i32 0\n"
         "ret:\n"
         "  ret i32 0\n"
         "}\n\n";
    return s;
  }

  static std::string build_pipe_globals() {
    return "@vx_mode_r = private unnamed_addr constant [2 x i8] c\"r\\00\"\n\n";
  }

  static std::string build_pipe_helpers() {
    std::string s;
    // pipe()：malloc 8 字节承载两个 fd，返回其地址
    s += "define ptr @vx_proc_pipe_impl() {\nentry:\n"
         "  %m = call ptr @malloc(i64 8)\n"
         "  %r = call i32 @pipe(ptr %m)\n"
         "  %ok = icmp eq i32 %r, 0\n"
         "  br i1 %ok, label %good, label %bad\n"
         "good:\n"
         "  ret ptr %m\n"
         "bad:\n"
         "  call void @free(ptr %m)\n"
         "  ret ptr null\n"
         "}\n\n";
    // read / write：size_t 与返回值按 i64 转换，VX 侧保持 i32
    s += "define i32 @vx_proc_read_impl(i32 %fd, ptr %buf, i32 %n) {\nentry:\n"
         "  %n64 = sext i32 %n to i64\n"
         "  %r = call i64 @read(i32 %fd, ptr %buf, i64 %n64)\n"
         "  %t = trunc i64 %r to i32\n"
         "  ret i32 %t\n"
         "}\n\n";
    s += "define i32 @vx_proc_write_impl(i32 %fd, ptr %buf, i32 %n) {\nentry:\n"
         "  %n64 = sext i32 %n to i64\n"
         "  %r = call i64 @write(i32 %fd, ptr %buf, i64 %n64)\n"
         "  %t = trunc i64 %r to i32\n"
         "  ret i32 %t\n"
         "}\n\n";
    // read_str：读入缓冲并补 NUL，直接得到 string
    s += "define ptr @vx_proc_read_str_impl(i32 %fd, i32 %max) {\nentry:\n"
         "  %m64 = sext i32 %max to i64\n"
         "  %sz = add i64 %m64, 1\n"
         "  %buf = call ptr @malloc(i64 %sz)\n"
         "  %n = call i32 @vx_proc_read_impl(i32 %fd, ptr %buf, i32 %max)\n"
         "  %neg = icmp slt i32 %n, 0\n"
         "  %n2 = select i1 %neg, i32 0, i32 %n\n"
         "  %i64 = zext i32 %n2 to i64\n"
         "  %p = getelementptr i8, ptr %buf, i64 %i64\n"
         "  store i8 0, ptr %p\n"
         "  ret ptr %buf\n"
         "}\n\n";
    s += "define i32 @vx_proc_write_str_impl(i32 %fd, ptr %str) {\nentry:\n"
         "  %len = call i64 @strlen(ptr %str)\n"
         "  %len32 = trunc i64 %len to i32\n"
         "  %r = call i32 @vx_proc_write_impl(i32 %fd, ptr %str, i32 %len32)\n"
         "  ret i32 %r\n"
         "}\n\n";
    // capture：popen + 分块 fread（容量翻倍 realloc），返回 NUL 结尾的 string
    s += "define ptr @vx_proc_capture_impl(ptr %cmd) {\nentry:\n"
         "  %fp = call ptr @popen(ptr %cmd, ptr @vx_mode_r)\n"
         "  %nul = icmp eq ptr %fp, null\n"
         "  br i1 %nul, label %fail, label %init\n"
         "init:\n"
         "  %cap = alloca i64\n"
         "  %len = alloca i64\n"
         "  %buf = alloca ptr\n"
         "  store i64 65536, ptr %cap\n"
         "  store i64 0, ptr %len\n"
         "  %b0 = call ptr @malloc(i64 65537)\n"
         "  store ptr %b0, ptr %buf\n"
         "  br label %loop\n"
         "loop:\n"
         "  %capv = load i64, ptr %cap\n"
         "  %lenv = load i64, ptr %len\n"
         "  %rem = sub i64 %capv, %lenv\n"
         "  %full = icmp eq i64 %rem, 0\n"
         "  br i1 %full, label %grow, label %read\n"
         "grow:\n"
         "  %nc = mul i64 %capv, 2\n"
         "  store i64 %nc, ptr %cap\n"
         "  %ncs = add i64 %nc, 1\n"
         "  %b = load ptr, ptr %buf\n"
         "  %nb = call ptr @realloc(ptr %b, i64 %ncs)\n"
         "  store ptr %nb, ptr %buf\n"
         "  br label %read\n"
         "read:\n"
         "  %b2 = load ptr, ptr %buf\n"
         "  %l2 = load i64, ptr %len\n"
         "  %pos = getelementptr i8, ptr %b2, i64 %l2\n"
         "  %c2 = load i64, ptr %cap\n"
         "  %l3 = load i64, ptr %len\n"
         "  %space = sub i64 %c2, %l3\n"
         "  %n = call i64 @fread(ptr %pos, i64 1, i64 %space, ptr %fp)\n"
         "  %nz = icmp eq i64 %n, 0\n"
         "  %l4 = load i64, ptr %len\n"
         "  %l5 = add i64 %l4, %n\n"
         "  store i64 %l5, ptr %len\n"
         "  br i1 %nz, label %done, label %loop\n"
         "done:\n"
         "  %cl = call i32 @pclose(ptr %fp)\n"
         "  %b3 = load ptr, ptr %buf\n"
         "  %l6 = load i64, ptr %len\n"
         "  %end = getelementptr i8, ptr %b3, i64 %l6\n"
         "  store i8 0, ptr %end\n"
         "  ret ptr %b3\n"
         "fail:\n"
         "  ret ptr null\n"
         "}\n\n";
    return s;
  }

  bool fail_inst(std::string msg) {
    return fail("后端暂不支持 " + std::move(msg) + " (函数 " + cur_name_ + ", 指令 #" + std::to_string(cur_index_) +
                ")");
  }

  static std::string symbol_of(std::string_view name) {
    return name == "main" ? std::string("vx_main") : std::string(name);
  }

  std::string new_temp() { return "%t" + std::to_string(temp_++); }
  std::string new_bb() { return "bb" + std::to_string(bb_++); }

  void line(std::string s) { body_ += "  " + s + "\n"; }

  void ensure_open() {
    if (terminated_) {
      body_ += new_bb() + ":\n";
      terminated_ = false;
    }
  }

  bool pop(Value& v) {
    if (stack_.empty())
      return fail("操作数栈下溢（内部错误）");
    v = stack_.back();
    stack_.pop_back();
    return true;
  }

  void push(Value v) { stack_.push_back(std::move(v)); }

  // 确保槽位 k 存在且类型为 vt；类型冲突返回空串并置 ok=false
  std::string ensure_slot(std::uint32_t k, VT vt, bool& ok) {
    ok = true;
    if (k >= slot_vt_.size()) {
      ok = false;
      return {};
    }
    if (slot_vt_[k] < 0) {
      slot_vt_[k] = static_cast<std::int8_t>(vt);
    } else if (slot_vt_[k] != static_cast<std::int8_t>(vt)) {
      ok = false;
      return {};
    }
    if (slot_alloca_[k].empty()) {
      slot_alloca_[k] = "%s" + std::to_string(k);
      allocas_ += "  " + slot_alloca_[k] + " = alloca " + vt_name(vt) + "\n";
    }
    return slot_alloca_[k];
  }

  static bool parse_slot(std::string_view s, std::uint32_t& out) {
    auto [p, ec] = std::from_chars(s.data(), s.data() + s.size(), out);
    return ec == std::errc() && p == s.data() + s.size();
  }

  // I32 族指令要求整型操作数；浮点值混入时干净报错（而非产出非法 IR）。
  bool need_i32(const Value& v) {
    if (v.vt == VT::I32)
      return true;
    return fail("I32 指令操作数类型不符（期望 i32，实际 " + std::string(vt_name(v.vt)) + "，函数 " + cur_name_ +
                ", 指令 #" + std::to_string(cur_index_) + "）");
  }

  // ---- 单条指令发射 ----
  bool emit_inst(const ir::Inst& inst, std::size_t index) {
    cur_index_ = index;

    // 标签（OP_NOOP 携带标签名）
    if (inst.op == ir::OP_NOOP) {
      if (inst.args.empty())
        return true;
      if (!terminated_)
        body_ += "  br label %" + inst.args + "\n";
      body_ += inst.args + ":\n";
      terminated_ = false;
      return true;
    }

    ensure_open();

    switch (inst.op) {
      case ir::OP_CONST_INT: {
        std::string digits = inst.args.size() > 1 ? inst.args.substr(1) : "0";
        if (digits.empty())
          digits = "0";
        push({digits, VT::I32});
        return true;
      }
      case ir::OP_CONST_FLOAT: {
        std::string digits = inst.args.size() > 1 ? inst.args.substr(1) : "0";
        if (digits.empty())
          digits = "0";
        push({digits, VT::F64});
        return true;
      }
      case ir::OP_CONST_BOOL:
        push({inst.args == "btrue" ? "1" : "0", VT::I32});
        return true;
      case ir::OP_CONST_NIL:
        push({"null", VT::PTR});
        return true;
      case ir::OP_CONST_STRING: {
        std::string bytes = inst.args.size() > 1 ? inst.args.substr(1) : "";
        std::string gname = ".str." + std::to_string(globals_.size());
        globals_.push_back("@" + gname + " = private unnamed_addr constant [" + std::to_string(bytes.size() + 1) +
                           " x i8] c\"" + escape_bytes(bytes) + "\"");
        push({"@" + gname, VT::PTR});
        return true;
      }
      case ir::OP_LOAD_VAR: {
        std::uint32_t k = 0;
        if (!parse_slot(inst.args, k))
          return fail_inst("LOAD_VAR 槽位非法");
        VT vt = (k < slot_vt_.size() && slot_vt_[k] >= 0) ? static_cast<VT>(slot_vt_[k]) : VT::I32;
        bool ok = false;
        std::string al = ensure_slot(k, vt, ok);
        if (!ok)
          return fail_inst("LOAD_VAR 槽位类型冲突");
        std::string t = new_temp();
        line(t + " = load " + vt_name(vt) + ", ptr " + al);
        push({t, vt});
        return true;
      }
      case ir::OP_STORE_VAR: {
        Value v;
        if (!pop(v))
          return false;
        std::uint32_t k = 0;
        if (!parse_slot(inst.args, k))
          return fail_inst("STORE_VAR 槽位非法");
        bool ok = false;
        std::string al = ensure_slot(k, v.vt, ok);
        if (!ok)
          return fail_inst("STORE_VAR 槽位类型冲突");
        line("store " + std::string(vt_name(v.vt)) + " " + v.val + ", ptr " + al);
        return true;
      }
      case ir::OP_I32_ADD:
      case ir::OP_I32_SUB:
      case ir::OP_I32_MUL:
      case ir::OP_I32_DIV:
      case ir::OP_I32_MOD: {
        Value b;
        Value a;
        if (!pop(b) || !pop(a))
          return false;
        if (!need_i32(a) || !need_i32(b))
          return false;
        const char* op = "add";
        switch (inst.op) {
          case ir::OP_I32_SUB:
            op = "sub";
            break;
          case ir::OP_I32_MUL:
            op = "mul";
            break;
          case ir::OP_I32_DIV:
            op = "sdiv";
            break;
          case ir::OP_I32_MOD:
            op = "srem";
            break;
          default:
            break;
        }
        std::string t = new_temp();
        line(t + " = " + op + " i32 " + a.val + ", " + b.val);
        push({t, VT::I32});
        return true;
      }
      case ir::OP_I32_EQ:
      case ir::OP_I32_NEQ:
      case ir::OP_I32_LT:
      case ir::OP_I32_GT:
      case ir::OP_I32_LE:
      case ir::OP_I32_GE: {
        Value b;
        Value a;
        if (!pop(b) || !pop(a))
          return false;
        if (!need_i32(a) || !need_i32(b))
          return false;
        const char* pred = "eq";
        switch (inst.op) {
          case ir::OP_I32_NEQ:
            pred = "ne";
            break;
          case ir::OP_I32_LT:
            pred = "slt";
            break;
          case ir::OP_I32_GT:
            pred = "sgt";
            break;
          case ir::OP_I32_LE:
            pred = "sle";
            break;
          case ir::OP_I32_GE:
            pred = "sge";
            break;
          default:
            break;
        }
        std::string t = new_temp();
        line(t + " = icmp " + pred + " i32 " + a.val + ", " + b.val);
        std::string z = new_temp();
        line(z + " = zext i1 " + t + " to i32");
        push({z, VT::I32});
        return true;
      }
      case ir::OP_I32_NEG: {
        Value a;
        if (!pop(a))
          return false;
        if (!need_i32(a))
          return false;
        std::string t = new_temp();
        line(t + " = sub i32 0, " + a.val);
        push({t, VT::I32});
        return true;
      }
      case ir::OP_F64_ADD:
      case ir::OP_F64_SUB:
      case ir::OP_F64_MUL:
      case ir::OP_F64_DIV: {
        Value b;
        Value a;
        if (!pop(b) || !pop(a))
          return false;
        if (a.vt != VT::F64 || b.vt != VT::F64)
          return fail_inst("F64 算术操作数非 double");
        const char* op = "fadd";
        switch (inst.op) {
          case ir::OP_F64_SUB:
            op = "fsub";
            break;
          case ir::OP_F64_MUL:
            op = "fmul";
            break;
          case ir::OP_F64_DIV:
            op = "fdiv";
            break;
          default:
            break;
        }
        std::string t = new_temp();
        line(t + " = " + op + " double " + a.val + ", " + b.val);
        push({t, VT::F64});
        return true;
      }
      case ir::OP_F64_EQ:
      case ir::OP_F64_NE:
      case ir::OP_F64_LT:
      case ir::OP_F64_GT:
      case ir::OP_F64_LE:
      case ir::OP_F64_GE: {
        Value b;
        Value a;
        if (!pop(b) || !pop(a))
          return false;
        if (a.vt != VT::F64 || b.vt != VT::F64)
          return fail_inst("F64 比较操作数非 double");
        const char* pred = "oeq";
        switch (inst.op) {
          case ir::OP_F64_NE:
            pred = "one";
            break;
          case ir::OP_F64_LT:
            pred = "olt";
            break;
          case ir::OP_F64_GT:
            pred = "ogt";
            break;
          case ir::OP_F64_LE:
            pred = "ole";
            break;
          case ir::OP_F64_GE:
            pred = "oge";
            break;
          default:
            break;
        }
        std::string t = new_temp();
        line(t + " = fcmp " + pred + " double " + a.val + ", " + b.val);
        std::string z = new_temp();
        line(z + " = zext i1 " + t + " to i32");
        push({z, VT::I32});
        return true;
      }
      case ir::OP_F64_NEG: {
        Value a;
        if (!pop(a))
          return false;
        if (a.vt != VT::F64)
          return fail_inst("F64_NEG 操作数非 double");
        std::string t = new_temp();
        line(t + " = fneg double " + a.val);
        push({t, VT::F64});
        return true;
      }
      case ir::OP_BOOL_NOT: {
        Value a;
        if (!pop(a))
          return false;
        if (!need_i32(a))
          return false;
        std::string t = new_temp();
        line(t + " = icmp eq i32 " + a.val + ", 0");
        std::string z = new_temp();
        line(z + " = zext i1 " + t + " to i32");
        push({z, VT::I32});
        return true;
      }
      case ir::OP_BOOL_AND:
      case ir::OP_I32_AND:
      case ir::OP_BOOL_OR:
      case ir::OP_I32_OR: {
        Value b;
        Value a;
        if (!pop(b) || !pop(a))
          return false;
        if (!need_i32(a) || !need_i32(b))
          return false;
        const bool is_and = inst.op == ir::OP_BOOL_AND || inst.op == ir::OP_I32_AND;
        std::string t = new_temp();
        line(t + " = " + (is_and ? "and" : "or") + " i32 " + a.val + ", " + b.val);
        push({t, VT::I32});
        return true;
      }
      case ir::OP_STR_CONCAT: {
        Value b;
        Value a;
        if (!pop(b) || !pop(a))
          return false;
        used_builtins_.insert("vx_str_concat");
        std::string t = new_temp();
        line(t + " = call ptr @vx_str_concat(ptr " + a.val + ", ptr " + b.val + ")");
        push({t, VT::PTR});
        return true;
      }
      case ir::OP_DEREF: {
        // *p：按 32 位字读取（非 ptr 值先转 inttoptr）
        Value p;
        if (!pop(p))
          return false;
        std::string base = p.val;
        if (p.vt != VT::PTR) {
          base = new_temp();
          line(base + " = inttoptr i32 " + p.val + " to ptr");
        }
        std::string t = new_temp();
        line(t + " = load i32, ptr " + base);
        push({t, VT::I32});
        return true;
      }
      case ir::OP_DEREF_STORE: {
        Value v;
        Value p;
        if (!pop(v) || !pop(p))
          return false;
        std::string base = p.val;
        if (p.vt != VT::PTR) {
          base = new_temp();
          line(base + " = inttoptr i32 " + p.val + " to ptr");
        }
        std::string val = v.val;
        if (v.vt == VT::PTR) {
          val = new_temp();
          line(val + " = ptrtoint ptr " + v.val + " to i32");
        }
        line("store i32 " + val + ", ptr " + base);
        return true;
      }
      case ir::OP_CONST_FUNC: {
        std::string_view body = inst.args;
        if (!body.starts_with("f;"))
          return fail_inst("CONST_FUNC 参数格式非法: " + std::string(body));
        const std::string fname(body.substr(2));
        const auto it = fn_by_name_.find(fname);
        if (it == fn_by_name_.end())
          return fail_inst("函数值引用的函数不存在: " + fname);
        if (it->second->params.size() > 4)
          return fail_inst("函数值最多支持 4 个参数: " + fname);
        func_refs_.insert(fname);
        push({"@vx_adapter_" + sanitize_sym(fname), VT::PTR});
        return true;
      }
      case ir::OP_STR_EQ: {
        Value b;
        Value a;
        if (!pop(b) || !pop(a))
          return false;
        if (a.vt != VT::PTR || b.vt != VT::PTR) {
          return fail_inst("STR_EQ 操作数须为字符串");
        }
        used_builtins_.insert("strcmp");
        std::string c = new_temp();
        line(c + " = call i32 @strcmp(ptr " + a.val + ", ptr " + b.val + ")");
        std::string t = new_temp();
        line(t + " = icmp eq i32 " + c + ", 0");
        std::string z = new_temp();
        line(z + " = zext i1 " + t + " to i32");
        push({z, VT::I32});
        return true;
      }
      case ir::OP_DUP: {
        if (stack_.empty())
          return fail_inst("DUP 栈为空");
        push(stack_.back());
        return true;
      }
      case ir::OP_POP: {
        Value v;
        return pop(v);
      }
      // ---- P4: NEWZ / FREE ----
      case ir::OP_NEWZ: {
        std::size_t sep = inst.args.find(';');
        std::size_t bytes = 8;
        if (sep != std::string::npos) {
          try {
            bytes = std::stoull(inst.args.substr(sep + 1));
          } catch (const std::invalid_argument&) {
            bytes = 8;
          } catch (const std::out_of_range&) {
            bytes = 8;
          }
        }
        if (bytes == 0)
          bytes = 8;
        used_builtins_.insert("malloc");
        std::string t = new_temp();
        line(t + " = call ptr @malloc(i64 " + std::to_string(bytes) + ")");
        push({t, VT::PTR});
        return true;
      }
      case ir::OP_FREE: {
        Value p;
        if (!pop(p))
          return false;
        used_builtins_.insert("free");
        line("call void @free(ptr " + p.val + ")");
        push({"0", VT::I32});
        return true;
      }
      // ---- P4: FIELD_LOAD / FIELD_STORE ----
      case ir::OP_FIELD_LOAD: {
        Value obj;
        if (!pop(obj))
          return false;
        if (obj.vt != VT::PTR)
          return fail_inst("FIELD_LOAD 对象须为指针");
        auto it = field_index_.find(inst.args);
        if (it == field_index_.end())
          return fail_inst("未知字段 " + inst.args);
        std::size_t idx = it->second;
        VT ft = (static_cast<unsigned int>(field_type_.contains(inst.args)) != 0U) ? field_type_[inst.args] : VT::I32;
        std::string gep = new_temp();
        line(gep + " = getelementptr i8, ptr " + obj.val + ", i64 " + std::to_string(idx * 8));
        std::string t = new_temp();
        line(t + " = load " + std::string(vt_name(ft)) + ", ptr " + gep);
        push({t, ft});
        return true;
      }
      case ir::OP_FIELD_STORE: {
        Value val;
        Value obj;
        if (!pop(val) || !pop(obj))
          return false;
        if (obj.vt != VT::PTR)
          return fail_inst("FIELD_STORE 对象须为指针");
        auto it = field_index_.find(inst.args);
        if (it == field_index_.end())
          return fail_inst("未知字段 " + inst.args);
        std::size_t idx = it->second;
        VT ft = (static_cast<unsigned int>(field_type_.contains(inst.args)) != 0U) ? field_type_[inst.args] : VT::I32;
        std::string v = val.val;
        VT vt = val.vt;
        // 类型转换到字段类型
        auto conv = [&](VT to) {
          if (vt == to)
            return;
          std::string t = new_temp();
          if (to == VT::I32 && vt == VT::I64) {
            line(t + " = trunc i64 " + v + " to i32");
          } else if (to == VT::I64 && vt == VT::I32) {
            line(t + " = zext i32 " + v + " to i64");
          } else if (to == VT::F64 && vt == VT::I32) {
            line(t + " = sitofp i32 " + v + " to double");
          } else if (to == VT::I32 && vt == VT::F64) {
            line(t + " = fptosi double " + v + " to i32");
          } else if (to == VT::PTR && vt == VT::I64) {
            line(t + " = inttoptr i64 " + v + " to ptr");
          } else if (to == VT::I64 && vt == VT::PTR) {
            line(t + " = ptrtoint ptr " + v + " to i64");
          } else { /* no-op conversion */
          }
          v = t;
          vt = to;
        };
        conv(ft);
        std::string gep = new_temp();
        line(gep + " = getelementptr i8, ptr " + obj.val + ", i64 " + std::to_string(idx * 8));
        line("store " + std::string(vt_name(ft)) + " " + v + ", ptr " + gep);
        return true;
      }
      // ---- P4: ARRAY_NEW ----
      case ir::OP_ARRAY_NEW: {
        int n = 0;
        try {
          n = std::stoi(inst.args);
        } catch (const std::invalid_argument&) {
          n = 0;
        } catch (const std::out_of_range&) {
          n = 0;
        }
        n = std::max(n, 0);
        std::vector<Value> vals(n);
        for (int i = n - 1; i >= 0; --i) {
          if (!pop(vals[i]))
            return false;
        }
        used_builtins_.insert("vx_array_new");
        used_builtins_.insert("vx_array_set");
        std::string arr = new_temp();
        line(arr + " = call ptr @vx_array_new(i64 " + std::to_string(n) + ")");
        for (int i = 0; i < n; ++i) {
          std::string iv = vals[i].val;
          VT vt = vals[i].vt;
          if (vt == VT::I32) {
            std::string t = new_temp();
            line(t + " = zext i32 " + iv + " to i64");
            iv = t;
          } else if (vt == VT::F64) {
            std::string t = new_temp();
            line(t + " = bitcast double " + iv + " to i64");
            iv = t;
          } else if (vt == VT::PTR) {
            std::string t = new_temp();
            line(t + " = ptrtoint ptr " + iv + " to i64");
            iv = t;
          }
          line("call void @vx_array_set(ptr " + arr + ", i64 " + std::to_string(i) + ", i64 " + iv + ")");
        }
        push({arr, VT::PTR});
        return true;
      }
      // ---- P4: INDEX_GET ----
      case ir::OP_INDEX_GET: {
        Value idxv;
        Value objv;
        if (!pop(idxv) || !pop(objv))
          return false;
        std::string idx = idxv.val;
        if (idxv.vt == VT::I32) {
          std::string t = new_temp();
          line(t + " = sext i32 " + idx + " to i64");
          idx = t;
        }
        if (inst.args == "map") {
          used_builtins_.insert("vx_map_get");
          std::string keyptr = "null";
          std::string isstr = "0";
          std::string keyi64 = idx;
          if (idxv.vt == VT::PTR) {
            keyptr = idxv.val;
            isstr = "1";
            std::string t = new_temp();
            line(t + " = ptrtoint ptr " + idxv.val + " to i64");
            keyi64 = t;
          }
          std::string t = new_temp();
          line(t + " = call i64 @vx_map_get(ptr " + objv.val + ", i64 " + keyi64 + ", i64 " + isstr + ", ptr " +
               keyptr + ")");
          std::string r = new_temp();
          line(r + " = trunc i64 " + t + " to i32");
          push({r, VT::I32});
          return true;
        }
        used_builtins_.insert("vx_array_get");
        std::string t = new_temp();
        line(t + " = call i64 @vx_array_get(ptr " + objv.val + ", i64 " + idx + ")");
        std::string elem = inst.args.empty() ? std::string("i32") : inst.args;
        if (elem == "f64") {
          std::string r = new_temp();
          line(r + " = bitcast i64 " + t + " to double");
          push({r, VT::F64});
        } else if (elem == "ptr") {
          std::string r = new_temp();
          line(r + " = inttoptr i64 " + t + " to ptr");
          push({r, VT::PTR});
        } else {
          std::string r = new_temp();
          line(r + " = trunc i64 " + t + " to i32");
          push({r, VT::I32});
        }
        return true;
      }
      // ---- P4: INDEX_SET ----
      case ir::OP_INDEX_SET: {
        Value val;
        Value idxv;
        Value objv;
        if (!pop(val) || !pop(idxv) || !pop(objv))
          return false;
        std::string idx = idxv.val;
        if (idxv.vt == VT::I32) {
          std::string t = new_temp();
          line(t + " = sext i32 " + idx + " to i64");
          idx = t;
        }
        std::string iv = val.val;
        if (val.vt == VT::I32) {
          std::string t = new_temp();
          line(t + " = zext i32 " + iv + " to i64");
          iv = t;
        } else if (val.vt == VT::F64) {
          std::string t = new_temp();
          line(t + " = bitcast double " + iv + " to i64");
          iv = t;
        } else if (val.vt == VT::PTR) {
          std::string t = new_temp();
          line(t + " = ptrtoint ptr " + iv + " to i64");
          iv = t;
        }
        if (inst.args == "map") {
          used_builtins_.insert("vx_map_set");
          std::string keyptr = "null";
          std::string isstr = "0";
          std::string keyi64 = idx;
          if (idxv.vt == VT::PTR) {
            keyptr = idxv.val;
            isstr = "1";
            std::string t = new_temp();
            line(t + " = ptrtoint ptr " + idxv.val + " to i64");
            keyi64 = t;
          }
          line("call void @vx_map_set(ptr " + objv.val + ", i64 " + keyi64 + ", i64 " + isstr + ", ptr " + keyptr +
               ", i64 " + iv + ")");
          return true;
        }
        used_builtins_.insert("vx_array_set");
        line("call void @vx_array_set(ptr " + objv.val + ", i64 " + idx + ", i64 " + iv + ")");
        return true;
      }
      // ---- P4: MAP_NEW ----
      case ir::OP_MAP_NEW: {
        int n = 0;
        try {
          n = std::stoi(inst.args);
        } catch (const std::invalid_argument&) {
          n = 0;
        } catch (const std::out_of_range&) {
          n = 0;
        }
        n = std::max(n, 0);
        std::vector<Value> keys(n);
        std::vector<Value> vals(n);
        for (int i = n - 1; i >= 0; --i) {
          if (!pop(vals[i]))
            return false;
          if (!pop(keys[i]))
            return false;
        }
        used_builtins_.insert("vx_map_new");
        used_builtins_.insert("vx_map_set");
        std::string m = new_temp();
        line(m + " = call ptr @vx_map_new(i64 " + std::to_string(n == 0 ? 1 : n) + ")");
        for (int i = 0; i < n; ++i) {
          std::string keyi64 = keys[i].val;
          std::string keyptr = "null";
          std::string isstr = "0";
          if (keys[i].vt == VT::I32) {
            std::string t = new_temp();
            line(t + " = zext i32 " + keyi64 + " to i64");
            keyi64 = t;
          } else if (keys[i].vt == VT::F64) {
            std::string t = new_temp();
            line(t + " = bitcast double " + keyi64 + " to i64");
            keyi64 = t;
          } else if (keys[i].vt == VT::PTR) {
            keyptr = keys[i].val;
            isstr = "1";
            std::string t = new_temp();
            line(t + " = ptrtoint ptr " + keys[i].val + " to i64");
            keyi64 = t;
          }
          std::string iv = vals[i].val;
          if (vals[i].vt == VT::I32) {
            std::string t = new_temp();
            line(t + " = zext i32 " + iv + " to i64");
            iv = t;
          } else if (vals[i].vt == VT::F64) {
            std::string t = new_temp();
            line(t + " = bitcast double " + iv + " to i64");
            iv = t;
          } else if (vals[i].vt == VT::PTR) {
            std::string t = new_temp();
            line(t + " = ptrtoint ptr " + iv + " to i64");
            iv = t;
          }
          line("call void @vx_map_set(ptr " + m + ", i64 " + keyi64 + ", i64 " + isstr + ", ptr " + keyptr + ", i64 " +
               iv + ")");
        }
        push({m, VT::PTR});
        return true;
      }
      case ir::OP_CALL:
        return emit_call(inst);
      case ir::OP_JUMP: {
        if (inst.args.empty())
          return fail_inst("JUMP 缺标签");
        line("br label %" + inst.args);
        terminated_ = true;
        return true;
      }
      case ir::OP_JUMP_IF_FALSE:
      case ir::OP_JUMP_IF_TRUE: {
        Value c;
        if (!pop(c))
          return false;
        if (!need_i32(c))
          return false;
        std::string t = new_temp();
        const char* pred = inst.op == ir::OP_JUMP_IF_FALSE ? "eq" : "ne";
        line(std::string(t) + " = icmp " + pred + " i32 " + c.val + ", 0");
        std::string fb = new_bb();
        line("br i1 " + t + ", label %" + inst.args + ", label %" + fb);
        body_ += fb + ":\n";
        terminated_ = false;
        return true;
      }
      case ir::OP_RETURN: {
        if (ret_void_) {
          if (!stack_.empty()) {
            Value v;
            pop(v);
          }
          line("ret void");
          terminated_ = true;
          return true;
        }
        Value v;
        if (!pop(v))
          return false;
        if (v.vt != ret_vt_)
          return fail_inst("RETURN 类型与函数返回类型不符");
        line("ret " + std::string(vt_name(ret_vt_)) + " " + v.val);
        terminated_ = true;
        return true;
      }
      case ir::OP_RETURN_VOID:
        line("ret void");
        terminated_ = true;
        return true;
      case ir::OP_HALT:
        line(ret_void_ ? "ret void" : "ret " + std::string(vt_name(ret_vt_)) + " 0");
        terminated_ = true;
        return true;
      default:
        return fail_inst(std::string("opcode ") + op_name(inst.op));
    }
  }

  // 解析调用目标并发射 call
  bool emit_call(const ir::Inst& inst) {
    std::string_view body = inst.args;
    if (body.size() < 2 || body[1] != ';')
      return fail_inst("CALL 参数格式非法: " + std::string(body));
    const char kind = body[0];
    const std::string name(body.substr(2));

    // proc 线程 / 指针内建：后端直接生成 IR，不走通用调用路径
    if (kind == 'f') {
      const TaskCall tc = task_call_of(name);
      if (tc != TaskCall::None)
        return emit_task_call(tc);
      if (name == "_vx_ptr_add" || name == "_vx_ptr_to_int" || name == "_vx_ptr_from_int")
        return emit_ptr_call(name);
      if (name.starts_with("_vx_proc_atomic_"))
        return emit_atomic_call(name);
    }

    std::string sym;
    std::vector<VT> arg_vts;
    bool has_ret = false;
    VT ret = VT::I32;

    if (kind == 'f') {
      auto it = fn_by_name_.find(name);
      if (it != fn_by_name_.end()) {
        const ir::FuncIR& callee = *it->second;
        sym = symbol_of(callee.name);
        for (const vx::compiler::Param& p : callee.params)
          arg_vts.push_back(vt_of(p.type));
        has_ret = !is_void(callee.ret_type);
        ret = has_ret ? vt_of(callee.ret_type) : VT::I32;
      } else if (const Builtin* b = lookup_builtin(name)) {
        sym = b->sym;
        arg_vts = b->args;
        has_ret = b->has_ret;
        ret = b->ret;
        if (b->helper)
          wants_pipe_ = true; // 由后端生成 IR 实现，不产生外部 declare
        else
          used_builtins_.insert(sym);
      } else if (auto it = extern_by_sym_.find(name); it != extern_by_sym_.end()) {
        // 来自 linkage 的外部符号（含 @imp / @cppimp）：用其签名发射 declare
        const ir::LinkEntry* e = it->second;
        sym = name;
        for (const vx::compiler::ir::AbiParam& p : e->params)
          arg_vts.push_back(vt_of(p.type));
        has_ret = !is_void(e->ret_type);
        ret = has_ret ? vt_of(e->ret_type) : VT::I32;
        used_externs_.insert(sym);
      } else {
        return fail_inst("未知调用目标 @" + name + "（extern 签名未序列化，且不在内置表）");
      }
    } else if (kind == 'm') {
      const ir::FuncIR* found = nullptr;
      if (name.contains('.')) {
        // 全限定符号（语义分析解析出的重载方法 "Class.method$<序号>"）：
        // 按具名函数精确匹配
        auto it = fn_by_name_.find(std::string_view(name));
        if (it != fn_by_name_.end()) {
          found = it->second;
        }
      } else {
        // 未限定方法名：查找唯一名为 "*.<method>" 的函数
        const std::string suffix = "." + name;
        for (const ir::FuncIR& f : mod_.funcs) {
          if (f.name.size() > suffix.size() && f.name.ends_with(suffix)) {
            if (found != nullptr)
              return fail_inst("方法 " + name + " 匹配到多个函数");
            found = &f;
          }
        }
      }
      if (found == nullptr)
        return fail_inst("未知方法 ." + name);
      sym = symbol_of(found->name);
      for (const vx::compiler::Param& p : found->params)
        arg_vts.push_back(vt_of(p.type));
      has_ret = !is_void(found->ret_type);
      ret = has_ret ? vt_of(found->ret_type) : VT::I32;
    } else {
      return fail_inst("CALL 调用类别非法: " + std::string(1, kind));
    }

    // 弹参（栈顶为最后一个实参）
    std::vector<Value> args(arg_vts.size());
    for (std::size_t i = arg_vts.size(); i-- > 0;) {
      if (!pop(args[i]))
        return false;
    }

    std::string call;
    std::string temp;
    if (has_ret) {
      temp = new_temp();
      call = temp + " = call " + vt_name(ret);
    } else {
      call = "call void";
    }
    call += " @" + sym + "(";
    for (std::size_t i = 0; i < args.size(); ++i) {
      if (i != 0U)
        call += ", ";
      std::string operand = args[i].val;
      // 实参值类型与形参声明不一致时按位型互转：FFI 边界放行任意实参类型
      // （如 waitpid(pid, 0, ...) 用整型 0 表示 NULL 的 ptr 形参），不转则 IR 类型不匹配。
      if (args[i].vt != arg_vts[i]) {
        if (arg_vts[i] == VT::PTR && args[i].vt == VT::I32) {
          const std::string t = new_temp();
          line(t + " = inttoptr i32 " + operand + " to ptr");
          operand = t;
        } else if (arg_vts[i] == VT::I32 && args[i].vt == VT::PTR) {
          const std::string t = new_temp();
          line(t + " = ptrtoint ptr " + operand + " to i32");
          operand = t;
        } else if (arg_vts[i] == VT::I64 && args[i].vt == VT::I32) {
          // 宽化：如 malloc(n * 4)（size_t 形参为 i64）
          const std::string t = new_temp();
          line(t + " = sext i32 " + operand + " to i64");
          operand = t;
        } else if (arg_vts[i] == VT::I32 && args[i].vt == VT::I64) {
          const std::string t = new_temp();
          line(t + " = trunc i64 " + operand + " to i32");
          operand = t;
        }
      }
      call += std::string(vt_name(arg_vts[i])) + " " + operand;
    }
    call += ")";
    line(call);
    // 栈机约定：每个表达式恰占一个栈槽。void 调用不留结果，但上游 codegen
    // 对 expr_stmt 恒发 OP_POP（对齐自举 codegen.vx），故此处压入占位值，
    // 使随后的 POP 有槽可弹（占位值不参与任何运算）。
    if (has_ret) {
      push({temp, ret});
    } else {
      push({"0", VT::I32});
    }
    return true;
  }

  // proc 线程 / 任务池调用：后端生成 IR 实现，不经通用 extern 路径
  bool emit_task_call(TaskCall tc) {
    auto as_ptr = [&](const Value& v) -> std::string {
      if (v.vt == VT::PTR)
        return v.val;
      std::string t = new_temp();
      line(t + " = inttoptr i32 " + v.val + " to ptr");
      return t;
    };
    auto as_i32 = [&](const Value& v) -> std::string {
      if (v.vt != VT::PTR)
        return v.val;
      std::string t = new_temp();
      line(t + " = ptrtoint ptr " + v.val + " to i32");
      return t;
    };
    // 任务实参按 64 位透传：ptr 用 ptrtoint、int 用 sext、float 用 bitcast，
    // 由被调函数适配器按自身形参类型转回；这样 64 位指针不会被截断。
    auto as_i64 = [&](const Value& v) -> std::string {
      if (v.vt == VT::I64)
        return v.val;
      std::string t = new_temp();
      if (v.vt == VT::PTR) {
        line(t + " = ptrtoint ptr " + v.val + " to i64");
      } else if (v.vt == VT::F64) {
        line(t + " = bitcast double " + v.val + " to i64");
      } else {
        line(t + " = sext i32 " + v.val + " to i64");
      }
      return t;
    };

    switch (tc) {
      case TaskCall::Spawn0:
      case TaskCall::Spawn1:
      case TaskCall::Spawn2:
      case TaskCall::Spawn3:
      case TaskCall::Spawn4: {
        const int n = task_arg_count(tc);
        std::vector<Value> av(static_cast<std::size_t>(n));
        for (int i = n - 1; i >= 0; --i) {
          if (!pop(av[static_cast<std::size_t>(i)]))
            return false;
        }
        Value fn;
        if (!pop(fn))
          return false;
        const std::string fnop = as_ptr(fn);
        const std::string t = new_temp();
        std::string callline = t + " = call i32 @vx_thread_spawn_impl(ptr " + fnop;
        for (int i = 0; i < 4; ++i) {
          const std::string arg = i < n ? as_i64(av[static_cast<std::size_t>(i)]) : std::string("0");
          callline += ", i64 " + arg;
        }
        callline += ")";
        line(callline);
        push({t, VT::I32});
        wants_threads_ = true;
        return true;
      }
      case TaskCall::PoolSubmit0:
      case TaskCall::PoolSubmit1:
      case TaskCall::PoolSubmit2:
      case TaskCall::PoolSubmit3:
      case TaskCall::PoolSubmit4: {
        const int n = task_arg_count(tc);
        std::vector<Value> av(static_cast<std::size_t>(n));
        for (int i = n - 1; i >= 0; --i) {
          if (!pop(av[static_cast<std::size_t>(i)]))
            return false;
        }
        Value fn;
        if (!pop(fn))
          return false;
        const std::string fnop = as_ptr(fn);
        const std::string t = new_temp();
        std::string line_txt = t + " = call i32 @vx_pool_submit_impl(ptr " + fnop;
        for (int i = 0; i < 4; ++i) {
          const std::string arg = i < n ? as_i64(av[static_cast<std::size_t>(i)]) : std::string("0");
          line_txt += ", i64 " + arg;
        }
        line_txt += ")";
        line(line_txt);
        push({t, VT::I32});
        wants_pool_ = true;
        return true;
      }
      case TaskCall::JoinInt:
      case TaskCall::JoinFloat:
      case TaskCall::JoinStr:
      case TaskCall::JoinPtr: {
        Value h;
        if (!pop(h))
          return false;
        std::string raw = new_temp();
        line(raw + " = call i64 @vx_thread_join_raw(i32 " + as_i32(h) + ")");
        std::string t = new_temp();
        if (tc == TaskCall::JoinFloat) {
          line(t + " = bitcast i64 " + raw + " to double");
          push({t, VT::F64});
        } else if (tc == TaskCall::JoinStr || tc == TaskCall::JoinPtr) {
          line(t + " = inttoptr i64 " + raw + " to ptr");
          push({t, VT::PTR});
        } else {
          line(t + " = trunc i64 " + raw + " to i32");
          push({t, VT::I32});
        }
        wants_threads_ = true;
        return true;
      }
      case TaskCall::PoolStart: {
        Value w;
        if (!pop(w))
          return false;
        std::string t = new_temp();
        line(t + " = call i32 @vx_pool_start_impl(i32 " + as_i32(w) + ")");
        push({t, VT::I32});
        wants_pool_ = true;
        return true;
      }
      case TaskCall::PoolWait:
      case TaskCall::PoolStop: {
        std::string t = new_temp();
        line(t + std::string(" = call i32 @") +
             (tc == TaskCall::PoolWait ? "vx_pool_wait_impl()" : "vx_pool_stop_impl()"));
        push({t, VT::I32});
        wants_pool_ = true;
        return true;
      }
      default:
        return fail_inst("未知线程/任务池调用");
    }
  }

  // ptr 库内建：add（字节偏移）/ to_int / from_int，直接发射指针 IR
  bool emit_ptr_call(std::string_view name) {
    if (name == "_vx_ptr_add") {
      Value off;
      Value p;
      if (!pop(off) || !pop(p))
        return false;
      std::string base = p.val;
      if (p.vt != VT::PTR) {
        base = new_temp();
        line(base + " = inttoptr i32 " + p.val + " to ptr");
      }
      std::string o64 = off.val;
      if (off.vt == VT::I32) {
        o64 = new_temp();
        line(o64 + " = sext i32 " + off.val + " to i64");
      }
      std::string t = new_temp();
      line(t + " = getelementptr i8, ptr " + base + ", i64 " + o64);
      push({t, VT::PTR});
      return true;
    }
    if (name == "_vx_ptr_to_int") {
      Value p;
      if (!pop(p))
        return false;
      std::string base = p.val;
      if (p.vt != VT::PTR) {
        base = new_temp();
        line(base + " = inttoptr i32 " + p.val + " to ptr");
      }
      std::string i64 = new_temp();
      line(i64 + " = ptrtoint ptr " + base + " to i64");
      std::string t = new_temp();
      line(t + " = trunc i64 " + i64 + " to i32");
      push({t, VT::I32});
      return true;
    }
    if (name == "_vx_ptr_from_int") {
      Value v;
      if (!pop(v))
        return false;
      std::string v64 = v.val;
      if (v.vt == VT::I32) {
        v64 = new_temp();
        line(v64 + " = sext i32 " + v.val + " to i64");
      }
      std::string t = new_temp();
      line(t + " = inttoptr i64 " + v64 + " to ptr");
      push({t, VT::PTR});
      return true;
    }
    return fail_inst("未知指针内建: " + std::string(name));
  }

  // proc 原子操作内建：直接发射 LLVM 原子指令（load/store/atomicrmw/cmpxchg/fence），
  // 语义为 32 位字、4 字节对齐、seq_cst 顺序。addr 必须是 4 字节对齐的 ptr。
  bool emit_atomic_call(std::string_view name) {
    auto pop_ptr = [&](std::string& p) -> bool {
      Value v;
      if (!pop(v))
        return false;
      if (v.vt == VT::PTR) {
        p = v.val;
        return true;
      }
      p = new_temp();
      line(p + " = inttoptr i32 " + v.val + " to ptr");
      return true;
    };
    auto pop_i32 = [&](std::string& x) -> bool {
      Value v;
      if (!pop(v))
        return false;
      if (v.vt != VT::PTR) {
        x = v.val;
        return true;
      }
      x = new_temp();
      line(x + " = ptrtoint ptr " + v.val + " to i32");
      return true;
    };

    if (name == "_vx_proc_atomic_load") {
      std::string p;
      if (!pop_ptr(p))
        return false;
      std::string t = new_temp();
      line(t + " = load atomic i32, ptr " + p + " seq_cst, align 4");
      push({t, VT::I32});
      return true;
    }
    if (name == "_vx_proc_atomic_store") {
      std::string v;
      std::string p;
      if (!pop_i32(v) || !pop_ptr(p))
        return false;
      line("store atomic i32 " + v + ", ptr " + p + " seq_cst, align 4");
      push({"0", VT::I32}); // void 调用占位，供随后的 POP 消费
      return true;
    }
    if (name == "_vx_proc_atomic_fetch_add" || name == "_vx_proc_atomic_fetch_sub") {
      std::string d;
      std::string p;
      if (!pop_i32(d) || !pop_ptr(p))
        return false;
      std::string t = new_temp();
      const std::string op = name == "_vx_proc_atomic_fetch_add" ? "add" : "sub";
      line(t + " = atomicrmw " + op + " ptr " + p + ", i32 " + d + " seq_cst");
      push({t, VT::I32});
      return true;
    }
    if (name == "_vx_proc_atomic_cas") {
      std::string des;
      std::string exp;
      std::string p;
      if (!pop_i32(des) || !pop_i32(exp) || !pop_ptr(p))
        return false;
      const std::string pair = new_temp();
      line(pair + " = cmpxchg ptr " + p + ", i32 " + exp + ", i32 " + des + " seq_cst seq_cst");
      std::string t = new_temp();
      line(t + " = extractvalue { i32, i1 } " + pair + ", 0");
      push({t, VT::I32});
      return true;
    }
    if (name == "_vx_proc_atomic_fence") {
      line("fence seq_cst");
      push({"0", VT::I32});
      return true;
    }
    return fail_inst("未知原子操作内建: " + std::string(name));
  }

  bool emit_function(const ir::FuncIR& f) {
    cur_name_ = std::string(f.name);
    const std::size_t slots = std::max<std::size_t>(f.local_count, f.params.size());
    slot_vt_.assign(slots, -1);
    slot_alloca_.assign(slots, "");
    stack_.clear();
    temp_ = 0;
    bb_ = 0;
    terminated_ = false;
    body_.clear();
    allocas_.clear();

    ret_void_ = is_void(f.ret_type);
    ret_vt_ = ret_void_ ? VT::I32 : vt_of(f.ret_type);

    const std::string sym = symbol_of(f.name);
    std::string sig = ret_void_ ? "void" : std::string(vt_name(ret_vt_));
    sig += " @" + sym + "(";
    for (std::size_t i = 0; i < f.params.size(); ++i) {
      if (i != 0U)
        sig += ", ";
      VT pv = vt_of(f.params[i].type);
      sig += std::string(vt_name(pv)) + " %a" + std::to_string(i);
    }
    sig += ")";

    // 参数写回槽位（参数占最前槽位）
    for (std::size_t i = 0; i < f.params.size(); ++i) {
      VT pv = vt_of(f.params[i].type);
      bool ok = false;
      std::string al = ensure_slot(static_cast<std::uint32_t>(i), pv, ok);
      if (!ok)
        return fail("参数槽位类型冲突: " + std::string(f.name));
      line("store " + std::string(vt_name(pv)) + " %a" + std::to_string(i) + ", ptr " + al);
    }

    for (std::size_t i = 0; i < f.instructions.size(); ++i) {
      if (!emit_inst(f.instructions[i], i))
        return false;
    }
    if (!terminated_) {
      line(ret_void_ ? "ret void" : "ret " + std::string(vt_name(ret_vt_)) + " 0");
      terminated_ = true;
    }

    func_block_ = "define " + sig + " {\n" + "entry:\n" + allocas_ + body_ + "}\n\n";
    return true;
  }

  bool emit_main_wrapper() {
    auto it = fn_by_name_.find("main");
    if (it == fn_by_name_.end()) {
      func_block_.clear();
      return true; // 无 main：不生成入口（库/中间产物场景）
    }
    const ir::FuncIR& m = *it->second;
    if (!m.params.empty())
      return fail("main 函数带参数，后端暂不支持生成入口包装");
    std::string w = "define i32 @main() {\nentry:\n";
    if (is_void(m.ret_type)) {
      w += "  call void @vx_main()\n  ret i32 0\n";
    } else if (vt_of(m.ret_type) == VT::I32) {
      w += "  %r = call i32 @vx_main()\n  ret i32 %r\n";
    } else {
      w += "  %r = call " + std::string(vt_name(vt_of(m.ret_type))) + " @vx_main()\n  ret i32 0\n";
    }
    w += "}\n\n";
    func_block_ = w;
    return true;
  }

  // 状态
  const ir::ModuleIR& mod_;
  std::string_view triple_;
  bool emit_entry_ = true;
  std::map<std::string_view, const ir::FuncIR*> fn_by_name_;
  std::map<std::string, const ir::LinkEntry*> extern_by_sym_;
  std::map<std::string, std::size_t> field_index_;
  std::map<std::string, VT> field_type_;
  std::set<std::string> used_externs_;
  std::set<std::string> used_builtins_;
  std::vector<std::string> globals_;

  // 函数值 / 线程 / 任务池 / 管道
  std::set<std::string> func_refs_; // 被 &fn 引用的函数名（生成通用适配器）
  bool wants_threads_ = false;
  bool wants_pool_ = false;
  bool wants_pipe_ = false;
  std::string task_decls_;
  std::string task_globals_;
  std::string task_defs_;
  std::string error_;

  // 每函数状态
  std::string cur_name_;
  std::size_t cur_index_ = 0;
  bool ret_void_ = true;
  VT ret_vt_ = VT::I32;
  std::vector<std::int8_t> slot_vt_;
  std::vector<std::string> slot_alloca_;
  std::vector<Value> stack_;
  int temp_ = 0;
  int bb_ = 0;
  bool terminated_ = false;
  std::string body_;
  std::string allocas_;
  std::string func_block_;
};

} // namespace

EmitResult emit_llvm_ir(const ir::ModuleIR& mod, std::string_view target_triple, bool emit_entry) {
  return Emitter(mod, target_triple, emit_entry).run();
}

} // namespace vx::backend
