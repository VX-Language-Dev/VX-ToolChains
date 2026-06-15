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

        let local_count = self.module.functions[main_idx].num_params.max(16) as usize;
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
                    match hook(self) {
                        DebugAction::Break => {
                            return Ok(self.handle_breakpoint());
                        }
                        _ => {}
                    }
                } else {
                    return Ok(self.handle_breakpoint());
                }
            }

            if frame.pc >= self.current_fn().instructions.len() {
                let leaving_frame = self.frames.pop().unwrap();
                self.cleanup_frame_allocs(&leaving_frame.owned_allocs);
                continue;
            }

            let fn_idx = self.frames.last().unwrap().fn_idx;
            let pc = self.frames.last().unwrap().pc;
            let (op, iarg, sarg_owned) = {
                let inst_ref = &self.module.functions[fn_idx].instructions[pc];
                (inst_ref.op, inst_ref.iarg, inst_ref.sarg.clone())
            };
            let sarg = sarg_owned.as_deref();
            self.frames.last_mut().unwrap().pc += 1;

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