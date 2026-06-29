use crate::parser::{Expr, expr_to_type_name};
use crate::OpCode;
use crate::compiler_bytecode::{BytecodeArg, ConstantValue};
use crate::compiler_core::{Compiler, LoopInfo};

use crate::parser::Stmt;
impl Compiler {
    pub fn compile_stmt(&mut self, s: &Stmt) -> Result<(), String> {
        match s {
            Expr::ExprStmt(expr, _, _) => {
                if let Expr::Assign(ref target, ref op, ref value, _, _) = **expr {
                    self.compile_assign(target, op, value)?;
                } else {
                    self.compile_expr(expr)?;
                }
            }
            Expr::VarDecl(name, ty, value, _, _, _) => {
                let declared = match ty {
                    Some(t) => Self::type_name_to_known_type(&expr_to_type_name(t)),
                    None => {
                        return Err(format!(
                            "VX Error: 变量 `{}` 缺少类型注解，VX 为纯静态类型语言",
                            name
                        ));
                    }
                };
                self.compile_expr(value)?;
                // 丢弃从初始值推导出的临时类型，变量类型以显式声明为准
                self.pop_stack_type();
                self.set_var_type(name, declared);
                let slot = self.allocate_slot(name);
                self.emit(OpCode::DefineVar, BytecodeArg::Int(slot as i32));
            }
            Expr::IfStmt(cond, body, elifs, else_body, _, _) => {
                self.compile_expr(cond)?;
                let jump_to_elif = self.emit(OpCode::JumpIfFalse, BytecodeArg::None);
                for x in body {
                    self.compile_stmt(x)?;
                }
                let mut exit_jumps: Vec<usize> = Vec::new();
                exit_jumps.push(self.emit(OpCode::Jump, BytecodeArg::None));
                self.patch(jump_to_elif, self.instructions.len());
                for (c, b) in elifs {
                    self.compile_expr(c)?;
                    let jump_to_next = self.emit(OpCode::JumpIfFalse, BytecodeArg::None);
                    for x in b {
                        self.compile_stmt(x)?;
                    }
                    exit_jumps.push(self.emit(OpCode::Jump, BytecodeArg::None));
                    self.patch(jump_to_next, self.instructions.len());
                }
                if let Some(b) = else_body {
                    for x in b {
                        self.compile_stmt(x)?;
                    }
                }
                let end_pc = self.instructions.len();
                for j in exit_jumps {
                    self.patch(j, end_pc);
                }
            }
            Expr::WhileStmt(cond, body, _, _) => {
                let start = self.instructions.len();
                self.loop_stack.push(LoopInfo {
                    start,
                    break_jumps: Vec::new(),
                    continue_jumps: Vec::new(),
                    label: None,
                });
                self.compile_expr(cond)?;
                let exit_j = self.emit(OpCode::JumpIfFalse, BytecodeArg::None);
                for x in body {
                    self.compile_stmt(x)?;
                }
                self.emit(OpCode::Jump, BytecodeArg::None);
                let exit_pc = self.instructions.len();
                self.patch(exit_j, exit_pc);
                self.patch(self.instructions.len() - 1, start);
                let (break_jumps, continue_jumps) = {
                    let info = self.loop_stack.last().unwrap();
                    (info.break_jumps.clone(), info.continue_jumps.clone())
                };
                for bj in &break_jumps {
                    self.patch(*bj, exit_pc);
                }
                for cj in &continue_jumps {
                    self.patch(*cj, start);
                }
                self.loop_stack.pop();
            }
            Expr::ForStmt(var, iter, body, _, _) => {
                let for_id = self.for_counter;
                self.for_counter += 1;
                let src_var = format!("__for_{}_src", for_id);
                let idx_var = format!("__for_{}_idx", for_id);
                self.compile_expr(iter)?;
                let src_slot = self.allocate_slot(&src_var);
                self.emit(OpCode::DefineVar, BytecodeArg::Int(src_slot as i32));
                let const_0 = self.add_const(ConstantValue::Int(0)) as i32;
                self.emit(OpCode::LoadConst, BytecodeArg::Int(const_0));
                let idx_slot = self.allocate_slot(&idx_var);
                self.emit(OpCode::DefineVar, BytecodeArg::Int(idx_slot as i32));
                let start = self.instructions.len();
                self.loop_stack.push(LoopInfo {
                    start,
                    break_jumps: Vec::new(),
                    continue_jumps: Vec::new(),
                    label: None,
                });
                let idx_slot2 = self.allocate_slot(&idx_var);
                self.emit(OpCode::LoadVar, BytecodeArg::Int(idx_slot2 as i32));
                let src_slot2 = self.allocate_slot(&src_var);
                self.emit(OpCode::LoadVar, BytecodeArg::Int(src_slot2 as i32));
                let const_len = self.add_const(ConstantValue::String("len".into())) as i32;
                self.emit(OpCode::LoadConst, BytecodeArg::Int(const_len));
                self.emit(OpCode::Call, BytecodeArg::Int(1));
                self.emit(OpCode::BinaryLt, BytecodeArg::None);
                let exit_j = self.emit(OpCode::JumpIfFalse, BytecodeArg::None);
                let src_slot3 = self.allocate_slot(&src_var);
                self.emit(OpCode::LoadVar, BytecodeArg::Int(src_slot3 as i32));
                let idx_slot3 = self.allocate_slot(&idx_var);
                self.emit(OpCode::LoadVar, BytecodeArg::Int(idx_slot3 as i32));
                self.emit(OpCode::IndexGet, BytecodeArg::None);
                let var_slot = self.allocate_slot(var);
                self.emit(OpCode::DefineVar, BytecodeArg::Int(var_slot as i32));
                for x in body {
                    self.compile_stmt(x)?;
                }
                let cont_pc = self.instructions.len();
                self.loop_stack.last_mut().unwrap().start = cont_pc;
                let idx_slot4 = self.allocate_slot(&idx_var);
                self.emit(OpCode::LoadVar, BytecodeArg::Int(idx_slot4 as i32));
                let const_1 = self.add_const(ConstantValue::Int(1)) as i32;
                self.emit(OpCode::LoadConst, BytecodeArg::Int(const_1));
                self.emit(OpCode::BinaryAdd, BytecodeArg::None);
                let idx_slot5 = self.allocate_slot(&idx_var);
                self.emit(OpCode::StoreVar, BytecodeArg::Int(idx_slot5 as i32));
                self.emit(OpCode::Jump, BytecodeArg::None);
                let exit_pc = self.instructions.len();
                self.patch(exit_j, exit_pc);
                self.patch(self.instructions.len() - 1, start);
                let (break_jumps, continue_jumps) = {
                    let info = self.loop_stack.last().unwrap();
                    (info.break_jumps.clone(), info.continue_jumps.clone())
                };
                for bj in &break_jumps {
                    self.patch(*bj, exit_pc);
                }
                for cj in &continue_jumps {
                    self.patch(*cj, cont_pc);
                }
                self.loop_stack.pop();
            }
            Expr::LoopStmt(label, body, _, _) => {
                let start = self.instructions.len();
                self.loop_stack.push(LoopInfo {
                    start,
                    break_jumps: Vec::new(),
                    continue_jumps: Vec::new(),
                    label: label.clone(),
                });
                for x in body {
                    self.compile_stmt(x)?;
                }
                self.emit(OpCode::Jump, BytecodeArg::None);
                let exit_pc = self.instructions.len();
                self.patch(self.instructions.len() - 1, start);
                let (break_jumps, continue_jumps) = {
                    let info = self.loop_stack.last().unwrap();
                    (info.break_jumps.clone(), info.continue_jumps.clone())
                };
                for bj in &break_jumps {
                    self.patch(*bj, exit_pc);
                }
                for cj in &continue_jumps {
                    self.patch(*cj, start);
                }
                self.loop_stack.pop();
            }
            Expr::BreakStmt(label, line, col) => {
                let idx = match label {
                    Some(ref l) => self
                        .loop_stack
                        .iter()
                        .rposition(|info| info.label.as_ref() == Some(l)),
                    None => self.loop_stack.len().checked_sub(1),
                };
                if idx.is_none() {
                    return Err(format!(
                        "VX Error [line {}, col {}]: break outside loop",
                        line, col
                    ));
                }
                let bj = self.emit(OpCode::Jump, BytecodeArg::None);
                self.loop_stack[idx.unwrap()].break_jumps.push(bj);
            }
            Expr::ContinueStmt(label, line, col) => {
                let idx = match label {
                    Some(ref l) => self
                        .loop_stack
                        .iter()
                        .rposition(|info| info.label.as_ref() == Some(l)),
                    None => self.loop_stack.len().checked_sub(1),
                };
                if idx.is_none() {
                    return Err(format!(
                        "VX Error [line {}, col {}]: continue outside loop",
                        line, col
                    ));
                }
                let cj = self.emit(OpCode::Jump, BytecodeArg::None);
                self.loop_stack[idx.unwrap()].continue_jumps.push(cj);
            }
            Expr::ReturnStmt(val, _, _) => {
                if let Some(v) = val {
                    self.compile_expr(v)?;
                } else {
                    self.emit(OpCode::LoadNil, BytecodeArg::None);
                }
                self.emit(OpCode::Return, BytecodeArg::None);
            }
            // FreeStmt 已裁减 → mem::free(ptr) 标准库函数调用, 由 CallExpr 分支处理
            // 不可达: parse_statement 不会产生其他 Expr 变体作为顶层语句
            _ => {}
        }
        Ok(())
    }
}
