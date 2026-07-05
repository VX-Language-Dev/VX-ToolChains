use std::collections::HashMap;
use std::fs;
use std::io;
use crate::parser::Expr;
use crate::OpCode;
use crate::parser::Stmt;
use crate::type_ir::{self, Type, TypeModule, TypeFunction, FuncId};
use crate::compiler_bytecode::{BytecodeArg, BytecodeFunction, ConstantValue, CompiledModule};
use crate::compiler_core::{Compiler, KnownType};
use crate::bytecode;
use crate::compiler_typeir::TypeIRSimulator;
use crate::token::Lexer;
use crate::parser::Parser;

impl Compiler {
    pub fn compile(&mut self, ast: &[Stmt]) -> Result<CompiledModule, String> {
        self.constants.clear();
        self.instructions.clear();
        self.functions.clear();
        self.loop_stack.clear();
        self.for_counter = 0;
        let mut structs = Vec::new();
        let mut classes = Vec::new();

        for s in ast {
            match s {
                Expr::StructDecl(name, _type_params, fields, _methods, _line, _col) => {
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
                        param_types: Vec::new(),
                    });
                    let name_const = self.add_const(ConstantValue::String(name.clone())) as i32;
                    self.emit(OpCode::LoadConst, BytecodeArg::Int(name_const));
                    self.emit(OpCode::StoreVar, BytecodeArg::String(name.clone()));
                }
                Expr::ClassDecl(name, _type_params, fields, methods, _parent, _interfaces, _line, _col) => {
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
                        param_types: Vec::new(),
                    });
                    for m in methods {
                        if let Expr::FuncDecl(mname, _type_params, params, _ret_type, mbody, _line, _col) = m.as_ref() {
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
                                self.compile_stmt(&x)?;
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
                                param_types: Vec::new(),
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
                Expr::ImportStmt(name, alias, dirs, _, _) => {
                    // 编译时解析 import：查找并编译标准库 .vx 源文件
                    // library_path 已支持点分路径解析（如 std.io → std + io）
                    let lib_path = self.settings.library_path(&name);
                    // 确定搜索路径：先试 mod.vx，再试 <name>.vx
                    let search_paths: Vec<String> = if dirs.is_empty() {
                        if let Some(ref base) = lib_path {
                            vec![
                                format!("{}/mod.vx", base),
                                format!("{}.vx", base),
                            ]
                        } else {
                            vec![]
                        }
                    } else {
                        let prefix = lib_path.clone().unwrap_or_default();
                        dirs.iter()
                            .map(|d| format!("{}/{}/mod.vx", prefix, d))
                            .collect()
                    };
                    // 尝试在搜索路径中找到并编译模块
                    let mut module_found = false;
                    for path in &search_paths {
                        if let Ok(src) = fs::read_to_string(path) {
                            module_found = true;
                            let mut std_ast = Self::parse_vx_source(&src, path)?;
                            // 展开宏
                            std_ast = self.expand_macros(std_ast)?;
                            // 编译 import 中的函数声明和 extern
                            self.compile_import_ast(&std_ast, alias.as_deref())?;
                            break;
                        }
                    }
                    if !module_found {
                        // module的 import 要求在后端阶段也能找到实现，所以仍保留 OpCode::Import
                        // 以支持没有标准库源码时的工作流
                        let combined_dirs = if dirs.is_empty() {
                            lib_path.clone()
                        } else {
                            let prefix = lib_path.clone().unwrap_or_default();
                            let paths: Vec<String> = dirs.iter()
                                .map(|d| format!("{}/{}", prefix, d))
                                .collect();
                            Some(paths.join(","))
                        };
                        self.emit(
                            OpCode::Import,
                            BytecodeArg::ImportTuple(name.clone(), alias.clone(), combined_dirs),
                        );
                    }
                }
                Expr::ExternDecl(fname, _type_params, _params, _ret_type, _line, _col) => {
                    // 注册 extern 函数到外部依赖（供 AOT 链接器识别）
                    // 注意：不加入 self.functions，这样 TypeIR 保持未解析状态，
                    // AOT 后端会通过 u32::MAX + ext_name 机制生成外部符号引用。
                    if !self.external_deps.contains(&fname) {
                        self.external_deps.push(fname.clone());
                    }
                }
                Expr::FuncDecl(fname, _type_params, params, _ret_type, body, _line, _col) => {
                    let save = std::mem::replace(&mut self.instructions, Vec::new());
                    let save_var_types = self.var_types.clone();
                    let save_var_slots = self.var_slots.clone();
                    let save_next_slot = self.next_slot;
                    self.var_types.clear();
                    self.var_slots.clear();
                    self.next_slot = 0;
                    for (pname, ptype) in params {
                        let known_type = match ptype.as_str() {
                            "int" => KnownType::Int,
                            "float" => KnownType::Float,
                            "bool" => KnownType::Bool,
                            "string" => KnownType::String,
                            _ => KnownType::Unknown,
                        };
                        self.var_types.insert(pname.clone(), known_type);
                        self.allocate_slot(&pname);
                    }
                    for x in body {
                        self.compile_stmt(&x)?;
                    }
                    self.var_types = save_var_types;
                    self.var_slots = save_var_slots;
                    self.next_slot = save_next_slot;
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
                        param_types: Vec::new(),
                    });
                    let fname_const = self.add_const(ConstantValue::String(fname.clone())) as i32;
                    self.emit(OpCode::LoadConst, BytecodeArg::Int(fname_const));
                    let fname_slot = self.allocate_slot(&fname);
                    self.emit(OpCode::StoreVar, BytecodeArg::Int(fname_slot as i32));
                }
                _ => {
                    self.compile_stmt(s)?;
                }
            }
        }
        if !self.instructions.is_empty() {
            // 如果用户定义了 main() 函数，在 __main__ 末尾调用它
            if self.functions.iter().any(|f| f.name == "main") {
                let main_const = self.add_const(ConstantValue::String("main".into())) as i32;
                self.emit(OpCode::LoadConst, BytecodeArg::Int(main_const));
                self.emit(OpCode::Call, BytecodeArg::Int(0));
                self.emit(OpCode::Pop, BytecodeArg::None);
            }
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
                    param_types: Vec::new(),
                },
            );
        }
        // Generate TypeIR from the compiled bytecode (before replacing self.functions)
        let type_ir_data = self.generate_type_ir(&self.functions);
        Ok(CompiledModule {
            functions: std::mem::replace(&mut self.functions, Vec::new()),
            constants: std::mem::replace(&mut self.constants, Vec::new()),
            structs,
            classes,
            type_ir_data,
            target_triple: String::new(),
            external_deps: Vec::new(),
        })
    }

    fn generate_type_ir(&self, functions: &[BytecodeFunction]) -> Vec<u8> {
        let mut type_mod = TypeModule::new();
        // 预构建函数名 → FuncId 映射
        let func_name_to_id: HashMap<String, FuncId> = functions.iter()
            .enumerate()
            .map(|(i, f)| (f.name.clone(), i as FuncId))
            .collect();

        for (i, func) in functions.iter().enumerate() {
            let mut tf = TypeFunction::new(&func.name, i as FuncId);
            tf.param_count = func.num_params as u32;
            tf.has_return = func.has_return;
            for param_name in &func.param_names {
                let ptype = self.get_var_type(param_name);
                tf.params.push((param_name.clone(), self.known_to_type(ptype)));
            }
            let mut sim = TypeIRSimulator::with_function_map(func_name_to_id.clone());
            for inst in &func.instructions {
                sim.translate_inst(inst, &self.constants);
            }
            let actual_var_count = sim.var_count();
            // 从模拟器获取 slot 类型信息填入 local_types
            for (&vid, vty) in sim.slot_types() {
                tf.local_types.insert(vid, vty.clone());
            }
            tf.body = sim.into_body();
            tf.var_count = actual_var_count;
            type_mod.functions.push(tf);
            type_mod.function_map.insert(i as FuncId, func.name.clone());
        }
        if let Some(main_idx) = functions.iter().position(|f| f.name == "__main__") {
            type_mod.entry_point = Some(main_idx as FuncId);
        }
        type_ir::serialize_type_module(&type_mod)
    }

    fn known_to_type(&self, kt: KnownType) -> Type {
        match kt {
            KnownType::Int => Type::Int,
            KnownType::Float => Type::Float,
            KnownType::Bool => Type::Bool,
            KnownType::String => Type::String,
            KnownType::Array => Type::Array(Box::new(Type::Unknown)),
            KnownType::Map => Type::Map(Box::new(Type::Unknown), Box::new(Type::Unknown)),
            KnownType::Instance => Type::Unknown,
            KnownType::Pointer => Type::Pointer(Box::new(Type::Unknown)),
            KnownType::Nil => Type::Unknown,
            KnownType::Unknown => Type::Unknown,
        }
    }

    /// 解析 VX 源码文件为 AST
    fn parse_vx_source(src: &str, path: &str) -> Result<Vec<Stmt>, String> {
        let lexer = Lexer::new(src);
        let tokens = lexer.tokenize().map_err(|e| {
            format!("Lex error in {}: {}", path, e)
        })?;
        let mut parser = Parser::new(tokens, src);
        parser.parse().map_err(|e| {
            format!("Parse error in {}: {}", path, e)
        })
    }

    /// 编译 import 进来的 AST（处理 extern 声明和函数定义）
    fn compile_import_ast(&mut self, ast: &[Stmt], alias: Option<&str>) -> Result<(), String> {
        for s in ast {
            match s {
                Expr::ImportStmt(name, sub_alias, dirs, _line, _col) => {
                    // 递归解析标准库内的子模块导入
                    let lib_path = self.settings.library_path(&name);
                    let search_paths: Vec<String> = if dirs.is_empty() {
                        if let Some(ref base) = lib_path {
                            vec![
                                format!("{}/mod.vx", base),
                                format!("{}.vx", base),
                            ]
                        } else {
                            vec![]
                        }
                    } else {
                        let prefix = lib_path.clone().unwrap_or_default();
                        dirs.iter()
                            .map(|d| format!("{}/{}/mod.vx", prefix, d))
                            .collect()
                    };
                    for path in &search_paths {
                        if let Ok(src) = fs::read_to_string(path) {
                            let mut sub_ast = Self::parse_vx_source(&src, path)?;
                            sub_ast = self.expand_macros(sub_ast)?;
                            // 嵌套 import 的 alias 传递：外层 alias > 子模块 alias
                            let effective_alias = sub_alias.as_deref().or(alias);
                            self.compile_import_ast(&sub_ast, effective_alias)?;
                            break;
                        }
                    }
                }
                Expr::ExternDecl(fname, _type_params, _params, _ret_type, _line, _col) => {
                    // 注册 extern 函数到外部依赖（不加入 functions 列表，
                    // 保持 TypeIR 未解析状态以触发 AOT 的外部符号引用）
                    if !self.external_deps.contains(&fname) {
                        self.external_deps.push(fname.clone());
                    }
                }
                Expr::FuncDecl(fname, _type_params, params, _ret_type, body, _line, _col) => {
                    // 确定函数名：如果指定了 alias，用 alias.funcname
                    let qualified_name = if let Some(a) = alias {
                        format!("{}.{}", a, fname)
                    } else {
                        fname.clone()
                    };
                    let save = std::mem::replace(&mut self.instructions, Vec::new());
                    let save_var_types = self.var_types.clone();
                    let save_var_slots = self.var_slots.clone();
                    let save_next_slot = self.next_slot;
                    self.var_types.clear();
                    self.var_slots.clear();
                    self.next_slot = 0;
                    for (pname, ptype) in params {
                        let known_type = match ptype.as_str() {
                            "int" => KnownType::Int,
                            "float" => KnownType::Float,
                            "bool" => KnownType::Bool,
                            "string" => KnownType::String,
                            _ => KnownType::Unknown,
                        };
                        self.var_types.insert(pname.clone(), known_type);
                        self.allocate_slot(&pname);
                    }
                    for x in body {
                        self.compile_stmt(&x)?;
                    }
                    self.var_types = save_var_types;
                    self.var_slots = save_var_slots;
                    self.next_slot = save_next_slot;
                    if !body
                        .iter()
                        .any(|x| matches!(&**x, Expr::ReturnStmt(_, _, _)))
                    {
                        self.emit(OpCode::LoadNil, BytecodeArg::None);
                        self.emit(OpCode::Return, BytecodeArg::None);
                    }
                    self.functions.push(BytecodeFunction {
                        name: qualified_name.clone(),
                        instructions: std::mem::replace(&mut self.instructions, save),
                        num_params: params.len(),
                        has_return: true,
                        param_names: params.iter().map(|p| p.0.clone()).collect(),
                        param_types: Vec::new(),
                    });
                    // 为别名函数注册到当前作用域
                    let fname_const = self.add_const(ConstantValue::String(qualified_name)) as i32;
                    self.emit(OpCode::LoadConst, BytecodeArg::Int(fname_const));
                    let fname_slot = self.allocate_slot(&fname);
                    self.emit(OpCode::StoreVar, BytecodeArg::Int(fname_slot as i32));
                }
                _ => {
                    // 跳过头文件中的表达式语句（如 import 嵌套, if 测试等）
                }
            }
        }
        Ok(())
    }

    pub fn save(&self, _der: &CompiledModule, path: &str) -> io::Result<()> {
        use std::io::BufWriter;

        let mut f = BufWriter::new(fs::File::create(path)?);

        let target = if _der.target_triple.is_empty() {
            "x86_64-unknown-linux-gnu"
        } else {
            &_der.target_triple
        };

        // 使用 VXOBJ v4 格式写入
        bytecode::write_vxobj_v4(
            &mut f,
            target,
            &_der.type_ir_data,
            &[],  // debug_data
            &[],  // source_map_data
            &[],  // external_deps
        )
    }
}
