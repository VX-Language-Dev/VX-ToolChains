// emitter_inst.hpp — 指令逐条发射（内部实现头，仅供 llvm_emitter.cpp 使用）
#pragma once

#include "emitter_decl.hpp"

namespace vx::backend {

namespace {

// 确保槽位 k 存在且类型为 vt；类型冲突返回空串并置 ok=false
inline std::string Emitter::ensure_slot(std::uint32_t k, VT vt, bool& ok) {
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

// ---- 单条指令发射 ----
inline bool Emitter::emit_inst(const ir::Inst& inst, std::size_t index) {
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
      // 槽位类型由参数声明或首次写入决定；var 参数槽为 64 位通用槽，
      // 指针/浮点/整数均按位无损转换后写入。
      const VT target = (k < slot_vt_.size() && slot_vt_[k] >= 0) ? static_cast<VT>(slot_vt_[k]) : v.vt;
      bool ok = false;
      std::string al = ensure_slot(k, target, ok);
      if (!ok)
        return fail_inst("STORE_VAR 槽位类型冲突");
      if (!coerce(v, target, "STORE_VAR"))
        return false;
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
      if (!coerce(a, VT::F64, "F64 算术操作数") || !coerce(b, VT::F64, "F64 算术操作数"))
        return false;
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
      if (!coerce(a, VT::F64, "F64 比较操作数") || !coerce(b, VT::F64, "F64 比较操作数"))
        return false;
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
      if (!coerce(a, VT::F64, "F64_NEG 操作数"))
        return false;
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
      if (!coerce(a, VT::PTR, "STR_CONCAT 操作数") || !coerce(b, VT::PTR, "STR_CONCAT 操作数"))
        return false;
      std::string t = new_temp();
      line(t + " = call ptr @vx_str_concat(ptr " + a.val + ", ptr " + b.val + ")");
      push({t, VT::PTR});
      return true;
    }
    // ---- 显式类型转换 ----
    case ir::OP_CAST_INT: {
      Value v;
      if (!pop(v))
        return false;
      if (v.vt == VT::I32) {
        push(v);
        return true;
      }
      std::string t = new_temp();
      if (v.vt == VT::F64) {
        line(t + " = fptosi double " + v.val + " to i32");
      } else if (v.vt == VT::PTR) {
        line(t + " = ptrtoint ptr " + v.val + " to i32");
      } else if (v.vt == VT::I64) {
        line(t + " = trunc i64 " + v.val + " to i32");
      } else {
        return fail_inst("CAST_INT 不支持的操作数类型");
      }
      push({t, VT::I32});
      return true;
    }
    case ir::OP_CAST_FLOAT: {
      Value v;
      if (!pop(v))
        return false;
      if (v.vt == VT::F64) {
        push(v);
        return true;
      }
      std::string t = new_temp();
      if (v.vt == VT::I32) {
        line(t + " = sitofp i32 " + v.val + " to double");
      } else if (v.vt == VT::I64) {
        const std::string i = new_temp();
        line(i + " = trunc i64 " + v.val + " to i32");
        line(t + " = sitofp i32 " + i + " to double");
      } else {
        return fail_inst("CAST_FLOAT 不支持的操作数类型");
      }
      push({t, VT::F64});
      return true;
    }
    case ir::OP_CAST_BOOL: {
      Value v;
      if (!pop(v))
        return false;
      const std::string c = new_temp();
      if (v.vt == VT::F64) {
        line(c + " = fcmp one double " + v.val + ", 0.0");
      } else if (v.vt == VT::PTR) {
        line(c + " = icmp ne ptr " + v.val + ", null");
      } else if (v.vt == VT::I64) {
        line(c + " = icmp ne i64 " + v.val + ", 0");
      } else if (v.vt == VT::I32) {
        line(c + " = icmp ne i32 " + v.val + ", 0");
      } else {
        return fail_inst("CAST_BOOL 不支持的操作数类型");
      }
      const std::string t = new_temp();
      line(t + " = zext i1 " + c + " to i32");
      push({t, VT::I32});
      return true;
    }
    case ir::OP_CAST_STR: {
      Value v;
      if (!pop(v))
        return false;
      if (v.vt == VT::PTR) {
        push(v); // 已是字符串指针
        return true;
      }
      const std::string t = new_temp();
      if (v.vt == VT::F64) {
        used_builtins_.insert("vx_str_from_float");
        line(t + " = call ptr @vx_str_from_float(double " + v.val + ")");
      } else if (v.vt == VT::I32) {
        if (inst.args == "b") {
          used_builtins_.insert("vx_str_from_bool");
          line(t + " = call ptr @vx_str_from_bool(i32 " + v.val + ")");
        } else {
          used_builtins_.insert("vx_str_from_int");
          line(t + " = call ptr @vx_str_from_int(i32 " + v.val + ")");
        }
      } else if (v.vt == VT::I64) {
        used_builtins_.insert("vx_str_from_int");
        const std::string i = new_temp();
        line(i + " = trunc i64 " + v.val + " to i32");
        line(t + " = call ptr @vx_str_from_int(i32 " + i + ")");
      } else {
        return fail_inst("CAST_STR 不支持的操作数类型");
      }
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
        if (!coerce(p, VT::PTR, "DEREF 指针"))
          return false;
        base = p.val;
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
        if (!coerce(p, VT::PTR, "DEREF_STORE 指针"))
          return false;
        base = p.val;
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
      if ((a.vt != VT::PTR || b.vt != VT::PTR) &&
          (!coerce(a, VT::PTR, "STR_EQ 操作数") || !coerce(b, VT::PTR, "STR_EQ 操作数")))
        return false;
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
      if (obj.vt != VT::PTR && !coerce(obj, VT::PTR, "FIELD_LOAD 对象"))
        return false;
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
      if (obj.vt != VT::PTR && !coerce(obj, VT::PTR, "FIELD_STORE 对象"))
        return false;
      auto it = field_index_.find(inst.args);
      if (it == field_index_.end())
        return fail_inst("未知字段 " + inst.args);
      std::size_t idx = it->second;
      VT ft = (static_cast<unsigned int>(field_type_.contains(inst.args)) != 0U) ? field_type_[inst.args] : VT::I32;
      if (!coerce(val, ft, "FIELD_STORE 字段值"))
        return false;
      std::string gep = new_temp();
      line(gep + " = getelementptr i8, ptr " + obj.val + ", i64 " + std::to_string(idx * 8));
      line("store " + std::string(vt_name(ft)) + " " + val.val + ", ptr " + gep);
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
        line(t + " = call i64 @vx_map_get(ptr " + objv.val + ", i64 " + keyi64 + ", i64 " + isstr + ", ptr " + keyptr +
             ")");
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
      if (!coerce(v, ret_vt_, "RETURN"))
        return false;
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

} // namespace

} // namespace vx::backend
