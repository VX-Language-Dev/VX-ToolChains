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

impl Compiler {
    /// 将参数类型字符串解析为 KnownType。
    ///
    /// 历史说明：v1.1.1 之前 `string` 是关键字，因此参数签名接受小写 `string`。
    /// 自 v1.1.1 起 `string` 已从关键字中移除并降级为标准库 `std::String` 类型标识符，
    /// 此处改为：
    ///   - 优先匹配大写 `String`（标准库类型签名，运行时动态解析为 KnownType::String）
    ///   - 仍兼容小写 `string`（向后兼容 v1.1.0 之前的源代码）
    ///   - 其他内置类型保持小写（与关键字一致）
    fn parse_param_type(type_str: &str) -> KnownType {
        match type_str {
            "int" => KnownType::Int,
            "float" => KnownType::Float,
            "bool" => KnownType::Bool,
            "string" | "String" => KnownType::String,
            _ => KnownType::Unknown,
        }
    }

    /// 为 struct/class 生成构造函数字节码（去重自 StructDecl/ClassDecl 分支）
    ///
    /// 字节码序列：
    ///   1. emit `<ctor>` 创建空实例（MakeStruct 或 MakeClass）
    ///   2. 对每个字段 emit `Dup / LoadVar / PropertySet / Pop` 把构造参数写入字段
    ///   3. emit `Return`
    /// 最后 emit `LoadConst <name>; StoreVar <name>` 将构造函数引用绑定到全局名。
    ///
    /// 注：构造函数的参数名沿用 `field_type` 作为惯例占位名（原行为保持一致），
    /// 这样生成的 param_names 仍表示"该字段的类型字符串"用于后续类型推断。
    fn emit_type_ctor(
        &mut self,
        ctor_op: OpCode,
        name: &str,
        field_names: &[String],
        field_param_names: Vec<String>,
    ) {
        let save = std::mem::replace(&mut self.instructions, Vec::new());
        self.emit(ctor_op, BytecodeArg::String(name.to_string()));
        for (i, fname) in field_names.iter().enumerate() {
            self.emit(OpCode::LoadVar, BytecodeArg::Int(i as i32));
            self.emit(OpCode::PropertySet, BytecodeArg::String(fname.clone()));
        }
        self.emit(OpCode::Return, BytecodeArg::None);
        self.functions.push(BytecodeFunction {
            name: name.to_string(),
            instructions: std::mem::replace(&mut self.instructions, save),
            num_params: field_names.len(),
            has_return: true,
            param_names: field_param_names,
        });
        let name_const = self.add_const(ConstantValue::String(name.to_string())) as i32;
        self.emit(OpCode::LoadConst, BytecodeArg::Int(name_const));
        self.emit(OpCode::StoreVar, BytecodeArg::String(name.to_string()));
    }

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
                Expr::StructDecl(name, _gen_params, fields, _, _, _) => {
                    structs.push((name.clone(), fields.iter().map(|f| f.1.clone()).collect()));
                    let field_names: Vec<String> = fields.iter().map(|f| f.1.clone()).collect();
                    let param_names: Vec<String> = fields.iter().map(|f| f.1.clone()).collect();
                    self.emit_type_ctor(OpCode::MakeStruct, name, &field_names, param_names);
                }
                Expr::ClassDecl(name, _, fields, methods, _, _, _, _) => {
                    classes.push((name.clone(), fields.iter().map(|f| f.1.clone()).collect()));
                    let field_names: Vec<String> = fields.iter().map(|f| f.1.clone()).collect();
                    let param_names: Vec<String> = fields.iter().map(|f| f.1.clone()).collect();
                    self.emit_type_ctor(OpCode::MakeClass, name, &field_names, param_names);
                    for m in methods {
                        if let Expr::FuncDecl(mname, _, params, _, mbody, _, _) = m.as_ref() {
                            let msave = std::mem::replace(&mut self.instructions, Vec::new());
                            let save_var_types = self.var_types.clone();
                            self.var_types.clear();
                            for (pname, ptype) in params {
                                self.var_types.insert(pname.clone(), Self::parse_param_type(ptype));
                            }
                            for x in mbody {
                                self.compile_stmt(x)?;
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
                Expr::ImportStmt(name, alias, dirs, _, _) => {
                    let lib_path = self.settings.library_path(&name);
                    // 跟踪外部依赖
                    if !self.external_deps.contains(&name) {
                        self.external_deps.push(name.clone());
                    }
                    // dirs 现在为 Vec<String> 多路径列表, 合并为逗号分隔的 Option<String>
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
                Expr::FuncDecl(fname, _, params, _, body, _, _) => {
                    let save = std::mem::replace(&mut self.instructions, Vec::new());
                    let save_var_types = self.var_types.clone();
                    let save_var_slots = self.var_slots.clone();
                    let save_next_slot = self.next_slot;
                    self.var_types.clear();
                    self.var_slots.clear();
                    self.next_slot = 0;
                    for (pname, ptype) in params {
                        self.var_types.insert(pname.clone(), Self::parse_param_type(ptype));
                        self.allocate_slot(&pname);
                    }
                    for x in body {
                        self.compile_stmt(x)?;
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
            external_deps: std::mem::replace(&mut self.external_deps, Vec::new()),
        })
    }

    fn generate_type_ir(&self, functions: &[BytecodeFunction]) -> Vec<u8> {
        let mut type_mod = TypeModule::new();
        // 预先构建函数名 → FuncId 映射，供 TypeIRSimulator 解析 Call callee
        let mut func_name_to_id: HashMap<String, FuncId> = HashMap::new();
        for (i, func) in functions.iter().enumerate() {
            func_name_to_id.insert(func.name.clone(), i as FuncId);
        }
        for (i, func) in functions.iter().enumerate() {
            let mut tf = TypeFunction::new(&func.name, i as FuncId);
            tf.param_count = func.num_params as u32;
            tf.has_return = func.has_return;
            // 若函数有返回值但未指定返回类型，默认按 Int 处理
            if tf.has_return && tf.return_type == Type::Void {
                tf.return_type = Type::Int;
            }
            for param_name in &func.param_names {
                let ptype = self.get_var_type(param_name);
                tf.params.push((param_name.clone(), self.known_to_type(ptype)));
            }
            let mut sim = TypeIRSimulator::with_function_map(func_name_to_id.clone());
            for inst in &func.instructions {
                sim.translate_inst(inst, &self.constants);
            }
            tf.body = sim.into_body();
            tf.var_count = tf.body.len() as u32;
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
            KnownType::Unknown => Type::Unknown,
            KnownType::Array => Type::Array(Box::new(Type::Unknown)),
            KnownType::Map => Type::Map(Box::new(Type::Unknown), Box::new(Type::Unknown)),
            KnownType::Instance => Type::Struct("Instance".to_string(), vec![]),
            KnownType::Pointer => Type::Pointer(Box::new(Type::Unknown)),
            KnownType::Nil => Type::Unknown,
        }
    }

    pub fn save(&self, der: &CompiledModule, path: &str) -> io::Result<()> {
        use std::io::BufWriter;

        let mut f = BufWriter::new(fs::File::create(path)?);

        // Write v3 format if TypeIR is present
        if !der.type_ir_data.is_empty() {
            // V3 模式: 先把 v2 字节码写入中间缓冲，再作为 Bytecode section 嵌入 v3
            let mut bytecode_buf = Vec::new();
            bytecode::write_vxobj_from_module(&mut bytecode_buf, der)?;
            let target = if der.target_triple.is_empty() {
                "x86_64-unknown-linux-gnu"
            } else {
                &der.target_triple
            };
            bytecode::write_vxobj_v3(
                &mut f, target,
                &der.type_ir_data, &bytecode_buf,
                &[], &[], &[],
            )
        } else {
            // V2 模式: 直接从 CompiledModule 序列化
            bytecode::write_vxobj_from_module(&mut f, der)
        }
    }

    /// 将编译产物保存为 VXCO 格式（跨平台中间文件）
    ///
    /// VXCO 格式不包含任何可执行文件特征，仅包含:
    /// - TypeIR: 类型化中间表示
    /// - DebugInfo: 调试信息（可选）
    /// - SourceMap: 源码映射（可选）
    /// - ExternalDeps: 外部依赖信息（用于动态链接）
    ///
    /// 链接器接收 VXCO 文件后负责生成目标平台的原生可执行文件。
    pub fn save_vxco(&self, der: &CompiledModule, path: &str) -> io::Result<()> {
        use std::io::BufWriter;

        let mut f = BufWriter::new(fs::File::create(path)?);

        let target = if der.target_triple.is_empty() {
            "x86_64-unknown-linux-gnu"
        } else {
            &der.target_triple
        };

        // 构建外部依赖信息
        let external_deps: Vec<crate::bytecode::ExternalDependency> = der
            .external_deps
            .iter()
            .map(|name| crate::bytecode::ExternalDependency::new(name))
            .collect();

        bytecode::write_vxco(
            &mut f,
            target,
            &der.type_ir_data,
            &[], // debug_data (预留)
            &[], // source_map_data (预留)
            &external_deps,
        )
    }
}