// ==================== VX Decompiler: TypeIR → VX Source ====================
//
// 功能: 将 VXOBJ v4 文件中的 TypeIR 段反编译为可读的 VX 源代码。
//
// 工作流程:
//   1. 解析 VXOBJ v4 容器，提取 TypeIR 段
//   2. 反序列化 TypeModule
//   3. 对每个函数，分析 TypedInstruction 列表 → 重建 VX 源码
//   4. 输出完整的 .vx 文件
//
// 反编译策略:
//   - 使用表达式栈重建算术/逻辑表达式
//   - 通过控制流分析识别 if/else/while/for 结构
//   - VarId → 虚拟变量名 (v0, v1, ...) 映射，尽量保留原始语义
//   - 外部函数调用标记为 vx_xxx()

use std::collections::HashMap;
use std::fmt::Write as FmtWrite;

use crate::bytecode::{self, VxObjV4Container};
use crate::type_ir::*;

// ==================== 表达式表示 ====================

#[derive(Debug, Clone)]
enum Expr {
    Int(i64),
    Float(f64),
    Bool(bool),
    String(String),
    Nil,
    Var(String),
    /// unary op(expr)
    Unary(String, Box<Expr>),
    /// lhs op rhs
    Binary(String, Box<Expr>, Box<Expr>),
    /// callee(args...)
    Call(String, Vec<Expr>),
    /// indirect_call(args...)
    IndirectCall(Box<Expr>, Vec<Expr>),
    /// StructName { field1: expr1, ... }
    StructNew(String, Vec<(String, Expr)>),
    /// expr.field_name
    Field(Box<Expr>, String),
    /// expr[index]
    Index(Box<Expr>, Box<Expr>),
    /// [expr1, expr2, ...]
    Array(Vec<Expr>),
    /// { key1: val1, ... }
    Map(Vec<(Expr, Expr)>),
    /// new Type(args...)
    #[allow(dead_code)]
    New(String, Vec<Expr>),
    /// &expr (address-of)
    AddrOf(Box<Expr>),
    /// &mut expr
    #[allow(dead_code)]
    AddrOfMut(Box<Expr>),
    /// *expr (dereference)
    Deref(Box<Expr>),
    /// move expr
    Move(Box<Expr>),
}

impl Expr {
    fn render(&self) -> String {
        match self {
            Expr::Int(v) => v.to_string(),
            Expr::Float(v) => {
                if v.fract() == 0.0 {
                    format!("{}.0", v)
                } else {
                    format!("{}", v)
                }
            }
            Expr::Bool(v) => {
                if *v { "true".to_string() } else { "false".to_string() }
            }
            Expr::String(v) => format!("\"{}\"", v),
            Expr::Nil => "nil".to_string(),
            Expr::Var(name) => name.clone(),
            Expr::Unary(op, e) => {
                if op == "!" {
                    format!("!{}", e.render())
                } else {
                    format!("{}{}", op, e.render())
                }
            }
            Expr::Binary(op, lhs, rhs) => format!("{} {} {}", lhs.render(), op, rhs.render()),
            Expr::Call(name, args) => {
                let args_str: Vec<String> = args.iter().map(|a| a.render()).collect();
                format!("{}({})", name, args_str.join(", "))
            }
            Expr::IndirectCall(callee, args) => {
                let args_str: Vec<String> = args.iter().map(|a| a.render()).collect();
                format!("{}({})", callee.render(), args_str.join(", "))
            }
            Expr::StructNew(name, fields) => {
                let fields_str: Vec<String> = fields.iter()
                    .map(|(k, v)| format!("{}: {}", k, v.render()))
                    .collect();
                format!("{} {{ {} }}", name, fields_str.join(", "))
            }
            Expr::Field(obj, name) => format!("{}.{}", obj.render(), name),
            Expr::Index(obj, idx) => format!("{}[{}]", obj.render(), idx.render()),
            Expr::Array(items) => {
                let items_str: Vec<String> = items.iter().map(|a| a.render()).collect();
                format!("[{}]", items_str.join(", "))
            }
            Expr::Map(pairs) => {
                let pairs_str: Vec<String> = pairs.iter()
                    .map(|(k, v)| format!("{}: {}", k.render(), v.render()))
                    .collect();
                format!("{{{}}}", pairs_str.join(", "))
            }
            Expr::New(name, args) => {
                let args_str: Vec<String> = args.iter().map(|a| a.render()).collect();
                format!("new {}({})", name, args_str.join(", "))
            }
            Expr::AddrOf(e) => format!("&{}", e.render()),
            Expr::AddrOfMut(e) => format!("&mut {}", e.render()),
            Expr::Deref(e) => format!("*{}", e.render()),
            Expr::Move(e) => format!("move {}", e.render()),
        }
    }
}

// ==================== 语句表示 ====================

#[derive(Debug, Clone)]
#[allow(dead_code)]
enum Stmt {
    /// name: type = expr
    VarDecl(String, Type, Expr),
    /// name = expr (reassignment)
    Assign(String, Expr),
    /// return expr
    Return(Option<Expr>),
    /// if cond { body } [else { else_body }]
    If(Expr, Vec<Stmt>, Vec<Stmt>),
    /// while cond { body }
    While(Expr, Vec<Stmt>),
    /// for name in iterable { body }
    For(String, Expr, Vec<Stmt>),
    /// { stmts... } (block)
    Block(Vec<Stmt>),
    /// expr; (expression statement, e.g. function call)
    ExprStmt(Expr),
    /// break
    Break,
    /// continue
    Continue,
    /// obj.field = value
    FieldAssign(Expr, String, Expr),
    /// arr[idx] = value
    IndexAssign(Expr, Expr, Expr),
    /// free(expr)
    Free(Expr),
}

#[allow(dead_code)]
fn render_stmts(stmts: &[Stmt], indent: usize) -> String {
    let mut out = String::new();
    for stmt in stmts {
        let _ = writeln!(out, "{}", render_stmt(stmt, indent));
    }
    out
}

fn render_stmt(stmt: &Stmt, indent: usize) -> String {
    let prefix = "    ".repeat(indent);
    match stmt {
        Stmt::VarDecl(name, ty, init) => {
            let type_str = type_to_vx_type(ty);
            format!("{}{}: {} = {}", prefix, name, type_str, init.render())
        }
        Stmt::Assign(name, value) => {
            format!("{}{} = {}", prefix, name, value.render())
        }
        Stmt::Return(None) => format!("{}return", prefix),
        Stmt::Return(Some(e)) => format!("{}return {}", prefix, e.render()),
        Stmt::ExprStmt(e) => {
            format!("{}{}", prefix, e.render())
        }
        Stmt::If(cond, body, else_body) => {
            let mut out = format!("{}if {}", prefix, cond.render());
            if !body.is_empty() {
                out.push('\n');
                out.push_str(&render_stmts(body, indent + 1));
            }
            if !else_body.is_empty() {
                out.push_str(&format!("{}else\n", prefix));
                out.push_str(&render_stmts(else_body, indent + 1));
            }
            out
        }
        Stmt::While(cond, body) => {
            let mut out = format!("{}while {}", prefix, cond.render());
            if !body.is_empty() {
                out.push('\n');
                out.push_str(&render_stmts(body, indent + 1));
            }
            out
        }
        Stmt::For(name, iter, body) => {
            let mut out = format!("{}for {} in {}", prefix, name, iter.render());
            if !body.is_empty() {
                out.push('\n');
                out.push_str(&render_stmts(body, indent + 1));
            }
            out
        }
        Stmt::Block(stmts) => {
            let mut out = String::new();
            for stmt in stmts {
                let _ = writeln!(out, "{}", render_stmt(stmt, indent));
            }
            out
        }
        Stmt::Break => format!("{}break", prefix),
        Stmt::Continue => format!("{}continue", prefix),
        Stmt::FieldAssign(obj, field, value) => {
            format!("{}{}.{} = {}", prefix, obj.render(), field, value.render())
        }
        Stmt::IndexAssign(obj, idx, value) => {
            format!("{}{}[{}] = {}", prefix, obj.render(), idx.render(), value.render())
        }
        Stmt::Free(e) => {
            format!("{}free({})", prefix, e.render())
        }
    }
}

// ==================== 反编译器主结构 ====================

pub struct Decompiler {
    /// 函数名 → FuncId 映射
    func_name_map: HashMap<FuncId, String>,
    /// 结构体布局: StructLayoutId → (name, fields)
    struct_layouts: HashMap<u32, (String, Vec<(String, Type)>)>,
    /// 外部函数映射: FuncId → external_name
    external_funcs: HashMap<FuncId, String>,
    /// 已生成的虚拟变量计数器（作用域内）
    var_counter: u32,
    /// VarId → 变量名
    var_names: HashMap<VarId, String>,
    /// 变量类型映射
    var_types: HashMap<VarId, Type>,
    /// 基本块分析结果
    basic_blocks: Vec<BasicBlock>,
    /// 变量定义点映射 (哪个指令定义了该 VarId)
    var_def_inst: HashMap<VarId, usize>,
    /// 当前函数中已经声明过的变量
    defined_vars: std::collections::HashSet<VarId>,
    /// VarId → 当前表达式映射（用于重建复杂表达式）
    var_expr_map: HashMap<VarId, Expr>,
}

#[derive(Debug, Clone)]
#[allow(dead_code)]
struct BasicBlock {
    /// 起始指令索引
    start: usize,
    /// 结束指令索引 (inclusive)
    end: usize,
    /// 前驱基本块
    predecessors: Vec<usize>,
    /// 后继基本块
    successors: Vec<usize>,
    /// 跳转目标 (无条件跳转目标，无则为 None)
    jump_target: Option<u32>,
    /// 条件跳转 (cond_var, true_target, false_target)
    cond_jump: Option<(VarId, u32, u32)>,
}

#[allow(dead_code)]
impl Decompiler {
    pub fn new() -> Self {
        Self {
            func_name_map: HashMap::new(),
            struct_layouts: HashMap::new(),
            external_funcs: HashMap::new(),
            var_counter: 0,
            var_names: HashMap::new(),
            var_types: HashMap::new(),
            basic_blocks: Vec::new(),
            var_def_inst: HashMap::new(),
            defined_vars: std::collections::HashSet::new(),
            var_expr_map: HashMap::new(),
        }
    }

    /// 主入口: 从 VXOBJ v4 文件反编译为 VX 源代码
    pub fn decompile_file(input_path: &str) -> Result<String, String> {
        let data = std::fs::read(input_path)
            .map_err(|e| format!("读取文件失败: {}", e))?;

        let container = VxObjV4Container::parse(&data)
            .map_err(|e| format!("VXOBJ 解析失败: {}", e))?;

        let type_ir_data = container.get_section(bytecode::SECTION_TYPE_IR)
            .ok_or_else(|| "未找到 TypeIR 段".to_string())?;

        let type_module = deserialize_type_module_result(type_ir_data)
            .map_err(|e| format!("TypeIR 反序列化失败: {}", e))?;

        let mut decompiler = Decompiler::new();
        decompiler.generate_source(&type_module, &container)
    }

    /// 从 TypeModule 生成 VX 源代码
    fn generate_source(&mut self, module: &TypeModule, container: &VxObjV4Container) -> Result<String, String> {
        // 建立函数名映射
        for (id, name) in &module.function_map {
            self.func_name_map.insert(*id, name.clone());
        }

        // 记录结构体布局
        for (i, (name, fields)) in module.struct_layouts.iter().enumerate() {
            self.struct_layouts.insert(i as u32, (name.clone(), fields.clone()));
        }

        // 记录外部函数
        for (func_id, linkage) in &module.linkage {
            match linkage {
                Linkage::External(name) => {
                    self.external_funcs.insert(*func_id, name.clone());
                }
                _ => {}
            }
        }

        // 收集已输出的函数名，避免重复
        let mut emitted_funcs = std::collections::HashSet::new();

        // 生成每个函数的源码
        let mut source = String::new();
        source.push_str("# Decompiled by VX Decompiler\n");
        source.push_str(&format!("# Target: {}\n\n", container.header.target_triple));

        // 结构体定义
        for (_, (name, fields)) in &self.struct_layouts {
            source.push_str(&format!("struct {}:\n", name));
            for (fname, ftype) in fields {
                let type_str = type_to_vx_type(ftype);
                source.push_str(&format!("    {}: {}\n", fname, type_str));
            }
            source.push('\n');
        }

        // 再输出内部函数定义
        for func in &module.functions {
            // 跳过外部函数
            if self.external_funcs.contains_key(&func.id) {
                continue;
            }
            // 跳过编译器生成的入口包装函数 __main__
            if func.name == "__main__" {
                continue;
            }
            let func_name = func.name.clone();
            // 跳过已输出的同名函数
            if emitted_funcs.contains(&func_name) {
                continue;
            }
            emitted_funcs.insert(func_name);

            let func_source = self.decompile_function(func, module)?;
            source.push_str(&func_source);
            source.push('\n');
        }

        Ok(source)
    }

    /// 反编译单个函数
    fn decompile_function(&mut self, func: &TypeFunction, module: &TypeModule) -> Result<String, String> {
        self.var_counter = 0;
        self.var_names.clear();
        self.var_types.clear();
        self.var_def_inst.clear();
        self.defined_vars.clear();
        self.var_expr_map.clear();

        // 注册参数名和类型：参数 VarId 从 0 开始
        for (i, (name, ty)) in func.params.iter().enumerate() {
            let vid = i as VarId;
            self.var_names.insert(vid, name.clone());
            self.var_types.insert(vid, ty.clone());
            self.defined_vars.insert(vid);
        }

        // 解析局部变量类型
        for (vid, ty) in &func.local_types {
            self.var_types.insert(*vid, ty.clone());
        }

        // 参数声明
        let params_str: Vec<String> = func.params.iter()
            .map(|(name, ty)| format!("{}: {}", name, type_to_vx_type(ty)))
            .collect();

        // 通过栈模拟构建函数体
        let body_stmts = self.build_function_body_stack(func, module);

        let return_type_str = type_to_vx_type(&func.return_type);
        let func_name = func.name.clone();

        let mut out = String::new();
        if return_type_str == "void" {
            out.push_str(&format!(
                "func {}({}):\n",
                func_name,
                params_str.join(", ")
            ));
        } else {
            out.push_str(&format!(
                "func {}({}) -> {}:\n",
                func_name,
                params_str.join(", "),
                return_type_str
            ));
        }

        // 函数体：若无语句，生成 pass
        if body_stmts.is_empty() {
            out.push_str("    pass\n");
        } else {
            for stmt in &body_stmts {
                let rendered = render_stmt(stmt, 1);
                out.push_str(&rendered);
                out.push('\n');
            }
        }

        Ok(out)
    }

    /// 分析控制流，识别基本块
    fn analyze_control_flow(&mut self, func: &TypeFunction) {
        let n = func.body.len();
        // 标记 leader 指令（基本块起点）
        let mut leaders = vec![false; n];
        if n > 0 {
            leaders[0] = true;
        }
        for (i, inst) in func.body.iter().enumerate() {
            match inst {
                TypedInstruction::Jump(target) => {
                    if (*target as usize) < n {
                        leaders[*target as usize] = true;
                    }
                    if i + 1 < n {
                        leaders[i + 1] = true;
                    }
                }
                TypedInstruction::JumpIfFalse(_, target) | TypedInstruction::JumpIfTrue(_, target) => {
                    if (*target as usize) < n {
                        leaders[*target as usize] = true;
                    }
                    if i + 1 < n {
                        leaders[i + 1] = true;
                    }
                }
                TypedInstruction::Return(_) => {
                    if i + 1 < n {
                        leaders[i + 1] = true;
                    }
                }
                _ => {}
            }
        }

        // 构建基本块
        self.basic_blocks.clear();
        let mut i = 0;
        let mut _block_id = 0;
        while i < n {
            let start = i;
            i += 1;
            while i < n && !leaders[i] {
                i += 1;
            }
            let end = i - 1;
            let mut bb = BasicBlock {
                start,
                end,
                predecessors: Vec::new(),
                successors: Vec::new(),
                jump_target: None,
                cond_jump: None,
            };

            // 分析最后一个指令
            if let Some(last_inst) = func.body.get(end) {
                match last_inst {
                    TypedInstruction::Jump(target) => {
                        bb.jump_target = Some(*target);
                    }
                    TypedInstruction::JumpIfFalse(cond, target) | TypedInstruction::JumpIfTrue(cond, target) => {
                        // 条件跳转: 判断是 if 还是 while 由上层决定
                        if end + 1 < n {
                            bb.cond_jump = Some((*cond, *target, (end + 1) as u32));
                        }
                    }
                    _ => {}
                }
            }
            self.basic_blocks.push(bb);
            _block_id += 1;
        }

        // 建立后继/前驱关系
        for bi in 0..self.basic_blocks.len() {
            let bb = &self.basic_blocks[bi];
            let _end = bb.end;
            if let Some(target) = bb.jump_target {
                // 无条件跳转
                if let Some(tbi) = self.block_containing(target as usize) {
                    self.basic_blocks[bi].successors.push(tbi);
                    self.basic_blocks[tbi].predecessors.push(bi);
                }
            } else if bb.cond_jump.is_some() {
                // 条件跳转: true/false 两个后继
                if let Some((_, t_true, t_false)) = bb.cond_jump {
                    if let Some(tbi) = self.block_containing(t_true as usize) {
                        self.basic_blocks[bi].successors.push(tbi);
                        self.basic_blocks[tbi].predecessors.push(bi);
                    }
                    if let Some(tbi) = self.block_containing(t_false as usize) {
                        self.basic_blocks[bi].successors.push(tbi);
                        self.basic_blocks[tbi].predecessors.push(bi);
                    }
                }
            } else {
                // fall-through
                if bi + 1 < self.basic_blocks.len() {
                    self.basic_blocks[bi].successors.push(bi + 1);
                    self.basic_blocks[bi + 1].predecessors.push(bi);
                }
            }
        }
    }

    fn block_containing(&self, inst_idx: usize) -> Option<usize> {
        for (i, bb) in self.basic_blocks.iter().enumerate() {
            if inst_idx >= bb.start && inst_idx <= bb.end {
                return Some(i);
            }
        }
        None
    }

    /// 翻译单条指令为表达式
    fn translate_instruction(
        &self,
        idx: usize,
        inst: &TypedInstruction,
        _body: &[TypedInstruction],
        expr_map: &HashMap<usize, Expr>,
        module: &TypeModule,
    ) -> Option<Expr> {
        match inst {
            TypedInstruction::ConstInt(v) => Some(Expr::Int(*v)),
            TypedInstruction::ConstFloat(v) => Some(Expr::Float(*v)),
            TypedInstruction::ConstBool(v) => Some(Expr::Bool(*v)),
            TypedInstruction::ConstString(v) => Some(Expr::String(v.clone())),
            TypedInstruction::ConstNil => Some(Expr::Nil),
            TypedInstruction::LoadVar(v) => {
                Some(Expr::Var(self.var_name(*v)))
            }
            TypedInstruction::StoreVar(v) => {
                // StoreVar 保存表达式栈顶的值到变量
                // 通常和 LoadVar 配对使用
                let val_expr = if idx > 0 {
                    expr_map.get(&(idx - 1)).cloned()
                } else {
                    None
                };
                val_expr.or(Some(Expr::Var(self.var_name(*v))))
            }
            TypedInstruction::I32Add(a, b) => self.make_binary("+", *a, *b, expr_map),
            TypedInstruction::I32Sub(a, b) => self.make_binary("-", *a, *b, expr_map),
            TypedInstruction::I32Mul(a, b) => self.make_binary("*", *a, *b, expr_map),
            TypedInstruction::I32Div(a, b) => self.make_binary("/", *a, *b, expr_map),
            TypedInstruction::I32Mod(a, b) => self.make_binary("%", *a, *b, expr_map),
            TypedInstruction::F64Add(a, b) => self.make_binary("+", *a, *b, expr_map),
            TypedInstruction::F64Sub(a, b) => self.make_binary("-", *a, *b, expr_map),
            TypedInstruction::F64Mul(a, b) => self.make_binary("*", *a, *b, expr_map),
            TypedInstruction::F64Div(a, b) => self.make_binary("/", *a, *b, expr_map),
            TypedInstruction::I32Eq(a, b) => self.make_binary("==", *a, *b, expr_map),
            TypedInstruction::I32Ne(a, b) => self.make_binary("!=", *a, *b, expr_map),
            TypedInstruction::I32Lt(a, b) => self.make_binary("<", *a, *b, expr_map),
            TypedInstruction::I32Gt(a, b) => self.make_binary(">", *a, *b, expr_map),
            TypedInstruction::I32Le(a, b) => self.make_binary("<=", *a, *b, expr_map),
            TypedInstruction::I32Ge(a, b) => self.make_binary(">=", *a, *b, expr_map),
            TypedInstruction::F64Eq(a, b) => self.make_binary("==", *a, *b, expr_map),
            TypedInstruction::F64Ne(a, b) => self.make_binary("!=", *a, *b, expr_map),
            TypedInstruction::F64Lt(a, b) => self.make_binary("<", *a, *b, expr_map),
            TypedInstruction::F64Gt(a, b) => self.make_binary(">", *a, *b, expr_map),
            TypedInstruction::F64Le(a, b) => self.make_binary("<=", *a, *b, expr_map),
            TypedInstruction::F64Ge(a, b) => self.make_binary(">=", *a, *b, expr_map),
            TypedInstruction::I32Neg(v) => self.make_unary("-", *v, expr_map),
            TypedInstruction::F64Neg(v) => self.make_unary("-", *v, expr_map),
            TypedInstruction::BoolNot(v) => self.make_unary("!", *v, expr_map),
            TypedInstruction::Call(func_id, args, ext_name) => {
                let func_name = if let Some(name) = ext_name {
                    name.clone()
                } else if let Some(ext_name) = self.external_funcs.get(func_id) {
                    ext_name.trim_start_matches("vx_").to_string()
                } else {
                    self.func_name_map.get(func_id).cloned()
                        .unwrap_or_else(|| format!("func_{}", func_id))
                };
                let arg_exprs: Vec<Expr> = args.iter()
                    .map(|a| Expr::Var(self.var_name(*a)))
                    .collect();
                Some(Expr::Call(func_name, arg_exprs))
            }
            TypedInstruction::CallIndirect(v, args) => {
                let arg_exprs: Vec<Expr> = args.iter()
                    .map(|a| Expr::Var(self.var_name(*a)))
                    .collect();
                Some(Expr::IndirectCall(
                    Box::new(Expr::Var(self.var_name(*v))),
                    arg_exprs,
                ))
            }
            TypedInstruction::Return(_v) => {
                // Return 不在表达式映射里处理，由构建语句处理
                None
            }
            TypedInstruction::MakeStruct(id, args) => {
                let layout = self.struct_layouts.get(&id.0);
                let fields = layout.map(|(_, f)| f.clone()).unwrap_or_default();
                let field_exprs: Vec<(String, Expr)> = fields.iter().enumerate()
                    .map(|(i, (fname, _))| {
                        let arg_expr = args.get(i)
                            .map(|a| Expr::Var(self.var_name(*a)))
                            .unwrap_or(Expr::Nil);
                        (fname.clone(), arg_expr)
                    })
                    .collect();
                let type_name = layout.map(|(n, _)| n.clone())
                    .unwrap_or_else(|| format!("Struct{}", id.0));
                Some(Expr::StructNew(type_name, field_exprs))
            }
            TypedInstruction::GetField(obj, idx) => {
                let obj_expr = Expr::Var(self.var_name(*obj));
                let field_name = self.get_field_name(*obj, *idx, module);
                Some(Expr::Field(Box::new(obj_expr), field_name))
            }
            TypedInstruction::SetField(_, _, _) => None,
            TypedInstruction::MakeArray(_base, args) => {
                let items: Vec<Expr> = args.iter()
                    .map(|a| Expr::Var(self.var_name(*a)))
                    .collect();
                Some(Expr::Array(items))
            }
            TypedInstruction::IndexGet(arr, idx) => {
                Some(Expr::Index(
                    Box::new(Expr::Var(self.var_name(*arr))),
                    Box::new(Expr::Var(self.var_name(*idx))),
                ))
            }
            TypedInstruction::IndexSet(_, _, _) => None,
            TypedInstruction::MakeMap(pairs) => {
                let pair_exprs: Vec<(Expr, Expr)> = pairs.iter()
                    .map(|(k, v)| (Expr::Var(self.var_name(*k)), Expr::Var(self.var_name(*v))))
                    .collect();
                Some(Expr::Map(pair_exprs))
            }
            TypedInstruction::Alloc(_) => None,
            TypedInstruction::Free(_) => None,
            TypedInstruction::OwnershipMove(_) => None,
            TypedInstruction::Borrow(_) => None,
            TypedInstruction::Deref(v) => Some(Expr::Var(self.var_name(*v))),
            TypedInstruction::AliveCheck(_) => None,
            TypedInstruction::Dup => None,
            TypedInstruction::Pop => None,
            _ => None,
        }
    }

    fn make_binary(&self, op: &str, a: VarId, b: VarId, _expr_map: &HashMap<usize, Expr>) -> Option<Expr> {
        Some(Expr::Binary(
            op.to_string(),
            Box::new(Expr::Var(self.var_name(a))),
            Box::new(Expr::Var(self.var_name(b))),
        ))
    }

    fn make_unary(&self, op: &str, v: VarId, _expr_map: &HashMap<usize, Expr>) -> Option<Expr> {
        Some(Expr::Unary(
            op.to_string(),
            Box::new(Expr::Var(self.var_name(v))),
        ))
    }

    /// 根据 VarId 获取或生成变量名
    fn var_name(&self, vid: VarId) -> String {
        // 检查是否在参数中已命名
        if let Some(name) = self.var_names.get(&vid) {
            return name.clone();
        }
        // 使用 v0, v1, ... 模式
        format!("v{}", vid)
    }

    /// 推断表达式类型
    fn infer_expr_type(&self, expr: &Expr, module: &TypeModule) -> Type {
        match expr {
            Expr::Int(_) => Type::Int,
            Expr::Float(_) => Type::Float,
            Expr::Bool(_) => Type::Bool,
            Expr::String(_) => Type::String,
            Expr::Nil => Type::Unknown,
            Expr::Var(name) => {
                // 通过名称反查 VarId
                for (vid, n) in &self.var_names {
                    if n == name {
                        return self.var_types.get(vid).cloned().unwrap_or(Type::Unknown);
                    }
                }
                Type::Unknown
            }
            Expr::Binary(op, a, b) => {
                let ta = self.infer_expr_type(a, module);
                let tb = self.infer_expr_type(b, module);
                if matches!(op.as_str(), "==" | "!=" | "<" | ">" | "<=" | ">=") {
                    Type::Bool
                } else if ta == Type::Float || tb == Type::Float {
                    Type::Float
                } else {
                    Type::Int
                }
            }
            Expr::Unary(_, a) => self.infer_expr_type(a, module),
            Expr::Call(name, _) => {
                // 查找函数返回类型
                if let Some(fid) = module.get_function_id(name) {
                    if let Some(f) = module.get_function(fid) {
                        return f.return_type.clone();
                    }
                }
                Type::Unknown
            }
            Expr::IndirectCall(callee, _) => {
                if let Expr::Var(name) = callee.as_ref() {
                    if let Some(fid) = module.get_function_id(name) {
                        if let Some(f) = module.get_function(fid) {
                            return f.return_type.clone();
                        }
                    }
                }
                Type::Unknown
            }
            _ => Type::Unknown,
        }
    }

    /// 返回二元操作的运算符字符串与结果类型
    fn binary_op_info(&self, inst: &TypedInstruction) -> (String, Type) {
        use TypedInstruction::*;
        match inst {
            I32Add(_, _) | F64Add(_, _) => ("+".to_string(), Type::Int),
            I32Sub(_, _) | F64Sub(_, _) => ("-".to_string(), Type::Int),
            I32Mul(_, _) | F64Mul(_, _) => ("*".to_string(), Type::Int),
            I32Div(_, _) | F64Div(_, _) => ("/".to_string(), Type::Int),
            I32Mod(_, _) => ("%".to_string(), Type::Int),
            I32Eq(_, _) | F64Eq(_, _) => ("==".to_string(), Type::Bool),
            I32Ne(_, _) | F64Ne(_, _) => ("!=".to_string(), Type::Bool),
            I32Lt(_, _) | F64Lt(_, _) => ("<".to_string(), Type::Bool),
            I32Gt(_, _) | F64Gt(_, _) => (">".to_string(), Type::Bool),
            I32Le(_, _) | F64Le(_, _) => ("<=".to_string(), Type::Bool),
            I32Ge(_, _) | F64Ge(_, _) => (">=".to_string(), Type::Bool),
            _ => ("?".to_string(), Type::Unknown),
        }
    }

    /// 返回一元操作的运算符字符串与结果类型
    fn unary_op_info(&self, inst: &TypedInstruction) -> (String, Type) {
        use TypedInstruction::*;
        match inst {
            I32Neg(_) | F64Neg(_) => ("-".to_string(), Type::Int),
            BoolNot(_) => ("!".to_string(), Type::Bool),
            _ => ("?".to_string(), Type::Unknown),
        }
    }

    /// 尝试推断字段名
    fn get_field_name(&self, obj_var: VarId, field_idx: u32, _module: &TypeModule) -> String {
        // 从变量类型中查找结构体字段名
        if let Some(ty) = self.var_types.get(&obj_var) {
            if let Type::Struct(_, fields) = ty {
                if let Some((fname, _)) = fields.get(field_idx as usize) {
                    return fname.clone();
                }
            }
        }
        // 从 struct_layouts 中查找
        for (_, (_, fields)) in &self.struct_layouts {
            if let Some((fname, _)) = fields.get(field_idx as usize) {
                return fname.clone();
            }
        }
        // 回退：使用 field0, field1, ...
        format!("field{}", field_idx)
    }

    /// 基于栈模拟构建函数体语句列表
    fn build_function_body_stack(&mut self, func: &TypeFunction, module: &TypeModule) -> Vec<Stmt> {
        use TypedInstruction::*;

        let mut stmts = Vec::new();
        let mut stack: Vec<Expr> = Vec::new();
        let mut i = 0;
        let body = &func.body;

        while i < body.len() {
            let inst = &body[i];
            match inst {
                ConstInt(v) => stack.push(Expr::Int(*v)),
                ConstFloat(v) => stack.push(Expr::Float(*v)),
                ConstBool(v) => stack.push(Expr::Bool(*v)),
                ConstString(v) => stack.push(Expr::String(v.clone())),
                ConstNil => stack.push(Expr::Nil),
                LoadVar(vid) => {
                    // 检查是否有已记录的表达式
                    if let Some(expr) = self.var_expr_map.get(vid) {
                        stack.push(expr.clone());
                    } else {
                        stack.push(Expr::Var(self.var_name(*vid)));
                    }
                }
                StoreVar(vid) => {
                    if let Some(val) = stack.pop() {
                        let name = self.var_name(*vid);
                        let is_decl = !self.defined_vars.contains(vid);
                        let ty = self.var_types.get(vid).cloned().unwrap_or_else(|| self.infer_expr_type(&val, module));
                        if is_decl {
                            self.var_types.insert(*vid, ty.clone());
                            stmts.push(Stmt::VarDecl(name, ty, val.clone()));
                            self.defined_vars.insert(*vid);
                        } else {
                            stmts.push(Stmt::Assign(name, val.clone()));
                        }
                        // 记录该变量的表达式
                        self.var_expr_map.insert(*vid, val);
                    }
                }
                I32Add(_, _) | I32Sub(_, _) | I32Mul(_, _) | I32Div(_, _) | I32Mod(_, _) |
                F64Add(_, _) | F64Sub(_, _) | F64Mul(_, _) | F64Div(_, _) |
                I32Eq(_, _) | I32Ne(_, _) | I32Lt(_, _) | I32Gt(_, _) | I32Le(_, _) | I32Ge(_, _) |
                F64Eq(_, _) | F64Ne(_, _) | F64Lt(_, _) | F64Gt(_, _) | F64Le(_, _) | F64Ge(_, _) => {
                    let (op, _) = self.binary_op_info(inst);
                    let b = stack.pop().unwrap_or(Expr::Nil);
                    let a = stack.pop().unwrap_or(Expr::Nil);
                    stack.push(Expr::Binary(op, Box::new(a), Box::new(b)));
                }
                I32Neg(_) | F64Neg(_) | BoolNot(_) => {
                    let (op, _) = self.unary_op_info(inst);
                    let a = stack.pop().unwrap_or(Expr::Nil);
                    stack.push(Expr::Unary(op, Box::new(a)));
                }
                Call(func_id, args, ext_name) => {
                    let func_name = if let Some(name) = ext_name {
                        name.clone()
                    } else if let Some(ext_name) = self.external_funcs.get(func_id) {
                        ext_name.trim_start_matches("vx_").to_string()
                    } else {
                        self.func_name_map.get(func_id).cloned()
                            .unwrap_or_else(|| format!("func_{}", func_id))
                    };
                    let arg_exprs: Vec<Expr> = args.iter()
                        .map(|a| {
                            if let Some(expr) = self.var_expr_map.get(a) {
                                expr.clone()
                            } else {
                                Expr::Var(self.var_name(*a))
                            }
                        })
                        .collect();
                    stack.push(Expr::Call(func_name, arg_exprs));
                }
                CallIndirect(vid, args) => {
                    let arg_exprs: Vec<Expr> = args.iter()
                        .map(|a| {
                            if let Some(expr) = self.var_expr_map.get(a) {
                                expr.clone()
                            } else {
                                Expr::Var(self.var_name(*a))
                            }
                        })
                        .collect();
                    let callee = if let Some(expr) = self.var_expr_map.get(vid) {
                        expr.clone()
                    } else {
                        Expr::Var(self.var_name(*vid))
                    };
                    stack.push(Expr::IndirectCall(Box::new(callee), arg_exprs));
                }
                Return(ret) => {
                    let ret_expr = if let Some(vid) = ret {
                        if let Some(expr) = self.var_expr_map.get(vid) {
                            Some(expr.clone())
                        } else {
                            Some(Expr::Var(self.var_name(*vid)))
                        }
                    } else {
                        None
                    };
                    // 先输出栈中剩余的函数调用
                    while !stack.is_empty() {
                        let e = stack.remove(0);
                        if matches!(e, Expr::Call(_, _) | Expr::IndirectCall(_, _)) {
                            stmts.push(Stmt::ExprStmt(e));
                        }
                    }
                    stmts.push(Stmt::Return(ret_expr));
                }
                Jump(_) => {}
                JumpIfFalse(_, _) | JumpIfTrue(_, _) => {}
                MakeStruct(id, args) => {
                    let layout = self.struct_layouts.get(&id.0);
                    let fields = layout.map(|(_, f)| f.clone()).unwrap_or_default();
                    let field_exprs: Vec<(String, Expr)> = fields.iter().enumerate()
                        .map(|(i, (fname, _))| {
                            let arg_expr = args.get(i)
                                .and_then(|a| self.var_expr_map.get(a).cloned())
                                .unwrap_or_else(|| Expr::Var(self.var_name(*args.get(i).unwrap_or(&0))));
                            (fname.clone(), arg_expr)
                        })
                        .collect();
                    let type_name = layout.map(|(n, _)| n.clone())
                        .unwrap_or_else(|| format!("Struct{}", id.0));
                    stack.push(Expr::StructNew(type_name, field_exprs));
                }
                GetField(obj, idx) => {
                    let obj_expr = if let Some(expr) = self.var_expr_map.get(obj) {
                        expr.clone()
                    } else {
                        Expr::Var(self.var_name(*obj))
                    };
                    let field_name = self.get_field_name(*obj, *idx, module);
                    stack.push(Expr::Field(Box::new(obj_expr), field_name));
                }
                SetField(obj, idx, val) => {
                    let obj_expr = if let Some(expr) = self.var_expr_map.get(obj) {
                        expr.clone()
                    } else {
                        Expr::Var(self.var_name(*obj))
                    };
                    let val_expr = if let Some(expr) = self.var_expr_map.get(val) {
                        expr.clone()
                    } else {
                        Expr::Var(self.var_name(*val))
                    };
                    let field_name = self.get_field_name(*obj, *idx, module);
                    stmts.push(Stmt::FieldAssign(obj_expr, field_name, val_expr));
                }
                MakeArray(_, args) => {
                    let items: Vec<Expr> = args.iter()
                        .map(|a| {
                            if let Some(expr) = self.var_expr_map.get(a) {
                                expr.clone()
                            } else {
                                Expr::Var(self.var_name(*a))
                            }
                        })
                        .collect();
                    stack.push(Expr::Array(items));
                }
                IndexGet(arr, idx) => {
                    let arr_expr = if let Some(expr) = self.var_expr_map.get(arr) {
                        expr.clone()
                    } else {
                        Expr::Var(self.var_name(*arr))
                    };
                    let idx_expr = if let Some(expr) = self.var_expr_map.get(idx) {
                        expr.clone()
                    } else {
                        Expr::Var(self.var_name(*idx))
                    };
                    stack.push(Expr::Index(Box::new(arr_expr), Box::new(idx_expr)));
                }
                IndexSet(arr, idx, val) => {
                    let arr_expr = if let Some(expr) = self.var_expr_map.get(arr) {
                        expr.clone()
                    } else {
                        Expr::Var(self.var_name(*arr))
                    };
                    let idx_expr = if let Some(expr) = self.var_expr_map.get(idx) {
                        expr.clone()
                    } else {
                        Expr::Var(self.var_name(*idx))
                    };
                    let val_expr = if let Some(expr) = self.var_expr_map.get(val) {
                        expr.clone()
                    } else {
                        Expr::Var(self.var_name(*val))
                    };
                    stmts.push(Stmt::IndexAssign(arr_expr, idx_expr, val_expr));
                }
                MakeMap(pairs) => {
                    let pair_exprs: Vec<(Expr, Expr)> = pairs.iter()
                        .map(|(k, v)| {
                            let k_expr = self.var_expr_map.get(k).cloned().unwrap_or_else(|| Expr::Var(self.var_name(*k)));
                            let v_expr = self.var_expr_map.get(v).cloned().unwrap_or_else(|| Expr::Var(self.var_name(*v)));
                            (k_expr, v_expr)
                        })
                        .collect();
                    stack.push(Expr::Map(pair_exprs));
                }
                Alloc(_) => {}
                Free(vid) => {
                    let e = if let Some(expr) = self.var_expr_map.get(vid) {
                        expr.clone()
                    } else {
                        Expr::Var(self.var_name(*vid))
                    };
                    stmts.push(Stmt::Free(e));
                }
                OwnershipMove(vid) => {
                    let e = if let Some(expr) = self.var_expr_map.get(vid) {
                        expr.clone()
                    } else {
                        Expr::Var(self.var_name(*vid))
                    };
                    stack.push(Expr::Move(Box::new(e)));
                }
                Borrow(vid) => {
                    let e = if let Some(expr) = self.var_expr_map.get(vid) {
                        expr.clone()
                    } else {
                        Expr::Var(self.var_name(*vid))
                    };
                    stack.push(Expr::AddrOf(Box::new(e)));
                }
                Deref(vid) => {
                    let e = if let Some(expr) = self.var_expr_map.get(vid) {
                        expr.clone()
                    } else {
                        Expr::Var(self.var_name(*vid))
                    };
                    stack.push(Expr::Deref(Box::new(e)));
                }
                AliveCheck(_) => {}
                Dup => {}
                Pop => { stack.pop(); }
                _ => {}
            }
            i += 1;
        }

        // 处理栈中剩余的表达式语句
        for e in stack {
            if matches!(e, Expr::Call(_, _) | Expr::IndirectCall(_, _)) {
                stmts.push(Stmt::ExprStmt(e));
            }
        }

        stmts
    }
}

// ==================== 类型转 VX 语法字符串 ====================

fn type_to_vx_type(ty: &Type) -> String {
    match ty {
        Type::Void => "void".to_string(),
        Type::Int => "int".to_string(),
        Type::Float => "float".to_string(),
        Type::Bool => "bool".to_string(),
        Type::String => "string".to_string(),
        Type::Struct(name, _) => name.clone(),
        Type::Array(inner) => format!("[]{}", type_to_vx_type(inner)),
        Type::Map(k, v) => format!("map[{}]{}", type_to_vx_type(k), type_to_vx_type(v)),
        Type::Func(params, ret) => {
            let params_str: Vec<String> = params.iter().map(|p| type_to_vx_type(p)).collect();
            format!("func({}) -> {}", params_str.join(", "), type_to_vx_type(ret))
        }
        Type::Pointer(inner) => format!("*{}", type_to_vx_type(inner)),
        Type::Generic(name, args) => {
            let args_str: Vec<String> = args.iter().map(|a| type_to_vx_type(a)).collect();
            format!("{}<{}>", name, args_str.join(", "))
        }
        Type::Unknown => "var".to_string(),
    }
}