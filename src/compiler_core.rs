// ==================== 编译器核心 ====================

use std::collections::HashMap;
use std::fs;
use std::io;
use std::process;

use vx_vm::parser::{Expr, Stmt};
use vx_vm::OpCode;
use crate::compiler_bytecode::{BytecodeArg, Instruction, BytecodeFunction, ConstantValue, CompiledModule};
use vx_vm::bytecode;

pub struct LoopInfo {
    pub start: usize,
    pub break_jumps: Vec<usize>,
    pub continue_jumps: Vec<usize>,
}

#[derive(Debug, Clone, Copy, PartialEq)]
pub enum KnownType {
    Int,
    Float,
    Bool,
    String,
    Unknown,
}

pub struct Compiler {
    vxmodel: HashMap<String, String>,
    constants: Vec<ConstantValue>,
    instructions: Vec<Instruction>,
    functions: Vec<BytecodeFunction>,
    loop_stack: Vec<LoopInfo>,
    for_counter: usize,
    var_types: HashMap<String, KnownType>,
    stack_types: Vec<KnownType>,
}

impl Compiler {
    pub fn new(vxmodel: HashMap<String, String>) -> Self {
        Self {
            vxmodel,
            constants: Vec::new(),
            instructions: Vec::new(),
            functions: Vec::new(),
            loop_stack: Vec::new(),
            for_counter: 0,
            var_types: HashMap::new(),
            stack_types: Vec::new(),
        }
    }

    fn push_stack_type(&mut self, t: KnownType) {
        self.stack_types.push(t);
    }

    fn pop_stack_type(&mut self) -> KnownType {
        self.stack_types.pop().unwrap_or(KnownType::Unknown)
    }



    fn set_var_type(&mut self, name: &str, t: KnownType) {
        self.var_types.insert(name.to_string(), t);
    }

    fn get_var_type(&self, name: &str) -> KnownType {
        self.var_types.get(name).copied().unwrap_or(KnownType::Unknown)
    }

    fn binary_op_specialized(&self, op: &str, left: KnownType, right: KnownType) -> Option<OpCode> {
        match (op, left, right) {
            ("+", KnownType::Int, KnownType::Int) => Some(OpCode::AddInt),
            ("+", KnownType::Float, KnownType::Float) => Some(OpCode::AddFloat),
            ("-", KnownType::Int, KnownType::Int) => Some(OpCode::SubInt),
            ("-", KnownType::Float, KnownType::Float) => Some(OpCode::SubFloat),
            ("*", KnownType::Int, KnownType::Int) => Some(OpCode::MulInt),
            ("*", KnownType::Float, KnownType::Float) => Some(OpCode::MulFloat),
            ("/", KnownType::Int, KnownType::Int) => Some(OpCode::DivInt),
            ("/", KnownType::Float, KnownType::Float) => Some(OpCode::DivFloat),
            ("%", KnownType::Int, KnownType::Int) => Some(OpCode::ModInt),
            ("==", KnownType::Int, KnownType::Int) => Some(OpCode::EqInt),
            ("==", KnownType::Float, KnownType::Float) => Some(OpCode::EqFloat),
            ("<", KnownType::Int, KnownType::Int) => Some(OpCode::LtInt),
            ("<", KnownType::Float, KnownType::Float) => Some(OpCode::LtFloat),
            (">", KnownType::Int, KnownType::Int) => Some(OpCode::GtInt),
            (">", KnownType::Float, KnownType::Float) => Some(OpCode::GtFloat),
            ("<=", KnownType::Int, KnownType::Int) => Some(OpCode::LeInt),
            ("<=", KnownType::Float, KnownType::Float) => Some(OpCode::LeFloat),
            (">=", KnownType::Int, KnownType::Int) => Some(OpCode::GeInt),
            (">=", KnownType::Float, KnownType::Float) => Some(OpCode::GeFloat),
            ("&&", KnownType::Bool, KnownType::Bool) => Some(OpCode::And),
            ("||", KnownType::Bool, KnownType::Bool) => Some(OpCode::Or),
            _ => None,
        }
    }

    fn unary_op_specialized(&self, op: &str, operand: KnownType) -> Option<OpCode> {
        match (op, operand) {
            ("-", KnownType::Int) => Some(OpCode::NegInt),
            ("-", KnownType::Float) => Some(OpCode::NegFloat),
            ("!", KnownType::Bool) => Some(OpCode::Not),
            _ => None,
        }
    }
    fn add_const(&mut self, v: ConstantValue) -> usize {
        self.constants.push(v.clone());
        self.constants.len() - 1
    }
    fn emit(&mut self, op: OpCode, arg: BytecodeArg) -> usize {
        self.instructions.push(Instruction { op, arg });
        self.instructions.len() - 1
    }
    fn patch(&mut self, pos: usize, tgt: usize) {
        if let Some(inst) = self.instructions.get_mut(pos) {
            inst.arg = match &inst.arg {
                BytecodeArg::None => BytecodeArg::Int(tgt as i32),
                _ => BytecodeArg::Int(tgt as i32),
            };
        }
    }
    pub fn compile_expr(&mut self, e: &Expr) {
        match e {
            Expr::IntLiteral(v, _, _) => {
                let idx = self.add_const(ConstantValue::Int(*v)) as i32;
                self.emit(OpCode::LoadConst, BytecodeArg::Int(idx));
                self.push_stack_type(KnownType::Int);
            }
            Expr::FloatLiteral(v, _, _) => {
                let idx = self.add_const(ConstantValue::Float(*v)) as i32;
                self.emit(OpCode::LoadConst, BytecodeArg::Int(idx));
                self.push_stack_type(KnownType::Float);
            }
            Expr::StringLiteral(v, _, _) => {
                let idx = self.add_const(ConstantValue::String(v.clone())) as i32;
                self.emit(OpCode::LoadConst, BytecodeArg::Int(idx));
                self.push_stack_type(KnownType::String);
            }
            Expr::BoolLiteral(v, _, _) => {
                if *v {
                    self.emit(OpCode::LoadTrue, BytecodeArg::None);
                } else {
                    self.emit(OpCode::LoadFalse, BytecodeArg::None);
                }
                self.push_stack_type(KnownType::Bool);
            }
            Expr::NilLiteral(_, _) => {
                self.emit(OpCode::LoadNil, BytecodeArg::None);
                self.push_stack_type(KnownType::Unknown);
            }
            Expr::Identifier(name, _, _) => match name.as_str() {
                "sys_argv" => {
                    self.emit(OpCode::SysArgv, BytecodeArg::None);
                    self.push_stack_type(KnownType::Unknown);
                }
                "os_system" => {
                    self.emit(OpCode::System, BytecodeArg::None);
                    self.push_stack_type(KnownType::Int);
                }
                "file_read" => {
                    self.emit(OpCode::FileRead, BytecodeArg::None);
                    self.push_stack_type(KnownType::String);
                }
                "file_write" => {
                    self.emit(OpCode::FileWrite, BytecodeArg::None);
                    self.push_stack_type(KnownType::Unknown);
                }
                "file_exists" => {
                    self.emit(OpCode::FileExists, BytecodeArg::None);
                    self.push_stack_type(KnownType::Bool);
                }
                _ => {
                    let var_type = self.get_var_type(name);
                    self.emit(OpCode::LoadVar, BytecodeArg::String(name.clone()));
                    self.push_stack_type(var_type);
                }
            },
            Expr::BinaryOp(op, left, right, _, _) => {
                self.compile_expr(left);
                self.compile_expr(right);
                let right_type = self.pop_stack_type();
                let left_type = self.pop_stack_type();
                let oc = self.binary_op_specialized(op, left_type, right_type)
                    .unwrap_or_else(|| match op.as_ref() {
                        "+" => OpCode::BinaryAdd,
                        "-" => OpCode::BinarySub,
                        "*" => OpCode::BinaryMul,
                        "/" => OpCode::BinaryDiv,
                        "%" => OpCode::BinaryMod,
                        "^" => OpCode::BinaryPow,
                        "==" => OpCode::BinaryEq,
                        "!=" => OpCode::BinaryNe,
                        "<" => OpCode::BinaryLt,
                        ">" => OpCode::BinaryGt,
                        "<=" => OpCode::BinaryLe,
                        ">=" => OpCode::BinaryGe,
                        "&&" => OpCode::BinaryAnd,
                        "||" => OpCode::BinaryOr,
                        _ => {
                            eprintln!("VX Error: 未知的二元操作符: {}", op);
                            process::exit(1);
                        }
                    });
                self.emit(oc, BytecodeArg::None);
                let result_type = match (op.as_ref(), left_type, right_type) {
                    ("+", KnownType::Int, KnownType::Int) => KnownType::Int,
                    ("+", KnownType::Float, KnownType::Float) => KnownType::Float,
                    ("-", KnownType::Int, KnownType::Int) => KnownType::Int,
                    ("-", KnownType::Float, KnownType::Float) => KnownType::Float,
                    ("*", KnownType::Int, KnownType::Int) => KnownType::Int,
                    ("*", KnownType::Float, KnownType::Float) => KnownType::Float,
                    ("/", KnownType::Int, KnownType::Int) => KnownType::Int,
                    ("/", KnownType::Float, KnownType::Float) => KnownType::Float,
                    ("%", KnownType::Int, KnownType::Int) => KnownType::Int,
                    ("==" | "!=" | "<" | ">" | "<=" | ">=", KnownType::Int, KnownType::Int) => KnownType::Bool,
                    ("==" | "!=" | "<" | ">" | "<=" | ">=", KnownType::Float, KnownType::Float) => KnownType::Bool,
                    ("&&" | "||", KnownType::Bool, KnownType::Bool) => KnownType::Bool,
                    _ => KnownType::Unknown,
                };
                self.push_stack_type(result_type);
            }
            Expr::UnaryOp(op, operand, _, _) => {
                self.compile_expr(operand);
                let operand_type = self.pop_stack_type();
                let oc = self.unary_op_specialized(&**op, operand_type)
                    .unwrap_or_else(|| {
                        if &**op == "-" {
                            OpCode::UnaryNeg
                        } else {
                            OpCode::UnaryNot
                        }
                    });
                self.emit(oc, BytecodeArg::None);
                let result_type = match (&**op, operand_type) {
                    ("-", KnownType::Int) => KnownType::Int,
                    ("-", KnownType::Float) => KnownType::Float,
                    ("!", KnownType::Bool) => KnownType::Bool,
                    _ => KnownType::Unknown,
                };
                self.push_stack_type(result_type);
            }
            Expr::CallExpr(callee, args, _, _) => {
                // 内置函数特殊处理: os_system / file_read / file_write / file_exists
                // 这些标识符对应的 OpCode 期望参数已在栈上，因此需先编译参数再发射 OpCode
                if let Expr::Identifier(name, _, _) = callee.as_ref() {
                    let builtin_op = match name.as_str() {
                        "os_system" => Some(OpCode::System),
                        "file_read" => Some(OpCode::FileRead),
                        "file_write" => Some(OpCode::FileWrite),
                        "file_exists" => Some(OpCode::FileExists),
                        _ => None,
                    };
                    if let Some(op) = builtin_op {
                        // 先编译参数（将参数推入栈），再发射对应的 OpCode
                        // OpCode::System/FileRead/FileWrite 会从栈顶弹出参数
                        for a in args {
                            self.compile_expr(a);
                        }
                        self.emit(op, BytecodeArg::None);
                        return;
                    }
                }

                if let Expr::PropertyAccess(obj, prop, _, _) = callee.as_ref() {
                    self.compile_expr(obj);
                    let idx = self.add_const(ConstantValue::String(prop.clone())) as i32;
                    self.emit(OpCode::LoadConst, BytecodeArg::Int(idx));
                    for a in args {
                        self.compile_expr(a);
                    }
                    self.emit(OpCode::Call, BytecodeArg::Int((1 + args.len()) as i32));
                } else {
                    self.compile_expr(callee);
                    for a in args {
                        self.compile_expr(a);
                    }
                    self.emit(OpCode::Call, BytecodeArg::Int(args.len() as i32));
                }
            }
            Expr::IndexAccess(obj, index, _, _) => {
                self.compile_expr(obj);
                self.compile_expr(index);
                self.emit(OpCode::IndexGet, BytecodeArg::None);
            }
            Expr::PropertyAccess(obj, prop, _, _) => {
                self.compile_expr(obj);
                self.emit(OpCode::PropertyGet, BytecodeArg::String(prop.clone()));
            }
            Expr::ArrayLiteral(elements, _, _) => {
                for x in elements {
                    self.compile_expr(x);
                }
                self.emit(OpCode::MakeArray, BytecodeArg::Int(elements.len() as i32));
            }
            Expr::MapLiteral(pairs, _, _) => {
                for (k, v) in pairs {
                    self.compile_expr(k);
                    self.compile_expr(v);
                }
                self.emit(OpCode::MakeMap, BytecodeArg::Int(pairs.len() as i32));
            }
            Expr::NewExpr(type_name, _, args, _, _) => {
                let idx = self.add_const(ConstantValue::String(type_name.clone())) as i32;
                self.emit(OpCode::LoadConst, BytecodeArg::Int(idx));
                for a in args {
                    self.compile_expr(a);
                }
                self.emit(OpCode::Call, BytecodeArg::Int(args.len() as i32));
            }
            Expr::NewzExpr(type_name, _, args, _, _) => {
                let idx = self.add_const(ConstantValue::String(type_name.clone())) as i32;
                self.emit(OpCode::LoadConst, BytecodeArg::Int(idx));
                for a in args {
                    self.compile_expr(a);
                }
                self.emit(OpCode::Newz, BytecodeArg::Int(args.len() as i32));
            }
            Expr::MoveExpr(target, _, _) => {
                self.compile_expr(target);
                self.emit(OpCode::OwnershipMove, BytecodeArg::None);
            }
            Expr::AddressOf(operand, _, _) => {
                self.compile_expr(operand);
                self.emit(OpCode::BorrowCheck, BytecodeArg::None);
                self.emit(OpCode::AddressOf, BytecodeArg::None);
            }
            Expr::Deref(operand, _, _) => {
                self.compile_expr(operand);
                self.emit(OpCode::AliveCheck, BytecodeArg::None);
                self.emit(OpCode::Deref, BytecodeArg::None);
            }
            Expr::PointerMember(obj, member, _, _) => {
                self.compile_expr(obj);
                self.emit(OpCode::AliveCheck, BytecodeArg::None);
                self.emit(OpCode::PropertyGet, BytecodeArg::String(member.clone()));
            }
            // 表达式位置不应出现语句级节点: 解析器保证此处不可达。
            // 添加新变体时编译器会报非穷尽 match 错误，强制显式处理。
            Expr::StructDecl(..)
            | Expr::ClassDecl(..)
            | Expr::EnumDecl(..)
            | Expr::UnionDecl(..)
            | Expr::FuncDecl(..)
            | Expr::ImportStmt(..)
            | Expr::VectorLiteral(..)
            | Expr::TypeExpr(..)
            | Expr::ExprStmt(..)
            | Expr::VarDecl(..)
            | Expr::Assign(..)
            | Expr::IfStmt(..)
            | Expr::WhileStmt(..)
            | Expr::ForStmt(..)
            | Expr::BreakStmt(..)
            | Expr::ContinueStmt(..)
            | Expr::ReturnStmt(..)
            | Expr::FreeStmt(..) => {}
        }
    }
    pub fn compile_assign(&mut self, target: &Expr, op: &str, value: &Expr) {
        if op == "=" {
            match target {
                Expr::Identifier(name, _, _) => {
                    self.compile_expr(value);
                    let value_type = self.pop_stack_type();
                    self.set_var_type(name, value_type);
                    self.emit(OpCode::StoreVar, BytecodeArg::String(name.clone()));
                }
                Expr::IndexAccess(obj, index, _, _) => {
                    self.compile_expr(value);
                    self.compile_expr(obj);
                    self.compile_expr(index);
                    self.emit(OpCode::IndexSet, BytecodeArg::None);
                }
                Expr::PropertyAccess(obj, prop, _, _) => {
                    self.compile_expr(value);
                    self.compile_expr(obj);
                    self.emit(OpCode::PropertySet, BytecodeArg::String(prop.clone()));
                    self.emit(OpCode::Pop, BytecodeArg::None);
                }
                _ => {}
            }
        } else {
            let bin_op = match op {
                "+=" => "+",
                "-=" => "-",
                "*=" => "*",
                "/=" => "/",
                "%=" => "%",
                "^=" => "^",
                _ => op,
            };
            match target {
                Expr::Identifier(name, _, _) => {
                    self.emit(OpCode::LoadVar, BytecodeArg::String(name.clone()));
                    let var_type = self.get_var_type(name);
                    self.push_stack_type(var_type);
                    self.compile_expr(value);
                    let value_type = self.pop_stack_type();
                    let oc = self.binary_op_specialized(bin_op, var_type, value_type)
                        .unwrap_or_else(|| match bin_op {
                            "+" => OpCode::BinaryAdd,
                            "-" => OpCode::BinarySub,
                            "*" => OpCode::BinaryMul,
                            "/" => OpCode::BinaryDiv,
                            "%" => OpCode::BinaryMod,
                            "^" => OpCode::BinaryPow,
                            _ => {
                                eprintln!("VX Error: 未知的二元操作符: {}", bin_op);
                                process::exit(1);
                            }
                        });
                    self.emit(oc, BytecodeArg::None);
                    let result_type = match (bin_op, var_type, value_type) {
                        ("+", KnownType::Int, KnownType::Int) => KnownType::Int,
                        ("+", KnownType::Float, KnownType::Float) => KnownType::Float,
                        ("-", KnownType::Int, KnownType::Int) => KnownType::Int,
                        ("-", KnownType::Float, KnownType::Float) => KnownType::Float,
                        ("*", KnownType::Int, KnownType::Int) => KnownType::Int,
                        ("*", KnownType::Float, KnownType::Float) => KnownType::Float,
                        ("/", KnownType::Int, KnownType::Int) => KnownType::Int,
                        ("/", KnownType::Float, KnownType::Float) => KnownType::Float,
                        ("%", KnownType::Int, KnownType::Int) => KnownType::Int,
                        _ => KnownType::Unknown,
                    };
                    self.set_var_type(name, result_type);
                    self.emit(OpCode::StoreVar, BytecodeArg::String(name.clone()));
                }
                Expr::IndexAccess(obj, index, _, _) => {
                    self.compile_expr(obj);
                    self.compile_expr(index);
                    self.emit(OpCode::IndexGet, BytecodeArg::None);
                    self.compile_expr(value);
                    let oc = match bin_op {
                        "+" => OpCode::BinaryAdd,
                        "-" => OpCode::BinarySub,
                        "*" => OpCode::BinaryMul,
                        "/" => OpCode::BinaryDiv,
                        "%" => OpCode::BinaryMod,
                        "^" => OpCode::BinaryPow,
                        _ => {
                            eprintln!("VX Error: 未知的二元操作符: {}", bin_op);
                            process::exit(1);
                        }
                    };
                    self.emit(oc, BytecodeArg::None);
                    let tmp = format!("__asg_v_{}", self.instructions.len());
                    self.emit(OpCode::StoreVar, BytecodeArg::String(tmp.clone()));
                    self.compile_expr(obj);
                    self.compile_expr(index);
                    self.emit(OpCode::LoadVar, BytecodeArg::String(tmp));
                    self.emit(OpCode::IndexSet, BytecodeArg::None);
                }
                Expr::PropertyAccess(obj, prop, _, _) => {
                    self.compile_expr(obj);
                    self.emit(OpCode::PropertyGet, BytecodeArg::String(prop.clone()));
                    self.compile_expr(value);
                    let oc = match bin_op {
                        "+" => OpCode::BinaryAdd,
                        "-" => OpCode::BinarySub,
                        "*" => OpCode::BinaryMul,
                        "/" => OpCode::BinaryDiv,
                        "%" => OpCode::BinaryMod,
                        "^" => OpCode::BinaryPow,
                        _ => {
                            eprintln!("VX Error: 未知的二元操作符: {}", bin_op);
                            process::exit(1);
                        }
                    };
                    self.emit(oc, BytecodeArg::None);
                    let tmp = format!("__asg_v_{}", self.instructions.len());
                    self.emit(OpCode::StoreVar, BytecodeArg::String(tmp.clone()));
                    self.compile_expr(obj);
                    self.emit(OpCode::PropertySet, BytecodeArg::String(prop.clone()));
                    self.emit(OpCode::Pop, BytecodeArg::None);
                }
                _ => {}
            }
        }
    }

    pub fn compile_stmt(&mut self, s: &Stmt) {
        match s {
            Expr::ExprStmt(expr, _, _) => {
                if let Expr::Assign(ref target, ref op, ref value, _, _) = **expr {
                    self.compile_assign(target, op, value);
                } else {
                    self.compile_expr(expr);
                }
            }
            Expr::VarDecl(name, _, value, _, _, _) => {
                self.compile_expr(value);
                let value_type = self.pop_stack_type();
                self.set_var_type(name, value_type);
                self.emit(OpCode::DefineVar, BytecodeArg::String(name.clone()));
            }
            Expr::IfStmt(cond, body, elifs, else_body, _, _) => {
                self.compile_expr(cond);
                let jump_to_elif = self.emit(OpCode::JumpIfFalse, BytecodeArg::None);
                for x in body {
                    self.compile_stmt(x);
                }
                let mut exit_jumps: Vec<usize> = Vec::new();
                exit_jumps.push(self.emit(OpCode::Jump, BytecodeArg::None));
                self.patch(jump_to_elif, self.instructions.len());
                for (c, b) in elifs {
                    self.compile_expr(c);
                    let jump_to_next = self.emit(OpCode::JumpIfFalse, BytecodeArg::None);
                    for x in b {
                        self.compile_stmt(x);
                    }
                    exit_jumps.push(self.emit(OpCode::Jump, BytecodeArg::None));
                    self.patch(jump_to_next, self.instructions.len());
                }
                if let Some(b) = else_body {
                    for x in b {
                        self.compile_stmt(x);
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
                });
                self.compile_expr(cond);
                let exit_j = self.emit(OpCode::JumpIfFalse, BytecodeArg::None);
                for x in body {
                    self.compile_stmt(x);
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
                self.compile_expr(iter);
                self.emit(OpCode::DefineVar, BytecodeArg::String(src_var.clone()));
                let const_0 = self.add_const(ConstantValue::Int(0)) as i32;
                self.emit(OpCode::LoadConst, BytecodeArg::Int(const_0));
                self.emit(OpCode::DefineVar, BytecodeArg::String(idx_var.clone()));
                let start = self.instructions.len();
                self.loop_stack.push(LoopInfo {
                    start,
                    break_jumps: Vec::new(),
                    continue_jumps: Vec::new(),
                });
                self.emit(OpCode::LoadVar, BytecodeArg::String(idx_var.clone()));
                self.emit(OpCode::LoadVar, BytecodeArg::String(src_var.clone()));
                let const_len = self.add_const(ConstantValue::String("len".into())) as i32;
                self.emit(OpCode::LoadConst, BytecodeArg::Int(const_len));
                self.emit(OpCode::Call, BytecodeArg::Int(1));
                self.emit(OpCode::BinaryLt, BytecodeArg::None);
                let exit_j = self.emit(OpCode::JumpIfFalse, BytecodeArg::None);
                self.emit(OpCode::LoadVar, BytecodeArg::String(src_var.clone()));
                self.emit(OpCode::LoadVar, BytecodeArg::String(idx_var.clone()));
                self.emit(OpCode::IndexGet, BytecodeArg::None);
                self.emit(OpCode::DefineVar, BytecodeArg::String(var.clone()));
                for x in body {
                    self.compile_stmt(x);
                }
                let cont_pc = self.instructions.len();
                self.loop_stack.last_mut().unwrap().start = cont_pc;
                self.emit(OpCode::LoadVar, BytecodeArg::String(idx_var.clone()));
                let const_1 = self.add_const(ConstantValue::Int(1)) as i32;
                self.emit(OpCode::LoadConst, BytecodeArg::Int(const_1));
                self.emit(OpCode::BinaryAdd, BytecodeArg::None);
                self.emit(OpCode::StoreVar, BytecodeArg::String(idx_var));
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
            Expr::BreakStmt(line, col) => {
                if self.loop_stack.is_empty() {
                    eprintln!("VX Error [line {}, col {}]: break outside loop", line, col);
                    process::exit(1);
                }
                let bj = self.emit(OpCode::Jump, BytecodeArg::None);
                self.loop_stack.last_mut().unwrap().break_jumps.push(bj);
            }
            Expr::ContinueStmt(line, col) => {
                if self.loop_stack.is_empty() {
                    eprintln!("VX Error [line {}, col {}]: continue outside loop", line, col);
                    process::exit(1);
                }
                let cj = self.emit(OpCode::Jump, BytecodeArg::None);
                self.loop_stack
                    .last_mut()
                    .unwrap()
                    .continue_jumps
                    .push(cj);
            }
            Expr::ReturnStmt(val, _, _) => {
                if let Some(v) = val {
                    self.compile_expr(v);
                } else {
                    self.emit(OpCode::LoadNil, BytecodeArg::None);
                }
                self.emit(OpCode::Return, BytecodeArg::None);
            }
            Expr::FreeStmt(target, _, _) => {
                self.compile_expr(target);
                self.emit(OpCode::Free, BytecodeArg::None);
            }
            // 不可达: parse_statement 不会产生其他 Expr 变体作为顶层语句
            _ => {}
        }
    }
    pub fn compile(&mut self, ast: &[Stmt]) -> CompiledModule {
        self.constants.clear();
        self.instructions.clear();
        self.functions.clear();
        self.loop_stack.clear();
        self.for_counter = 0;
        let mut structs = Vec::new();
        let mut classes = Vec::new();

        for s in ast {
            match s {
                Expr::StructDecl(name, fields, _, _, _) => {
                    structs.push((name.clone(), fields.iter().map(|f| f.1.clone()).collect()));
                    let save = std::mem::replace(&mut self.instructions, Vec::new());
                    self.emit(OpCode::MakeStruct, BytecodeArg::String(name.clone()));
                    for (_, fname) in fields {
                        self.emit(OpCode::Dup, BytecodeArg::None);
                        self.emit(OpCode::LoadVar, BytecodeArg::String(fname.clone()));
                        self.emit(OpCode::PropertySet, BytecodeArg::String(fname.clone()));
                        self.emit(OpCode::Pop, BytecodeArg::None);
                    }
                    self.emit(OpCode::Return, BytecodeArg::None);
                    self.functions.push(BytecodeFunction {
                        name: name.clone(),
                        instructions: std::mem::replace(&mut self.instructions, save),
                        num_params: fields.len(),
                        has_return: true,
                        param_names: fields.iter().map(|f| f.1.clone()).collect(),
                    });
                    let name_const = self.add_const(ConstantValue::String(name.clone())) as i32;
                    self.emit(OpCode::LoadConst, BytecodeArg::Int(name_const));
                    self.emit(OpCode::StoreVar, BytecodeArg::String(name.clone()));
                }
                Expr::ClassDecl(name, fields, methods, _, _, _, _) => {
                    classes.push((name.clone(), fields.iter().map(|f| f.1.clone()).collect()));
                    let save = std::mem::replace(&mut self.instructions, Vec::new());
                    self.emit(OpCode::MakeClass, BytecodeArg::String(name.clone()));
                    for f in fields {
                        self.emit(OpCode::Dup, BytecodeArg::None);
                        self.emit(OpCode::LoadVar, BytecodeArg::String(f.1.clone()));
                        self.emit(OpCode::PropertySet, BytecodeArg::String(f.1.clone()));
                        self.emit(OpCode::Pop, BytecodeArg::None);
                    }
                    self.emit(OpCode::Return, BytecodeArg::None);
                    self.functions.push(BytecodeFunction {
                        name: name.clone(),
                        instructions: std::mem::replace(&mut self.instructions, save),
                        num_params: fields.len(),
                        has_return: true,
                        param_names: fields.iter().map(|f| f.1.clone()).collect(),
                    });
                    for m in methods {
                        if let Expr::FuncDecl(mname, params, _, mbody, _, _) = m.as_ref() {
                            let msave = std::mem::replace(&mut self.instructions, Vec::new());
                            let save_var_types = self.var_types.clone();
                            self.var_types.clear();
                            for (pname, ptype) in params {
                                let known_type = match ptype.as_str() {
                                    "int" => KnownType::Int,
                                    "float" => KnownType::Float,
                                    "bool" => KnownType::Bool,
                                    "string" => KnownType::String,
                                    _ => KnownType::Unknown,
                                };
                                self.var_types.insert(pname.clone(), known_type);
                            }
                            for x in mbody {
                                self.compile_stmt(x);
                            }
                            self.var_types = save_var_types;
                            if !mbody
                                .iter()
                                .any(|x| matches!(&**x, Expr::ReturnStmt(_, _, _)))
                            {
                                self.emit(OpCode::LoadNil, BytecodeArg::None);
                                self.emit(OpCode::Return, BytecodeArg::None);
                            }
                            let method_name = format!("{}_{}", name, mname);
                            self.functions.push(BytecodeFunction {
                                name: method_name,
                                instructions: std::mem::replace(&mut self.instructions, msave),
                                num_params: params.len(),
                                has_return: true,
                                param_names: params.iter().map(|p| p.0.clone()).collect(),
                            });
                            let mname_const = self
                                .add_const(ConstantValue::String(format!("{}_{}", name, mname)))
                                as i32;
                            self.emit(OpCode::LoadConst, BytecodeArg::Int(mname_const));
                            self.emit(
                                OpCode::StoreVar,
                                BytecodeArg::String(format!("{}_{}", name, mname)),
                            );
                        }
                    }
                }
                Expr::EnumDecl(_, _, _, _) => {}
                Expr::UnionDecl(_, _, _, _) => {}
                Expr::ImportStmt(name, alias, _dirs, _, _) => {
                    let lib_path = self.vxmodel.get(name).cloned();
                    self.emit(
                        OpCode::Import,
                        BytecodeArg::ImportTuple(name.clone(), alias.clone(), lib_path),
                    );
                }
                Expr::FuncDecl(fname, params, _, body, _, _) => {
                    let save = std::mem::replace(&mut self.instructions, Vec::new());
                    let save_var_types = self.var_types.clone();
                    self.var_types.clear();
                    for (pname, ptype) in params {
                        let known_type = match ptype.as_str() {
                            "int" => KnownType::Int,
                            "float" => KnownType::Float,
                            "bool" => KnownType::Bool,
                            "string" => KnownType::String,
                            _ => KnownType::Unknown,
                        };
                        self.var_types.insert(pname.clone(), known_type);
                    }
                    for x in body {
                        self.compile_stmt(x);
                    }
                    self.var_types = save_var_types;
                    if !body
                        .iter()
                        .any(|x| matches!(&**x, Expr::ReturnStmt(_, _, _)))
                    {
                        self.emit(OpCode::LoadNil, BytecodeArg::None);
                        self.emit(OpCode::Return, BytecodeArg::None);
                    }
                    self.functions.push(BytecodeFunction {
                        name: fname.clone(),
                        instructions: std::mem::replace(&mut self.instructions, save),
                        num_params: params.len(),
                        has_return: true,
                        param_names: params.iter().map(|p| p.0.clone()).collect(),
                    });
                    let fname_const = self.add_const(ConstantValue::String(fname.clone())) as i32;
                    self.emit(OpCode::LoadConst, BytecodeArg::Int(fname_const));
                    self.emit(OpCode::StoreVar, BytecodeArg::String(fname.clone()));
                }
                _ => {
                    self.compile_stmt(s);
                }
            }
        }
        if !self.instructions.is_empty() {
            self.emit(OpCode::LoadNil, BytecodeArg::None);
            self.emit(OpCode::Return, BytecodeArg::None);
            self.functions.insert(
                0,
                BytecodeFunction {
                    name: "__main__".into(),
                    instructions: std::mem::replace(&mut self.instructions, Vec::new()),
                    num_params: 0,
                    has_return: false,
                    param_names: Vec::new(),
                },
            );
        }
        CompiledModule {
            functions: std::mem::replace(&mut self.functions, Vec::new()),
            constants: std::mem::replace(&mut self.constants, Vec::new()),
            structs,
            classes,
        }
    }
    pub fn save(&self, der: &CompiledModule, path: &str) -> io::Result<()> {
        use std::io::BufWriter;

        let mut f = BufWriter::new(fs::File::create(path)?);

        let constants: Vec<bytecode::SerializedConstant> = der
            .constants
            .iter()
            .map(|c| match c {
                ConstantValue::Nil => bytecode::SerializedConstant::Nil,
                ConstantValue::Bool(b) => bytecode::SerializedConstant::Bool(*b),
                ConstantValue::Int(v) => bytecode::SerializedConstant::Int(*v),
                ConstantValue::Float(v) => bytecode::SerializedConstant::Float(*v),
                ConstantValue::String(s) => bytecode::SerializedConstant::String(s.clone()),
            })
            .collect();

        let mut struct_map = HashMap::new();
        for (n, f) in &der.structs {
            struct_map.insert(n.clone(), f.clone());
        }
        for (n, f) in &der.classes {
            struct_map.insert(n.clone(), f.clone());
        }

        let mut func_data: Vec<(
            &str,
            u32,
            bool,
            Vec<String>,
            Vec<(u8, u8, Option<i32>, Option<String>)>,
        )> = Vec::with_capacity(der.functions.len());

        let mut temp_strings = Vec::new();
        for fn_ in &der.functions {
            let mut insts: Vec<(u8, u8, Option<i32>, Option<String>)> =
                Vec::with_capacity(fn_.instructions.len());
            for inst in &fn_.instructions {
                let (arg_type, iarg, sarg) = match &inst.arg {
                    BytecodeArg::None => (0, None, None),
                    BytecodeArg::Int(v) => (1, Some(*v), None),
                    BytecodeArg::String(s) => (2, None, Some(s.clone())),
                    BytecodeArg::ImportTuple(a, b, c) => {
                        let s = format!(
                            "{},{},{}",
                            b.as_deref().unwrap_or(""),
                            c.as_deref().unwrap_or(""),
                            a,
                        );
                        temp_strings.push(s.clone());
                        (2, None, Some(s))
                    }
                };
                insts.push((inst.op as u8, arg_type, iarg, sarg));
            }
            func_data.push((
                fn_.name.as_str(),
                fn_.num_params as u32,
                fn_.has_return,
                fn_.param_names.clone(),
                insts,
            ));
        }

        let func_refs: Vec<(
            &str,
            u32,
            bool,
            &[String],
            &[(u8, u8, Option<i32>, Option<String>)],
        )> = func_data
            .iter()
            .map(|(name, np, hr, pn, insts)| {
                (*name, *np, *hr, pn.as_slice(), insts.as_slice())
            })
            .collect();

        bytecode::write_vxobj(&mut f, &constants, &func_refs, &struct_map)
    }
}
