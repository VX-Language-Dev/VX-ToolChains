// ==================== 编译器核心 ====================
// 编译器数据结构、构造函数和核心辅助方法。
// 表达式/语句/模块编译分别拆分至 compiler_expr / compiler_stmt / compiler_module。

use std::collections::HashMap;

use crate::OpCode;
use crate::compiler_bytecode::{BytecodeArg, Instruction, BytecodeFunction, ConstantValue};

pub type BytecodeInstructionTuple = (u8, u8, Option<i32>, Option<String>);
pub type VxobjFunctionData<'a> = (&'a str, u32, bool, Vec<String>, Vec<BytecodeInstructionTuple>);
pub type VxobjFunctionRef<'a> = (&'a str, u32, bool, &'a [String], &'a [BytecodeInstructionTuple]);

pub struct LoopInfo {
    pub start: usize,
    pub break_jumps: Vec<usize>,
    pub continue_jumps: Vec<usize>,
    pub label: Option<String>,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum KnownType {
    Int,
    Float,
    Bool,
    String,
    Array,
    Map,
    Instance,
    Pointer,
    Nil,
    Unknown,
}

impl KnownType {
    /// 返回类型的显示名称
    pub fn as_str(self) -> &'static str {
        match self {
            KnownType::Int => "int",
            KnownType::Float => "float",
            KnownType::Bool => "bool",
            KnownType::String => "string",
            KnownType::Array => "array",
            KnownType::Map => "map",
            KnownType::Instance => "instance",
            KnownType::Pointer => "pointer",
            KnownType::Nil => "nil",
            KnownType::Unknown => "unknown",
        }
    }

    /// 判断是否为数值类型（可用于算术运算）
    pub fn is_numeric(self) -> bool {
        matches!(self, KnownType::Int | KnownType::Float)
    }

    /// 判断是否为复合类型
    pub fn is_compound(self) -> bool {
        matches!(self, KnownType::Array | KnownType::Map | KnownType::Instance)
    }
}

pub struct Compiler {
    pub(crate) settings: crate::VxSettings,
    pub(crate) constants: Vec<ConstantValue>,
    pub(crate) instructions: Vec<Instruction>,
    pub(crate) functions: Vec<BytecodeFunction>,
    pub(crate) loop_stack: Vec<LoopInfo>,
    pub(crate) for_counter: usize,
    pub(crate) var_types: HashMap<String, KnownType>,
    pub(crate) var_slots: HashMap<String, u32>,
    pub(crate) next_slot: u32,
    pub(crate) stack_types: Vec<KnownType>,
    pub opt_level: u8,
    pub warn_dead_code: bool,
    pub error_dead_code: bool,
    /// 宏注册表，用于编译时宏展开
    pub(crate) macros: crate::macros::MacroRegistry,
    /// 外部依赖（import 语句），用于静态链接时的动态库链接
    pub(crate) external_deps: Vec<String>,
}

impl Compiler {
    pub fn new(settings: crate::VxSettings) -> Self {
        Self {
            settings,
            constants: Vec::new(),
            instructions: Vec::new(),
            functions: Vec::new(),
            loop_stack: Vec::new(),
            for_counter: 0,
            var_types: HashMap::new(),
            var_slots: HashMap::new(),
            next_slot: 0,
            stack_types: Vec::new(),
            opt_level: 0,
            warn_dead_code: false,
            error_dead_code: false,
            macros: crate::macros::MacroRegistry::new(),
            external_deps: Vec::new(),
        }
    }

    pub fn with_options(mut self, opt_level: u8, warn_dead_code: bool, error_dead_code: bool) -> Self {
        self.opt_level = opt_level;
        self.warn_dead_code = warn_dead_code;
        self.error_dead_code = error_dead_code;
        self
    }

    /// 在编译之前展开宏
    pub fn expand_macros(&mut self, ast: Vec<crate::parser::Expr>) -> Result<Vec<crate::parser::Expr>, String> {
        let mut expanded = Vec::new();
        
        for stmt in ast {
            match stmt {
                crate::parser::Expr::MacroDef(name, params, body, line, col) => {
                    // 注册宏定义
                    let mac = crate::macros::Macro {
                        name,
                        params,
                        body,
                        line,
                        col,
                    };
                    self.macros.register_macro(mac)?;
                    // 宏定义本身不产生代码，跳过
                }
                crate::parser::Expr::MacroCall(name, args, _line, _col) => {
                    // 展开宏调用
                    let expanded_exprs = self.macros.expand_macro(&name, &args)?;
                    expanded.extend(expanded_exprs);
                }
                _ => {
                    // 递归处理嵌套表达式中的宏
                    let processed = self.process_expr_for_macros(stmt)?;
                    expanded.push(Box::new(processed));
                }
            }
        }
        
        Ok(expanded.into_iter().map(|boxed| *boxed).collect())
    }

    /// 递归处理表达式，展开其中嵌套的宏调用
    fn process_expr_for_macros(&mut self, expr: crate::parser::Expr) -> Result<crate::parser::Expr, String> {
        use crate::parser::Expr;
        
        match expr {
            Expr::CallExpr(func, args, line, col) => {
                // 检查函数名是否是宏调用（通过#符号调用的宏已在parse阶段转换为MacroCall）
                // 这里主要处理嵌套在其他表达式中的情况
                let new_func = Box::new(self.process_expr_for_macros(*func)?);
                let new_args: Result<Vec<Box<Expr>>, String> = args
                    .into_iter()
                    .map(|arg| Ok(Box::new(self.process_expr_for_macros(*arg)?)))
                    .collect();
                let new_args = new_args?;
                
                Ok(Expr::CallExpr(new_func, new_args, line, col))
            }
            
            // 处理条件语句中的宏
            Expr::IfStmt(condition, then_branch, elif_branches, else_branch, line, col) => {
                let new_condition = Box::new(self.process_expr_for_macros(*condition)?);
                let new_then_branch: Result<Vec<Box<Expr>>, String> = then_branch
                    .into_iter()
                    .map(|stmt| Ok(Box::new(self.process_expr_for_macros(*stmt)?)))
                    .collect();
                let new_then_branch = new_then_branch?;
                let new_elif_branches: Result<Vec<(Box<Expr>, Vec<Box<Expr>>)>, String> = elif_branches
                    .into_iter()
                    .map(|(cond, body)| {
                        let new_cond = self.process_expr_for_macros(*cond)?;
                        let new_body: Result<Vec<Box<Expr>>, String> = body
                            .into_iter()
                            .map(|stmt| Ok(Box::new(self.process_expr_for_macros(*stmt)?)))
                            .collect();
                        Ok((Box::new(new_cond), new_body?))
                    })
                    .collect();
                let new_elif_branches = new_elif_branches?;
                let new_else_branch = if let Some(branch) = else_branch {
                    let processed: Result<Vec<Box<Expr>>, String> = branch
                        .into_iter()
                        .map(|stmt| Ok(Box::new(self.process_expr_for_macros(*stmt)?)))
                        .collect();
                    Some(processed?)
                } else {
                    None
                };
                
                Ok(Expr::IfStmt(
                    new_condition, 
                    new_then_branch, 
                    new_elif_branches, 
                    new_else_branch, 
                    line, 
                    col
                ))
            }
            
            // 处理while循环
            Expr::WhileStmt(condition, body, line, col) => {
                let new_condition = Box::new(self.process_expr_for_macros(*condition)?);
                let new_body: Result<Vec<Box<Expr>>, String> = body
                    .into_iter()
                    .map(|stmt| Ok(Box::new(self.process_expr_for_macros(*stmt)?)))
                    .collect();
                
                Ok(Expr::WhileStmt(new_condition, new_body?, line, col))
            }
            
            // 处理for循环
            Expr::ForStmt(var, iterable, body, line, col) => {
                let new_iterable = Box::new(self.process_expr_for_macros(*iterable)?);
                let new_body: Result<Vec<Box<Expr>>, String> = body
                    .into_iter()
                    .map(|stmt| Ok(Box::new(self.process_expr_for_macros(*stmt)?)))
                    .collect();
                
                Ok(Expr::ForStmt(var, new_iterable, new_body?, line, col))
            }
            
            // 处理二元操作
            Expr::BinaryOp(op, left, right, line, col) => {
                Ok(Expr::BinaryOp(
                    op,
                    Box::new(self.process_expr_for_macros(*left)?),
                    Box::new(self.process_expr_for_macros(*right)?),
                    line,
                    col,
                ))
            }
            
            // 处理一元操作
            Expr::UnaryOp(op, operand, line, col) => {
                Ok(Expr::UnaryOp(
                    op,
                    Box::new(self.process_expr_for_macros(*operand)?),
                    line,
                    col,
                ))
            }
            
            // 其他表达式类型保持不变
            _ => Ok(expr),
        }
    }

    /// 获取宏系统的统计信息
    pub fn get_macro_stats(&self) -> (u64, u64, f64) {
        self.macros.get_stats()
    }

    pub(crate) fn allocate_slot(&mut self, name: &str) -> u32 {
        if let Some(&slot) = self.var_slots.get(name) { return slot; }
        let slot = self.next_slot;
        self.next_slot += 1;
        self.var_slots.insert(name.to_string(), slot);
        slot
    }

    pub(crate) fn push_stack_type(&mut self, t: KnownType) { self.stack_types.push(t); }
    pub(crate) fn pop_stack_type(&mut self) -> KnownType { self.stack_types.pop().unwrap_or(KnownType::Unknown) }
    pub(crate) fn set_var_type(&mut self, name: &str, t: KnownType) { self.var_types.insert(name.to_string(), t); }
    pub(crate) fn get_var_type(&self, name: &str) -> KnownType { self.var_types.get(name).copied().unwrap_or(KnownType::Unknown) }

    pub(crate) fn binary_op_specialized(&self, op: &str, left: KnownType, right: KnownType) -> Option<OpCode> {
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

    pub(crate) fn unary_op_specialized(&self, op: &str, operand: KnownType) -> Option<OpCode> {
        match (op, operand) {
            ("-", KnownType::Int) => Some(OpCode::NegInt),
            ("-", KnownType::Float) => Some(OpCode::NegFloat),
            ("!" | "not", KnownType::Bool) => Some(OpCode::Not),
            _ => None,
        }
    }

    pub(crate) fn add_const(&mut self, v: ConstantValue) -> usize { self.constants.push(v.clone()); self.constants.len() - 1 }
    pub(crate) fn emit(&mut self, op: OpCode, arg: BytecodeArg) -> usize { self.instructions.push(Instruction { op, arg }); self.instructions.len() - 1 }
    pub(crate) fn patch(&mut self, pos: usize, tgt: usize) {
        if let Some(inst) = self.instructions.get_mut(pos) {
            inst.arg = match &inst.arg {
                BytecodeArg::None => BytecodeArg::Int(tgt as i32),
                _ => BytecodeArg::Int(tgt as i32),
            };
        }
    }
}
