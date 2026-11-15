// emitter_scaffold.hpp — 运行时脚手架 IR 生成（适配器 / 线程 / 任务池 / 管道）（内部实现头，仅供 llvm_emitter.cpp
// 使用）
#pragma once

#include "emitter_decl.hpp"

namespace vx::backend {

namespace {

// ---- 函数值适配器 / 线程 / 任务池 / 管道：IR 生成 ----
inline std::string Emitter::sanitize_sym(std::string_view s) {
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
inline std::string Emitter::build_adapters() {
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

inline std::string Emitter::build_task_decls(bool task_helpers, bool pipe_helpers) {
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

inline std::string Emitter::build_thread_globals() {
  return "@vx_thread_count = global i32 0\n"
         "@vx_thread_slots = global [1024 x ptr] zeroinitializer\n"
         "@vx_thread_results = global [1024 x i64] zeroinitializer\n\n";
}

inline std::string Emitter::build_thread_helpers() const {
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

inline std::string Emitter::build_pool_globals() {
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

inline std::string Emitter::build_pool_helpers() const {
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

inline std::string Emitter::build_pipe_globals() {
  return "@vx_mode_r = private unnamed_addr constant [2 x i8] c\"r\\00\"\n\n";
}

inline std::string Emitter::build_pipe_helpers() {
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

} // namespace

} // namespace vx::backend
