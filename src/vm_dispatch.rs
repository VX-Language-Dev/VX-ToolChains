use std::collections::HashMap;
use crate::opcode::OpCode;
use crate::value::Value;
use crate::vm::VM;

pub enum DispatchResult {
    Continue,
    Return(Value),
    Error(String),
}

impl VM {
    #[inline]
    pub(crate) fn exec_load_store(&mut self, op: OpCode, iarg: Option<i32>, sarg: Option<&str>) -> DispatchResult {
        match op {
            OpCode::LoadConst => {
                let idx = iarg.unwrap_or(0) as usize;
                if idx >= self.module.constants.len() {
                    return DispatchResult::Error("Constant index out of bounds".into());
                }
                self.push(self.module.constants[idx].clone());
            }
            OpCode::LoadNil => self.push(Value::Nil),
            OpCode::LoadTrue => self.push(Value::Bool(true)),
            OpCode::LoadFalse => self.push(Value::Bool(false)),
            OpCode::LoadVar => {
                let slot = iarg.unwrap_or(0) as usize;
                if slot < self.current_frame().locals.len() {
                    self.push(self.current_frame().locals[slot].clone());
                } else {
                    let name = sarg.unwrap_or("").to_string();
                    if let Some(v) = self.globals.get(&name) {
                        self.push(v.clone());
                    } else {
                        return DispatchResult::Error(format!("Undefined variable at slot {}", slot));
                    }
                }
            }
            OpCode::StoreVar => {
                let slot = iarg.unwrap_or(0) as usize;
                let v = self.pop();
                if slot < self.current_frame().locals.len() {
                    self.current_frame_mut().locals[slot] = v;
                } else {
                    let name = sarg.unwrap_or("").to_string();
                    self.globals.insert(name, v);
                }
            }
            OpCode::DefineVar => {
                let slot = iarg.unwrap_or(0) as usize;
                let v = self.pop();
                if slot >= self.current_frame().locals.len() {
                    self.current_frame_mut().locals.resize(slot + 1, Value::Nil);
                }
                self.current_frame_mut().locals[slot] = v;
            }
            OpCode::Dup => {
                if let Some(v) = self.peek(0) {
                    self.push(v.clone());
                } else {
                    return DispatchResult::Error("Stack underflow on DUP".into());
                }
            }
            OpCode::Pop => {
                self.pop();
            }
            _ => unreachable!(),
        }
        DispatchResult::Continue
    }

    #[inline]
    pub(crate) fn exec_call_return(&mut self, op: OpCode, iarg: Option<i32>) -> DispatchResult {
        match op {
            OpCode::Call => {
                let num_args = iarg.unwrap_or(0) as usize;
                if num_args > self.stack_len() {
                    return DispatchResult::Error("Not enough arguments on stack".into());
                }
                let mut args: Vec<Value> =
                    self.drain_stack(self.stack_len() - num_args..self.stack_len());
                args.reverse();
                let callee = self.pop();

                match &callee {
                    Value::String(ref s) => {
                        if let Some(&fn_idx) = self.module.function_map.get(s) {
                            if let Err(e) = self.call_user_function(fn_idx, &args) {
                                return DispatchResult::Error(e);
                            }
                        } else if let Some(f) = self.builtins.get(s) {
                            let result = f(&mut args);
                            self.push(result);
                        } else {
                            return DispatchResult::Error(format!("Unknown function: {}", s));
                        }
                    }
                    _ => return DispatchResult::Error("Callee is not callable".into()),
                }
            }
            OpCode::Return => {
                let ret = self.pop();
                let leaving_frame = self.frames.pop().unwrap();
                self.cleanup_frame_allocs(&leaving_frame.owned_allocs);
                if let Some(frame) = self.frames.last_mut() {
                    while self.stack.len() > frame.stack_base {
                        self.stack.pop();
                    }
                    self.push(ret);
                } else {
                    return DispatchResult::Return(ret);
                }
            }
            _ => unreachable!(),
        }
        DispatchResult::Continue
    }

    #[inline]
    pub(crate) fn exec_jump(&mut self, op: OpCode, iarg: Option<i32>) -> DispatchResult {
        match op {
            OpCode::Jump => {
                let target = iarg.unwrap_or(0) as usize;
                self.current_frame_mut().pc = target;
            }
            OpCode::JumpIfFalse => {
                let target = iarg.unwrap_or(0) as usize;
                let v = self.pop();
                if !v.is_truthy() {
                    self.current_frame_mut().pc = target;
                }
            }
            OpCode::JumpIfTrue => {
                let target = iarg.unwrap_or(0) as usize;
                let v = self.pop();
                if v.is_truthy() {
                    self.current_frame_mut().pc = target;
                }
            }
            _ => unreachable!(),
        }
        DispatchResult::Continue
    }

    #[inline]
    pub(crate) fn exec_binary_arith(&mut self, op: OpCode) -> DispatchResult {
        match op {
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
                    _ => return DispatchResult::Error("Type mismatch in +".into()),
                };
                self.push(result);
            }
            OpCode::BinarySub => {
                let b = self.pop();
                let a = self.pop();
                let result = match (&a, &b) {
                    (Value::Int(ai), Value::Int(bi)) => Value::Int(ai - bi),
                    (Value::Float(af), Value::Float(bf)) => Value::Float(af - bf),
                    _ => return DispatchResult::Error("Type mismatch in -".into()),
                };
                self.push(result);
            }
            OpCode::BinaryMul => {
                let b = self.pop();
                let a = self.pop();
                let result = match (&a, &b) {
                    (Value::Int(ai), Value::Int(bi)) => Value::Int(ai * bi),
                    (Value::Float(af), Value::Float(bf)) => Value::Float(af * bf),
                    _ => return DispatchResult::Error("Type mismatch in *".into()),
                };
                self.push(result);
            }
            OpCode::BinaryDiv => {
                let b = self.pop();
                let a = self.pop();
                let result = match (&a, &b) {
                    (Value::Int(ai), Value::Int(bi)) if *bi != 0 => Value::Int(ai / bi),
                    (Value::Float(af), Value::Float(bf)) if *bf != 0.0 => Value::Float(af / bf),
                    _ => return DispatchResult::Error("Type mismatch in /".into()),
                };
                self.push(result);
            }
            OpCode::BinaryMod => {
                let b = self.pop();
                let a = self.pop();
                let result = match (&a, &b) {
                    (Value::Int(ai), Value::Int(bi)) if *bi != 0 => Value::Int(ai % bi),
                    _ => return DispatchResult::Error("Type mismatch in %".into()),
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
                    _ => return DispatchResult::Error("Type mismatch in ^".into()),
                };
                self.push(result);
            }
            _ => unreachable!(),
        }
        DispatchResult::Continue
    }

    #[inline]
    pub(crate) fn exec_binary_cmp(&mut self, op: OpCode) -> DispatchResult {
        match op {
            OpCode::BinaryEq => {
                let b = self.pop();
                let a = self.pop();
                self.push(Value::Bool(a == b));
            }
            OpCode::BinaryNe => {
                let b = self.pop();
                let a = self.pop();
                self.push(Value::Bool(a != b));
            }
            OpCode::BinaryLt => {
                let b = self.pop();
                let a = self.pop();
                let result = match (&a, &b) {
                    (Value::Int(ai), Value::Int(bi)) => Value::Bool(ai < bi),
                    (Value::Float(af), Value::Float(bf)) => Value::Bool(af < bf),
                    _ => return DispatchResult::Error("Type mismatch in <".into()),
                };
                self.push(result);
            }
            OpCode::BinaryGt => {
                let b = self.pop();
                let a = self.pop();
                let result = match (&a, &b) {
                    (Value::Int(ai), Value::Int(bi)) => Value::Bool(ai > bi),
                    (Value::Float(af), Value::Float(bf)) => Value::Bool(af > bf),
                    _ => return DispatchResult::Error("Type mismatch in >".into()),
                };
                self.push(result);
            }
            OpCode::BinaryLe => {
                let b = self.pop();
                let a = self.pop();
                let result = match (&a, &b) {
                    (Value::Int(ai), Value::Int(bi)) => Value::Bool(ai <= bi),
                    (Value::Float(af), Value::Float(bf)) => Value::Bool(af <= bf),
                    _ => return DispatchResult::Error("Type mismatch in <=".into()),
                };
                self.push(result);
            }
            OpCode::BinaryGe => {
                let b = self.pop();
                let a = self.pop();
                let result = match (&a, &b) {
                    (Value::Int(ai), Value::Int(bi)) => Value::Bool(ai >= bi),
                    (Value::Float(af), Value::Float(bf)) => Value::Bool(af >= bf),
                    _ => return DispatchResult::Error("Type mismatch in >=".into()),
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
            _ => unreachable!(),
        }
        DispatchResult::Continue
    }

    #[inline]
    pub(crate) fn exec_specialized_arith(&mut self, op: OpCode) -> DispatchResult {
        match op {
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
                self.push_int(a - b);
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
                    return DispatchResult::Error("Division by zero".into());
                }
            }
            OpCode::DivFloat => {
                let b = self.pop_float();
                let a = self.pop_float();
                if b != 0.0 {
                    self.push_float(a / b);
                } else {
                    return DispatchResult::Error("Division by zero".into());
                }
            }
            OpCode::ModInt => {
                let b = self.pop_int();
                let a = self.pop_int();
                if b != 0 {
                    self.push_int(a % b);
                } else {
                    return DispatchResult::Error("Modulo by zero".into());
                }
            }
            _ => unreachable!(),
        }
        DispatchResult::Continue
    }

    #[inline]
    pub(crate) fn exec_specialized_cmp(&mut self, op: OpCode) -> DispatchResult {
        match op {
            OpCode::EqInt => { let b = self.pop_int(); let a = self.pop_int(); self.push_bool(a == b); }
            OpCode::EqFloat => { let b = self.pop_float(); let a = self.pop_float(); self.push_bool(a == b); }
            OpCode::LtInt => { let b = self.pop_int(); let a = self.pop_int(); self.push_bool(a < b); }
            OpCode::LtFloat => { let b = self.pop_float(); let a = self.pop_float(); self.push_bool(a < b); }
            OpCode::GtInt => { let b = self.pop_int(); let a = self.pop_int(); self.push_bool(a > b); }
            OpCode::GtFloat => { let b = self.pop_float(); let a = self.pop_float(); self.push_bool(a > b); }
            OpCode::LeInt => { let b = self.pop_int(); let a = self.pop_int(); self.push_bool(a <= b); }
            OpCode::LeFloat => { let b = self.pop_float(); let a = self.pop_float(); self.push_bool(a <= b); }
            OpCode::GeInt => { let b = self.pop_int(); let a = self.pop_int(); self.push_bool(a >= b); }
            OpCode::GeFloat => { let b = self.pop_float(); let a = self.pop_float(); self.push_bool(a >= b); }
            _ => unreachable!(),
        }
        DispatchResult::Continue
    }

    #[inline]
    pub(crate) fn exec_specialized_logic_unary(&mut self, op: OpCode) -> DispatchResult {
        match op {
            OpCode::And => { let b = self.pop_bool(); let a = self.pop_bool(); self.push_bool(a && b); }
            OpCode::Or => { let b = self.pop_bool(); let a = self.pop_bool(); self.push_bool(a || b); }
            OpCode::NegInt => { let a = self.pop_int(); self.push_int(-a); }
            OpCode::NegFloat => { let a = self.pop_float(); self.push_float(-a); }
            OpCode::Not => { let a = self.pop_bool(); self.push_bool(!a); }
            OpCode::UnaryNeg => {
                let a = self.pop();
                let result = match a {
                    Value::Int(i) => Value::Int(-i),
                    Value::Float(f) => Value::Float(-f),
                    _ => return DispatchResult::Error("Type mismatch in unary -".into()),
                };
                self.push(result);
            }
            OpCode::UnaryNot => {
                let a = self.pop();
                self.push(Value::Bool(!a.is_truthy()));
            }
            _ => unreachable!(),
        }
        DispatchResult::Continue
    }

    #[inline]
    pub(crate) fn exec_collection(&mut self, op: OpCode, iarg: Option<i32>, sarg: Option<&str>) -> DispatchResult {
        match op {
            OpCode::MakeArray => {
                let count = iarg.unwrap_or(0) as usize;
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
                            return DispatchResult::Error("Array index out of bounds".into());
                        }
                        arr[*i as usize].clone()
                    }
                    (Value::Map(map), _) => {
                        let key = idx.to_string();
                        map.get(&key).cloned().unwrap_or(Value::Nil)
                    }
                    (Value::String(s), Value::Int(i)) => {
                        if *i < 0 || (*i as usize) >= s.len() {
                            return DispatchResult::Error("String index out of bounds".into());
                        }
                        Value::String(s.chars().nth(*i as usize).unwrap().to_string())
                    }
                    _ => return DispatchResult::Error("Cannot index this type".into()),
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
                                return DispatchResult::Error("Array index out of bounds in assignment".into());
                            }
                            arr[i as usize] = val;
                        }
                    }
                    Value::Map(map) => {
                        let key = idx.to_string();
                        map.insert(key, val);
                    }
                    _ => return DispatchResult::Error("Cannot index-assign this type".into()),
                }
                self.push(obj);
            }
            OpCode::MakeMap => {
                let count = iarg.unwrap_or(0) as usize;
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
            OpCode::MakeStruct | OpCode::MakeClass => {
                let name = sarg.unwrap_or("").to_string();
                let mut inst_val = Value::Instance { class_name: name.clone(), fields: HashMap::new() };
                if let Some(fields) = self.module.struct_defs.get(&name) {
                    if let Value::Instance { fields: ref mut inst_fields, .. } = inst_val {
                        for field in fields {
                            inst_fields.insert(field.clone(), Value::Nil);
                        }
                    }
                }
                self.push(inst_val);
            }
            _ => unreachable!(),
        }
        DispatchResult::Continue
    }

    #[inline]
    pub(crate) fn exec_property(&mut self, op: OpCode, sarg: Option<&str>) -> DispatchResult {
        match op {
            OpCode::PropertyGet => {
                let prop_name = sarg.unwrap_or("").to_string();
                let obj = self.pop();
                let obj_type_name = obj.type_name();
                let result = match &obj {
                    Value::Pointer { alloc_id, .. } => {
                        if let Err(e) = self.validate_pointer(&obj) {
                            return DispatchResult::Error(e);
                        }
                        if let Some(rec) = self.alloc_registry.get(alloc_id) {
                            match &rec.instance {
                                Value::Instance { fields, .. } | Value::Map(fields) => {
                                    if let Some(v) = fields.get(&prop_name) {
                                        v.clone()
                                    } else {
                                        let method_name = format!("{}_{}", rec.instance.type_name(), prop_name);
                                        if self.module.function_map.contains_key(&method_name) {
                                            Value::String(method_name)
                                        } else {
                                            return DispatchResult::Error(format!("Property not found: {}", prop_name));
                                        }
                                    }
                                }
                                _ => return DispatchResult::Error("Cannot access property on dereferenced pointer type".into()),
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
                                return DispatchResult::Error(format!("Property not found: {}", prop_name));
                            }
                        }
                    }
                    Value::Array(arr) if prop_name == "length" => Value::Int(arr.len() as i64),
                    Value::String(s) if prop_name == "length" => Value::Int(s.len() as i64),
                    _ => return DispatchResult::Error("Cannot access property on this type".into()),
                };
                self.push(result);
            }
            OpCode::PropertySet => {
                let prop_name = sarg.unwrap_or("").to_string();
                let val = self.pop();
                let mut obj = self.pop();
                if let Value::Pointer { alloc_id, .. } = &obj {
                    let alloc_id = *alloc_id;
                    if let Ok(true) = self.validate_pointer(&obj) {
                        if let Some(rec) = self.alloc_registry.get_mut(&alloc_id) {
                            if let Value::Instance { fields, .. } | Value::Map(fields) = &mut rec.instance {
                                fields.insert(prop_name, val);
                            }
                        }
                    }
                } else if let Value::Instance { fields, .. } | Value::Map(fields) = &mut obj {
                    fields.insert(prop_name, val);
                } else {
                    return DispatchResult::Error("Cannot set property on this type".into());
                }
                self.push(obj);
            }
            _ => unreachable!(),
        }
        DispatchResult::Continue
    }

    #[inline]
    pub(crate) fn exec_pointer(&mut self, op: OpCode, sarg: Option<&str>) -> DispatchResult {
        match op {
            OpCode::AddressOf => {
                let v = self.pop();
                self.push(v);
            }
            OpCode::Deref => {
                let ptr = self.pop();
                match self.deref_pointer(&ptr) {
                    Ok(v) => self.push(v),
                    Err(e) => return DispatchResult::Error(e),
                }
            }
            OpCode::PointerMember => {
                let prop_name = sarg.unwrap_or("").to_string();
                let ptr = self.pop();
                let result = match &ptr {
                    Value::Pointer { alloc_id, .. } => {
                        if let Err(e) = self.validate_pointer(&ptr) {
                            return DispatchResult::Error(e);
                        }
                        if let Some(rec) = self.alloc_registry.get(alloc_id) {
                            match &rec.instance {
                                Value::Instance { fields, .. } | Value::Map(fields) => {
                                    if let Some(v) = fields.get(&prop_name) {
                                        v.clone()
                                    } else {
                                        let method_name = format!("{}_{}", rec.instance.type_name(), prop_name);
                                        if self.module.function_map.contains_key(&method_name) {
                                            Value::String(method_name)
                                        } else {
                                            return DispatchResult::Error(format!("Pointer member not found: {}", prop_name));
                                        }
                                    }
                                }
                                _ => return DispatchResult::Error("Cannot access member through non-instance pointer".into()),
                            }
                        } else {
                            Value::Nil
                        }
                    }
                    Value::Instance { fields, .. } | Value::Map(fields) => {
                        fields.get(&prop_name).cloned().unwrap_or(Value::Nil)
                    }
                    _ => return DispatchResult::Error("Cannot access member through non-pointer type".into()),
                };
                self.push(result);
            }
            _ => unreachable!(),
        }
        DispatchResult::Continue
    }

    #[inline]
    pub(crate) fn exec_new(&mut self, op: OpCode, iarg: Option<i32>) -> DispatchResult {
        match op {
            OpCode::New => {
                let class_name_val = self.pop();
                if let Value::String(class_name) = class_name_val {
                    let mut inst_val = Value::Instance { class_name: class_name.clone(), fields: HashMap::new() };
                    if let Some(fields) = self.module.struct_defs.get(&class_name) {
                        if let Value::Instance { fields: fields_map, .. } = &mut inst_val {
                            for field in fields {
                                fields_map.insert(field.clone(), Value::Nil);
                            }
                        }
                    }
                    self.push(inst_val);
                } else {
                    return DispatchResult::Error("new: expected class name string".into());
                }
            }
            OpCode::Newz => {
                let num_args = iarg.unwrap_or(0) as usize;
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
                    self.push(Value::Pointer { alloc_id, generation: gen, class_name });
                } else {
                    return DispatchResult::Error("newz: expected class name string".into());
                }
            }
            _ => unreachable!(),
        }
        DispatchResult::Continue
    }

    #[inline]
    pub(crate) fn exec_memory_safety(&mut self, op: OpCode) -> DispatchResult {
        match op {
            OpCode::Free => {
                let ptr = self.pop();
                if let Value::Pointer { alloc_id, generation, .. } = ptr {
                    if let Err(e) = self.free_allocation(alloc_id, generation) {
                        return DispatchResult::Error(e);
                    }
                } else {
                    return DispatchResult::Error("free: can only free heap pointers (newz allocations)".into());
                }
            }
            OpCode::OwnershipMove => {
                if let Some(v) = self.peek(0) {
                    if let Value::String(ref name) = v {
                        self.moved_vars.insert(name.clone());
                    }
                }
            }
            OpCode::ScopeDrop => {
                let owned = self.frames.last().map(|f| f.owned_allocs.clone()).unwrap_or_default();
                self.cleanup_frame_allocs(&owned);
            }
            OpCode::BorrowCheck => {}
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
            _ => unreachable!(),
        }
        DispatchResult::Continue
    }

    #[inline]
    pub(crate) fn exec_syscall(&mut self, op: OpCode, sarg: Option<&str>) -> DispatchResult {
        match op {
            OpCode::Halt => {
                self.frames.clear();
                return DispatchResult::Return(Value::Nil);
            }
            OpCode::Import => {
                if let Some(sarg_val) = sarg {
                    let parts: Vec<&str> = sarg_val.split(',').collect();
                    let module_name = parts.last().copied().unwrap_or("unknown");
                    if !module_name.is_empty() {
                        self.globals.insert(
                            module_name.to_string(),
                            Value::Instance { class_name: module_name.to_string(), fields: HashMap::new() },
                        );
                    }
                }
            }
            OpCode::SysArgv => {
                let args: Vec<Value> = self.argv.iter()
                    .map(|s| Value::String(s.clone()))
                    .collect();
                self.push(Value::Array(args));
            }
            OpCode::System => {
                let cmd_arg = self.pop();
                match cmd_arg {
                    Value::String(ref cmd) => {
                        match self.run_shell_command(cmd) {
                            Ok(s) => {
                                self.push(Value::Int(s.code().unwrap_or(-1) as i64));
                            }
                            Err(_) => {
                                self.push(Value::Int(-1));
                            }
                        }
                    }
                    _ => {
                        return DispatchResult::Error("os_system 参数必须为字符串类型".into());
                    }
                }
            }
            OpCode::FileRead => {
                let path_val = self.pop();
                match path_val {
                    Value::String(ref path) => {
                        match std::fs::read_to_string(path) {
                            Ok(content) => self.push(Value::String(content)),
                            Err(e) => {
                                return DispatchResult::Error(format!("file_read failed: {}", e));
                            }
                        }
                    }
                    _ => return DispatchResult::Error("file_read: parameter must be a string path".into()),
                }
            }
            OpCode::FileWrite => {
                let content_val = self.pop();
                let path_val = self.pop();
                match (path_val, content_val) {
                    (Value::String(ref path), Value::String(ref content)) => {
                        match std::fs::write(path, content) {
                            Ok(_) => self.push(Value::Bool(true)),
                            Err(e) => {
                                return DispatchResult::Error(format!("file_write failed: {}", e));
                            }
                        }
                    }
                    (Value::String(ref path), other) => {
                        match std::fs::write(path, other.to_string()) {
                            Ok(_) => self.push(Value::Bool(true)),
                            Err(e) => {
                                return DispatchResult::Error(format!("file_write failed: {}", e));
                            }
                        }
                    }
                    _ => return DispatchResult::Error("file_write: parameters must be (string path, string content)".into()),
                }
            }
            OpCode::FileExists => {
                let path_val = self.pop();
                match path_val {
                    Value::String(ref path) => {
                        self.push(Value::Bool(std::path::Path::new(path).exists()));
                    }
                    _ => return DispatchResult::Error("file_exists: parameter must be a string path".into()),
                }
            }
            _ => unreachable!(),
        }
        DispatchResult::Continue
    }

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