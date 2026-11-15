// emitter_decl.hpp — Emitter 类声明（内部实现头，仅供 llvm_emitter.cpp 使用）
#pragma once

#include "emitter_text.hpp"
#include "emitter_value.hpp"
#include "vx/backend/llvm_emitter.hpp"

#include <charconv>
#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace vx::backend {

namespace {

namespace ir = vx::compiler::ir;

class Emitter {
public:
  Emitter(const ir::ModuleIR& mod, std::string_view triple, bool emit_entry)
      : mod_(mod), triple_(triple), emit_entry_(emit_entry) {}

  EmitResult run();

private:
  bool fail(std::string msg) {
    if (error_.empty())
      error_ = std::move(msg);
    return false;
  }

  // ---- 函数值适配器 / 线程 / 任务池 / 管道：IR 生成 ----
  static std::string sanitize_sym(std::string_view s);

  // 通用适配器：i64(i64,i64,i64,i64) → 目标函数原生签名。
  // 函数值（&fn）取的是适配器地址，因此可跨文件、可存入变量、可传给任意接收 ptr 的 API。
  // 通用适配器：i64(i64,i64,i64,i64) → 目标函数原生签名。
  // 函数值（&fn）取的是适配器地址，因此可跨文件、可存入变量、可传给任意接收 ptr 的 API。
  std::string build_adapters();

  static std::string build_task_decls(bool task_helpers, bool pipe_helpers);

  static std::string build_thread_globals();

  std::string build_thread_helpers() const;

  static std::string build_pool_globals();

  std::string build_pool_helpers() const;

  static std::string build_pipe_globals();

  static std::string build_pipe_helpers();

  bool fail_inst(std::string msg) {
    return fail("后端暂不支持 " + std::move(msg) + " (函数 " + cur_name_ + ", 指令 #" + std::to_string(cur_index_) +
                ")");
  }

  static std::string symbol_of(std::string_view name) {
    return name == "main" ? std::string("vx_main") : std::string(name);
  }

  std::string new_temp() { return "%t" + std::to_string(temp_++); }
  std::string new_bb() { return "bb" + std::to_string(bb_++); }

  void line(std::string s) {
    if (debug_ && dbg_scope_ != 0) {
      s += ", !dbg !" + std::to_string(dbg_location(dbg_scope_, cur_dbg_line_));
    }
    body_ += "  " + s + "\n";
  }

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
  std::string ensure_slot(std::uint32_t k, VT vt, bool& ok);

  static bool parse_slot(std::string_view s, std::uint32_t& out) {
    auto [p, ec] = std::from_chars(s.data(), s.data() + s.size(), out);
    return ec == std::errc() && p == s.data() + s.size();
  }

  // I32 族指令要求整型操作数；I64（var 通用槽）截断后参与运算，
  // 浮点值混入时干净报错（而非产出非法 IR）。
  bool need_i32(Value& v) {
    if (v.vt == VT::I32)
      return true;
    if (v.vt == VT::I64)
      return coerce(v, VT::I32, "I32 指令操作数");
    return fail("I32 指令操作数类型不符（期望 i32，实际 " + std::string(vt_name(v.vt)) + "，函数 " + cur_name_ +
                ", 指令 #" + std::to_string(cur_index_) + "）");
  }

  // 通用值类型转换：I64 是 64 位通用槽（var）。
  // 允许的转换保持位值不丢失：i32<->i64（sext/trunc）、ptr<->i64（ptrtoint/inttoptr）、
  // f64<->i64（bitcast）、i32<->ptr、i32<->f64。
  bool coerce(Value& v, VT to, std::string_view what) {
    if (v.vt == to)
      return true;
    const std::string t = new_temp();
    switch (to) {
      case VT::I64:
        if (v.vt == VT::I32)
          line(t + " = sext i32 " + v.val + " to i64");
        else if (v.vt == VT::PTR)
          line(t + " = ptrtoint ptr " + v.val + " to i64");
        else if (v.vt == VT::F64)
          line(t + " = bitcast double " + v.val + " to i64");
        else
          return fail(std::string(what) + "：无法转换为 64 位值");
        break;
      case VT::I32:
        if (v.vt == VT::I64)
          line(t + " = trunc i64 " + v.val + " to i32");
        else if (v.vt == VT::PTR)
          line(t + " = ptrtoint ptr " + v.val + " to i32");
        else
          return fail(std::string(what) + "：无法转换为 i32");
        break;
      case VT::F64:
        if (v.vt == VT::I64)
          line(t + " = bitcast i64 " + v.val + " to double");
        else
          return fail(std::string(what) + "：无法转换为 double");
        break;
      case VT::PTR:
        if (v.vt == VT::I64)
          line(t + " = inttoptr i64 " + v.val + " to ptr");
        else if (v.vt == VT::I32)
          line(t + " = inttoptr i32 " + v.val + " to ptr");
        else
          return fail(std::string(what) + "：无法转换为指针");
        break;
    }
    v = {t, to};
    return true;
  }

  // ---- 单条指令发射 ----
  bool emit_inst(const ir::Inst& inst, std::size_t index);

  // ---- 调试信息（DWARF LineTablesOnly）----
  static std::string md_escape(std::string_view s);
  int new_md_id() { return md_next_++; }
  int dbg_location(int scope, int line);
  int new_disubprogram(const ir::FuncIR& f);
  void emit_debug_header();

  // 解析调用目标并发射 call
  bool emit_call(const ir::Inst& inst);

  // proc 线程 / 任务池调用：后端生成 IR 实现，不经通用 extern 路径
  bool emit_task_call(TaskCall tc);

  // ptr 库内建：add（字节偏移）/ to_int / from_int，直接发射指针 IR
  bool emit_ptr_call(std::string_view name);

  // proc 原子操作内建：直接发射 LLVM 原子指令（load/store/atomicrmw/cmpxchg/fence），
  // 语义为 32 位字、4 字节对齐、seq_cst 顺序。addr 必须是 4 字节对齐的 ptr。
  // proc 原子操作内建：直接发射 LLVM 原子指令（load/store/atomicrmw/cmpxchg/fence），
  // 语义为 32 位字、4 字节对齐、seq_cst 顺序。addr 必须是 4 字节对齐的 ptr。
  bool emit_atomic_call(std::string_view name);

  bool emit_function(const ir::FuncIR& f);

  bool emit_main_wrapper();

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
  // ---- 调试信息状态 ----
  bool debug_ = false;
  int md_next_ = 0;             // 下一个元数据编号
  int dbg_scope_ = 0;           // 当前函数的 DISubprogram 编号（0 = 无）
  int cur_dbg_line_ = 1;        // 当前指令源码行
  std::map<int, int> dbg_locs_; // (scope<<20|line) → DILocation 编号
  std::string metadata_;        // 函数体之后追加的元数据文本
  std::vector<Value> stack_;
  int temp_ = 0;
  int bb_ = 0;
  bool terminated_ = false;
  std::string body_;
  std::string allocas_;
  std::string func_block_;
};

} // namespace

} // namespace vx::backend
