// emitter_call.hpp — 调用发射（内置 / 任务 / 函数值 / 原子）（内部实现头，仅供 llvm_emitter.cpp 使用）
#pragma once

#include "emitter_decl.hpp"

namespace vx::backend {

namespace {

// 解析调用目标并发射 call
inline bool Emitter::emit_call(const ir::Inst& inst) {
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
    // 实参值类型与形参声明不一致时按位型互转：FFI 边界放行任意实参类型
    // （如 waitpid(pid, 0, ...) 用整型 0 表示 NULL 的 ptr 形参），不转则 IR 类型不匹配。
    if (!coerce(args[i], arg_vts[i], "CALL 实参"))
      return false;
    call += std::string(vt_name(arg_vts[i])) + " " + args[i].val;
  }
  call += ")";
  line(call);
  // 栈机约定：每个表达式恰占一个栈槽。void 调用不留结果，但上游 codegen
  // 对 expr_stmt 恒发 OP_POP，故此处压入占位值，
  // 使随后的 POP 有槽可弹（占位值不参与任何运算）。
  if (has_ret) {
    push({temp, ret});
  } else {
    push({"0", VT::I32});
  }
  return true;
}

// proc 线程 / 任务池调用：后端生成 IR 实现，不经通用 extern 路径
inline bool Emitter::emit_task_call(TaskCall tc) {
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
inline bool Emitter::emit_ptr_call(std::string_view name) {
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
inline bool Emitter::emit_atomic_call(std::string_view name) {
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

} // namespace

} // namespace vx::backend
