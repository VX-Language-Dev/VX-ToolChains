// ==================== VM 核心 ====================

use std::collections::{HashMap, HashSet};
use std::sync::Arc;
use crate::value::Value;
use crate::instruction::{Instruction, Function, Module, CallFrame};
use crate::memory_safety::AllocRecord;
use crate::opcode::OpCode;

// Debugging support
#[derive(Debug, Clone, Copy)]
pub enum DebugAction {
    Continue,
    StepInto,
    StepOver,
    StepOut,
    Break,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum StepMode {
    None,
    Into,
    Over,
    Out,
}

pub type DebugHook = Option<Box<dyn Fn(&VM) -> DebugAction>>;

pub struct VM {
    pub module: Module,
    pub frames: Vec<CallFrame>,
    pub stack: Vec<Value>,
    pub globals: HashMap<String, Value>,
    pub(crate) builtins: HashMap<String, fn(&mut [Value]) -> Value>,
    pub(crate) alloc_registry: HashMap<u64, AllocRecord>,
    pub(crate) next_alloc_id: u64,
    /// 命令行参数，供 SysArgv 指令使用
    pub argv: Vec<String>,
    /// 移动 (moved) 后的变量槽位集合，用于运行时纵深防御
    pub(crate) moved_vars: HashSet<String>,
    // Debugging support
    pub debug_hook: DebugHook,
    pub breakpoints: HashSet<usize>,
    pub step_mode: StepMode,
    pub step_count: usize,
}

impl Default for VM {
    fn default() -> Self {
        Self::new()
    }
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
            argv: Vec::new(),
            moved_vars: HashSet::new(),
            // Debugging support
            debug_hook: None,
            breakpoints: HashSet::new(),
            step_mode: StepMode::None,
            step_count: 0,
        };

        // Built-in functions
        vm.define_builtin("out", |args| {
            if args.is_empty() {
                println!();
            } else {
                print!("{}", args[0]);
                for i in 1..args.len() {
                    print!(" {}", args[i]);
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
                    Arc::make_mut(arr).push(val);
                }
            }
            Value::nil()
        });

        // ---- VX 自举所需扩展内建函数 ----
        vm.define_builtin("ord", |args| {
            if args.is_empty() {
                return Value::int(0);
            }
            match &args[0] {
                Value::String(s) => {
                    if let Some(c) = s.chars().next() {
                        Value::int(c as i64)
                    } else {
                        Value::int(0)
                    }
                }
                _ => Value::int(0),
            }
        });

        vm.define_builtin("chr", |args| {
            if args.is_empty() {
                return Value::string("".to_string());
            }
            match &args[0] {
                Value::Int(i) => {
                    if let Some(c) = char::from_u32(*i as u32) {
                        Value::string(c.to_string())
                    } else {
                        Value::string("".to_string())
                    }
                }
                _ => Value::string("".to_string()),
            }
        });

        vm.define_builtin("float", |args| {
            if args.is_empty() {
                return Value::float(0.0);
            }
            match &args[0] {
                Value::Int(i) => Value::float(*i as f64),
                Value::Float(f) => Value::float(*f),
                Value::Bool(b) => Value::float(if *b { 1.0 } else { 0.0 }),
                Value::String(s) => s.parse::<f64>().map(Value::float).unwrap_or(Value::float(0.0)),
                _ => Value::float(0.0),
            }
        });

        vm.define_builtin("parse_int", |args| {
            if args.is_empty() {
                return Value::nil();
            }
            match &args[0] {
                Value::String(s) => s.parse::<i64>().map(Value::int).unwrap_or(Value::nil()),
                Value::Int(i) => Value::int(*i),
                _ => Value::nil(),
            }
        });

        vm.define_builtin("parse_float", |args| {
            if args.is_empty() {
                return Value::nil();
            }
            match &args[0] {
                Value::String(s) => s.parse::<f64>().map(Value::float).unwrap_or(Value::nil()),
                Value::Float(f) => Value::float(*f),
                Value::Int(i) => Value::float(*i as f64),
                _ => Value::nil(),
            }
        });

        // ---- 反射内建函数（自举所需） ----
        vm.define_builtin("typeof", |args| {
            if args.is_empty() {
                return Value::string("Nil");
            }
            Value::string(args[0].type_name())
        });

        vm.define_builtin("is_int", |args| {
            if args.is_empty() {
                return Value::bool(false);
            }
            Value::bool(matches!(args[0], Value::Int(_)))
        });

        vm.define_builtin("is_float", |args| {
            if args.is_empty() {
                return Value::bool(false);
            }
            Value::bool(matches!(args[0], Value::Float(_)))
        });

        vm.define_builtin("is_bool", |args| {
            if args.is_empty() {
                return Value::bool(false);
            }
            Value::bool(matches!(args[0], Value::Bool(_)))
        });

        vm.define_builtin("is_string", |args| {
            if args.is_empty() {
                return Value::bool(false);
            }
            Value::bool(matches!(args[0], Value::String(_)))
        });

        vm.define_builtin("is_array", |args| {
            if args.is_empty() {
                return Value::bool(false);
            }
            Value::bool(matches!(args[0], Value::Array(_)))
        });

        vm.define_builtin("is_map", |args| {
            if args.is_empty() {
                return Value::bool(false);
            }
            Value::bool(matches!(args[0], Value::Map(_)))
        });

        vm.define_builtin("is_nil", |args| {
            if args.is_empty() {
                return Value::bool(true);
            }
            Value::bool(matches!(args[0], Value::Nil))
        });

        vm.define_builtin("is_instance", |args| {
            if args.is_empty() {
                return Value::bool(false);
            }
            Value::bool(matches!(args[0], Value::Instance { .. }))
        });

        vm.define_builtin("file_read_bytes", |args| {
            if args.is_empty() {
                return Value::nil();
            }
            match &args[0] {
                Value::String(path) => match std::fs::read(path.as_ref()) {
                    Ok(bytes) => Value::Array(Arc::new(bytes.into_iter().map(|b| Value::int(b as i64)).collect())),
                    Err(_) => Value::nil(),
                },
                _ => Value::nil(),
            }
        });

        vm.define_builtin("file_write_bytes", |args| {
            if args.len() < 2 {
                return Value::bool(false);
            }
            let path = match &args[0] {
                Value::String(s) => s.to_string(),
                _ => return Value::bool(false),
            };
            let bytes: Vec<u8> = match &args[1] {
                Value::Array(arr) => arr.iter()
                    .map(|v| match v {
                        Value::Int(i) => *i as u8,
                        _ => 0,
                    })
                    .collect(),
                _ => return Value::bool(false),
            };
            match std::fs::write(&path, bytes) {
                Ok(_) => Value::bool(true),
                Err(_) => Value::bool(false),
            }
        });

        vm.define_builtin("file_size", |args| {
            if args.is_empty() {
                return Value::int(-1);
            }
            match &args[0] {
                Value::String(path) => match std::fs::metadata(path.as_ref()) {
                    Ok(m) => Value::int(m.len() as i64),
                    Err(_) => Value::int(-1),
                },
                _ => Value::int(-1),
            }
        });

        vm.define_builtin("is_dir", |args| {
            if args.is_empty() {
                return Value::bool(false);
            }
            match &args[0] {
                Value::String(path) => match std::fs::metadata(path.as_ref()) {
                    Ok(m) => Value::bool(m.is_dir()),
                    Err(_) => Value::bool(false),
                },
                _ => Value::bool(false),
            }
        });

        vm.define_builtin("mkdir", |args| {
            if args.is_empty() {
                return Value::bool(false);
            }
            match &args[0] {
                Value::String(path) => match std::fs::create_dir_all(path.as_ref()) {
                    Ok(_) => Value::bool(true),
                    Err(_) => Value::bool(false),
                },
                _ => Value::bool(false),
            }
        });

        vm.define_builtin("remove_file", |args| {
            if args.is_empty() {
                return Value::bool(false);
            }
            match &args[0] {
                Value::String(path) => match std::fs::remove_file(path.as_ref()) {
                    Ok(_) => Value::bool(true),
                    Err(_) => Value::bool(false),
                },
                _ => Value::bool(false),
            }
        });

        vm.define_builtin("remove_dir", |args| {
            if args.is_empty() {
                return Value::bool(false);
            }
            match &args[0] {
                Value::String(path) => match std::fs::remove_dir_all(path.as_ref()) {
                    Ok(_) => Value::bool(true),
                    Err(_) => Value::bool(false),
                },
                _ => Value::bool(false),
            }
        });

        vm.define_builtin("get_env", |args| {
            if args.is_empty() {
                return Value::nil();
            }
            match &args[0] {
                Value::String(key) => match std::env::var(key.as_ref()) {
                    Ok(v) => Value::string(v),
                    Err(_) => Value::nil(),
                },
                _ => Value::nil(),
            }
        });

        vm.define_builtin("set_env", |args| {
            if args.len() < 2 {
                return Value::bool(false);
            }
            let key = match &args[0] {
                Value::String(s) => s.to_string(),
                _ => return Value::bool(false),
            };
            let val = match &args[1] {
                Value::String(s) => s.to_string(),
                other => other.to_string(),
            };
            match std::env::set_var(&key, val) {
                _ => Value::bool(true),
            }
        });

        vm.define_builtin("unset_env", |args| {
            if args.is_empty() {
                return Value::bool(false);
            }
            match &args[0] {
                Value::String(key) => {
                    std::env::remove_var(key.as_ref());
                    Value::bool(true)
                }
                _ => Value::bool(false),
            }
        });

        vm.define_builtin("current_dir", |_args| {
            match std::env::current_dir() {
                Ok(p) => Value::string(p.to_string_lossy().to_string()),
                Err(_) => Value::nil(),
            }
        });

        vm.define_builtin("exit", |args| {
            let code = if args.is_empty() {
                0
            } else {
                match &args[0] {
                    Value::Int(i) => *i as i32,
                    _ => 0,
                }
            };
            std::process::exit(code);
        });

        vm.define_builtin("run_cmd", |args| {
            if args.is_empty() {
                return Value::Array(Arc::new(vec![Value::int(-1), Value::string("".to_string()), Value::string("".to_string())]));
            }
            let cmd = match &args[0] {
                Value::String(s) => s.to_string(),
                _ => return Value::Array(Arc::new(vec![Value::int(-1), Value::string("".to_string()), Value::string("".to_string())])),
            };
            match std::process::Command::new("sh").arg("-c").arg(&cmd).output() {
                Ok(output) => {
                    let status = output.status.code().unwrap_or(-1) as i64;
                    let stdout = String::from_utf8_lossy(&output.stdout).to_string();
                    let stderr = String::from_utf8_lossy(&output.stderr).to_string();
                    Value::Array(Arc::new(vec![Value::int(status), Value::string(stdout), Value::string(stderr)]))
                }
                Err(e) => Value::Array(Arc::new(vec![Value::int(-1), Value::string("".to_string()), Value::string(e.to_string())])),
            }
        });

        // ---- 迭代器内建函数（for 循环支持）----
        vm.define_builtin("iterate", |args| {
            if args.is_empty() {
                return Value::nil();
            }
            // 返回迭代器状态：数组 [items, index]
            match &args[0] {
                Value::Array(arr) => {
                    let idx = if args.len() > 1 {
                        match &args[1] {
                            Value::Int(i) => *i as usize,
                            _ => 0,
                        }
                    } else {
                        0
                    };
                    // 存储原始数组和当前位置
                    let iter_state = Value::Array(Arc::new(vec![
                        args[0].clone(),
                        Value::int(idx as i64),
                    ]));
                    Value::Array(Arc::new(vec![iter_state, Value::int(arr.len() as i64)]))
                }
                Value::String(s) => {
                    // 字符串迭代器
                    let idx = if args.len() > 1 {
                        match &args[1] {
                            Value::Int(i) => *i as usize,
                            _ => 0,
                        }
                    } else {
                        0
                    };
                    let iter_state = Value::Array(Arc::new(vec![
                        args[0].clone(),
                        Value::int(idx as i64),
                    ]));
                    Value::Array(Arc::new(vec![iter_state, Value::int(s.len() as i64)]))
                }
                Value::Map(m) => {
                    // Map 迭代器：返回键数组
                    let keys: Vec<Value> = m.keys().map(|k| Value::String(k.clone().into())).collect();
                    let keys_len = keys.len();
                    let idx = if args.len() > 1 {
                        match &args[1] {
                            Value::Int(i) => *i as usize,
                            _ => 0,
                        }
                    } else {
                        0
                    };
                    let iter_state = Value::Array(Arc::new(vec![
                        Value::Array(Arc::new(keys)),
                        Value::int(idx as i64),
                    ]));
                    Value::Array(Arc::new(vec![iter_state, Value::int(keys_len as i64)]))
                }
                _ => Value::nil(),
            }
        });

        vm.define_builtin("range", |args| {
            if args.is_empty() {
                return Value::Array(Arc::new(vec![]));
            }
            let start: i64 = if args.len() > 1 {
                match &args[0] {
                    Value::Int(i) => *i,
                    _ => 0,
                }
            } else {
                0
            };
            let end: i64 = if args.len() > 1 {
                match &args[1] {
                    Value::Int(i) => *i,
                    _ => start,
                }
            } else {
                match &args[0] {
                    Value::Int(i) => *i,
                    _ => 0,
                }
            };
            let step: i64 = if args.len() > 2 {
                match &args[2] {
                    Value::Int(i) => *i,
                    _ => 1,
                }
            } else {
                1
            };
            let mut result = Vec::new();
            if step > 0 {
                let mut i = start;
                while i < end {
                    result.push(Value::int(i));
                    i += step;
                }
            } else if step < 0 {
                let mut i = start;
                while i > end {
                    result.push(Value::int(i));
                    i += step;
                }
            }
            Value::Array(Arc::new(result))
        });

        vm.define_builtin("enumerate", |args| {
            if args.is_empty() {
                return Value::nil();
            }
            let container = &args[0];
            match container {
                Value::Array(arr) => {
                    let result: Vec<Value> = arr.iter().enumerate()
                        .map(|(i, v)| Value::Array(Arc::new(vec![Value::int(i as i64), v.clone()])))
                        .collect();
                    Value::Array(Arc::new(result))
                }
                Value::String(s) => {
                    let result: Vec<Value> = s.chars().enumerate()
                        .map(|(i, c)| Value::Array(Arc::new(vec![Value::int(i as i64), Value::string(c.to_string())])))
                        .collect();
                    Value::Array(Arc::new(result))
                }
                _ => Value::nil(),
            }
        });

        vm
    }

    pub(crate) fn define_builtin(&mut self, name: &str, f: fn(&mut [Value]) -> Value) {
        self.builtins.insert(name.to_string(), f);
        self.globals
            .insert(name.to_string(), Value::string(name.to_string()));
    }

    pub fn load_module(&mut self, bytecode_data: &[u8]) -> Result<bool, String> {
        let parsed = crate::bytecode::parse_vxobj(bytecode_data)
            .map_err(|e| format!("Failed to parse VXOBJ bytecode: {}", e))?;

        // 加载常量池
        self.module.constants.reserve(parsed.constants.len());
        for c in &parsed.constants {
            let val = match c.tag {
                0 => Value::Nil,
                1 => {
                    if c.data.len() < 8 {
                        return Err(format!(
                            "Constant tag=Int but data too short ({} bytes, need 8)",
                            c.data.len()
                        ));
                    }
                    let mut buf = [0u8; 8];
                    buf.copy_from_slice(&c.data[..8]);
                    Value::Int(i64::from_be_bytes(buf))
                }
                2 => {
                    if c.data.len() < 8 {
                        return Err(format!(
                            "Constant tag=Float but data too short ({} bytes, need 8)",
                            c.data.len()
                        ));
                    }
                    let mut buf = [0u8; 8];
                    buf.copy_from_slice(&c.data[..8]);
                    Value::Float(f64::from_be_bytes(buf))
                }
                3 => Value::String(String::from_utf8_lossy(&c.data).to_string().into()),
                4 => Value::Bool(c.data.first().copied().unwrap_or(0) != 0),
                _ => Value::Nil,
            };
            self.module.constants.push(val);
        }

        // 加载函数
        self.module.functions.reserve(parsed.functions.len());
        for f in &parsed.functions {
            let constants = Vec::new();
            let mut instructions = Vec::with_capacity(f.instructions.len());
            for inst in &f.instructions {
                let op = OpCode::try_from(inst.op)?;
                let instruction = match inst.arg_type {
                    0 => Instruction::new(op),
                    1 => Instruction::with_iarg(op, inst.iarg.unwrap_or(0)),
                    2 => Instruction::with_sarg(op, inst.sarg.clone().unwrap_or_default()),
                    _ => return Err(format!("Unknown arg type: {}", inst.arg_type)),
                };
                instructions.push(instruction);
            }

            self.module.functions.push(Function {
                name: f.name.clone(),
                instructions,
                constants,
                num_params: f.num_params,
                has_return: f.has_return,
                param_names: f.param_names.clone(),
            });
        }

        // 加载结构体定义
        self.module.struct_defs = parsed.struct_defs.clone();

        // 构建函数映射表
        self.module.function_map.clear();
        for (i, func) in self.module.functions.iter().enumerate() {
            self.module.function_map.insert(func.name.clone(), i);
        }

        Ok(true)
    }

    pub(crate) fn push(&mut self, v: Value) {
        self.stack.push(v);
    }

    pub(crate) fn pop(&mut self) -> Value {
        self.stack.pop().unwrap_or(Value::Nil)
    }

    pub(crate) fn peek(&self, offset: usize) -> Option<&Value> {
        let len = self.stack.len();
        if offset < len {
            Some(&self.stack[len - 1 - offset])
        } else {
            None
        }
    }

    pub(crate) fn pop_int(&mut self) -> i64 {
        match self.pop() {
            Value::Int(i) => i,
            _ => 0,
        }
    }

    pub(crate) fn pop_float(&mut self) -> f64 {
        match self.pop() {
            Value::Float(f) => f,
            Value::Int(i) => i as f64,
            _ => 0.0,
        }
    }

    pub(crate) fn pop_bool(&mut self) -> bool {
        match self.pop() {
            Value::Bool(b) => b,
            _ => false,
        }
    }

    pub(crate) fn push_int(&mut self, i: i64) {
        self.push(Value::Int(i));
    }

    pub(crate) fn push_float(&mut self, f: f64) {
        self.push(Value::Float(f));
    }

    pub(crate) fn push_bool(&mut self, b: bool) {
        self.push(Value::Bool(b));
    }

    pub(crate) fn stack_len(&self) -> usize {
        self.stack.len()
    }

    /// 从栈底索引 `range.start..range.end` 区间弹出元素。
    ///
    /// `range.start` / `range.end` 以"从栈底开始的索引"计：0 表示栈底，
    /// `stack_len() - 1` 表示栈顶。返回顺序为**栈顶到栈底**（即弹出顺序），
    /// 调用者如需栈底到栈顶顺序应自行 `reverse()`。
    ///
    /// 与原始实现相比，本版本使用 `Vec::drain` 一次性移除区间，避免 `Vec::remove`
    /// 的 O(n²) 行为。
    pub(crate) fn drain_stack(&mut self, range: std::ops::Range<usize>) -> Vec<Value> {
        let total_len = self.stack_len();
        if range.start >= total_len || range.end > total_len || range.start >= range.end {
            return Vec::new();
        }
        // drain 返回栈底到栈顶顺序，reverse 后变为栈顶到栈底（弹出顺序）
        let mut result: Vec<Value> = self.stack.drain(range).collect();
        result.reverse();
        result
    }
    pub fn current_frame(&self) -> Option<&CallFrame> {
        self.frames.last()
    }
    pub(crate) fn current_frame_mut(&mut self) -> Option<&mut CallFrame> {
        self.frames.last_mut()
    }
    pub fn current_fn(&self) -> Option<&Function> {
        self.current_frame().map(|frame| &self.module.functions[frame.fn_idx])
    }

    /// 创建运行时错误 Result。调用方需使用 `return self.runtime_error(...)` 提前返回。
    /// 注意：此方法不打印任何内容，由最外层调用者统一负责错误输出。
    pub(crate) fn runtime_error<T>(&self, msg: &str) -> Result<T, String> {
        Err(msg.to_string())
    }

    pub(crate) fn call_user_function(&mut self, fn_idx: usize, args: &[Value]) -> Result<(), String> {
        let fun = self.module.functions.get(fn_idx).ok_or_else(|| {
            format!("call_user_function: invalid function index {}", fn_idx)
        })?;
        if args.len() != fun.num_params as usize {
            return Err(format!(
                "Argument count mismatch for {}: expected {}, got {}",
                fun.name,
                fun.num_params,
                args.len()
            ));
        }
        // 单一 Vec 栈后无需 flush TOS 缓存
        let mut frame = CallFrame {
            fn_idx,
            pc: 0,
            stack_base: self.stack.len(),
            locals: vec![Value::Nil; fun.num_params as usize],
            owned_allocs: Vec::new(),
        };
        for (i, arg) in args.iter().enumerate() {
            frame.locals[i] = arg.clone();
        }
        self.frames.push(frame);
        Ok(())
    }

    /// Handle a breakpoint or step event by entering a debug REPL
    pub(crate) fn handle_breakpoint(&mut self) -> Value {
        let frame = match self.current_frame() {
            Some(f) => f,
            None => return Value::Nil,
        };
        let fn_name = self.current_fn().map(|f| f.name.clone()).unwrap_or_else(|| "unknown".to_string());
        println!("\n=== VX Debugger ===");
        println!("Breakpoint hit at PC: {}", frame.pc);
        println!("Current function: {}", fn_name);
        println!("Stack depth: {}", self.stack.len());
        // Simple debug REPL - in a real implementation, this would be more sophisticated
        loop {
            print!("(vxdbg) ");
            let mut input = String::new();
            if std::io::stdin().read_line(&mut input).unwrap_or(0) == 0 {
                break; // EOF
            }
            let input = input.trim();
            if input.is_empty() {
                continue;
            }
            match input {
                "continue" | "c" => {
                    println!("Continuing execution...");
                    break;
                }
                "step" | "s" => {
                    println!("Stepping into next instruction...");
                    self.step_mode = StepMode::Into;
                    self.step_count = 1;
                    break;
                }
                "next" | "n" => {
                    println!("Stepping over next instruction...");
                    self.step_mode = StepMode::Over;
                    self.step_count = 1;
                    break;
                }
                "finish" | "f" => {
                    println!("Stepping out of current function...");
                    self.step_mode = StepMode::Out;
                    self.step_count = 1;
                    break;
                }
                "backtrace" | "bt" => {
                    self.print_backtrace();
                }
                "info locals" => {
                    self.print_locals();
                }
                "info globals" => {
                    self.print_globals();
                }
                "info stack" => {
                    self.print_stack();
                }
                "help" | "h" => {
                    println!("Available commands:");
                    println!("  continue/c - Continue execution");
                    println!("  step/s     - Step into next instruction");
                    println!("  next/n     - Step over next instruction");
                    println!("  finish/f   - Step out of current function");
                    println!("  backtrace/bt - Print call stack");
                    println!("  info locals - Print local variables");
                    println!("  info globals - Print global variables");
                    println!("  info stack - Print stack contents");
                    println!("  help/h     - Show this help");
                }
                _ => {
                    println!("Unknown command: {}", input);
                    println!("Type 'help' for available commands");
                }
            }
        }
        Value::Nil
    }

    /// Print the call stack
    fn print_backtrace(&self) {
        println!("Call stack (most recent first):");
        for (i, frame) in self.frames.iter().enumerate().rev() {
            let func_name = self.module.functions.get(frame.fn_idx)
                .map(|f| f.name.as_str())
                .unwrap_or("unknown");
            println!("  #{}: {} at PC {}", self.frames.len() - 1 - i, func_name, frame.pc);
        }
    }

    /// Print local variables of the current frame
    fn print_locals(&self) {
        let frame = match self.current_frame() {
            Some(f) => f,
            None => {
                println!("No active frame.");
                return;
            }
        };
        println!("Local variables:");
        for (i, value) in frame.locals.iter().enumerate() {
            println!("  [{}] {:?}", i, value);
        }
    }

    /// Print global variables
    fn print_globals(&self) {
        println!("Global variables:");
        for (name, value) in &self.globals {
            println!("  {} = {:?}", name, value);
        }
    }

    /// Print the stack contents
    fn print_stack(&self) {
        println!("Stack (top to bottom):");
        for (i, value) in self.stack.iter().enumerate().rev() {
            println!("  [{}] {:?}", self.stack.len() - 1 - i, value);
        }
    }

    /// Set a breakpoint at a specific program counter position
    pub fn set_breakpoint(&mut self, pc: usize) {
        self.breakpoints.insert(pc);
    }

    /// Clear a breakpoint at a specific program counter position
    pub fn clear_breakpoint(&mut self, pc: usize) {
        self.breakpoints.remove(&pc);
    }

    /// Clear all breakpoints
    pub fn clear_all_breakpoints(&mut self) {
        self.breakpoints.clear();
    }

    /// Set the debug hook function
    pub fn set_debug_hook(&mut self, hook: DebugHook) {
        self.debug_hook = hook;
    }
}