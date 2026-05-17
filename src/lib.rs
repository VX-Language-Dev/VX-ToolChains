use std::collections::HashMap;

// ==================== OpCode ====================

#[derive(Clone, Copy, Debug, PartialEq)]
#[repr(u8)]
pub enum OpCode {
    LoadConst = 0x01,
    LoadNil = 0x02,
    LoadTrue = 0x03,
    LoadFalse = 0x04,
    LoadVar = 0x05,
    StoreVar = 0x06,
    DefineVar = 0x07,
    Call = 0x08,
    Return = 0x09,
    MakeFunction = 0x0A,
    Jump = 0x0B,
    JumpIfFalse = 0x0C,
    JumpIfTrue = 0x0D,
    Break = 0x0E,
    Continue = 0x0F,
    BinaryAdd = 0x10,
    BinarySub = 0x11,
    BinaryMul = 0x12,
    BinaryDiv = 0x13,
    BinaryMod = 0x14,
    BinaryPow = 0x15,
    BinaryEq = 0x16,
    BinaryNe = 0x17,
    BinaryLt = 0x18,
    BinaryGt = 0x19,
    BinaryLe = 0x1A,
    BinaryGe = 0x1B,
    BinaryAnd = 0x1C,
    BinaryOr = 0x1D,
    UnaryNeg = 0x1E,
    UnaryNot = 0x1F,
    MakeStruct = 0x20,
    MakeClass = 0x21,
    MakeEnum = 0x22,
    MakeUnion = 0x23,
    LoadField = 0x24,
    StoreField = 0x25,
    MakeArray = 0x26,
    MakeMap = 0x27,
    IndexGet = 0x28,
    IndexSet = 0x29,
    PropertyGet = 0x2A,
    PropertySet = 0x2B,
    AddressOf = 0x2C,
    Deref = 0x2D,
    PointerMember = 0x2E,
    Import = 0x2F,
    New = 0x30,
    Halt = 0x31,
    SysArgv = 0x32,
    System = 0x33,
    FileRead = 0x34,
    FileWrite = 0x35,
    FileExists = 0x36,
    Dup = 0x37,
    Pop = 0x38,
    // Memory Safety / Ownership
    NewZ = 0x39,
    Free = 0x3A,
    OwnershipMove = 0x3B,
    ScopeDrop = 0x3C,
    BorrowCheck = 0x3D,
    AliveCheck = 0x3E,
}

impl TryFrom<u8> for OpCode {
    type Error = String;
    fn try_from(v: u8) -> Result<Self, Self::Error> {
        match v {
            0x01 => Ok(OpCode::LoadConst),
            0x02 => Ok(OpCode::LoadNil),
            0x03 => Ok(OpCode::LoadTrue),
            0x04 => Ok(OpCode::LoadFalse),
            0x05 => Ok(OpCode::LoadVar),
            0x06 => Ok(OpCode::StoreVar),
            0x07 => Ok(OpCode::DefineVar),
            0x08 => Ok(OpCode::Call),
            0x09 => Ok(OpCode::Return),
            0x0A => Ok(OpCode::MakeFunction),
            0x0B => Ok(OpCode::Jump),
            0x0C => Ok(OpCode::JumpIfFalse),
            0x0D => Ok(OpCode::JumpIfTrue),
            0x0E => Ok(OpCode::Break),
            0x0F => Ok(OpCode::Continue),
            0x10 => Ok(OpCode::BinaryAdd),
            0x11 => Ok(OpCode::BinarySub),
            0x12 => Ok(OpCode::BinaryMul),
            0x13 => Ok(OpCode::BinaryDiv),
            0x14 => Ok(OpCode::BinaryMod),
            0x15 => Ok(OpCode::BinaryPow),
            0x16 => Ok(OpCode::BinaryEq),
            0x17 => Ok(OpCode::BinaryNe),
            0x18 => Ok(OpCode::BinaryLt),
            0x19 => Ok(OpCode::BinaryGt),
            0x1A => Ok(OpCode::BinaryLe),
            0x1B => Ok(OpCode::BinaryGe),
            0x1C => Ok(OpCode::BinaryAnd),
            0x1D => Ok(OpCode::BinaryOr),
            0x1E => Ok(OpCode::UnaryNeg),
            0x1F => Ok(OpCode::UnaryNot),
            0x20 => Ok(OpCode::MakeStruct),
            0x21 => Ok(OpCode::MakeClass),
            0x22 => Ok(OpCode::MakeEnum),
            0x23 => Ok(OpCode::MakeUnion),
            0x24 => Ok(OpCode::LoadField),
            0x25 => Ok(OpCode::StoreField),
            0x26 => Ok(OpCode::MakeArray),
            0x27 => Ok(OpCode::MakeMap),
            0x28 => Ok(OpCode::IndexGet),
            0x29 => Ok(OpCode::IndexSet),
            0x2A => Ok(OpCode::PropertyGet),
            0x2B => Ok(OpCode::PropertySet),
            0x2C => Ok(OpCode::AddressOf),
            0x2D => Ok(OpCode::Deref),
            0x2E => Ok(OpCode::PointerMember),
            0x2F => Ok(OpCode::Import),
            0x30 => Ok(OpCode::New),
            0x31 => Ok(OpCode::Halt),
            0x32 => Ok(OpCode::SysArgv),
            0x33 => Ok(OpCode::System),
            0x34 => Ok(OpCode::FileRead),
            0x35 => Ok(OpCode::FileWrite),
            0x36 => Ok(OpCode::FileExists),
            0x37 => Ok(OpCode::Dup),
            0x38 => Ok(OpCode::Pop),
            0x39 => Ok(OpCode::NewZ),
            0x3A => Ok(OpCode::Free),
            0x3B => Ok(OpCode::OwnershipMove),
            0x3C => Ok(OpCode::ScopeDrop),
            0x3D => Ok(OpCode::BorrowCheck),
            0x3E => Ok(OpCode::AliveCheck),
            _ => Err(format!("Unknown opcode: 0x{:02X}", v)),
        }
    }
}

// ==================== Value ====================

#[derive(Clone, Debug, PartialEq)]
pub enum Value {
    Nil,
    Int(i64),
    Float(f64),
    Bool(bool),
    String(String),
    Array(Vec<Value>),
    Map(HashMap<String, Value>),
    Instance {
        class_name: String,
        fields: HashMap<String, Value>,
    },
    Pointer {
        alloc_id: u64,
        generation: u32,
        class_name: String,
    },
}

impl Value {
    fn type_name(&self) -> String {
        match self {
            Value::Nil => "Nil".to_string(),
            Value::Int(_) => "Int".to_string(),
            Value::Float(_) => "Float".to_string(),
            Value::Bool(_) => "Bool".to_string(),
            Value::String(_) => "String".to_string(),
            Value::Array(_) => "Array".to_string(),
            Value::Map(_) => "Map".to_string(),
            Value::Instance { class_name, .. } => class_name.clone(),
            Value::Pointer { class_name, .. } => class_name.clone(),
        }
    }

    pub fn nil() -> Self {
        Value::Nil
    }
    pub fn int(i: i64) -> Self {
        Value::Int(i)
    }
    pub fn float(f: f64) -> Self {
        Value::Float(f)
    }
    pub fn bool(b: bool) -> Self {
        Value::Bool(b)
    }
    pub fn string(s: String) -> Self {
        Value::String(s)
    }
    pub fn array() -> Self {
        Value::Array(Vec::new())
    }
    pub fn map() -> Self {
        Value::Map(HashMap::new())
    }
    pub fn instance(class_name: String) -> Self {
        Value::Instance {
            class_name,
            fields: HashMap::new(),
        }
    }
    pub fn pointer(alloc_id: u64, generation: u32, class_name: String) -> Self {
        Value::Pointer {
            alloc_id,
            generation,
            class_name,
        }
    }

    pub fn is_truthy(&self) -> bool {
        match self {
            Value::Nil => false,
            Value::Bool(b) => *b,
            Value::Int(i) => *i != 0,
            Value::Float(f) => *f != 0.0,
            Value::String(s) => !s.is_empty(),
            Value::Array(arr) => !arr.is_empty(),
            Value::Map(map) => !map.is_empty(),
            Value::Instance { .. } => true,
            Value::Pointer { alloc_id, .. } => *alloc_id != 0,
        }
    }

    pub fn length(&self) -> usize {
        match self {
            Value::String(s) => s.len(),
            Value::Array(arr) => arr.len(),
            Value::Map(map) => map.len(),
            _ => 0,
        }
    }

    pub fn to_string(&self) -> String {
        match self {
            Value::Nil => "nil".to_string(),
            Value::Int(i) => i.to_string(),
            Value::Float(f) => {
                let s = f.to_string();
                if let Some(_dot_pos) = s.find('.') {
                    let last_nonzero = s.trim_end_matches('0').trim_end_matches('.').len();
                    s[..last_nonzero].to_string()
                } else {
                    s
                }
            }
            Value::Bool(b) => {
                if *b {
                    "true".to_string()
                } else {
                    "false".to_string()
                }
            }
            Value::String(s) => s.clone(),
            Value::Array(arr) => {
                let items: Vec<String> = arr.iter().map(|v| v.to_string()).collect();
                format!("[{}]", items.join(", "))
            }
            Value::Map(map) => {
                let items: Vec<String> = map
                    .iter()
                    .map(|(k, v)| format!("{}: {}", k, v.to_string()))
                    .collect();
                format!("{{{}}}", items.join(", "))
            }
            Value::Instance { class_name, fields } => {
                let items: Vec<String> = fields
                    .iter()
                    .map(|(k, v)| format!("{}={}", k, v.to_string()))
                    .collect();
                format!("{}({})", class_name, items.join(", "))
            }
            Value::Pointer {
                class_name,
                alloc_id,
                generation,
            } => {
                format!("{}*({}:{})", class_name, alloc_id, generation)
            }
        }
    }
}

// ==================== Instruction ====================

#[derive(Clone, Debug)]
pub struct Instruction {
    pub op: OpCode,
    pub iarg: Option<i32>,
    pub sarg: Option<String>,
}

impl Instruction {
    pub fn new(op: OpCode) -> Self {
        Self {
            op,
            iarg: None,
            sarg: None,
        }
    }
    pub fn with_iarg(op: OpCode, arg: i32) -> Self {
        Self {
            op,
            iarg: Some(arg),
            sarg: None,
        }
    }
    pub fn with_sarg(op: OpCode, arg: String) -> Self {
        Self {
            op,
            iarg: None,
            sarg: Some(arg),
        }
    }
}

// ==================== Function & Module ====================

#[derive(Clone, Debug)]
pub struct Function {
    pub name: String,
    pub instructions: Vec<Instruction>,
    pub constants: Vec<Value>,
    pub num_params: u32,
    pub has_return: bool,
    pub param_names: Vec<String>,
}

#[derive(Clone, Debug)]
pub struct Module {
    pub constants: Vec<Value>,
    pub functions: Vec<Function>,
    pub function_map: HashMap<String, usize>,
    pub struct_defs: HashMap<String, Vec<String>>,
}

// ==================== CallFrame & AllocRecord ====================

#[derive(Clone, Debug)]
pub struct CallFrame {
    pub fn_idx: usize,
    pub pc: usize,
    pub stack_base: usize,
    pub locals: HashMap<String, Value>,
    pub owned_allocs: Vec<u64>,
}

#[derive(Clone, Debug)]
pub struct AllocRecord {
    pub id: u64,
    pub generation: u32,
    pub alive: bool,
    pub class_name: String,
    pub instance: Value,
}

// ==================== VM ====================

pub struct VM {
    module: Module,
    frames: Vec<CallFrame>,
    stack: Vec<Value>,
    globals: HashMap<String, Value>,
    builtins: HashMap<String, fn(&mut [Value]) -> Value>,
    alloc_registry: HashMap<u64, AllocRecord>,
    next_alloc_id: u64,
}

impl VM {
    pub fn new() -> Self {
        let mut vm = VM {
            module: Module {
                constants: Vec::new(),
                functions: Vec::new(),
                function_map: HashMap::new(),
                struct_defs: HashMap::new(),
            },
            frames: Vec::new(),
            stack: Vec::new(),
            globals: HashMap::new(),
            builtins: HashMap::new(),
            alloc_registry: HashMap::new(),
            next_alloc_id: 1,
        };

        // Built-in functions
        vm.define_builtin("out", |args| {
            if args.is_empty() {
                println!();
            } else {
                print!("{}", args[0].to_string());
                for i in 1..args.len() {
                    print!(" {}", args[i].to_string());
                }
                println!();
            }
            Value::nil()
        });

        vm.define_builtin("len", |args| {
            if args.is_empty() {
                return Value::int(0);
            }
            Value::int(args[0].length() as i64)
        });

        vm.define_builtin("str", |args| {
            if args.is_empty() {
                return Value::string("nil".to_string());
            }
            Value::string(args[0].to_string())
        });

        vm.define_builtin("int", |args| {
            if args.is_empty() {
                return Value::int(0);
            }
            match &args[0] {
                Value::Int(i) => Value::int(*i),
                Value::Float(f) => Value::int(*f as i64),
                Value::Bool(b) => Value::int(if *b { 1 } else { 0 }),
                Value::String(s) => s.parse::<i64>().map(Value::int).unwrap_or(Value::int(0)),
                _ => Value::int(0),
            }
        });

        vm.define_builtin("push", |args| {
            if args.len() >= 2 {
                let val = args[1].clone();
                if let Value::Array(ref mut arr) = args[0] {
                    arr.push(val);
                }
            }
            Value::nil()
        });

        vm
    }

    fn define_builtin(&mut self, name: &str, f: fn(&mut [Value]) -> Value) {
        self.builtins.insert(name.to_string(), f);
        self.globals
            .insert(name.to_string(), Value::string(name.to_string()));
    }

    pub fn load_module(&mut self, bytecode: &[u8]) -> Result<bool, String> {
        if bytecode.len() < 9 || &bytecode[0..5] != b"VXOBJ" {
            return Err("Invalid magic".to_string());
        }
        let mut pos = 5;

        let version = read_u32(bytecode, &mut pos);
        if version != 2 {
            return Err(format!("Unsupported version: {}", version));
        }

        // Read constants
        let num_globals = read_u32(bytecode, &mut pos);
        self.module.constants.reserve(num_globals as usize);
        for _ in 0..num_globals {
            self.module
                .constants
                .push(read_constant(bytecode, &mut pos)?);
        }

        // Read functions
        let num_functions = read_u32(bytecode, &mut pos);
        self.module.functions.reserve(num_functions as usize);
        for _ in 0..num_functions {
            let name = read_string(bytecode, &mut pos);
            let num_params = read_u32(bytecode, &mut pos);
            let has_return = bytecode[pos] != 0;
            pos += 1;

            let num_params_names = read_u32(bytecode, &mut pos);
            let mut param_names = Vec::with_capacity(num_params_names as usize);
            for _ in 0..num_params_names {
                param_names.push(read_string(bytecode, &mut pos));
            }

            let num_local_consts = read_u32(bytecode, &mut pos);
            let mut constants = Vec::with_capacity(num_local_consts as usize);
            for _ in 0..num_local_consts {
                constants.push(read_constant(bytecode, &mut pos)?);
            }

            let num_insts = read_u32(bytecode, &mut pos);
            let mut instructions = Vec::with_capacity(num_insts as usize);
            for _ in 0..num_insts {
                let op = OpCode::try_from(bytecode[pos]).map_err(|e| e)?;
                pos += 1;
                let arg_type = bytecode[pos];
                pos += 1;

                let inst = if arg_type == 0 {
                    Instruction::new(op)
                } else if arg_type == 1 {
                    Instruction::with_iarg(op, read_i32(bytecode, &mut pos))
                } else if arg_type == 2 {
                    Instruction::with_sarg(op, read_string(bytecode, &mut pos))
                } else {
                    return Err("Unknown arg type".to_string());
                };
                instructions.push(inst);
            }

            let _fn_idx = self.module.functions.len();
            self.module.functions.push(Function {
                name,
                instructions,
                constants,
                num_params,
                has_return,
                param_names,
            });
        }

        // Struct definitions (optional)
        if pos < bytecode.len() {
            let num_structs = read_u32(bytecode, &mut pos);
            for _ in 0..num_structs {
                let name = read_string(bytecode, &mut pos);
                let num_fields = read_u32(bytecode, &mut pos);
                let mut fields = Vec::with_capacity(num_fields as usize);
                for _ in 0..num_fields {
                    fields.push(read_string(bytecode, &mut pos));
                }
                self.module.struct_defs.insert(name, fields);
            }
        }

        // Build function map
        for (i, func) in self.module.functions.iter().enumerate() {
            self.module.function_map.insert(func.name.clone(), i);
        }

        Ok(true)
    }

    fn push(&mut self, v: Value) {
        self.stack.push(v);
    }
    fn pop(&mut self) -> Value {
        self.stack.pop().unwrap_or(Value::nil())
    }
    fn peek(&self, offset: usize) -> Option<&Value> {
        if self.stack.len() > offset {
            Some(&self.stack[self.stack.len() - 1 - offset])
        } else {
            None
        }
    }
    fn current_frame(&self) -> &CallFrame {
        &self.frames[self.frames.len() - 1]
    }
    fn current_frame_mut(&mut self) -> &mut CallFrame {
        let idx = self.frames.len() - 1;
        &mut self.frames[idx]
    }
    fn current_fn(&self) -> &Function {
        &self.module.functions[self.current_frame().fn_idx]
    }

    fn runtime_error(&self, msg: &str) -> ! {
        eprintln!("[Runtime Error] {}", msg);
        panic!("{}", msg);
    }

    // ==================== 内存安全运行时 ====================

    fn alloc_heap(&mut self, class_name: String, instance: Value) -> u64 {
        let id = self.next_alloc_id;
        self.next_alloc_id += 1;
        self.alloc_registry.insert(
            id,
            AllocRecord {
                id,
                generation: 0,
                alive: true,
                class_name,
                instance,
            },
        );
        if !self.frames.is_empty() {
            self.current_frame_mut().owned_allocs.push(id);
        }
        id
    }

    fn validate_pointer(&self, ptr: &Value) -> bool {
        let (alloc_id, generation) = match ptr {
            Value::Pointer {
                alloc_id,
                generation,
                ..
            } => (*alloc_id, *generation),
            _ => {
                self.runtime_error("Expected a pointer for dereference/free operation");
            }
        };

        if let Some(rec) = self.alloc_registry.get(&alloc_id) {
            if !rec.alive {
                self.runtime_error(&format!(
                    "Dangling pointer: allocation {} has been freed (use-after-free)",
                    alloc_id
                ));
            }
            if rec.generation != generation {
                self.runtime_error(&format!(
                    "Stale pointer: generation mismatch for allocation {} (expected gen {}, got {})",
                    alloc_id, rec.generation, generation
                ));
            }
            true
        } else {
            self.runtime_error(&format!(
                "Dangling pointer: allocation {} does not exist (use-after-free)",
                alloc_id
            ));
        }
    }

    fn deref_pointer(&self, ptr: &Value) -> Value {
        if !self.validate_pointer(ptr) {
            return Value::nil();
        }
        if let Value::Pointer { alloc_id, .. } = ptr {
            self.alloc_registry
                .get(alloc_id)
                .map(|r| r.instance.clone())
                .unwrap_or(Value::nil())
        } else {
            Value::nil()
        }
    }

    fn free_allocation(&mut self, alloc_id: u64, generation: u32) {
        if let Some(rec) = self.alloc_registry.get(&alloc_id) {
            if !rec.alive {
                self.runtime_error(&format!(
                    "Double-free: allocation {} has already been freed",
                    alloc_id
                ));
            }
            if rec.generation != generation {
                self.runtime_error(&format!(
                    "Double-free: generation mismatch for allocation {}",
                    alloc_id
                ));
            }
            // 执行释放：递增代际，标记为非存活
            if let Some(rec) = self.alloc_registry.get_mut(&alloc_id) {
                rec.generation += 1;
                rec.alive = false;
            }
            // 从当前帧的所有权列表中移除
            if !self.frames.is_empty() {
                let owned = &mut self.current_frame_mut().owned_allocs;
                owned.retain(|&id| id != alloc_id);
            }
        } else {
            self.runtime_error(&format!(
                "Double-free: allocation {} does not exist",
                alloc_id
            ));
        }
    }

    fn cleanup_frame_allocs(&mut self, frame: &CallFrame) {
        for alloc_id in &frame.owned_allocs {
            if let Some(rec) = self.alloc_registry.get_mut(alloc_id) {
                rec.generation += 1;
                rec.alive = false;
            }
        }
    }

    fn call_user_function(&mut self, fn_idx: usize, args: &[Value]) -> Result<(), String> {
        let fun = &self.module.functions[fn_idx];
        if args.len() != fun.num_params as usize {
            return Err(format!(
                "Argument count mismatch for {}: expected {}, got {}",
                fun.name,
                fun.num_params,
                args.len()
            ));
        }
        let mut frame = CallFrame {
            fn_idx,
            pc: 0,
            stack_base: self.stack.len(),
            locals: HashMap::new(),
            owned_allocs: Vec::new(),
        };
        for (i, arg) in args.iter().enumerate() {
            frame.locals.insert(fun.param_names[i].clone(), arg.clone());
        }
        self.frames.push(frame);
        Ok(())
    }

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

        self.frames.push(CallFrame {
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
                    // &x: 将值包装为引用（运行时层面，直接传递共享指针即可）
                    let v = self.pop();
                    self.push(v);
                }
                OpCode::Deref => {
                    // *p: 解引用指针
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
                    // 存活检查: 验证指针是否仍可用
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

// ==================== 辅助函数 ====================

fn read_u8(data: &[u8], pos: &mut usize) -> u8 {
    let v = data[*pos];
    *pos += 1;
    v
}

fn read_u32(data: &[u8], pos: &mut usize) -> u32 {
    let v = ((data[*pos] as u32) << 24)
        | ((data[*pos + 1] as u32) << 16)
        | ((data[*pos + 2] as u32) << 8)
        | (data[*pos + 3] as u32);
    *pos += 4;
    v
}

fn read_i32(data: &[u8], pos: &mut usize) -> i32 {
    read_u32(data, pos) as i32
}

fn read_u64(data: &[u8], pos: &mut usize) -> u64 {
    let mut v: u64 = 0;
    for _ in 0..8 {
        v = (v << 8) | (data[*pos] as u64);
        *pos += 1;
    }
    v
}

fn read_double(data: &[u8], pos: &mut usize) -> f64 {
    let bits = read_u64(data, pos);
    f64::from_bits(bits)
}

fn read_string(data: &[u8], pos: &mut usize) -> String {
    let len = read_u32(data, pos) as usize;
    let s = String::from_utf8_lossy(&data[*pos..*pos + len]).to_string();
    *pos += len;
    s
}

fn read_constant(data: &[u8], pos: &mut usize) -> Result<Value, String> {
    let type_id = read_u8(data, pos);
    match type_id {
        0 => Ok(Value::nil()),
        1 => Ok(Value::int(read_u64(data, pos) as i64)),
        2 => Ok(Value::float(read_double(data, pos))),
        3 => Ok(Value::string(read_string(data, pos))),
        4 => Ok(Value::bool(read_u8(data, pos) != 0)),
        _ => Err(format!("Unknown constant type: {}", type_id)),
    }
}
