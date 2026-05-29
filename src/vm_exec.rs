// ==================== VM Executor ====================

use std::collections::HashMap;
use crate::opcode::OpCode;
use crate::value::Value;
use crate::vm::VM;

impl VM {
    pub fn run(&mut self) -> Value {
        if self.module.functions.is_empty() {
            return Value::nil();
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
            locals: HashMap::new(),
            owned_allocs: Vec::new(),
        });

        while let Some(frame) = self.frames.last() {
            if frame.pc >= self.current_fn().instructions.len() {
                let leaving_frame = self.frames.pop().unwrap();
                self.cleanup_frame_allocs(&leaving_frame);
                continue;
            }

            let inst = self.current_fn().instructions[frame.pc].clone();
            self.current_frame_mut().pc += 1;

            match inst.op {
                // ===== Load / Store =====
                OpCode::LoadConst => {
                    let idx = inst.iarg.unwrap_or(0) as usize;
                    if idx >= self.module.constants.len() {
                        self.runtime_error("Constant index out of bounds");
                    }
                    self.push(self.module.constants[idx].clone());
                }
                OpCode::LoadNil => self.push(Value::nil()),
                OpCode::LoadTrue => self.push(Value::bool(true)),
                OpCode::LoadFalse => self.push(Value::bool(false)),
                OpCode::LoadVar => {
                    let name = inst.sarg.clone().unwrap_or_default();
                    if let Some(v) = self.current_frame().locals.get(&name) {
                        self.push(v.clone());
                    } else if let Some(v) = self.globals.get(&name) {
                        self.push(v.clone());
                    } else {
                        self.runtime_error(&format!("Undefined variable: {}", name));
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
                        self.runtime_error("Stack underflow on DUP");
                    }
                }
                OpCode::Pop => {
                    self.pop();
                }

                // ===== Call / Return =====
                OpCode::Call => {
                    let num_args = inst.iarg.unwrap_or(0) as usize;
                    if num_args > self.stack.len() {
                        self.runtime_error("Not enough arguments on stack");
                    }
                    let mut args: Vec<Value> =
                        self.stack.drain(self.stack.len() - num_args..).collect();
                    args.reverse();
                    let callee = self.pop();

                    match &callee {
                        Value::String(ref s) => {
                            if let Some(&fn_idx) = self.module.function_map.get(s) {
                                self.call_user_function(fn_idx, &args).ok();
                            } else if let Some(f) = self.builtins.get(s) {
                                let result = f(&mut args);
                                self.push(result);
                            } else {
                                self.runtime_error(&format!("Unknown function: {}", s));
                            }
                        }
                        _ => self.runtime_error("Callee is not callable"),
                    }
                }
                OpCode::Return => {
                    let ret = self.pop();
                    let leaving_frame = self.frames.pop().unwrap();
                    self.cleanup_frame_allocs(&leaving_frame);
                    if let Some(_frame) = self.frames.last() {
                        self.push(ret);
                    } else {
                        return ret;
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
                        (Value::Int(ai), Value::Int(bi)) => Value::int(ai + bi),
                        (Value::Float(af), Value::Float(bf)) => Value::float(af + bf),
                        (Value::String(as_), Value::String(bs)) => {
                            Value::string(format!("{}{}", as_, bs))
                        }
                        (Value::String(as_), _) => {
                            Value::string(format!("{}{}", as_, b.to_string()))
                        }
                        _ => self.runtime_error("Type mismatch in +"),
                    };
                    self.push(result);
                }
                OpCode::BinarySub => {
                    let b = self.pop();
                    let a = self.pop();
                    let result = match (&a, &b) {
                        (Value::Int(ai), Value::Int(bi)) => Value::int(ai - bi),
                        (Value::Float(af), Value::Float(bf)) => Value::float(af - bf),
                        _ => self.runtime_error("Type mismatch in -"),
                    };
                    self.push(result);
                }
                OpCode::BinaryMul => {
                    let b = self.pop();
                    let a = self.pop();
                    let result = match (&a, &b) {
                        (Value::Int(ai), Value::Int(bi)) => Value::int(ai * bi),
                        (Value::Float(af), Value::Float(bf)) => Value::float(af * bf),
                        _ => self.runtime_error("Type mismatch in *"),
                    };
                    self.push(result);
                }
                OpCode::BinaryDiv => {
                    let b = self.pop();
                    let a = self.pop();
                    let result = match (&a, &b) {
                        (Value::Int(ai), Value::Int(bi)) if *bi != 0 => Value::int(ai / bi),
                        (Value::Float(af), Value::Float(bf)) if *bf != 0.0 => Value::float(af / bf),
                        _ => self.runtime_error("Type mismatch in /"),
                    };
                    self.push(result);
                }
                OpCode::BinaryMod => {
                    let b = self.pop();
                    let a = self.pop();
                    let result = match (&a, &b) {
                        (Value::Int(ai), Value::Int(bi)) if *bi != 0 => Value::int(ai % bi),
                        _ => self.runtime_error("Type mismatch in %"),
                    };
                    self.push(result);
                }
                OpCode::BinaryPow => {
                    let b = self.pop();
                    let a = self.pop();
                    let result = match (&a, &b) {
                        (Value::Int(ai), Value::Int(bi)) => {
                            Value::int(((*ai as f64).powf(*bi as f64)) as i64)
                        }
                        (Value::Float(af), Value::Float(bf)) => Value::float(af.powf(*bf)),
                        _ => self.runtime_error("Type mismatch in ^"),
                    };
                    self.push(result);
                }

                // ===== Binary Comparison =====
                OpCode::BinaryEq => {
                    let b = self.pop();
                    let a = self.pop();
                    let eq = a == b;
                    self.push(Value::bool(eq));
                }
                OpCode::BinaryNe => {
                    let b = self.pop();
                    let a = self.pop();
                    let ne = a != b;
                    self.push(Value::bool(ne));
                }
                OpCode::BinaryLt => {
                    let b = self.pop();
                    let a = self.pop();
                    let result = match (&a, &b) {
                        (Value::Int(ai), Value::Int(bi)) => Value::bool(ai < bi),
                        (Value::Float(af), Value::Float(bf)) => Value::bool(af < bf),
                        _ => self.runtime_error("Type mismatch in <"),
                    };
                    self.push(result);
                }
                OpCode::BinaryGt => {
                    let b = self.pop();
                    let a = self.pop();
                    let result = match (&a, &b) {
                        (Value::Int(ai), Value::Int(bi)) => Value::bool(ai > bi),
                        (Value::Float(af), Value::Float(bf)) => Value::bool(af > bf),
                        _ => self.runtime_error("Type mismatch in >"),
                    };
                    self.push(result);
                }
                OpCode::BinaryLe => {
                    let b = self.pop();
                    let a = self.pop();
                    let result = match (&a, &b) {
                        (Value::Int(ai), Value::Int(bi)) => Value::bool(ai <= bi),
                        (Value::Float(af), Value::Float(bf)) => Value::bool(af <= bf),
                        _ => self.runtime_error("Type mismatch in <="),
                    };
                    self.push(result);
                }
                OpCode::BinaryGe => {
                    let b = self.pop();
                    let a = self.pop();
                    let result = match (&a, &b) {
                        (Value::Int(ai), Value::Int(bi)) => Value::bool(ai >= bi),
                        (Value::Float(af), Value::Float(bf)) => Value::bool(af >= bf),
                        _ => self.runtime_error("Type mismatch in >="),
                    };
                    self.push(result);
                }
                OpCode::BinaryAnd => {
                    let b = self.pop();
                    let a = self.pop();
                    self.push(Value::bool(a.is_truthy() && b.is_truthy()));
                }
                OpCode::BinaryOr => {
                    let b = self.pop();
                    let a = self.pop();
                    self.push(Value::bool(a.is_truthy() || b.is_truthy()));
                }

                // ===== Unary =====
                OpCode::UnaryNeg => {
                    let a = self.pop();
                    let result = match a {
                        Value::Int(i) => Value::int(-i),
                        Value::Float(f) => Value::float(-f),
                        _ => self.runtime_error("Type mismatch in unary -"),
                    };
                    self.push(result);
                }
                OpCode::UnaryNot => {
                    let a = self.pop();
                    self.push(Value::bool(!a.is_truthy()));
                }

                // ===== Array =====
                OpCode::MakeArray => {
                    let count = inst.iarg.unwrap_or(0) as usize;
                    let mut tmp: Vec<Value> =
                        self.stack.drain(self.stack.len() - count..).collect();
                    tmp.reverse();
                    self.push(Value::Array(tmp));
                }
                OpCode::IndexGet => {
                    let idx = self.pop();
                    let obj = self.pop();
                    let result = match (&obj, &idx) {
                        (Value::Array(arr), Value::Int(i)) => {
                            if *i < 0 || (*i as usize) >= arr.len() {
                                self.runtime_error(&format!("Array index out of bounds: {}", i));
                            }
                            arr[*i as usize].clone()
                        }
                        (Value::Map(map), _) => {
                            let key = idx.to_string();
                            map.get(&key).cloned().unwrap_or(Value::nil())
                        }
                        (Value::String(s), Value::Int(i)) => {
                            if *i < 0 || (*i as usize) >= s.len() {
                                self.runtime_error("String index out of bounds");
                            }
                            Value::string(s.chars().nth(*i as usize).unwrap().to_string())
                        }
                        _ => self.runtime_error("Cannot index this type"),
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
                                    self.runtime_error("Array index out of bounds in assignment");
                                }
                                arr[i as usize] = val;
                            }
                        }
                        Value::Map(map) => {
                            let key = idx.to_string();
                            map.insert(key, val);
                        }
                        _ => self.runtime_error("Cannot index-assign this type"),
                    }
                    self.push(obj);
                }

                // ===== Map =====
                OpCode::MakeMap => {
                    let count = inst.iarg.unwrap_or(0) as usize;
                    let mut tmp: Vec<Value> =
                        self.stack.drain(self.stack.len() - count * 2..).collect();
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
                    let mut inst_val = Value::instance(name.clone());
                    if let Some(fields) = self.module.struct_defs.get(&name) {
                        if let Value::Instance {
                            fields: ref mut inst_fields,
                            ..
                        } = inst_val
                        {
                            for field in fields {
                                inst_fields.insert(field.clone(), Value::nil());
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
                            if !self.validate_pointer(&obj) {
                                Value::nil()
                            } else if let Some(rec) = self.alloc_registry.get(alloc_id) {
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
                                                Value::string(method_name)
                                            } else {
                                                self.runtime_error(&format!(
                                                    "Property not found: {}",
                                                    prop_name
                                                ));
                                            }
                                        }
                                    }
                                    _ => self.runtime_error(
                                        "Cannot access property on dereferenced pointer type",
                                    ),
                                }
                            } else {
                                Value::nil()
                            }
                        }
                        Value::Instance { fields, .. } | Value::Map(fields) => {
                            if let Some(v) = fields.get(&prop_name) {
                                v.clone()
                            } else {
                                let method_name = format!("{}_{}", obj_type_name, prop_name);
                                if self.module.function_map.contains_key(&method_name) {
                                    Value::string(method_name)
                                } else {
                                    self.runtime_error(&format!(
                                        "Property not found: {}",
                                        prop_name
                                    ));
                                }
                            }
                        }
                        Value::Array(arr) if prop_name == "length" => Value::int(arr.len() as i64),
                        Value::String(s) if prop_name == "length" => Value::int(s.len() as i64),
                        _ => self.runtime_error("Cannot access property on this type"),
                    };
                    self.push(result);
                }
                OpCode::PropertySet => {
                    let prop_name = inst.sarg.clone().unwrap_or_default();
                    let val = self.pop();
                    let mut obj = self.pop();
                    if let Value::Pointer { alloc_id, .. } = &obj {
                        let alloc_id = *alloc_id;
                        if self.validate_pointer(&obj) {
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
                        self.runtime_error("Cannot set property on this type");
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
                    self.push(self.deref_pointer(&ptr));
                }
                OpCode::PointerMember => {
                    let prop_name = inst.sarg.clone().unwrap_or_default();
                    let ptr = self.pop();
                    let result = match &ptr {
                        Value::Pointer { alloc_id, .. } => {
                            if !self.validate_pointer(&ptr) {
                                Value::nil()
                            } else if let Some(rec) = self.alloc_registry.get(alloc_id) {
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
                                                Value::string(method_name)
                                            } else {
                                                self.runtime_error(&format!(
                                                    "Pointer member not found: {}",
                                                    prop_name
                                                ));
                                            }
                                        }
                                    }
                                    _ => self.runtime_error(
                                        "Cannot access member through non-instance pointer",
                                    ),
                                }
                            } else {
                                Value::nil()
                            }
                        }
                        Value::Instance { fields, .. } | Value::Map(fields) => {
                            fields.get(&prop_name).cloned().unwrap_or(Value::nil())
                        }
                        _ => self.runtime_error("Cannot access member through non-pointer type"),
                    };
                    self.push(result);
                }

                OpCode::New => {
                    let class_name_val = self.pop();
                    if let Value::String(class_name) = class_name_val {
                        let mut inst_val = Value::instance(class_name.clone());
                        if let Some(fields) = self.module.struct_defs.get(&class_name) {
                            if let Value::Instance {
                                fields: fields_map, ..
                            } = &mut inst_val
                            {
                                for field in fields {
                                    fields_map.insert(field.clone(), Value::nil());
                                }
                            }
                        }
                        self.push(inst_val);
                    } else {
                        self.runtime_error("new: expected class name string");
                    }
                }

                // ===== HALT =====
                OpCode::Halt => {
                    self.frames.clear();
                    return Value::nil();
                }

                // ===== Import =====
                OpCode::Import => {
                    if let Some(ref sarg) = inst.sarg {
                        let parts: Vec<&str> = sarg.split(',').collect();
                        let module_name = parts.last().copied().unwrap_or("unknown");
                        if !module_name.is_empty() {
                            self.globals.insert(
                                module_name.to_string(),
                                Value::instance(module_name.to_string()),
                            );
                        }
                    }
                }

                // ===== SysArgv / System / File I/O =====
                OpCode::SysArgv => {
                    self.push(Value::Array(Vec::new()));
                }
                OpCode::System => {
                    self.pop();
                    self.push(Value::Int(0));
                }
                OpCode::FileRead => {
                    self.pop();
                    self.push(Value::nil());
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
                OpCode::NewZ => {
                    let num_args = inst.iarg.unwrap_or(0) as usize;
                    let mut args: Vec<Value> =
                        self.stack.drain(self.stack.len() - num_args..).collect();
                    args.reverse();
                    let class_name_val = self.pop();

                    if let Value::String(class_name) = class_name_val {
                        let mut inst_val = Value::instance(class_name.clone());
                        if let Some(fields) = self.module.struct_defs.get(&class_name) {
                            let mut fields_map = HashMap::new();
                            for (i, field) in fields.iter().enumerate() {
                                let val = args.get(i).cloned().unwrap_or(Value::nil());
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
                        self.push(Value::pointer(alloc_id, gen, class_name));
                    } else {
                        self.runtime_error("newz: expected class name string");
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
                        self.free_allocation(alloc_id, generation);
                    } else {
                        self.runtime_error("free: can only free heap pointers (newz allocations)");
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
                            if !self.validate_pointer(v) {
                                self.pop();
                                self.push(Value::nil());
                            }
                        }
                    }
                }

                _ => self.runtime_error(&format!("Unimplemented opcode: {:?}", inst.op)),
            }
        }

        Value::nil()
    }
}
