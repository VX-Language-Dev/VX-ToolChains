// ==================== VM 核心 ====================

use std::collections::HashMap;
use crate::value::Value;
use crate::instruction::{Instruction, Function, Module, CallFrame};
use crate::memory_safety::AllocRecord;
use crate::opcode::OpCode;

pub struct VM {
    pub(crate) module: Module,
    pub(crate) frames: Vec<CallFrame>,
    pub(crate) stack: Vec<Value>,
    pub(crate) globals: HashMap<String, Value>,
    pub(crate) builtins: HashMap<String, fn(&mut [Value]) -> Value>,
    pub(crate) alloc_registry: HashMap<u64, AllocRecord>,
    pub(crate) next_alloc_id: u64,
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
            self.module.constants.push(match c {
                crate::bytecode::SerializedConstant::Nil => Value::Nil,
                crate::bytecode::SerializedConstant::Bool(b) => Value::Bool(*b),
                crate::bytecode::SerializedConstant::Int(i) => Value::Int(*i),
                crate::bytecode::SerializedConstant::Float(f) => Value::Float(*f),
                crate::bytecode::SerializedConstant::String(s) => Value::String(s.clone()),
            });
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
        self.stack.push(v);
    }
    pub(crate) fn pop(&mut self) -> Value {
        self.stack.pop().unwrap_or(Value::nil())
    }
    pub(crate) fn peek(&self, offset: usize) -> Option<&Value> {
        if self.stack.len() > offset {
            Some(&self.stack[self.stack.len() - 1 - offset])
        } else {
            None
        }
    }
    pub(crate) fn current_frame(&self) -> &CallFrame {
        &self.frames[self.frames.len() - 1]
    }
    pub(crate) fn current_frame_mut(&mut self) -> &mut CallFrame {
        let idx = self.frames.len() - 1;
        &mut self.frames[idx]
    }
    pub(crate) fn current_fn(&self) -> &Function {
        &self.module.functions[self.current_frame().fn_idx]
    }

    pub(crate) fn runtime_error(&self, msg: &str) -> ! {
        eprintln!("[Runtime Error] {}", msg);
        panic!("{}", msg);
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
}
