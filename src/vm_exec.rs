// ==================== VM Executor ====================

use std::collections::HashMap;
use crate::opcode::OpCode;
use crate::value::Value;
use crate::vm::{VM, DebugAction, StepMode};

impl VM {
    pub fn run(&mut self) -> Result<Value, String> {
        if self.module.functions.is_empty() {
            return Ok(Value::Nil);
        }

        let main_idx = self
            .module
            .function_map
            .get("__main__")
            .copied()
            .unwrap_or(0);

        self.frames.push(crate::instruction::CallFrame {
            fn_idx: main_idx,
            pc: 0,
            stack_base: 0,
            tos_base: 0,
            locals: HashMap::new(),
            owned_allocs: Vec::new(),
        });

        while let Some(frame) = self.frames.last() {
            // Debugging support: check hook and breakpoints before executing instruction
            if let Some(ref hook) = self.debug_hook {
                match hook(self) {
                    DebugAction::Break => {
                        return Ok(self.handle_breakpoint());
                    }
                    DebugAction::StepInto => {
                        self.step_mode = StepMode::Into;
                        self.step_count = 1;
                    }
                    DebugAction::StepOver => {
                        self.step_mode = StepMode::Over;
                        self.step_count = 1;
                    }
                    DebugAction::StepOut => {
                        self.step_mode = StepMode::Out;
                        self.step_count = 1;
                    }
                    DebugAction::Continue => {}
                }
            }

            // Breakpoint check: if we are about to execute an instruction at a breakpoint
            if self.breakpoints.contains(&frame.pc) {
                if let Some(ref hook) = self.debug_hook {
                    match hook(self) {
                        DebugAction::Break => {
                            return Ok(self.handle_breakpoint());
                        }
                        _ => {} // continue based on hook
                    }
                } else {
                    return Ok(self.handle_breakpoint());
                }
            }

            if frame.pc >= self.current_fn().instructions.len() {
                let leaving_frame = self.frames.pop().unwrap();
                self.cleanup_frame_allocs(&leaving_frame);
                continue;
            }

            let fn_idx = self.frames.last().unwrap().fn_idx;
            let pc = self.frames.last().unwrap().pc;
            let inst = &self.module.functions[fn_idx].instructions[pc];
            self.frames.last_mut().unwrap().pc += 1;

            // Step handling: if we are in step mode and step_count>0, we want to stop after this instruction
            // We'll execute the instruction and then check for step completion
            match inst.op {
                // ===== Load / Store =====
                OpCode::LoadConst => {
                    let idx = inst.iarg.unwrap_or(0) as usize;
                    if idx >= self.module.constants.len() {
                        return self.runtime_error("Constant index out of bounds");
                    }
                    self.push(self.module.constants[idx].clone());
                }
                OpCode::LoadNil => self.push(Value::Nil),
                OpCode::LoadTrue => self.push(Value::Bool(true)),
                OpCode::LoadFalse => self.push(Value::Bool(false)),
                OpCode::LoadVar => {
                    let name = inst.sarg.clone().unwrap_or_default();
                    if let Some(v) = self.current_frame().locals.get(&name) {
                        self.push(v.clone());
                    } else if let Some(v) = self.globals.get(&name) {
                        self.push(v.clone());
                    } else {
                        return self.runtime_error(&format!("Undefined variable: {}", name));
                    }
                }
                OpCode::StoreVar => {
                    let name = inst.sarg.clone().unwrap_or_default();
                    let v = self.pop();
                    if self.current_frame().locals.contains_key(&name) {
                        self.current_frame_mut().locals.insert(name, v);
                    } else {
                        self.globals.insert(name, v);
                    }
                }
                OpCode::DefineVar => {
                    let name = inst.sarg.clone().unwrap_or_default();
                    let v = self.pop();
                    self.current_frame_mut().locals.insert(name, v);
                }
                OpCode::Dup => {
                    if let Some(v) = self.peek(0) {
                        self.push(v.clone());
                    } else {
                        return self.runtime_error("Stack underflow on DUP");
                    }
                }
                OpCode::Pop => {
                    self.pop();
                }

                // ===== Call / Return =====
                OpCode::Call => {
                    let num_args = inst.iarg.unwrap_or(0) as usize;
                    if num_args > self.stack_len() {
                        return self.runtime_error("Not enough arguments on stack");
                    }
                    let mut args: Vec<Value> =
                        self.drain_stack(self.stack_len() - num_args..self.stack_len());
                    args.reverse();
                    let callee = self.pop();

                    match &callee {
                        Value::String(ref s) => {
                            if let Some(&fn_idx) = self.module.function_map.get(s) {
                                if let Err(e) = self.call_user_function(fn_idx, &args) {
                                    return self.runtime_error(&e);
                                }
                            } else if let Some(f) = self.builtins.get(s) {
                                let result = f(&mut args);
                                self.push(result);
                            } else {
                                return self.runtime_error(&format!("Unknown function: {}", s));
                            }
                        }
                        _ => return self.runtime_error("Callee is not callable"),
                    }
                }
                OpCode::Return => {
                    let ret = self.pop();
                    let leaving_frame = self.frames.pop().unwrap();
                    self.cleanup_frame_allocs(&leaving_frame);
                    if let Some(frame) = self.frames.last_mut() {
                        while self.stack.len() > frame.stack_base {
                            self.stack.pop();
                        }
                        self.push(ret);
                    } else {
                        return Ok(ret);
                    }
                }

                // ===== Jump =====
                OpCode::Jump => {
                    let target = inst.iarg.unwrap_or(0) as usize;
                    self.current_frame_mut().pc = target;
                }
                OpCode::JumpIfFalse => {
                    let target = inst.iarg.unwrap_or(0) as usize;
                    let v = self.pop();
                    if !v.is_truthy() {
                        self.current_frame_mut().pc = target;
                    }
                }
                OpCode::JumpIfTrue => {
                    let target = inst.iarg.unwrap_or(0) as usize;
                    let v = self.pop();
                    if v.is_truthy() {
                        self.current_frame_mut().pc = target;
                    }
                }

                // ===== Binary Arithmetic =====
                OpCode::BinaryAdd => {
                    let b = self.pop();
                    let a = self.pop();
                    let result = match (&a, &b) {
                        (Value::Int(ai), Value::Int(bi)) => Value::Int(ai + bi),
                        (Value::Float(af), Value::Float(bf)) => Value::Float(af + bf),
                        (Value::String(as_), Value::String(bs)) => {
                            Value::String(format!("{}{}", as_, bs))
                        }
                        (Value::String(as_), _) => {
                            Value::String(format!("{}{}", as_, b.to_string()))
                        }
                        _ => return self.runtime_error("Type mismatch in +"),
                    };
                    self.push(result);
                }
                OpCode::BinarySub => {
                    let b = self.pop();
                    let a = self.pop();
                    let result = match (&a, &b) {
                        (Value::Int(ai), Value::Int(bi)) => Value::Int(ai - bi),
                        (Value::Float(af), Value::Float(bf)) => Value::Float(af - bf),
                        _ => return self.runtime_error("Type mismatch in -"),
                    };
                    self.push(result);
                }
                OpCode::BinaryMul => {
                    let b = self.pop();
                    let a = self.pop();
                    let result = match (&a, &b) {
                        (Value::Int(ai), Value::Int(bi)) => Value::Int(ai * bi),
                        (Value::Float(af), Value::Float(bf)) => Value::Float(af * bf),
                        _ => return self.runtime_error("Type mismatch in *"),
                    };
                    self.push(result);
                }
                OpCode::BinaryDiv => {
                    let b = self.pop();
                    let a = self.pop();
                    let result = match (&a, &b) {
                        (Value::Int(ai), Value::Int(bi)) if *bi != 0 => Value::Int(ai / bi),
                        (Value::Float(af), Value::Float(bf)) if *bf != 0.0 => Value::Float(af / bf),
                        _ => return self.runtime_error("Type mismatch in /"),
                    };
                    self.push(result);
                }
                OpCode::BinaryMod => {
                    let b = self.pop();
                    let a = self.pop();
                    let result = match (&a, &b) {
                        (Value::Int(ai), Value::Int(bi)) if *bi != 0 => Value::Int(ai % bi),
                        _ => return self.runtime_error("Type mismatch in %"),
                    };
                    self.push(result);
                }
                OpCode::BinaryPow => {
                    let b = self.pop();
                    let a = self.pop();
                    let result = match (&a, &b) {
                        (Value::Int(ai), Value::Int(bi)) => {
                            Value::Int(((*ai as f64).powf(*bi as f64)) as i64)
                        }
                        (Value::Float(af), Value::Float(bf)) => Value::Float(af.powf(*bf)),
                        _ => return self.runtime_error("Type mismatch in ^"),
                    };
                    self.push(result);
                }

                // ===== Binary Comparison =====
                OpCode::BinaryEq => {
                    let b = self.pop();
                    let a = self.pop();
                    let eq = a == b;
                    self.push(Value::Bool(eq));
                }
                OpCode::BinaryNe => {
                    let b = self.pop();
                    let a = self.pop();
                    let ne = a != b;
                    self.push(Value::Bool(ne));
                }
                OpCode::BinaryLt => {
                    let b = self.pop();
                    let a = self.pop();
                    let result = match (&a, &b) {
                        (Value::Int(ai), Value::Int(bi)) => Value::Bool(ai < bi),
                        (Value::Float(af), Value::Float(bf)) => Value::Bool(af < bf),
                        _ => return self.runtime_error("Type mismatch in <"),
                    };
                    self.push(result);
                }
                OpCode::BinaryGt => {
                    let b = self.pop();
                    let a = self.pop();
                    let result = match (&a, &b) {
                        (Value::Int(ai), Value::Int(bi)) => Value::Bool(ai > bi),
                        (Value::Float(af), Value::Float(bf)) => Value::Bool(af > bf),
                        _ => return self.runtime_error("Type mismatch in >"),
                    };
                    self.push(result);
                }
                OpCode::BinaryLe => {
                    let b = self.pop();
                    let a = self.pop();
                    let result = match (&a, &b) {
                        (Value::Int(ai), Value::Int(bi)) => Value::Bool(ai <= bi),
                        (Value::Float(af), Value::Float(bf)) => Value::Bool(af <= bf),
                        _ => return self.runtime_error("Type mismatch in <="),
                    };
                    self.push(result);
                }
                OpCode::BinaryGe => {
                    let b = self.pop();
                    let a = self.pop();
                    let result = match (&a, &b) {
                        (Value::Int(ai), Value::Int(bi)) => Value::Bool(ai >= bi),
                        (Value::Float(af), Value::Float(bf)) => Value::Bool(af >= bf),
                        _ => return self.runtime_error("Type mismatch in >="),
                    };
                    self.push(result);
                }
                OpCode::BinaryAnd => {
                    let b = self.pop();
                    let a = self.pop();
                    self.push(Value::Bool(a.is_truthy() && b.is_truthy()));
                }
                OpCode::BinaryOr => {
                    let b = self.pop();
                    let a = self.pop();
                    self.push(Value::Bool(a.is_truthy() || b.is_truthy()));
                }

                // ===== Type-Specialized Binary Arithmetic =====
                OpCode::AddInt => {
                    let b = self.pop_int();
                    let a = self.pop_int();
                    self.push_int(a + b);
                }
                OpCode::AddFloat => {
                    let b = self.pop_float();
                    let a = self.pop_float();
                    self.push_float(a + b);
                }
                OpCode::SubInt => {
                    let b = self.pop_int();
                    let a = self.pop_int();
                    let result = a - b;
                    self.push_int(result);
                }
                OpCode::SubFloat => {
                    let b = self.pop_float();
                    let a = self.pop_float();
                    self.push_float(a - b);
                }
                OpCode::MulInt => {
                    let b = self.pop_int();
                    let a = self.pop_int();
                    self.push_int(a * b);
                }
                OpCode::MulFloat => {
                    let b = self.pop_float();
                    let a = self.pop_float();
                    self.push_float(a * b);
                }
                OpCode::DivInt => {
                    let b = self.pop_int();
                    let a = self.pop_int();
                    if b != 0 {
                        self.push_int(a / b);
                    } else {
                        return self.runtime_error("Division by zero");
                    }
                }
                OpCode::DivFloat => {
                    let b = self.pop_float();
                    let a = self.pop_float();
                    if b != 0.0 {
                        self.push_float(a / b);
                    } else {
                        return self.runtime_error("Division by zero");
                    }
                }
                OpCode::ModInt => {
                    let b = self.pop_int();
                    let a = self.pop_int();
                    if b != 0 {
                        self.push_int(a % b);
                    } else {
                        return self.runtime_error("Modulo by zero");
                    }
                }

                // ===== Type-Specialized Comparison =====
                OpCode::EqInt => {
                    let b = self.pop_int();
                    let a = self.pop_int();
                    self.push_bool(a == b);
                }
                OpCode::EqFloat => {
                    let b = self.pop_float();
                    let a = self.pop_float();
                    self.push_bool(a == b);
                }
                OpCode::LtInt => {
                    let b = self.pop_int();
                    let a = self.pop_int();
                    self.push_bool(a < b);
                }
                OpCode::LtFloat => {
                    let b = self.pop_float();
                    let a = self.pop_float();
                    self.push_bool(a < b);
                }
                OpCode::GtInt => {
                    let b = self.pop_int();
                    let a = self.pop_int();
                    self.push_bool(a > b);
                }
                OpCode::GtFloat => {
                    let b = self.pop_float();
                    let a = self.pop_float();
                    self.push_bool(a > b);
                }
                OpCode::LeInt => {
                    let b = self.pop_int();
                    let a = self.pop_int();
                    self.push_bool(a <= b);
                }
                OpCode::LeFloat => {
                    let b = self.pop_float();
                    let a = self.pop_float();
                    self.push_bool(a <= b);
                }
                OpCode::GeInt => {
                    let b = self.pop_int();
                    let a = self.pop_int();
                    self.push_bool(a >= b);
                }
                OpCode::GeFloat => {
                    let b = self.pop_float();
                    let a = self.pop_float();
                    self.push_bool(a >= b);
                }

                // ===== Type-Specialized Logical =====
                OpCode::And => {
                    let b = self.pop_bool();
                    let a = self.pop_bool();
                    self.push_bool(a && b);
                }
                OpCode::Or => {
                    let b = self.pop_bool();
                    let a = self.pop_bool();
                    self.push_bool(a || b);
                }

                // ===== Type-Specialized Unary =====
                OpCode::NegInt => {
                    let a = self.pop_int();
                    self.push_int(-a);
                }
                OpCode::NegFloat => {
                    let a = self.pop_float();
                    self.push_float(-a);
                }
                OpCode::Not => {
                    let a = self.pop_bool();
                    self.push_bool(!a);
                }

                // ===== Unary =====
                OpCode::UnaryNeg => {
                    let a = self.pop();
                    let result = match a {
                        Value::Int(i) => Value::Int(-i),
                        Value::Float(f) => Value::Float(-f),
                        _ => return self.runtime_error("Type mismatch in unary -"),
                    };
                    self.push(result);
                }
                OpCode::UnaryNot => {
                    let a = self.pop();
                    self.push(Value::Bool(!a.is_truthy()));
                }

                // ===== Array =====
                OpCode::MakeArray => {
                    let count = inst.iarg.unwrap_or(0) as usize;
                    let mut tmp: Vec<Value> =
                        self.drain_stack(self.stack_len() - count..self.stack_len());
                    tmp.reverse();
                    self.push(Value::Array(tmp));
                }
                OpCode::IndexGet => {
                    let idx = self.pop();
                    let obj = self.pop();
                    let result = match (&obj, &idx) {
                        (Value::Array(arr), Value::Int(i)) => {
                            if *i < 0 || (*i as usize) >= arr.len() {
                                return self.runtime_error(&format!("Array index out of bounds: {}", i));
                            }
                            arr[*i as usize].clone()
                        }
                        (Value::Map(map), _) => {
                            let key = idx.to_string();
                            map.get(&key).cloned().unwrap_or(Value::Nil)
                        }
                        (Value::String(s), Value::Int(i)) => {
                            if *i < 0 || (*i as usize) >= s.len() {
                                return self.runtime_error("String index out of bounds");
                            }
                            Value::String(s.chars().nth(*i as usize).unwrap().to_string())
                        }
                        _ => return self.runtime_error("Cannot index this type"),
                    };
                    self.push(result);
                }
                OpCode::IndexSet => {
                    let val = self.pop();
                    let idx = self.pop();
                    let mut obj = self.pop();
                    match &mut obj {
                        Value::Array(arr) => {
                            if let Value::Int(i) = idx {
                                if i < 0 || (i as usize) >= arr.len() {
                                    return self.runtime_error("Array index out of bounds in assignment");
                                }
                                arr[i as usize] = val;
                            }
                        }
                        Value::Map(map) => {
                            let key = idx.to_string();
                            map.insert(key, val);
                        }
                        _ => return self.runtime_error("Cannot index-assign this type"),
                    }
                    self.push(obj);
                }

                // ===== Map =====
                OpCode::MakeMap => {
                    let count = inst.iarg.unwrap_or(0) as usize;
                    let mut tmp: Vec<Value> =
                        self.drain_stack(self.stack_len() - count * 2..self.stack_len());
                    tmp.reverse();
                    let mut map = HashMap::new();
                    for i in 0..count {
                        let key = tmp[i * 2].to_string();
                        map.insert(key, tmp[i * 2 + 1].clone());
                    }
                    self.push(Value::Map(map));
                }

                // ===== Struct / Instance =====
                OpCode::MakeStruct | OpCode::MakeClass => {
                    let name = inst.sarg.clone().unwrap_or_default();
                    let mut inst_val = Value::Instance { class_name: name.clone(), fields: HashMap::new() };
                    if let Some(fields) = self.module.struct_defs.get(&name) {
                        if let Value::Instance {
                            fields: ref mut inst_fields,
                            ..
                        } = inst_val
                        {
                            for field in fields {
                                inst_fields.insert(field.clone(), Value::Nil);
                            }
                        }
                    }
                    self.push(inst_val);
                }

                // ===== Property access =====
                OpCode::PropertyGet => {
                    let prop_name = inst.sarg.clone().unwrap_or_default();
                    let obj = self.pop();
                    let obj_type_name = obj.type_name();
                    let result = match &obj {
                        Value::Pointer { alloc_id, .. } => {
                            self.validate_pointer(&obj)?;
                            if let Some(rec) = self.alloc_registry.get(alloc_id) {
                                match &rec.instance {
                                    Value::Instance { fields, .. } | Value::Map(fields) => {
                                        if let Some(v) = fields.get(&prop_name) {
                                            v.clone()
                                        } else {
                                            let method_name = format!(
                                                "{}_{}",
                                                rec.instance.type_name(),
                                                prop_name
                                            );
                                            if self.module.function_map.contains_key(&method_name) {
                                                Value::String(method_name)
                                            } else {
                                                return self.runtime_error(&format!(
                                                    "Property not found: {}",
                                                    prop_name
                                                ));
                                            }
                                        }
                                    }
                                    _ => return self.runtime_error(
                                        "Cannot access property on dereferenced pointer type",
                                    ),
                                }
                            } else {
                                Value::Nil
                            }
                        }
                        Value::Instance { fields, .. } | Value::Map(fields) => {
                            if let Some(v) = fields.get(&prop_name) {
                                v.clone()
                            } else {
                                let method_name = format!("{}_{}", obj_type_name, prop_name);
                                if self.module.function_map.contains_key(&method_name) {
                                    Value::String(method_name)
                                } else {
                                    return self.runtime_error(&format!(
                                        "Property not found: {}",
                                        prop_name
                                    ));
                                }
                            }
                        }
                        Value::Array(arr) if prop_name == "length" => Value::Int(arr.len() as i64),
                        Value::String(s) if prop_name == "length" => Value::Int(s.len() as i64),
                        _ => return self.runtime_error("Cannot access property on this type"),
                    };
                    self.push(result);
                }
                OpCode::PropertySet => {
                    let prop_name = inst.sarg.clone().unwrap_or_default();
                    let val = self.pop();
                    let mut obj = self.pop();
                    if let Value::Pointer { alloc_id, .. } = &obj {
                        let alloc_id = *alloc_id;
                        if self.validate_pointer(&obj)? {
                            if let Some(rec) = self.alloc_registry.get_mut(&alloc_id) {
                                if let Value::Instance { fields, .. } | Value::Map(fields) =
                                    &mut rec.instance
                                {
                                    fields.insert(prop_name, val);
                                }
                            }
                        }
                    } else if let Value::Instance { fields, .. } | Value::Map(fields) = &mut obj {
                        fields.insert(prop_name, val);
                    } else {
                        return self.runtime_error("Cannot set property on this type");
                    }
                    self.push(obj);
                }

                // ===== Pointer Operations =====
                OpCode::AddressOf => {
                    let v = self.pop();
                    self.push(v);
                }
                OpCode::Deref => {
                    let ptr = self.pop();
                    self.push(self.deref_pointer(&ptr)?);
                }
                OpCode::PointerMember => {
                    let prop_name = inst.sarg.clone().unwrap_or_default();
                    let ptr = self.pop();
                    let result = match &ptr {
                        Value::Pointer { alloc_id, .. } => {
                            self.validate_pointer(&ptr)?;
                            if let Some(rec) = self.alloc_registry.get(alloc_id) {
                                match &rec.instance {
                                    Value::Instance { fields, .. } | Value::Map(fields) => {
                                        if let Some(v) = fields.get(&prop_name) {
                                            v.clone()
                                        } else {
                                            let method_name = format!(
                                                "{}_{}",
                                                rec.instance.type_name(),
                                                prop_name
                                            );
                                            if self.module.function_map.contains_key(&method_name) {
                                                Value::String(method_name)
                                            } else {
                                                return self.runtime_error(&format!(
                                                    "Pointer member not found: {}",
                                                    prop_name
                                                ));
                                            }
                                        }
                                    }
                                    _ => return self.runtime_error(
                                        "Cannot access member through non-instance pointer",
                                    ),
                                }
                            } else {
                                Value::Nil
                            }
                        }
                        Value::Instance { fields, .. } | Value::Map(fields) => {
                            fields.get(&prop_name).cloned().unwrap_or(Value::Nil)
                        }
                        _ => return self.runtime_error("Cannot access member through non-pointer type"),
                    };
                    self.push(result);
                }

                OpCode::New => {
                    let class_name_val = self.pop();
                    if let Value::String(class_name) = class_name_val {
                        let mut inst_val = Value::Instance { class_name: class_name.clone(), fields: HashMap::new() };
                        if let Some(fields) = self.module.struct_defs.get(&class_name) {
                            if let Value::Instance {
                                fields: fields_map, ..
                            } = &mut inst_val
                            {
                                for field in fields {
                                    fields_map.insert(field.clone(), Value::Nil);
                                }
                            }
                        }
                        self.push(inst_val);
                    } else {
                        return self.runtime_error("new: expected class name string");
                    }
                }

                // ===== HALT =====
                OpCode::Halt => {
                    self.frames.clear();
                    return Ok(Value::Nil);
                }

                // ===== Import =====
                OpCode::Import => {
                    if let Some(ref sarg) = inst.sarg {
                        let parts: Vec<&str> = sarg.split(',').collect();
                        let module_name = parts.last().copied().unwrap_or("unknown");
                        if !module_name.is_empty() {
                            self.globals.insert(
                                module_name.to_string(),
                                Value::Instance { class_name: module_name.to_string(), fields: HashMap::new() },
                            );
                        }
                    }
                }

                // ===== SysArgv / System / File I/O =====
                OpCode::SysArgv => {
                    self.push(Value::Array(Vec::new()));
                }
                OpCode::System => {
                    let cmd_arg = self.pop();
                    match cmd_arg {
                        Value::String(ref cmd) => {
                            // 使用 shell 执行命令以支持管道、重定向等
                            let status = self.run_shell_command(cmd);
                            match status {
                                Ok(s) => {
                                    self.push(Value::Int(s.code().unwrap_or(-1) as i64));
                                }
                                Err(_) => {
                                    self.push(Value::Int(-1));
                                }
                            }
                        }
                        _ => {
                            return self.runtime_error("os_system 参数必须为字符串类型");
                        }
                    }
                }
                OpCode::FileRead => {
                    self.pop();
                    self.push(Value::Nil);
                }
                OpCode::FileWrite => {
                    self.pop();
                    self.pop();
                }
                OpCode::FileExists => {
                    self.pop();
                    self.push(Value::Bool(false));
                }

                // ===== Memory Safety / Ownership =====
                OpCode::Newz => {
                    let num_args = inst.iarg.unwrap_or(0) as usize;
                    let mut args: Vec<Value> =
                        self.drain_stack(self.stack_len() - num_args..self.stack_len());
                    args.reverse();
                    let class_name_val = self.pop();

                    if let Value::String(class_name) = class_name_val {
                        let mut inst_val = Value::Instance { class_name: class_name.clone(), fields: HashMap::new() };
                        if let Some(fields) = self.module.struct_defs.get(&class_name) {
                            let mut fields_map = HashMap::new();
                            for (i, field) in fields.iter().enumerate() {
                                let val = args.get(i).cloned().unwrap_or(Value::Nil);
                                fields_map.insert(field.clone(), val);
                            }
                            inst_val = Value::Instance {
                                class_name: class_name.clone(),
                                fields: fields_map,
                            };
                        }
                        let alloc_id = self.alloc_heap(class_name.clone(), inst_val);
                        let gen = self
                            .alloc_registry
                            .get(&alloc_id)
                            .map(|r| r.generation)
                            .unwrap_or(0);
                        self.push(Value::Pointer { alloc_id: alloc_id, generation: gen, class_name: class_name });
                    } else {
                        return self.runtime_error("newz: expected class name string");
                    }
                }
                OpCode::Free => {
                    let ptr = self.pop();
                    if let Value::Pointer {
                        alloc_id,
                        generation,
                        ..
                    } = ptr
                    {
                        self.free_allocation(alloc_id, generation)?;
                    } else {
                        return self.runtime_error("free: can only free heap pointers (newz allocations)");
                    }
                }
                OpCode::OwnershipMove => {
                    // 所有权转移: 编译期已检查，运行时只需确保值正常传递
                }
                OpCode::ScopeDrop => {
                    // 作用域结束时的自动清理，当前实现通过cleanup_frame_allocs在RETURN时处理
                }
                OpCode::BorrowCheck => {
                    // 借用检查: 编译期OwnershipChecker已验证安全性
                }
                OpCode::AliveCheck => {
                    if let Some(v) = self.peek(0) {
                        if let Value::Pointer { .. } = v {
                            if self.validate_pointer(v).is_err() {
                                self.pop();
                                self.push(Value::Nil);
                            }
                        }
                    }
                }

                _ => return self.runtime_error(&format!("Unimplemented opcode: {:?}", inst.op)),
            }

            // Step handling: if we are in step mode and step_count>0, decrement after executing instruction
            if self.step_count > 0 {
                self.step_count -= 1;
                if self.step_count == 0 {
                    return Ok(self.handle_breakpoint());
                }
            }
        }

        Ok(Value::Nil)
    }

    /// 平台特定的 shell 命令执行 (Windows 使用 cmd /C, Unix 使用 sh -c)
    #[cfg(target_os = "windows")]
    fn run_shell_command(&self, cmd: &str) -> std::io::Result<std::process::ExitStatus> {
        std::process::Command::new("cmd")
            .args(["/C", cmd])
            .stdout(std::process::Stdio::inherit())
            .stderr(std::process::Stdio::inherit())
            .status()
    }

    #[cfg(not(target_os = "windows"))]
    fn run_shell_command(&self, cmd: &str) -> std::io::Result<std::process::ExitStatus> {
        std::process::Command::new("sh")
            .args(["-c", cmd])
            .stdout(std::process::Stdio::inherit())
            .stderr(std::process::Stdio::inherit())
            .status()
    }
}
