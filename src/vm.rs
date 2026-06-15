// ==================== VM 核心 ====================

use std::collections::{HashMap, HashSet};
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
    /// 已移动 (moved) 的变量槽位集合，用于运行时纵深防御
    pub(crate) moved_vars: HashSet<String>,
    // Top-of-Stack caching for performance
    tos: [Value; 3],
    tos_depth: usize,
    // Debugging support
    pub debug_hook: Option<Box<dyn Fn(&VM) -> DebugAction>>,
    pub breakpoints: HashSet<usize>,
    pub step_mode: StepMode,
    pub step_count: usize,
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
            tos: [Value::Nil, Value::Nil, Value::Nil],
            tos_depth: 0,
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
                    let mut buf = [0u8; 8];
                    buf.copy_from_slice(&c.data[..8.min(c.data.len())]);
                    Value::Int(i64::from_be_bytes(buf))
                }
                2 => {
                    let mut buf = [0u8; 8];
                    buf.copy_from_slice(&c.data[..8.min(c.data.len())]);
                    Value::Float(f64::from_be_bytes(buf))
                }
                3 => Value::String(String::from_utf8_lossy(&c.data).to_string()),
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
                let op = OpCode::try_from(inst.op).map_err(|e| e)?;
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
        if self.tos_depth < 3 {
            self.tos[self.tos_depth] = v;
            self.tos_depth += 1;
        } else {
            self.stack.push(std::mem::replace(&mut self.tos[2], v.clone()));
            self.tos[2] = v;
        }
    }

    pub(crate) fn pop(&mut self) -> Value {
        if self.tos_depth > 0 {
            self.tos_depth -= 1;
            std::mem::replace(&mut self.tos[self.tos_depth], Value::Nil)
        } else if let Some(v) = self.stack.pop() {
            v
        } else {
            Value::Nil
        }
    }

    pub(crate) fn peek(&self, offset: usize) -> Option<&Value> {
        let tos_idx = self.tos_depth as isize - 1 - offset as isize;
        if tos_idx >= 0 && (tos_idx as usize) < 3 {
            Some(&self.tos[tos_idx as usize])
        } else if self.stack.len() > offset - self.tos_depth {
            Some(&self.stack[self.stack.len() - 1 - (offset - self.tos_depth)])
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
        self.stack.len() + self.tos_depth
    }

    pub(crate) fn drain_stack(&mut self, range: std::ops::Range<usize>) -> Vec<Value> {
        let total_len = self.stack_len();
        let start_idx = range.start;
        let end_idx = range.end;
        
        if start_idx >= total_len || end_idx > total_len || start_idx > end_idx {
            return Vec::new();
        }
        
        let num_elements = end_idx - start_idx;
        let mut result = Vec::with_capacity(num_elements);
        
        for i in start_idx..end_idx {
            let from_top = total_len - 1 - i;
            if (from_top) < self.tos_depth {
                let tos_idx = self.tos_depth - 1 - from_top;
                result.push(std::mem::replace(&mut self.tos[tos_idx], Value::Nil));
            } else {
                let stack_idx = from_top - self.tos_depth;
                if stack_idx < self.stack.len() {
                    result.push(self.stack.remove(stack_idx));
                }
            }
        }
        
        let tos_drained = num_elements.min(self.tos_depth);
        self.tos_depth = self.tos_depth.saturating_sub(tos_drained);
        
        result.reverse();
        result
    }
    pub fn current_frame(&self) -> &CallFrame {
        &self.frames[self.frames.len() - 1]
    }
    pub(crate) fn current_frame_mut(&mut self) -> &mut CallFrame {
        let idx = self.frames.len() - 1;
        &mut self.frames[idx]
    }
    pub fn current_fn(&self) -> &Function {
        &self.module.functions[self.current_frame().fn_idx]
    }

    /// 创建运行时错误 Result。调用方需使用 `return self.runtime_error(...)` 提前返回。
    /// 注意：此方法不打印任何内容，由最外层调用者统一负责错误输出。
    pub(crate) fn runtime_error<T>(&self, msg: &str) -> Result<T, String> {
        Err(msg.to_string())
    }

    pub(crate) fn call_user_function(&mut self, fn_idx: usize, args: &[Value]) -> Result<(), String> {
        let fun = &self.module.functions[fn_idx];
        if args.len() != fun.num_params as usize {
            return Err(format!(
                "Argument count mismatch for {}: expected {}, got {}",
                fun.name,
                fun.num_params,
                args.len()
            ));
        }
        while self.tos_depth > 0 {
            self.tos_depth -= 1;
            self.stack.push(std::mem::replace(&mut self.tos[self.tos_depth], Value::Nil));
        }
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
        println!("\n=== VX Debugger ===");
        println!("Breakpoint hit at PC: {}", self.current_frame().pc);
        println!("Current function: {}", self.current_fn().name);
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
            let func = &self.module.functions[frame.fn_idx];
            println!("  #{}: {} at PC {}", self.frames.len() - 1 - i, func.name, frame.pc);
        }
    }

    /// Print local variables of the current frame
    fn print_locals(&self) {
        let frame = self.current_frame();
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
    pub fn set_debug_hook(&mut self, hook: Option<Box<dyn Fn(&VM) -> DebugAction>>) {
        self.debug_hook = hook;
    }
}