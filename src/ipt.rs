// VX Language Compiler v3.0 (Rust Port)
// Token 处理和 AST 解析模块已拆分到 token.rs 和 parser.rs

use std::collections::HashMap;
use std::env;
use std::fs;
use std::path::Path;
use std::process;
use std::io;

use vx_vm::bytecode;

// 引入拆分的模块
mod token;
mod parser;

use token::Lexer;
use parser::{Parser, Expr, Stmt};

// ==================== 字节码 ====================
#[repr(u8)]
#[derive(Debug, Clone, Copy, PartialEq)]
#[allow(dead_code)]
enum OpCode {
    LoadConst = 0x01,
    LoadNil = 0x02,
    LoadTrue = 0x03,
    LoadFalse = 0x04,
    LoadVar = 0x05,
    StoreVar = 0x06,
    DefineVar = 0x07,
    Call = 0x08,
    Return = 0x09,
    MakeFunction = 0x0a,
    Jump = 0x0b,
    JumpIfFalse = 0x0c,
    JumpIfTrue = 0x0d,
    Break = 0x0e,
    Continue = 0x0f,
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
    BinaryLe = 0x1a,
    BinaryGe = 0x1b,
    BinaryAnd = 0x1c,
    BinaryOr = 0x1d,
    UnaryNeg = 0x1e,
    UnaryNot = 0x1f,
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
    PropertyGet = 0x2a,
    PropertySet = 0x2b,
    AddressOf = 0x2c,
    Deref = 0x2d,
    PointerMember = 0x2e,
    Import = 0x2f,
    New = 0x30,
    Halt = 0x31,
    SysArgv = 0x32,
    System = 0x33,
    FileRead = 0x34,
    FileWrite = 0x35,
    FileExists = 0x36,
    Dup = 0x37,
    Pop = 0x38,
    Newz = 0x39,
    Free = 0x3a,
    OwnershipMove = 0x3b,
    ScopeDrop = 0x3c,
    BorrowCheck = 0x3d,
    AliveCheck = 0x3e,
}

#[derive(Debug, Clone)]
enum BytecodeArg {
    None,
    Int(i32),
    String(String),
    ImportTuple(String, Option<String>, Option<String>),
}

#[derive(Debug, Clone)]
struct Instruction {
    op: OpCode,
    arg: BytecodeArg,
}

#[derive(Debug, Clone)]
struct BytecodeFunction {
    name: String,
    instructions: Vec<Instruction>,
    num_params: usize,
    has_return: bool,
    param_names: Vec<String>,
}

#[derive(Debug, Clone)]
#[allow(dead_code)]
enum ConstantValue {
    Nil,
    Bool(bool),
    Int(i64),
    Float(f64),
    String(String),
}

struct CompiledModule {
    functions: Vec<BytecodeFunction>,
    constants: Vec<ConstantValue>,
    structs: Vec<(String, Vec<String>)>,
    classes: Vec<(String, Vec<String>)>,
}

// ==================== 所有权检查器 ====================
#[derive(Debug, Clone, PartialEq)]
enum OwnershipState {
    Owned,
    Moved,
    Borrowed,
    Freed,
}

struct OwnershipChecker {
    source: String,
    scopes: Vec<HashMap<String, OwnershipState>>,
    heap_vars: std::collections::HashSet<String>,
    borrows: HashMap<String, String>,
    errors: Vec<String>,
}

impl OwnershipChecker {
    fn new(source: &str) -> Self {
        Self {
            source: source.to_string(),
            scopes: vec![HashMap::new()],
            heap_vars: std::collections::HashSet::new(),
            borrows: HashMap::new(),
            errors: Vec::new(),
        }
    }
    fn push_scope(&mut self) {
        self.scopes.push(HashMap::new());
    }
    fn pop_scope(&mut self) {
        if self.scopes.len() <= 1 {
            return;
        }
        let scope = self.scopes.pop().unwrap();
        for (var, state) in &scope {
            if self.heap_vars.contains(var) && *state == OwnershipState::Owned {
                self.errors.push(format!(
                    "堆变量 '{}' 在作用域结束时未被显式释放（可能内存泄漏），请调用 free({})",
                    var, var
                ));
            }
        }
        let to_remove: Vec<String> = self
            .borrows
            .iter()
            .filter(|(_, o)| scope.contains_key(o.as_str()))
            .map(|(b, _)| b.clone())
            .collect();
        for b in to_remove {
            self.borrows.remove(&b);
        }
    }
    fn declare_var(&mut self, name: &str, is_heap: bool) {
        self.scopes
            .last_mut()
            .unwrap()
            .insert(name.to_string(), OwnershipState::Owned);
        if is_heap {
            self.heap_vars.insert(name.to_string());
        }
    }
    fn get_state(&self, name: &str) -> Option<OwnershipState> {
        for scope in self.scopes.iter().rev() {
            if let Some(s) = scope.get(name) {
                return Some(s.clone());
            }
        }
        None
    }
    fn set_state(&mut self, name: &str, state: OwnershipState) {
        for scope in self.scopes.iter_mut().rev() {
            if scope.contains_key(name) {
                scope.insert(name.to_string(), state);
                return;
            }
        }
    }
    fn check_use(&mut self, name: &str, line: usize, _col: usize) -> bool {
        if let Some(s) = self.get_state(name) {
            if s == OwnershipState::Moved {
                self.errors.push(format!(
                    "变量 '{}' 的所有权已被转移（use-after-move）\n {} | {}",
                    name,
                    line,
                    self.get_src_line(line)
                ));
                return false;
            }
            if s == OwnershipState::Freed {
                self.errors.push(format!(
                    "变量 '{}' 已被释放（use-after-free/悬垂指针）\n {} | {}",
                    name,
                    line,
                    self.get_src_line(line)
                ));
                return false;
            }
        }
        true
    }
    fn check_free(&mut self, name: &str, _line: usize, _col: usize) -> bool {
        if let Some(s) = self.get_state(name) {
            if s == OwnershipState::Moved {
                self.errors
                    .push(format!("变量 '{}' 所有权已转移，无法释放", name));
                return false;
            }
            if s == OwnershipState::Freed {
                self.errors
                    .push(format!("变量 '{}' 已被释放（双重释放/double-free）", name));
                return false;
            }
            if s == OwnershipState::Borrowed {
                self.errors.push(format!(
                    "变量 '{}' 存在活跃借用，无法释放（违反借用规则）",
                    name
                ));
                return false;
            }
            let active: Vec<_> = self
                .borrows
                .iter()
                .filter(|(_, o)| *o == name)
                .map(|(b, _)| b.clone())
                .collect();
            if !active.is_empty() {
                self.errors.push(format!(
                    "变量 '{}' 存在活跃借用 {:?}，无法释放",
                    name, active
                ));
                return false;
            }
            return true;
        }
        self.errors.push(format!("未定义的变量 '{}'", name));
        false
    }
    fn do_free(&mut self, name: &str) {
        self.set_state(name, OwnershipState::Freed);
        self.heap_vars.remove(name);
    }
    fn check_move(&mut self, src: &str, _line: usize, _col: usize) -> bool {
        if let Some(s) = self.get_state(src) {
            if s == OwnershipState::Moved {
                self.errors
                    .push(format!("变量 '{}' 所有权已转移，无法再次移动", src));
                return false;
            }
            if s == OwnershipState::Freed {
                self.errors
                    .push(format!("变量 '{}' 已被释放，无法移动", src));
                return false;
            }
            if s == OwnershipState::Borrowed {
                self.errors
                    .push(format!("变量 '{}' 存在活跃借用，无法移动", src));
                return false;
            }
            return true;
        }
        self.errors.push(format!("未定义的变量 '{}'", src));
        false
    }
    fn do_move(&mut self, src: &str, dst: &str) {
        self.set_state(src, OwnershipState::Moved);
        let is_heap = self.heap_vars.contains(src);
        self.declare_var(dst, is_heap);
    }
    fn do_borrow(&mut self, owner: &str, borrower: &str) {
        self.borrows.insert(borrower.to_string(), owner.to_string());
        self.set_state(owner, OwnershipState::Borrowed);
        self.declare_var(borrower, false); // borrower is a reference, not a heap owner
    }
    fn check_assign(&mut self, name: &str, value: &Expr, line: usize, col: usize) {
        match value {
            Expr::NewzExpr(_, _, _, _, _) => {
                if self.get_state(name) == Some(OwnershipState::Owned)
                    && self.heap_vars.contains(name)
                {
                    self.errors.push(format!(
                        "变量 '{}' 持有堆所有权，赋值前请先释放（内存泄漏风险）",
                        name
                    ));
                }
                self.declare_var(name, true);
            }
            Expr::Identifier(src, _, _) => {
                if self.get_state(src) == Some(OwnershipState::Owned)
                    && self.heap_vars.contains(src)
                {
                    self.do_move(src, name);
                } else {
                    self.check_use(src, line, col);
                }
            }
            Expr::MoveExpr(target, _, _) => {
                if let Expr::Identifier(src, _, _) = target.as_ref() {
                    self.do_move(src, name);
                } else {
                    self.errors.push("move 只能应用于标识符".to_string());
                }
            }
            Expr::AddressOf(operand, _, _) => {
                if let Expr::Identifier(src, _, _) = operand.as_ref() {
                    self.do_borrow(src, name);
                }
            }
            _ => {
                if self.get_state(name) == Some(OwnershipState::Owned)
                    && self.heap_vars.contains(name)
                {
                    self.errors.push(format!(
                        "变量 '{}' 持有堆所有权，覆盖赋值将导致内存泄漏",
                        name
                    ));
                }
            }
        }
    }
    fn get_src_line(&self, line: usize) -> String {
        if line > 0 && line <= self.source.lines().count() {
            self.source.lines().nth(line - 1).unwrap().to_string()
        } else {
            String::new()
        }
    }
    fn check_ast(&mut self, ast: &[Stmt]) {
        for stmt in ast {
            self._check_stmt(stmt);
        }
    }
    fn _check_stmt(&mut self, s: &Stmt) {
        match s {
            Expr::VarDecl(name, _, value, _, line, col) => match value.as_ref() {
                Expr::NewzExpr(_, _, _, _, _) => self.declare_var(name, true),
                Expr::Identifier(src, _, _) => {
                    if self.get_state(src) == Some(OwnershipState::Owned)
                        && self.heap_vars.contains(src)
                    {
                        self.do_move(src, name);
                    } else {
                        self.check_use(src, *line, *col);
                        self.declare_var(name, false);
                    }
                }
                Expr::MoveExpr(target, _, _) => {
                    if let Expr::Identifier(src, _, _) = target.as_ref() {
                        if self.check_move(src, *line, *col) {
                            self.do_move(src, name);
                        }
                    }
                }
                Expr::AddressOf(operand, _, _) => {
                    if let Expr::Identifier(src, _, _) = operand.as_ref() {
                        self.do_borrow(src, name);
                        self.declare_var(name, false);
                    }
                }
                _ => {
                    self.declare_var(name, false);
                }
            },
            Expr::Assign(target, _, value, line, col) => {
                if let Expr::Identifier(name, _, _) = target.as_ref() {
                    self.check_assign(name, value, *line, *col);
                }
            }
            Expr::FreeStmt(target, line, col) => {
                if let Expr::Identifier(name, _, _) = target.as_ref() {
                    if !self.heap_vars.contains(name.as_str()) {
                        self.errors
                            .push(format!("变量 '{}' 不是堆指针，无法释放", name));
                    } else if self.check_free(name, *line, *col) {
                        self.do_free(name);
                    }
                } else if let Expr::Deref(op, _, _) = target.as_ref() {
                    if let Expr::Identifier(name, _, _) = op.as_ref() {
                        if self.check_free(name, *line, *col) {
                            self.do_free(name);
                        }
                    }
                } else {
                    self.errors
                        .push("free 只能应用于堆指针标识符或解引用".to_string());
                }
            }
            Expr::ExprStmt(expr, line, col) => {
                if let Expr::Identifier(name, _, _) = expr.as_ref() {
                    self.check_use(name, *line, *col);
                }
            }
            Expr::IfStmt(cond, body, elifs, else_body, _, _) => {
                self.push_scope();
                self._check_expr(cond);
                for stmt in body {
                    self._check_stmt(stmt);
                }
                self.pop_scope();
                for (c, b) in elifs {
                    self.push_scope();
                    self._check_expr(c);
                    for stmt in b {
                        self._check_stmt(stmt);
                    }
                    self.pop_scope();
                }
                if let Some(b) = else_body {
                    self.push_scope();
                    for stmt in b {
                        self._check_stmt(stmt);
                    }
                    self.pop_scope();
                }
            }
            Expr::WhileStmt(cond, body, _, _) => {
                self.push_scope();
                self._check_expr(cond);
                for stmt in body {
                    self._check_stmt(stmt);
                }
                self.pop_scope();
            }
            Expr::ForStmt(var, iter, body, _, _) => {
                self.push_scope();
                self.declare_var(var, false);
                self._check_expr(iter);
                for stmt in body {
                    self._check_stmt(stmt);
                }
                self.pop_scope();
            }
            Expr::FuncDecl(_, params, _, body, _, _) => {
                self.push_scope();
                for (p, _) in params {
                    self.declare_var(p, false);
                }
                for stmt in body {
                    self._check_stmt(stmt);
                }
                self.pop_scope();
            }
            Expr::ReturnStmt(val, _line, _col) => {
                if let Some(box_e) = val.as_ref() {
                    if let Expr::Identifier(src, _, _) = box_e.as_ref() {
                        if self.get_state(src) == Some(OwnershipState::Owned)
                            && self.heap_vars.contains(src)
                        {
                            self.errors.push(format!(
                                "返回堆变量 '{}' 会转移所有权，调用者需负责释放",
                                src
                            ));
                        }
                    }
                }
            }
            _ => {}
        }
    }
    fn _check_expr(&mut self, e: &Expr) {
        match e {
            Expr::Identifier(name, l, c) => {
                self.check_use(name, *l, *c);
            }
            Expr::BinaryOp(_, left, right, _, _) => {
                self._check_expr(left);
                self._check_expr(right);
            }
            Expr::UnaryOp(_, operand, _, _) => {
                self._check_expr(operand);
            }
            Expr::CallExpr(_, args, _, _) => {
                for a in args {
                    if let Expr::Identifier(name, l, c) = a.as_ref() {
                        self.check_use(name, *l, *c);
                    }
                }
            }
            Expr::PropertyAccess(obj, _, _, _) => {
                self._check_expr(obj);
            }
            Expr::IndexAccess(obj, index, _, _) => {
                self._check_expr(obj);
                self._check_expr(index);
            }
            Expr::Deref(op, _, _) => {
                if let Expr::Identifier(name, l, c) = op.as_ref() {
                    self.check_use(name, *l, *c);
                }
            }
            Expr::AddressOf(op, _, _) => {
                if let Expr::Identifier(name, l, c) = op.as_ref() {
                    self.check_use(name, *l, *c);
                }
            }
            _ => {}
        }
    }
}

// ==================== 编译器 ====================
struct LoopInfo {
    start: usize,
    break_jumps: Vec<usize>,
    continue_jumps: Vec<usize>,
}

struct Compiler {
    vxmodel: HashMap<String, String>,
    constants: Vec<ConstantValue>,
    instructions: Vec<Instruction>,
    functions: Vec<BytecodeFunction>,
    loop_stack: Vec<LoopInfo>,
    for_counter: usize,
}

impl Compiler {
    fn new(vxmodel: HashMap<String, String>) -> Self {
        Self {
            vxmodel,
            constants: Vec::new(),
            instructions: Vec::new(),
            functions: Vec::new(),
            loop_stack: Vec::new(),
            for_counter: 0,
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
    fn compile_expr(&mut self, e: &Expr) {
        match e {
            Expr::IntLiteral(v, _, _) => {
                let idx = self.add_const(ConstantValue::Int(*v)) as i32;
                self.emit(OpCode::LoadConst, BytecodeArg::Int(idx));
            }
            Expr::FloatLiteral(v, _, _) => {
                let idx = self.add_const(ConstantValue::Float(*v)) as i32;
                self.emit(OpCode::LoadConst, BytecodeArg::Int(idx));
            }
            Expr::StringLiteral(v, _, _) => {
                let idx = self.add_const(ConstantValue::String(v.clone())) as i32;
                self.emit(OpCode::LoadConst, BytecodeArg::Int(idx));
            }
            Expr::BoolLiteral(v, _, _) => {
                if *v {
                    self.emit(OpCode::LoadTrue, BytecodeArg::None);
                } else {
                    self.emit(OpCode::LoadFalse, BytecodeArg::None);
                }
            }
            Expr::NilLiteral(_, _) => {
                self.emit(OpCode::LoadNil, BytecodeArg::None);
            }
            Expr::Identifier(name, _, _) => match name.as_str() {
                "sys_argv" => {
                    self.emit(OpCode::SysArgv, BytecodeArg::None);
                }
                "os_system" => {
                    self.emit(OpCode::System, BytecodeArg::None);
                }
                "file_read" => {
                    self.emit(OpCode::FileRead, BytecodeArg::None);
                }
                "file_write" => {
                    self.emit(OpCode::FileWrite, BytecodeArg::None);
                }
                "file_exists" => {
                    self.emit(OpCode::FileExists, BytecodeArg::None);
                }
                _ => {
                    self.emit(OpCode::LoadVar, BytecodeArg::String(name.clone()));
                }
            },
            Expr::BinaryOp(op, left, right, _, _) => {
                self.compile_expr(left);
                self.compile_expr(right);
                let oc = match op.as_ref() {
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
                        // 对于未知的操作符，输出错误信息并退出
                        eprintln!("VX Error: 未知的二元操作符: {}", op);
                        process::exit(1);
                    }
                };
                self.emit(oc, BytecodeArg::None);
            }
            Expr::UnaryOp(op, operand, _, _) => {
                self.compile_expr(operand);
                if &**op == "-" {
                    self.emit(OpCode::UnaryNeg, BytecodeArg::None);
                } else {
                    self.emit(OpCode::UnaryNot, BytecodeArg::None);
                }
            }
            Expr::CallExpr(callee, args, _, _) => {
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
            _ => {}
        }
    }
    /// 编译赋值操作（被 ExprStmt(Assign) 重用）
    fn compile_assign(&mut self, target: &Expr, op: &str, value: &Expr) {
        if op == "=" {
            match target {
                Expr::Identifier(name, _, _) => {
                    self.compile_expr(value);
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

    fn compile_stmt(&mut self, s: &Stmt) {
        match s {
            Expr::ExprStmt(expr, _, _) => {
                // Assign 被解析器包裹在 ExprStmt 中，需要特殊处理
                if let Expr::Assign(ref target, ref op, ref value, _, _) = **expr {
                    self.compile_assign(target, op, value);
                } else {
                    self.compile_expr(expr);
                }
            }
            Expr::VarDecl(name, _, value, _, _, _) => {
                self.compile_expr(value);
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
            _ => {}
        }
    }
    fn compile(&mut self, ast: &[Stmt]) -> CompiledModule {
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
                            for x in mbody {
                                self.compile_stmt(x);
                            }
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
                    for x in body {
                        self.compile_stmt(x);
                    }
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
    fn save(&self, der: &CompiledModule, path: &str) -> io::Result<()> {
        use std::io::BufWriter;

        let mut f = BufWriter::new(fs::File::create(path)?);

        // 将编译结果转换为 bytecode 模块所需的数据结构
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

        // 构建函数数据
        let mut func_data: Vec<(
            &str,
            u32,
            bool,
            Vec<String>,
            Vec<(u8, u8, Option<i32>, Option<String>)>,
        )> = Vec::with_capacity(der.functions.len());

        let mut temp_strings = Vec::new(); // 保持 String 所有权
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

        // 转为 bytecode 模块期望的引用形式
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

// ==================== 主程序 ====================
fn parse_vxmodel<P: AsRef<Path>>(path: P) -> HashMap<String, String> {
    let mut map = HashMap::new();
    if let Ok(content) = fs::read_to_string(path.as_ref()) {
        for line in content.lines() {
            let line = line.trim();
            if line.is_empty() || line.starts_with('#') {
                continue;
            }
            if let Some((k, v)) = line.split_once(':') {
                map.insert(k.trim().to_string(), v.trim().to_string());
            }
        }
    }
    map
}

fn main() {
    let args: Vec<String> = env::args().collect();
    if args.len() < 2 {
        eprintln!("Usage: vxcompiler <input.vx> [-o output.vxobj]");
        process::exit(1);
    }
    let input = &args[1];
    let output = if args.len() > 3 && args[2] == "-o" {
        args[3].clone()
    } else {
        input.replacen(".vx", ".vxobj", 1)
    };

    let source_dir = fs::canonicalize(input)
        .ok()
        .and_then(|p| p.parent().map(|p| p.to_string_lossy().to_string()))
        .unwrap_or_default();
    let vxmodel_path = Path::new(&source_dir).join("vxmodel");
    if !fs::metadata(&vxmodel_path).is_ok() {
        eprintln!("VX Error: 缺少 vxmodel 文件: {}", vxmodel_path.display());
        process::exit(1);
    }
    let vxmodel = parse_vxmodel(&vxmodel_path);

    let src = match fs::read_to_string(input) {
        Err(e) => {
            eprintln!("读取失败: {}", e);
            process::exit(1);
        }
        Ok(s) => s,
    };
    let lexer = Lexer::new(&src);
    let tokens = match lexer.tokenize() {
        Err(e) => {
            eprintln!("{}", e);
            process::exit(1);
        }
        Ok(t) => t,
    };
    let mut parser = Parser::new(tokens, &src);
    let ast = match parser.parse() {
        Err(e) => {
            eprintln!("{}", e);
            process::exit(1);
        }
        Ok(a) => a,
    };

    let mut checker = OwnershipChecker::new(&src);
    checker.check_ast(&ast);
    if !checker.errors.is_empty() {
        for err in &checker.errors {
            eprintln!("[Ownership Warning] {}", err);
        }
        eprintln!(
            "所有权检查发现 {} 个问题，请修复后重新编译",
            checker.errors.len()
        );
        process::exit(1);
    }

    let mut comp = Compiler::new(vxmodel);
    let der = comp.compile(&ast);
    match comp.save(&der, &output) {
        Ok(_) => println!("Compiled: {}", output),
        Err(e) => {
            eprintln!("保存失败: {}", e);
            process::exit(1);
        }
    }
    println!("已内置 VPM 系统接口：sys_argv / os_system / file_read / file_write / file_exists");
    println!("已启用内存安全模式：newz(堆分配) / free(显式释放) / move(所有权转移) / &(借用检查)");
}
