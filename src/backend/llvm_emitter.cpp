// llvm_emitter.cpp — TypeIR（虚拟栈机）→ LLVM IR 文本（SSA 直接发射）。
// 设计：发射期维护「编译期操作数栈」（Value{val, vt}），槽位类型由写入者推断；
// 条件分支自动开 fallthrough 基本块；标签处若非终止块则补 br，保证 IR 合法。
#include "vx/backend/llvm_emitter.hpp"

#include <charconv>
#include <cstdint>
#include <cstdio>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace vx::backend {
namespace {

namespace ir = vx::compiler::ir;

// ---- 值类型 ----
enum class VT : std::int8_t { I32 = 0, I64 = 1, F64 = 2, PTR = 3 };

const char *vt_name(VT t) {
  switch (t) {
    case VT::I32: return "i32";
    case VT::I64: return "i64";
    case VT::F64: return "double";
    case VT::PTR: return "ptr";
  }
  return "i32";
}

VT vt_of(std::string_view ty) {
  if (ty == "float") return VT::F64;
  if (ty == "string") return VT::PTR;
  if (ty == "i64") return VT::I64;
  return VT::I32; // int/bool/var/空
}

bool is_void(std::string_view ty) { return ty.empty() || ty == "void"; }

// 裸值 + 类型（val 为 LLVM 操作数文本，不含类型；常量形如 "5"/"null"，寄存器形如 "%t3"）
struct Value {
  std::string val;
  VT vt = VT::I32;
};

// ---- 内置运行时符号表 ----
struct Builtin {
  const char *sym;
  std::vector<VT> args;
  bool has_ret;
  VT ret;
};

// 键为 VX 源码中可见的调用名，sym 为链接期实际符号（可与键不同）。
static const std::map<std::string_view, Builtin> kBuiltinTable = {
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
    // 系统能力：命令行参数 / 文件 I/O / 执行 shell 命令（实现见 vxrt.c）
    {"sys_argv_count", {"vx_argv_count", {}, true, VT::I32}},
    {"sys_argv_get", {"vx_argv_get", {VT::I32}, true, VT::PTR}},
    {"file_read", {"vx_file_read", {VT::PTR}, true, VT::PTR}},
    {"file_write", {"vx_file_write", {VT::PTR, VT::PTR}, true, VT::I32}},
    {"file_exists", {"vx_file_exists", {VT::PTR}, true, VT::I32}},
    {"system", {"vx_system", {VT::PTR}, true, VT::I32}},
};

// 按 VX 源码中的调用名查找
const Builtin *lookup_builtin(std::string_view name) {
  auto it = kBuiltinTable.find(name);
  return it == kBuiltinTable.end() ? nullptr : &it->second;
}

// 按链接期实际符号查找（发射 declare 时使用）
const Builtin *lookup_builtin_by_sym(std::string_view sym) {
  for (const auto &[name, b] : kBuiltinTable) {
    if (sym == b.sym) return &b;
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

const char *op_name(std::uint8_t op) {
  switch (op) {
    case ir::OP_CONST_INT: return "OP_CONST_INT";
    case ir::OP_CONST_FLOAT: return "OP_CONST_FLOAT";
    case ir::OP_CONST_BOOL: return "OP_CONST_BOOL";
    case ir::OP_CONST_STRING: return "OP_CONST_STRING";
    case ir::OP_CONST_NIL: return "OP_CONST_NIL";
    case ir::OP_LOAD_VAR: return "OP_LOAD_VAR";
    case ir::OP_STORE_VAR: return "OP_STORE_VAR";
    case ir::OP_I32_ADD: return "OP_I32_ADD";
    case ir::OP_I32_SUB: return "OP_I32_SUB";
    case ir::OP_I32_MUL: return "OP_I32_MUL";
    case ir::OP_I32_DIV: return "OP_I32_DIV";
    case ir::OP_I32_MOD: return "OP_I32_MOD";
    case ir::OP_F64_ADD: return "OP_F64_ADD";
    case ir::OP_F64_SUB: return "OP_F64_SUB";
    case ir::OP_F64_MUL: return "OP_F64_MUL";
    case ir::OP_F64_DIV: return "OP_F64_DIV";
    case ir::OP_I64_ADD: return "OP_I64_ADD";
    case ir::OP_I32_EQ: return "OP_I32_EQ";
    case ir::OP_I32_NEQ: return "OP_I32_NEQ";
    case ir::OP_I32_LT: return "OP_I32_LT";
    case ir::OP_I32_GT: return "OP_I32_GT";
    case ir::OP_I32_LE: return "OP_I32_LE";
    case ir::OP_I32_GE: return "OP_I32_GE";
    case ir::OP_F64_EQ: return "OP_F64_EQ";
    case ir::OP_F64_NE: return "OP_F64_NE";
    case ir::OP_F64_LT: return "OP_F64_LT";
    case ir::OP_F64_GT: return "OP_F64_GT";
    case ir::OP_F64_LE: return "OP_F64_LE";
    case ir::OP_F64_GE: return "OP_F64_GE";
    case ir::OP_I32_NEG: return "OP_I32_NEG";
    case ir::OP_F64_NEG: return "OP_F64_NEG";
    case ir::OP_BOOL_NOT: return "OP_BOOL_NOT";
    case ir::OP_I32_AND: return "OP_I32_AND";
    case ir::OP_I32_OR: return "OP_I32_OR";
    case ir::OP_BOOL_AND: return "OP_BOOL_AND";
    case ir::OP_BOOL_OR: return "OP_BOOL_OR";
    case ir::OP_JUMP: return "OP_JUMP";
    case ir::OP_JUMP_IF_FALSE: return "OP_JUMP_IF_FALSE";
    case ir::OP_JUMP_IF_TRUE: return "OP_JUMP_IF_TRUE";
    case ir::OP_STR_EQ: return "OP_STR_EQ";
    case ir::OP_NEQ_INT: return "OP_NEQ_INT";
    case ir::OP_CALL: return "OP_CALL";
    case ir::OP_RETURN: return "OP_RETURN";
    case ir::OP_RETURN_VOID: return "OP_RETURN_VOID";
    case ir::OP_STRUCT_NEW: return "OP_STRUCT_NEW";
    case ir::OP_FIELD_LOAD: return "OP_FIELD_LOAD";
    case ir::OP_FIELD_STORE: return "OP_FIELD_STORE";
    case ir::OP_ARRAY_NEW: return "OP_ARRAY_NEW";
    case ir::OP_INDEX_GET: return "OP_INDEX_GET";
    case ir::OP_INDEX_SET: return "OP_INDEX_SET";
    case ir::OP_MAP_NEW: return "OP_MAP_NEW";
    case ir::OP_DUP: return "OP_DUP";
    case ir::OP_POP: return "OP_POP";
    case ir::OP_STR_CONCAT: return "OP_STR_CONCAT";
    case ir::OP_HALT: return "OP_HALT";
    case ir::OP_NOOP: return "OP_NOOP";
    default: return "unknown";
  }
}

class Emitter {
public:
  Emitter(const ir::ModuleIR &mod, std::string_view triple, bool emit_entry)
      : mod_(mod), triple_(triple), emit_entry_(emit_entry) {}

  EmitResult run() {
    for (const ir::FuncIR &f : mod_.funcs) {
      fn_by_name_[f.name] = &f;
    }
    // 外部符号表：key 为链接符号名（VX ABI 名优先，回退到声明名）
    for (const ir::LinkEntry &e : mod_.linkage) {
      const std::string key = e.abi_symbol.empty() ? e.name : e.abi_symbol;
      extern_by_sym_[key] = &e;
    }
    // P4：类/结构字段布局（字段名 -> 索引/类型；首个定义优先）
    for (const ir::StructLayout &sl : mod_.structs) {
      for (std::size_t i = 0; i < sl.fields.size(); ++i) {
        if (field_index_.find(sl.fields[i].name) == field_index_.end()) {
          field_index_[sl.fields[i].name] = i;
          field_type_[sl.fields[i].name] = vt_of(sl.fields[i].type);
        }
      }
    }
    std::string funcs_txt;
    for (const ir::FuncIR &f : mod_.funcs) {
      if (!emit_function(f)) return {false, error_, {}};
      funcs_txt += func_block_;
    }
    // main 入口包装（动态库不需要进程入口）
    if (emit_entry_) {
      if (!emit_main_wrapper()) return {false, error_, {}};
      funcs_txt += func_block_;
    }

    // 组装输出
    std::string out;
    out += "; VXOBJ -> LLVM IR by vlnk\n";
    out += "target triple = \"" + std::string(triple_) + "\"\n\n";
    for (const std::string &sym : used_builtins_) {
      const Builtin *b = lookup_builtin_by_sym(sym);
      if (!b) continue;
      out += "declare ";
      out += b->has_ret ? vt_name(b->ret) : "void";
      out += " @" + std::string(b->sym) + "(";
      for (std::size_t i = 0; i < b->args.size(); ++i) {
        if (i) out += ", ";
        out += vt_name(b->args[i]);
      }
      out += ")\n";
    }
    if (!used_builtins_.empty()) out += "\n";
    // 来自 linkage 的外部符号声明（@imp / @cppimp / 普通 extern）
    for (const std::string &sym : used_externs_) {
      auto it = extern_by_sym_.find(sym);
      if (it == extern_by_sym_.end()) continue;
      const ir::LinkEntry *e = it->second;
      const bool has_ret = !is_void(e->ret_type);
      out += "declare ";
      out += has_ret ? vt_name(vt_of(e->ret_type)) : "void";
      out += " @" + sym + "(";
      for (std::size_t i = 0; i < e->params.size(); ++i) {
        if (i) out += ", ";
        out += vt_name(vt_of(e->params[i].type));
      }
      out += ")\n";
    }
    if (!used_externs_.empty()) out += "\n";
    for (const std::string &g : globals_) out += g + "\n";
    if (!globals_.empty()) out += "\n";
    out += funcs_txt;

    EmitResult r;
    r.ok = true;
    r.ir = std::move(out);
    return r;
  }

private:
  bool fail(std::string msg) {
    if (error_.empty()) error_ = std::move(msg);
    return false;
  }

  bool fail_inst(std::string msg) {
    return fail("后端暂不支持 " + std::move(msg) + " (函数 " + cur_name_ +
                ", 指令 #" + std::to_string(cur_index_) + ")");
  }

  std::string symbol_of(std::string_view name) {
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

  bool pop(Value &v) {
    if (stack_.empty()) return fail("操作数栈下溢（内部错误）");
    v = stack_.back();
    stack_.pop_back();
    return true;
  }

  void push(Value v) { stack_.push_back(std::move(v)); }

  // 确保槽位 k 存在且类型为 vt；类型冲突返回空串并置 ok=false
  std::string ensure_slot(std::uint32_t k, VT vt, bool &ok) {
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

  static bool parse_slot(std::string_view s, std::uint32_t &out) {
    auto [p, ec] = std::from_chars(s.data(), s.data() + s.size(), out);
    return ec == std::errc() && p == s.data() + s.size();
  }

  // I32 族指令要求整型操作数；浮点值混入时干净报错（而非产出非法 IR）。
  bool need_i32(const Value &v) {
    if (v.vt == VT::I32) return true;
    return fail("I32 指令操作数类型不符（期望 i32，实际 " +
                std::string(vt_name(v.vt)) + "，函数 " + cur_name_ +
                ", 指令 #" + std::to_string(cur_index_) + "）");
  }

  // ---- 单条指令发射 ----
  bool emit_inst(const ir::Inst &inst, std::size_t index) {
    cur_index_ = index;

    // 标签（OP_NOOP 携带标签名）
    if (inst.op == ir::OP_NOOP) {
      if (inst.args.empty()) return true;
      if (!terminated_) body_ += "  br label %" + inst.args + "\n";
      body_ += inst.args + ":\n";
      terminated_ = false;
      return true;
    }

    ensure_open();

    switch (inst.op) {
      case ir::OP_CONST_INT: {
        std::string digits =
            inst.args.size() > 1 ? inst.args.substr(1) : "0";
        if (digits.empty()) digits = "0";
        push({digits, VT::I32});
        return true;
      }
      case ir::OP_CONST_FLOAT: {
        std::string digits =
            inst.args.size() > 1 ? inst.args.substr(1) : "0";
        if (digits.empty()) digits = "0";
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
        globals_.push_back("@" + gname +
                           " = private unnamed_addr constant [" +
                           std::to_string(bytes.size() + 1) + " x i8] c\"" +
                           escape_bytes(bytes) + "\"");
        push({"@" + gname, VT::PTR});
        return true;
      }
      case ir::OP_LOAD_VAR: {
        std::uint32_t k = 0;
        if (!parse_slot(inst.args, k)) return fail_inst("LOAD_VAR 槽位非法");
        VT vt = (k < slot_vt_.size() && slot_vt_[k] >= 0)
                    ? static_cast<VT>(slot_vt_[k])
                    : VT::I32;
        bool ok = false;
        std::string al = ensure_slot(k, vt, ok);
        if (!ok) return fail_inst("LOAD_VAR 槽位类型冲突");
        std::string t = new_temp();
        line(t + " = load " + vt_name(vt) + ", ptr " + al);
        push({t, vt});
        return true;
      }
      case ir::OP_STORE_VAR: {
        Value v;
        if (!pop(v)) return false;
        std::uint32_t k = 0;
        if (!parse_slot(inst.args, k)) return fail_inst("STORE_VAR 槽位非法");
        bool ok = false;
        std::string al = ensure_slot(k, v.vt, ok);
        if (!ok) return fail_inst("STORE_VAR 槽位类型冲突");
        line("store " + std::string(vt_name(v.vt)) + " " + v.val + ", ptr " +
             al);
        return true;
      }
      case ir::OP_I32_ADD:
      case ir::OP_I32_SUB:
      case ir::OP_I32_MUL:
      case ir::OP_I32_DIV:
      case ir::OP_I32_MOD: {
        Value b, a;
        if (!pop(b) || !pop(a)) return false;
        if (!need_i32(a) || !need_i32(b)) return false;
        const char *op = "add";
        switch (inst.op) {
          case ir::OP_I32_SUB: op = "sub"; break;
          case ir::OP_I32_MUL: op = "mul"; break;
          case ir::OP_I32_DIV: op = "sdiv"; break;
          case ir::OP_I32_MOD: op = "srem"; break;
          default: break;
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
        Value b, a;
        if (!pop(b) || !pop(a)) return false;
        if (!need_i32(a) || !need_i32(b)) return false;
        const char *pred = "eq";
        switch (inst.op) {
          case ir::OP_I32_NEQ: pred = "ne"; break;
          case ir::OP_I32_LT: pred = "slt"; break;
          case ir::OP_I32_GT: pred = "sgt"; break;
          case ir::OP_I32_LE: pred = "sle"; break;
          case ir::OP_I32_GE: pred = "sge"; break;
          default: break;
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
        if (!pop(a)) return false;
        if (!need_i32(a)) return false;
        std::string t = new_temp();
        line(t + " = sub i32 0, " + a.val);
        push({t, VT::I32});
        return true;
      }
      case ir::OP_F64_ADD:
      case ir::OP_F64_SUB:
      case ir::OP_F64_MUL:
      case ir::OP_F64_DIV: {
        Value b, a;
        if (!pop(b) || !pop(a)) return false;
        if (a.vt != VT::F64 || b.vt != VT::F64)
          return fail_inst("F64 算术操作数非 double");
        const char *op = "fadd";
        switch (inst.op) {
          case ir::OP_F64_SUB: op = "fsub"; break;
          case ir::OP_F64_MUL: op = "fmul"; break;
          case ir::OP_F64_DIV: op = "fdiv"; break;
          default: break;
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
        Value b, a;
        if (!pop(b) || !pop(a)) return false;
        if (a.vt != VT::F64 || b.vt != VT::F64)
          return fail_inst("F64 比较操作数非 double");
        const char *pred = "oeq";
        switch (inst.op) {
          case ir::OP_F64_NE: pred = "one"; break;
          case ir::OP_F64_LT: pred = "olt"; break;
          case ir::OP_F64_GT: pred = "ogt"; break;
          case ir::OP_F64_LE: pred = "ole"; break;
          case ir::OP_F64_GE: pred = "oge"; break;
          default: break;
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
        if (!pop(a)) return false;
        if (a.vt != VT::F64) return fail_inst("F64_NEG 操作数非 double");
        std::string t = new_temp();
        line(t + " = fneg double " + a.val);
        push({t, VT::F64});
        return true;
      }
      case ir::OP_BOOL_NOT: {
        Value a;
        if (!pop(a)) return false;
        if (!need_i32(a)) return false;
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
        Value b, a;
        if (!pop(b) || !pop(a)) return false;
        if (!need_i32(a) || !need_i32(b)) return false;
        const bool is_and =
            inst.op == ir::OP_BOOL_AND || inst.op == ir::OP_I32_AND;
        std::string t = new_temp();
        line(t + " = " + (is_and ? "and" : "or") + " i32 " + a.val + ", " +
             b.val);
        push({t, VT::I32});
        return true;
      }
      case ir::OP_STR_CONCAT: {
        Value b, a;
        if (!pop(b) || !pop(a)) return false;
        used_builtins_.insert("vx_str_concat");
        std::string t = new_temp();
        line(t + " = call ptr @vx_str_concat(ptr " + a.val + ", ptr " + b.val +
             ")");
        push({t, VT::PTR});
        return true;
      }
      case ir::OP_STR_EQ: {
        Value b, a;
        if (!pop(b) || !pop(a)) return false;
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
        if (stack_.empty()) return fail_inst("DUP 栈为空");
        push(stack_.back());
        return true;
      }
      case ir::OP_POP: {
        Value v;
        if (!pop(v)) return false;
        return true;
      }
      // ---- P4: NEWZ / FREE ----
      case ir::OP_NEWZ: {
        std::size_t sep = inst.args.find(';');
        std::size_t bytes = 8;
        if (sep != std::string::npos) {
          try { bytes = std::stoull(inst.args.substr(sep + 1)); } catch (...) { bytes = 8; }
        }
        if (bytes == 0) bytes = 8;
        used_builtins_.insert("malloc");
        std::string t = new_temp();
        line(t + " = call ptr @malloc(i64 " + std::to_string(bytes) + ")");
        push({t, VT::PTR});
        return true;
      }
      case ir::OP_FREE: {
        Value p;
        if (!pop(p)) return false;
        used_builtins_.insert("free");
        line("call void @free(ptr " + p.val + ")");
        push({"0", VT::I32});
        return true;
      }
      // ---- P4: FIELD_LOAD / FIELD_STORE ----
      case ir::OP_FIELD_LOAD: {
        Value obj;
        if (!pop(obj)) return false;
        if (obj.vt != VT::PTR) return fail_inst("FIELD_LOAD 对象须为指针");
        auto it = field_index_.find(inst.args);
        if (it == field_index_.end()) return fail_inst("未知字段 " + inst.args);
        std::size_t idx = it->second;
        VT ft = field_type_.count(inst.args) ? field_type_[inst.args] : VT::I32;
        std::string gep = new_temp();
        line(gep + " = getelementptr i8, ptr " + obj.val + ", i64 " +
             std::to_string(idx * 8));
        std::string t = new_temp();
        line(t + " = load " + std::string(vt_name(ft)) + ", ptr " + gep);
        push({t, ft});
        return true;
      }
      case ir::OP_FIELD_STORE: {
        Value val, obj;
        if (!pop(val) || !pop(obj)) return false;
        if (obj.vt != VT::PTR) return fail_inst("FIELD_STORE 对象须为指针");
        auto it = field_index_.find(inst.args);
        if (it == field_index_.end()) return fail_inst("未知字段 " + inst.args);
        std::size_t idx = it->second;
        VT ft = field_type_.count(inst.args) ? field_type_[inst.args] : VT::I32;
        std::string v = val.val;
        VT vt = val.vt;
        // 类型转换到字段类型
        auto conv = [&](VT to) {
          if (vt == to) return;
          std::string t = new_temp();
          if (to == VT::I32 && vt == VT::I64) line(t + " = trunc i64 " + v + " to i32");
          else if (to == VT::I64 && vt == VT::I32) line(t + " = zext i32 " + v + " to i64");
          else if (to == VT::F64 && vt == VT::I32) line(t + " = sitofp i32 " + v + " to double");
          else if (to == VT::I32 && vt == VT::F64) line(t + " = fptosi double " + v + " to i32");
          else if (to == VT::PTR && vt == VT::I64) line(t + " = inttoptr i64 " + v + " to ptr");
          else if (to == VT::I64 && vt == VT::PTR) line(t + " = ptrtoint ptr " + v + " to i64");
          else { /* no-op conversion */ }
          v = t; vt = to;
        };
        conv(ft);
        std::string gep = new_temp();
        line(gep + " = getelementptr i8, ptr " + obj.val + ", i64 " +
             std::to_string(idx * 8));
        line("store " + std::string(vt_name(ft)) + " " + v + ", ptr " + gep);
        return true;
      }
      // ---- P4: ARRAY_NEW ----
      case ir::OP_ARRAY_NEW: {
        int n = 0;
        try { n = std::stoi(inst.args); } catch (...) { n = 0; }
        if (n < 0) n = 0;
        std::vector<Value> vals(n);
        for (int i = n - 1; i >= 0; --i) {
          if (!pop(vals[i])) return false;
        }
        used_builtins_.insert("vx_array_new");
        used_builtins_.insert("vx_array_set");
        std::string arr = new_temp();
        line(arr + " = call ptr @vx_array_new(i64 " + std::to_string(n) + ")");
        for (int i = 0; i < n; ++i) {
          std::string iv = vals[i].val;
          VT vt = vals[i].vt;
          if (vt == VT::I32) { std::string t = new_temp(); line(t + " = zext i32 " + iv + " to i64"); iv = t; }
          else if (vt == VT::F64) { std::string t = new_temp(); line(t + " = bitcast double " + iv + " to i64"); iv = t; }
          else if (vt == VT::PTR) { std::string t = new_temp(); line(t + " = ptrtoint ptr " + iv + " to i64"); iv = t; }
          line("call void @vx_array_set(ptr " + arr + ", i64 " +
               std::to_string(i) + ", i64 " + iv + ")");
        }
        push({arr, VT::PTR});
        return true;
      }
      // ---- P4: INDEX_GET ----
      case ir::OP_INDEX_GET: {
        Value idxv, objv;
        if (!pop(idxv) || !pop(objv)) return false;
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
          line(t + " = call i64 @vx_map_get(ptr " + objv.val + ", i64 " + keyi64 +
               ", i64 " + isstr + ", ptr " + keyptr + ")");
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
        Value val, idxv, objv;
        if (!pop(val) || !pop(idxv) || !pop(objv)) return false;
        std::string idx = idxv.val;
        if (idxv.vt == VT::I32) {
          std::string t = new_temp();
          line(t + " = sext i32 " + idx + " to i64");
          idx = t;
        }
        std::string iv = val.val;
        if (val.vt == VT::I32) { std::string t = new_temp(); line(t + " = zext i32 " + iv + " to i64"); iv = t; }
        else if (val.vt == VT::F64) { std::string t = new_temp(); line(t + " = bitcast double " + iv + " to i64"); iv = t; }
        else if (val.vt == VT::PTR) { std::string t = new_temp(); line(t + " = ptrtoint ptr " + iv + " to i64"); iv = t; }
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
          line("call void @vx_map_set(ptr " + objv.val + ", i64 " + keyi64 +
               ", i64 " + isstr + ", ptr " + keyptr + ", i64 " + iv + ")");
          return true;
        }
        used_builtins_.insert("vx_array_set");
        line("call void @vx_array_set(ptr " + objv.val + ", i64 " + idx +
             ", i64 " + iv + ")");
        return true;
      }
      // ---- P4: MAP_NEW ----
      case ir::OP_MAP_NEW: {
        int n = 0;
        try { n = std::stoi(inst.args); } catch (...) { n = 0; }
        if (n < 0) n = 0;
        std::vector<Value> keys(n), vals(n);
        for (int i = n - 1; i >= 0; --i) {
          if (!pop(vals[i])) return false;
          if (!pop(keys[i])) return false;
        }
        used_builtins_.insert("vx_map_new");
        used_builtins_.insert("vx_map_set");
        std::string m = new_temp();
        line(m + " = call ptr @vx_map_new(i64 " + std::to_string(n == 0 ? 1 : n) + ")");
        for (int i = 0; i < n; ++i) {
          std::string keyi64 = keys[i].val;
          std::string keyptr = "null";
          std::string isstr = "0";
          if (keys[i].vt == VT::I32) { std::string t = new_temp(); line(t + " = zext i32 " + keyi64 + " to i64"); keyi64 = t; }
          else if (keys[i].vt == VT::F64) { std::string t = new_temp(); line(t + " = bitcast double " + keyi64 + " to i64"); keyi64 = t; }
          else if (keys[i].vt == VT::PTR) { keyptr = keys[i].val; isstr = "1"; std::string t = new_temp(); line(t + " = ptrtoint ptr " + keys[i].val + " to i64"); keyi64 = t; }
          std::string iv = vals[i].val;
          if (vals[i].vt == VT::I32) { std::string t = new_temp(); line(t + " = zext i32 " + iv + " to i64"); iv = t; }
          else if (vals[i].vt == VT::F64) { std::string t = new_temp(); line(t + " = bitcast double " + iv + " to i64"); iv = t; }
          else if (vals[i].vt == VT::PTR) { std::string t = new_temp(); line(t + " = ptrtoint ptr " + iv + " to i64"); iv = t; }
          line("call void @vx_map_set(ptr " + m + ", i64 " + keyi64 +
               ", i64 " + isstr + ", ptr " + keyptr + ", i64 " + iv + ")");
        }
        push({m, VT::PTR});
        return true;
      }
      case ir::OP_CALL:
        return emit_call(inst);
      case ir::OP_JUMP: {
        if (inst.args.empty()) return fail_inst("JUMP 缺标签");
        line("br label %" + inst.args);
        terminated_ = true;
        return true;
      }
      case ir::OP_JUMP_IF_FALSE:
      case ir::OP_JUMP_IF_TRUE: {
        Value c;
        if (!pop(c)) return false;
        if (!need_i32(c)) return false;
        std::string t = new_temp();
        const char *pred = inst.op == ir::OP_JUMP_IF_FALSE ? "eq" : "ne";
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
        if (!pop(v)) return false;
        if (v.vt != ret_vt_) return fail_inst("RETURN 类型与函数返回类型不符");
        line("ret " + std::string(vt_name(ret_vt_)) + " " + v.val);
        terminated_ = true;
        return true;
      }
      case ir::OP_RETURN_VOID:
        line("ret void");
        terminated_ = true;
        return true;
      case ir::OP_HALT:
        line(ret_void_ ? "ret void"
                       : "ret " + std::string(vt_name(ret_vt_)) + " 0");
        terminated_ = true;
        return true;
      default:
        return fail_inst(std::string("opcode ") + op_name(inst.op));
    }
  }

  // 解析调用目标并发射 call
  bool emit_call(const ir::Inst &inst) {
    std::string_view body = inst.args;
    if (body.size() < 2 || body[1] != ';')
      return fail_inst("CALL 参数格式非法: " + std::string(body));
    const char kind = body[0];
    const std::string name(body.substr(2));

    std::string sym;
    std::vector<VT> arg_vts;
    bool has_ret = false;
    VT ret = VT::I32;

    if (kind == 'f') {
      auto it = fn_by_name_.find(name);
      if (it != fn_by_name_.end()) {
        const ir::FuncIR &callee = *it->second;
        sym = symbol_of(callee.name);
        for (const vx::compiler::Param &p : callee.params)
          arg_vts.push_back(vt_of(p.type));
        has_ret = !is_void(callee.ret_type);
        ret = has_ret ? vt_of(callee.ret_type) : VT::I32;
      } else if (const Builtin *b = lookup_builtin(name)) {
        sym = b->sym;
        arg_vts = b->args;
        has_ret = b->has_ret;
        ret = b->ret;
        used_builtins_.insert(sym);
      } else if (auto it = extern_by_sym_.find(name);
                 it != extern_by_sym_.end()) {
        // 来自 linkage 的外部符号（含 @imp / @cppimp）：用其签名发射 declare
        const ir::LinkEntry *e = it->second;
        sym = name;
        for (const vx::compiler::ir::AbiParam &p : e->params)
          arg_vts.push_back(vt_of(p.type));
        has_ret = !is_void(e->ret_type);
        ret = has_ret ? vt_of(e->ret_type) : VT::I32;
        used_externs_.insert(sym);
      } else {
        return fail_inst("未知调用目标 @" + name +
                         "（extern 签名未序列化，且不在内置表）");
      }
    } else if (kind == 'm') {
      // 方法：查找唯一名为 "*.<method>" 的函数
      const std::string suffix = "." + name;
      const ir::FuncIR *found = nullptr;
      for (const ir::FuncIR &f : mod_.funcs) {
        if (f.name.size() > suffix.size() &&
            f.name.compare(f.name.size() - suffix.size(), suffix.size(),
                           suffix) == 0) {
          if (found) return fail_inst("方法 " + name + " 匹配到多个函数");
          found = &f;
        }
      }
      if (!found) return fail_inst("未知方法 ." + name);
      sym = symbol_of(found->name);
      for (const vx::compiler::Param &p : found->params)
        arg_vts.push_back(vt_of(p.type));
      has_ret = !is_void(found->ret_type);
      ret = has_ret ? vt_of(found->ret_type) : VT::I32;
    } else {
      return fail_inst("CALL 调用类别非法: " + std::string(1, kind));
    }

    // 弹参（栈顶为最后一个实参）
    std::vector<Value> args(arg_vts.size());
    for (std::size_t i = arg_vts.size(); i-- > 0;) {
      if (!pop(args[i])) return false;
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
      if (i) call += ", ";
      call += std::string(vt_name(arg_vts[i])) + " " + args[i].val;
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

  bool emit_function(const ir::FuncIR &f) {
    cur_name_ = std::string(f.name);
    const std::size_t slots =
        std::max<std::size_t>(f.local_count, f.params.size());
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
      if (i) sig += ", ";
      VT pv = vt_of(f.params[i].type);
      sig += std::string(vt_name(pv)) + " %a" + std::to_string(i);
    }
    sig += ")";

    // 参数写回槽位（参数占最前槽位）
    for (std::size_t i = 0; i < f.params.size(); ++i) {
      VT pv = vt_of(f.params[i].type);
      bool ok = false;
      std::string al = ensure_slot(static_cast<std::uint32_t>(i), pv, ok);
      if (!ok) return fail("参数槽位类型冲突: " + std::string(f.name));
      line("store " + std::string(vt_name(pv)) + " %a" + std::to_string(i) +
           ", ptr " + al);
    }

    for (std::size_t i = 0; i < f.instructions.size(); ++i) {
      if (!emit_inst(f.instructions[i], i)) return false;
    }
    if (!terminated_) {
      line(ret_void_ ? "ret void"
                     : "ret " + std::string(vt_name(ret_vt_)) + " 0");
      terminated_ = true;
    }

    func_block_ = "define " + sig + " {\n" + "entry:\n" + allocas_ + body_ +
                  "}\n\n";
    return true;
  }

  bool emit_main_wrapper() {
    auto it = fn_by_name_.find("main");
    if (it == fn_by_name_.end()) {
      func_block_.clear();
      return true; // 无 main：不生成入口（库/中间产物场景）
    }
    const ir::FuncIR &m = *it->second;
    if (!m.params.empty())
      return fail("main 函数带参数，后端暂不支持生成入口包装");
    std::string w = "define i32 @main() {\nentry:\n";
    if (is_void(m.ret_type)) {
      w += "  call void @vx_main()\n  ret i32 0\n";
    } else if (vt_of(m.ret_type) == VT::I32) {
      w += "  %r = call i32 @vx_main()\n  ret i32 %r\n";
    } else {
      w += "  %r = call " + std::string(vt_name(vt_of(m.ret_type))) +
           " @vx_main()\n  ret i32 0\n";
    }
    w += "}\n\n";
    func_block_ = w;
    return true;
  }

  // 状态
  const ir::ModuleIR &mod_;
  std::string_view triple_;
  bool emit_entry_ = true;
  std::map<std::string_view, const ir::FuncIR *> fn_by_name_;
  std::map<std::string, const ir::LinkEntry *> extern_by_sym_;
  std::map<std::string, std::size_t> field_index_;
  std::map<std::string, VT> field_type_;
  std::set<std::string> used_externs_;
  std::set<std::string> used_builtins_;
  std::vector<std::string> globals_;
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

EmitResult emit_llvm_ir(const ir::ModuleIR &mod, std::string_view target_triple,
                        bool emit_entry) {
  return Emitter(mod, target_triple, emit_entry).run();
}

} // namespace vx::backend
