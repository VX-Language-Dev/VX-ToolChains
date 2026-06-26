use crate::opcode::OpCode;
use crate::value::Value;
use crate::vm::{VM, DebugAction, StepMode};
use crate::vm_dispatch::DispatchResult;

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

        let num_params = self
            .module
            .functions
            .get(main_idx)
            .map(|f| f.num_params)
            .unwrap_or(0);
        let local_count = num_params.max(16) as usize;
        self.frames.push(crate::instruction::CallFrame {
            fn_idx: main_idx,
            pc: 0,
            stack_base: 0,
            locals: vec![Value::Nil; local_count],
            owned_allocs: Vec::new(),
        });

        while let Some(frame) = self.frames.last() {
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

            if self.breakpoints.contains(&frame.pc) {
                if let Some(ref hook) = self.debug_hook {
                    if let DebugAction::Break = hook(self) {
                        return Ok(self.handle_breakpoint());
                    }
                } else {
                    return Ok(self.handle_breakpoint());
                }
            }

            let fn_idx = frame.fn_idx;
            let pc = frame.pc;

            if pc >= self.module.functions[fn_idx].instructions.len() {
                let leaving_frame = self.frames.pop().ok_or_else(|| {
                    "VM invariant: frame unexpectedly missing during function exit".to_string()
                })?;
                self.cleanup_frame_allocs(&leaving_frame.owned_allocs);
                continue;
            }

            // 拆出指令中的 op/iarg 字段（两个都是 Copy），
            // sarg 用 Box<str> 的引用形式透传给 dispatcher（无堆分配）。
            // 为避免与后续 &mut self 调用冲突，使用裸指针解引用 sarg。
            // 在 dispatcher 调用期间 self.module 不会被修改（Vec 不会因 push 而失效），
            // 因此此裸指针访问是安全的。
            let op = self.module.functions[fn_idx].instructions[pc].op;
            let iarg = self.module.functions[fn_idx].instructions[pc].iarg;

            if let Some(frame_mut) = self.frames.last_mut() {
                frame_mut.pc += 1;
            } else {
                return Err("VM invariant: frame unexpectedly missing during instruction advance".to_string());
            }

            // 现在 pc 已递增，重新读取 sarg 时不再持有对 self 的借用。
            // 用裸指针避免与后续 &mut self 冲突。
            let sarg: Option<&str> = unsafe {
                let sarg_field = &self.module.functions[fn_idx].instructions[pc].sarg
                    as *const Option<Box<str>>;
                (*sarg_field).as_deref()
            };

            let result = match op {
                OpCode::LoadConst | OpCode::LoadNil | OpCode::LoadTrue | OpCode::LoadFalse
                | OpCode::LoadVar | OpCode::StoreVar | OpCode::DefineVar
                | OpCode::Dup | OpCode::Pop => {
                    self.exec_load_store(op, iarg, sarg)
                }

                OpCode::Call | OpCode::Return => {
                    self.exec_call_return(op, iarg)
                }

                OpCode::Jump | OpCode::JumpIfFalse | OpCode::JumpIfTrue => {
                    self.exec_jump(op, iarg)
                }

                OpCode::BinaryAdd | OpCode::BinarySub | OpCode::BinaryMul
                | OpCode::BinaryDiv | OpCode::BinaryMod | OpCode::BinaryPow => {
                    self.exec_binary_arith(op)
                }

                OpCode::BinaryEq | OpCode::BinaryNe | OpCode::BinaryLt | OpCode::BinaryGt
                | OpCode::BinaryLe | OpCode::BinaryGe | OpCode::BinaryAnd | OpCode::BinaryOr => {
                    self.exec_binary_cmp(op)
                }

                OpCode::AddInt | OpCode::AddFloat | OpCode::SubInt | OpCode::SubFloat
                | OpCode::MulInt | OpCode::MulFloat | OpCode::DivInt | OpCode::DivFloat
                | OpCode::ModInt => {
                    self.exec_specialized_arith(op)
                }

                OpCode::EqInt | OpCode::EqFloat | OpCode::LtInt | OpCode::LtFloat
                | OpCode::GtInt | OpCode::GtFloat | OpCode::LeInt | OpCode::LeFloat
                | OpCode::GeInt | OpCode::GeFloat => {
                    self.exec_specialized_cmp(op)
                }

                OpCode::And | OpCode::Or | OpCode::NegInt | OpCode::NegFloat
                | OpCode::Not | OpCode::UnaryNeg | OpCode::UnaryNot => {
                    self.exec_specialized_logic_unary(op)
                }

                OpCode::MakeArray | OpCode::IndexGet | OpCode::IndexSet
                | OpCode::MakeMap | OpCode::MakeStruct | OpCode::MakeClass => {
                    self.exec_collection(op, iarg, sarg)
                }

                OpCode::PropertyGet | OpCode::PropertySet => {
                    self.exec_property(op, sarg)
                }

                OpCode::AddressOf | OpCode::Deref | OpCode::PointerMember => {
                    self.exec_pointer(op, sarg)
                }

                OpCode::New | OpCode::Newz => {
                    self.exec_new(op, iarg)
                }

                OpCode::Free | OpCode::OwnershipMove | OpCode::ScopeDrop
                | OpCode::BorrowCheck | OpCode::AliveCheck => {
                    self.exec_memory_safety(op)
                }

                OpCode::Halt | OpCode::Import | OpCode::SysArgv | OpCode::System
                | OpCode::FileRead | OpCode::FileWrite | OpCode::FileExists => {
                    self.exec_syscall(op, sarg)
                }

                OpCode::Iterate | OpCode::Next => {
                    self.exec_iterator(op)
                }

                _ => DispatchResult::Error(format!("Unimplemented opcode: {:?}", op)),
            };

            match result {
                DispatchResult::Continue => {}
                DispatchResult::Return(v) => return Ok(v),
                DispatchResult::Error(e) => return Err(e),
            }

            if self.step_count > 0 {
                self.step_count -= 1;
                if self.step_count == 0 {
                    return Ok(self.handle_breakpoint());
                }
            }
        }

        Ok(Value::Nil)
    }
}