// emitter_func.hpp — 函数与入口发射（内部实现头，仅供 llvm_emitter.cpp 使用）
#pragma once

#include "emitter_decl.hpp"

namespace vx::backend {

namespace {

// ---- 调试信息（DWARF LineTablesOnly）----
inline std::string Emitter::md_escape(std::string_view s) {
  std::string out;
  out.reserve(s.size());
  for (const char c : s) {
    if (c == '\\' || c == '"')
      out += '\\';
    out += c;
  }
  return out;
}

inline int Emitter::dbg_location(int scope, int line) {
  const int key = (scope << 20) | (line & 0xFFFFF);
  const auto it = dbg_locs_.find(key);
  if (it != dbg_locs_.end())
    return it->second;
  const int id = new_md_id();
  metadata_ += "!" + std::to_string(id) + " = !DILocation(line: " + std::to_string(line) + ", column: 1, scope: !" +
               std::to_string(scope) + ")\n";
  dbg_locs_[key] = id;
  return id;
}

inline void Emitter::emit_debug_header() {
  const std::string file = md_escape(mod_.source_file.empty() ? "<vx>" : mod_.source_file);
  metadata_ += "!llvm.dbg.cu = !{!0}\n";
  metadata_ += "!llvm.module.flags = !{!2, !3}\n";
  metadata_ += "!0 = distinct !DICompileUnit(language: DW_LANG_C99, file: !1, producer: \"vxc\", isOptimized: "
               "false, runtimeVersion: 0, emissionKind: LineTablesOnly)\n";
  metadata_ += "!1 = !DIFile(filename: \"" + file + "\", directory: \"\")\n";
  metadata_ += "!2 = !{i32 2, !\"Dwarf Version\", i32 5}\n";
  metadata_ += "!3 = !{i32 2, !\"Debug Info Version\", i32 3}\n";
  md_next_ = 4;
}

inline int Emitter::new_disubprogram(const ir::FuncIR& f) {
  const int sp = new_md_id();
  const int st = new_md_id();
  const int list = new_md_id();
  metadata_ += "!" + std::to_string(sp) + " = distinct !DISubprogram(name: \"" + md_escape(f.name) +
               "\", scope: !1, file: !1, line: 1, type: !" + std::to_string(st) +
               ", scopeLine: 1, spFlags: DISPFlagDefinition, unit: !0)\n";
  metadata_ += "!" + std::to_string(st) + " = !DISubroutineType(types: !" + std::to_string(list) + ")\n";
  metadata_ += "!" + std::to_string(list) + " = !{null}\n";
  return sp;
}

inline EmitResult Emitter::run() {
  debug_ = mod_.debug;
  metadata_.clear();
  md_next_ = 0;
  if (debug_)
    emit_debug_header();
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
    dbg_scope_ = 0;
    dbg_locs_.clear();
    if (debug_)
      dbg_scope_ = new_disubprogram(f);
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
  if (!metadata_.empty()) {
    out += "\n";
    out += metadata_;
  }

  EmitResult r;
  r.ok = true;
  r.ir = std::move(out);
  return r;
}

inline bool Emitter::emit_function(const ir::FuncIR& f) {
  cur_name_ = std::string(f.name);
  std::size_t slots = std::max<std::size_t>(f.local_count, f.params.size());
  slots = std::max<std::size_t>(slots, f.local_types.size());
  slot_vt_.assign(slots, -1);
  slot_alloca_.assign(slots, "");
  // 静态局部类型：参数槽由签名决定，其余槽位按 local_types 预分配，
  // 避免「var 槽首次写入推断」导致的后续类型冲突/指针截断。
  for (std::size_t k = f.params.size(); k < f.local_types.size(); ++k) {
    const std::uint8_t tag = f.local_types[k];
    if (tag == 0xFF || tag == ir::TYPE_UNKNOWN || tag == ir::TYPE_VOID)
      continue;
    slot_vt_[k] = static_cast<std::int8_t>(vt_of_tag(tag));
  }
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
    cur_dbg_line_ = (i < f.lines.size() && f.lines[i] > 0) ? f.lines[i] : 1;
    if (!emit_inst(f.instructions[i], i))
      return false;
  }
  if (!terminated_) {
    line(ret_void_ ? "ret void" : "ret " + std::string(vt_name(ret_vt_)) + " 0");
    terminated_ = true;
  }

  const std::string dbg_attr = (debug_ && dbg_scope_ != 0) ? " !dbg !" + std::to_string(dbg_scope_) : "";
  func_block_ = "define " + sig + dbg_attr + " {\n" + "entry:\n" + allocas_ + body_ + "}\n\n";
  return true;
}

inline bool Emitter::emit_main_wrapper() {
  dbg_scope_ = 0; // 入口包装不携带调试位置
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

} // namespace

} // namespace vx::backend
