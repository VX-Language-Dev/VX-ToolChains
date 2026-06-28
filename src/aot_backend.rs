// ==================== Cranelift AOT Backend ====================
// 将 TypeIR 编译为目标平台原生机器码
//
// 编译流水线:
//   TypeIR Module → CLIF IR (cranelift-frontend) → 机器码编译 → ELF/Mach-O/PE 对象文件
//
// 使用方式:
//   - 宿主原生 AOT:  `AotBackend::host_native()`  → 编译为当前 CPU
//   - 指定目标 AOT:  `AotBackend::for_target("aarch64-...")`  → 交叉编译
//
// 平台支持 (cranelift-codegen "all-arch" feature):
//   ✅ x86_64 (linux / windows / darwin)   — 宿主优先支持
//   ✅ aarch64 (linux / darwin)            — Apple Silicon / ARM 服务器
//   ⚠️ riscv64 (linux)                      — 实验性
//   ⚠️ s390x (linux)                        — 实验性

use std::collections::HashMap;
use target_lexicon::Triple;

use cranelift_codegen::ir::{InstBuilder, UserExternalNameRef, UserFuncName};
use cranelift_codegen::settings::Configurable;
use cranelift_module::Module;

use crate::type_ir::*;

// ==================== CLIF 类型映射 ====================

/// 将 TypeIR 类型映射到 Cranelift IR 原生类型
fn type_to_clif(ty: &Type) -> crate::aot_backend::types::Type {
    use crate::aot_backend::types;
    match ty {
        Type::Void => unreachable!("Void should be handled at signature level"),
        Type::Int => types::I64,
        Type::Bool => types::I64,   // bool 按 i64 编码 (0/1)
        Type::Float => types::F64,
        Type::String => types::I64, // 指针 → i64
        Type::Pointer(_) => types::I64,
        Type::Array(_) | Type::Map(_, _) | Type::Func(_, _) | Type::Generic(_, _) => types::I64,
        Type::Struct(_, _) => types::I64, // 结构体引用 → 指针
        Type::Unknown => types::I64,
    }
}

// ==================== AOT 后端主结构 ====================

use cranelift_codegen as cl;
use cranelift_frontend as clf;
use cranelift_module as clm;
use cranelift_object as clo;

// 重导出以便在本文件内引用
mod types {
    pub use cranelift_codegen::ir::types::*;
}

/// AOT 编译器：TypeIR → 原生机器码对象文件
pub struct AotBackend {
    /// Cranelift 模块管理器 (对象文件后端)
    /// 使用 Option 以便 finish 时取出所有权
    module: Option<clo::ObjectModule>,
    /// FunctionBuilder 重用上下文
    builder_ctx: clf::FunctionBuilderContext,
    /// 编译上下文
    ctx: cl::Context,
    /// 完整的 ISA 实例
    isa: cl::isa::OwnedTargetIsa,
}

impl AotBackend {
    /// 创建针对宿主 CPU 的 AOT 后端 (自动检测)
    pub fn host_native() -> Result<Self, String> {
        // 自动探测宿主 CPU 特性
        let isa_builder = cranelift_native::builder()
            .map_err(|e| format!("Failed to probe host CPU: {}", e))?;

        // 启用优化设置
        let mut flag_builder = cl::settings::builder();
        flag_builder
            .set("opt_level", "speed")
            .map_err(|e| format!("Failed to set opt_level: {}", e))?;
        flag_builder
            .set("enable_verifier", "false")
            .map_err(|e| format!("Failed to set verifier: {}", e))?;

        let flags = cl::settings::Flags::new(flag_builder);
        let isa: cl::isa::OwnedTargetIsa = isa_builder
            .finish(flags)
            .map_err(|e| format!("Failed to create host ISA: {}", e))?;

        // 对象文件后端
        let obj_builder = clo::ObjectBuilder::new(
            isa.clone(),
            "vx_aot_module".to_string(),
            clm::default_libcall_names(),
        )
        .map_err(|e| format!("Failed to create object builder: {}", e))?;

        Ok(AotBackend {
            module: Some(clo::ObjectModule::new(obj_builder)),
            builder_ctx: clf::FunctionBuilderContext::new(),
            ctx: cl::Context::new(),
            isa,
        })
    }

    /// 创建针对指定目标 triple 的 AOT 后端 (交叉编译)
    #[allow(dead_code)]
    pub fn for_target(target_triple: &str) -> Result<Self, String> {
        let triple: Triple = target_triple
            .parse()
            .map_err(|e| format!("Invalid target triple '{}': {}", target_triple, e))?;

        // 根据 triple 构造 ISA
        let mut flag_builder = cl::settings::builder();
        flag_builder
            .set("opt_level", "speed")
            .map_err(|e| format!("opt_level: {}", e))?;

        let isa = match triple.architecture {
            target_lexicon::Architecture::X86_64 => {
                let ib = cl::isa::lookup(triple.clone())
                    .map_err(|e| format!("x86_64 ISA lookup for '{}': {}", target_triple, e))?;
                ib.finish(cl::settings::Flags::new(flag_builder))
                    .map_err(|e| format!("x86_64 ISA: {}", e))?
            }
            target_lexicon::Architecture::Aarch64(_) => {
                // ARM64 交叉编译: 需要 cranelift-codegen "all-arch" feature
                let ib = cl::isa::lookup(triple.clone())
                    .map_err(|e| format!("aarch64 ISA lookup for '{}': {}", target_triple, e))?;
                ib.finish(cl::settings::Flags::new(flag_builder))
                    .map_err(|e| format!("aarch64 ISA: {}", e))?
            }
            _ => {
                // 尝试通用 ISA 查找
                let ib = cl::isa::lookup(triple)
                    .map_err(|e| format!("Unsupported architecture '{}': {}", target_triple, e))?;
                ib.finish(cl::settings::Flags::new(flag_builder))
                    .map_err(|e| format!("ISA finish: {}", e))?
            }
        };

        let obj_builder = clo::ObjectBuilder::new(
            isa.clone(),
            format!("vx_aot_{}", target_triple),
            clm::default_libcall_names(),
        )
        .map_err(|e| format!("Object builder: {}", e))?;

        Ok(AotBackend {
            module: Some(clo::ObjectModule::new(obj_builder)),
            builder_ctx: clf::FunctionBuilderContext::new(),
            ctx: cl::Context::new(),
            isa,
        })
    }

    /// 主编译入口：将 TypeModule 编译为原生机器码对象文件
    ///
    /// 返回: ELF/Mach-O/PE 对象文件字节 (可在链接阶段使用)
    pub fn compile_module(&mut self, type_module: &TypeModule) -> Result<Vec<u8>, String> {
        let total = type_module.functions.len();
        if total == 0 {
            return Err("No functions in TypeModule".into());
        }

        // ===== 阶段 1: 声明所有函数 (支持互递归) =====
        let mut declared_ids: HashMap<FuncId, clm::FuncId> = HashMap::new();

        for func in &type_module.functions {
            let mut sig = cl::ir::Signature::new(self.isa.default_call_conv());

            // 参数
            for (_, pty) in &func.params {
                sig.params
                    .push(cl::ir::AbiParam::new(type_to_clif(pty)));
            }
            // 返回值
            if func.has_return && func.return_type != Type::Void {
                sig.returns
                    .push(cl::ir::AbiParam::new(type_to_clif(&func.return_type)));
            }

            // main / __main__ 作为导出符号，便于链接器定位入口点
            let linkage = if func.name == "main" || func.name == "__main__" {
                clm::Linkage::Export
            } else {
                clm::Linkage::Local
            };
            let cl_func_id = self
                .module
                .as_mut()
                .unwrap()
                .declare_function(&func.name, linkage, &sig)
                .map_err(|e| format!("Failed to declare '{}': {}", func.name, e))?;

            declared_ids.insert(func.id, cl_func_id);
        }

        // ===== 阶段 2: 定义每个函数 (编译 TypeIR → CLIF) =====
        for func in &type_module.functions {
            let cl_id = declared_ids[&func.id];
            self.compile_function(func, cl_id, &declared_ids, type_module)?;
        }

        // ===== 阶段 3: 生成对象文件 =====
        let product = self.module.take().unwrap().finish();
        let obj_data = product
            .emit()
            .map_err(|e| format!("Object emit failed: {}", e))?;

        Ok(obj_data)
    }

    /// 编译单个 TypeIR 函数 → CLIF IR → 机器码
    fn compile_function(
        &mut self,
        func: &TypeFunction,
        module_func_id: clm::FuncId,
        all_func_ids: &HashMap<FuncId, clm::FuncId>,
        type_module: &TypeModule,
    ) -> Result<(), String> {
        use cl::ir::{AbiParam, Signature};

        // 构建 CLIF 函数签名
        let mut sig = Signature::new(self.isa.default_call_conv());
        for (_, pty) in &func.params {
            sig.params.push(AbiParam::new(type_to_clif(pty)));
        }
        if func.has_return && func.return_type != Type::Void {
            sig.returns
                .push(AbiParam::new(type_to_clif(&func.return_type)));
        }

        let name = UserFuncName::user(0, module_func_id.as_u32());
        let mut clif_func = cl::ir::Function::with_name_signature(name, sig);

        // ===== 使用 FunctionBuilder 构建 IR =====
        let mut builder =
            clf::FunctionBuilder::new(&mut clif_func, &mut self.builder_ctx);

        let entry_block = builder.create_block();
        builder.append_block_params_for_function_params(entry_block);
        builder.switch_to_block(entry_block);

        // 变量映射: TypeIR VarId → CLIF Variable
        let mut var_map: HashMap<VarId, clf::Variable> = HashMap::new();

        // 注册参数变量
        for (i, (_pname, _pty)) in func.params.iter().enumerate() {
            let var = clf::Variable::from_u32(i as u32);
            builder.declare_var(var, type_to_clif(&_pty));
            let val = builder.block_params(entry_block)[i];
            builder.def_var(var, val);
            var_map.insert(i as VarId, var);
        }

        // 为函数体中的所有局部变量声明 CLIF Variable
        for (vid, vty) in &func.local_types {
            if let std::collections::hash_map::Entry::Vacant(e) = var_map.entry(*vid) {
                let var = clf::Variable::from_u32(*vid);
                builder.declare_var(var, type_to_clif(vty));
                e.insert(var);
            }
        }

        // ===== 逐指令翻译 TypeIR → CLIF IR =====
        let module_ref = self.module.as_mut().unwrap();
        let result = compile_body(&mut builder, func, &mut var_map, all_func_ids, type_module, module_ref);

        builder.finalize();

        if result.is_err() {
            return result;
        }

        // ===== 编译到机器码并写入对象文件 =====
        self.ctx.func = clif_func;

        module_ref
            .define_function(module_func_id, &mut self.ctx)
            .map_err(|e| format!("Define '{}': {}", func.name, e))?;

        module_ref
            .clear_context(&mut self.ctx);

        Ok(())
    }
}

/// 翻译 TypeIR 指令体到 CLIF IR
fn compile_body(
    builder: &mut clf::FunctionBuilder,
    func: &TypeFunction,
    var_map: &mut HashMap<VarId, clf::Variable>,
    all_func_ids: &HashMap<FuncId, clm::FuncId>,
    type_module: &TypeModule,
    module: &mut clo::ObjectModule,
) -> Result<(), String> {
        use TypedInstruction::*;

        // 块映射: TypeIR 跳转目标 → CLIF Block
        let mut block_map: HashMap<u32, cl::ir::Block> = HashMap::new();
        // 残留值栈: 用于函数内操作数传递
        let mut value_stack: Vec<cl::ir::Value> = Vec::new();

        // 第一遍: 为所有跳转目标预创建块
        for inst in &func.body {
            match inst {
                Jump(target) | JumpIfFalse(_, target) | JumpIfTrue(_, target) => {
                    block_map.entry(*target).or_insert_with(|| builder.create_block());
                }
                _ => {}
            }
        }

        for inst in func.body.iter() {
            match inst {
                // ---- 常量 ----
                ConstInt(v) => {
                    let r = builder.ins().iconst(types::I64, *v);
                    value_stack.push(r);
                }
                ConstFloat(v) => {
                    let r = builder.ins().f64const(*v);
                    value_stack.push(r);
                }
                ConstBool(v) => {
                    let r = builder.ins().iconst(types::I64, if *v { 1 } else { 0 });
                    value_stack.push(r);
                }
                ConstString(_s) => {
                    // 字符串常量: 暂用指针占位
                    let r = builder.ins().iconst(types::I64, 0);
                    value_stack.push(r);
                }
                ConstNil => {
                    let r = builder.ins().iconst(types::I64, 0);
                    value_stack.push(r);
                }

                // ---- 变量存取 ----
                LoadVar(vid) => {
                    if let Some(var) = var_map.get(vid) {
                        let val = builder.use_var(*var);
                        value_stack.push(val);
                    } else {
                        return Err(format!("LoadVar: unknown var {}", vid));
                    }
                }
                StoreVar(vid) => {
                    if let Some(val) = value_stack.pop() {
                        if let Some(var) = var_map.get(vid) {
                            builder.def_var(*var, val);
                        } else {
                            // 目标变量未注册 → 声明并定义
                            let var = clf::Variable::from_u32(*vid);
                            builder.declare_var(var, type_to_clif(
                                func.local_types.get(vid).unwrap_or(&Type::Unknown)
                            ));
                            builder.def_var(var, val);
                            var_map.insert(*vid, var);
                        }
                    }
                }

                // ---- 整数算术 ----
                I32Add(a, b) => {
                    let (va, vb) = binary_operands(builder, var_map, *a, *b, &mut value_stack)?;
                    let r = builder.ins().iadd(va, vb);
                    value_stack.push(r);
                }
                I32Sub(a, b) => {
                    let (va, vb) = binary_operands(builder, var_map, *a, *b, &mut value_stack)?;
                    let r = builder.ins().isub(va, vb);
                    value_stack.push(r);
                }
                I32Mul(a, b) => {
                    let (va, vb) = binary_operands(builder, var_map, *a, *b, &mut value_stack)?;
                    let r = builder.ins().imul(va, vb);
                    value_stack.push(r);
                }
                I32Div(a, b) => {
                    let (va, vb) = binary_operands(builder, var_map, *a, *b, &mut value_stack)?;
                    let r = builder.ins().sdiv(va, vb);
                    value_stack.push(r);
                }
                I32Mod(a, b) => {
                    let (va, vb) = binary_operands(builder, var_map, *a, *b, &mut value_stack)?;
                    let r = builder.ins().srem(va, vb);
                    value_stack.push(r);
                }

                // ---- 浮点算术 ----
                F64Add(a, b) => {
                    let (va, vb) = binary_operands(builder, var_map, *a, *b, &mut value_stack)?;
                    let r = builder.ins().fadd(va, vb);
                    value_stack.push(r);
                }
                F64Sub(a, b) => {
                    let (va, vb) = binary_operands(builder, var_map, *a, *b, &mut value_stack)?;
                    let r = builder.ins().fsub(va, vb);
                    value_stack.push(r);
                }
                F64Mul(a, b) => {
                    let (va, vb) = binary_operands(builder, var_map, *a, *b, &mut value_stack)?;
                    let r = builder.ins().fmul(va, vb);
                    value_stack.push(r);
                }
                F64Div(a, b) => {
                    let (va, vb) = binary_operands(builder, var_map, *a, *b, &mut value_stack)?;
                    let r = builder.ins().fdiv(va, vb);
                    value_stack.push(r);
                }

                // ---- 整数比较 ----
                I32Eq(a, b) => {
                    let (va, vb) = binary_operands(builder, var_map, *a, *b, &mut value_stack)?;
                    let cmp = builder.ins().icmp(cl::ir::condcodes::IntCC::Equal, va, vb);
                    let r = builder.ins().uextend(types::I64, cmp);
                    value_stack.push(r);
                }
                I32Ne(a, b) => {
                    let (va, vb) = binary_operands(builder, var_map, *a, *b, &mut value_stack)?;
                    let cmp = builder.ins().icmp(cl::ir::condcodes::IntCC::NotEqual, va, vb);
                    let r = builder.ins().uextend(types::I64, cmp);
                    value_stack.push(r);
                }
                I32Lt(a, b) => {
                    let (va, vb) = binary_operands(builder, var_map, *a, *b, &mut value_stack)?;
                    let cmp = builder.ins().icmp(cl::ir::condcodes::IntCC::SignedLessThan, va, vb);
                    let r = builder.ins().uextend(types::I64, cmp);
                    value_stack.push(r);
                }
                I32Gt(a, b) => {
                    let (va, vb) = binary_operands(builder, var_map, *a, *b, &mut value_stack)?;
                    let cmp = builder.ins().icmp(cl::ir::condcodes::IntCC::SignedGreaterThan, va, vb);
                    let r = builder.ins().uextend(types::I64, cmp);
                    value_stack.push(r);
                }
                I32Le(a, b) => {
                    let (va, vb) = binary_operands(builder, var_map, *a, *b, &mut value_stack)?;
                    let cmp = builder.ins().icmp(cl::ir::condcodes::IntCC::SignedLessThanOrEqual, va, vb);
                    let r = builder.ins().uextend(types::I64, cmp);
                    value_stack.push(r);
                }
                I32Ge(a, b) => {
                    let (va, vb) = binary_operands(builder, var_map, *a, *b, &mut value_stack)?;
                    let cmp = builder.ins().icmp(cl::ir::condcodes::IntCC::SignedGreaterThanOrEqual, va, vb);
                    let r = builder.ins().uextend(types::I64, cmp);
                    value_stack.push(r);
                }

                // ---- 浮点比较 ----
                F64Eq(a, b) => {
                    let (va, vb) = binary_operands(builder, var_map, *a, *b, &mut value_stack)?;
                    let cmp = builder.ins().fcmp(cl::ir::condcodes::FloatCC::Equal, va, vb);
                    let r = builder.ins().uextend(types::I64, cmp);
                    value_stack.push(r);
                }
                F64Ne(a, b) => {
                    let (va, vb) = binary_operands(builder, var_map, *a, *b, &mut value_stack)?;
                    let cmp = builder.ins().fcmp(cl::ir::condcodes::FloatCC::NotEqual, va, vb);
                    let r = builder.ins().uextend(types::I64, cmp);
                    value_stack.push(r);
                }
                F64Lt(a, b) => {
                    let (va, vb) = binary_operands(builder, var_map, *a, *b, &mut value_stack)?;
                    let cmp = builder.ins().fcmp(cl::ir::condcodes::FloatCC::LessThan, va, vb);
                    let r = builder.ins().uextend(types::I64, cmp);
                    value_stack.push(r);
                }
                F64Gt(a, b) => {
                    let (va, vb) = binary_operands(builder, var_map, *a, *b, &mut value_stack)?;
                    let cmp = builder.ins().fcmp(cl::ir::condcodes::FloatCC::GreaterThan, va, vb);
                    let r = builder.ins().uextend(types::I64, cmp);
                    value_stack.push(r);
                }
                F64Le(a, b) => {
                    let (va, vb) = binary_operands(builder, var_map, *a, *b, &mut value_stack)?;
                    let cmp = builder.ins().fcmp(cl::ir::condcodes::FloatCC::LessThanOrEqual, va, vb);
                    let r = builder.ins().uextend(types::I64, cmp);
                    value_stack.push(r);
                }
                F64Ge(a, b) => {
                    let (va, vb) = binary_operands(builder, var_map, *a, *b, &mut value_stack)?;
                    let cmp = builder.ins().fcmp(cl::ir::condcodes::FloatCC::GreaterThanOrEqual, va, vb);
                    let r = builder.ins().uextend(types::I64, cmp);
                    value_stack.push(r);
                }

                // ---- 一元运算 ----
                I32Neg(vid) => {
                    let v = get_var_value(builder, var_map, *vid, &mut value_stack)?;
                    let r = builder.ins().ineg(v);
                    value_stack.push(r);
                }
                F64Neg(vid) => {
                    let v = get_var_value(builder, var_map, *vid, &mut value_stack)?;
                    let r = builder.ins().fneg(v);
                    value_stack.push(r);
                }
                BoolNot(vid) => {
                    let v = get_var_value(builder, var_map, *vid, &mut value_stack)?;
                    // bool_not: v == 0 ? 1 : 0
                    let zero = builder.ins().iconst(types::I64, 0);
                    let one = builder.ins().iconst(types::I64, 1);
                    let cmp = builder.ins().icmp(cl::ir::condcodes::IntCC::Equal, v, zero);
                    let r = builder.ins().select(cmp, one, zero);
                    value_stack.push(r);
                }

                // ---- 控制流 ----
                Jump(target) => {
                    let target_block = *block_map.get(target).unwrap_or(&builder.create_block());
                    let current_block = builder.current_block().unwrap();
                    builder.ins().jump(target_block, &[]);
                    builder.seal_block(current_block);
                    // 在新块上继续
                    builder.switch_to_block(target_block);
                }
                JumpIfFalse(cond_var, target) => {
                    let cond = get_var_value(builder, var_map, *cond_var, &mut value_stack)?;
                    let target_block = *block_map.get(target).unwrap_or(&builder.create_block());
                    let fallthrough = builder.create_block();

                    let zero = builder.ins().iconst(types::I64, 0);
                    let is_false = builder.ins().icmp(cl::ir::condcodes::IntCC::Equal, cond, zero);
                    let current_block = builder.current_block().unwrap();
                    builder.ins().brif(is_false, target_block, &[], fallthrough, &[]);
                    builder.seal_block(current_block);
                    builder.switch_to_block(fallthrough);
                    builder.seal_block(target_block);
                }
                JumpIfTrue(cond_var, target) => {
                    let cond = get_var_value(builder, var_map, *cond_var, &mut value_stack)?;
                    let target_block = *block_map.get(target).unwrap_or(&builder.create_block());
                    let fallthrough = builder.create_block();

                    let zero = builder.ins().iconst(types::I64, 0);
                    let is_true = builder.ins().icmp(cl::ir::condcodes::IntCC::NotEqual, cond, zero);
                    let current_block = builder.current_block().unwrap();
                    builder.ins().brif(is_true, target_block, &[], fallthrough, &[]);
                    builder.seal_block(current_block);
                    builder.switch_to_block(fallthrough);
                    builder.seal_block(target_block);
                }

                // ---- 函数调用 ----
                Call(callee_id, args) => {
                    if let Some(&cl_callee_id) = all_func_ids.get(callee_id) {
                        let callee_ref = module
                            .declare_func_in_func(cl_callee_id, builder.func);

                        // 收集参数
                        let mut arg_vals: Vec<cl::ir::Value> = Vec::new();
                        for arg_vid in args {
                            let v = get_var_value(builder, var_map, *arg_vid, &mut value_stack)?;
                            arg_vals.push(v);
                        }

                        let call_inst = builder.ins().call(callee_ref, &arg_vals);
                        let results = builder.inst_results(call_inst);
                        if !results.is_empty() {
                            value_stack.push(results[0]);
                        }
                    } else {
                        // 外部函数: 尝试通过名称在模块中查找
                        if let Some(_callee_name) = type_module.function_map.get(callee_id) {
                            let mut arg_vals = Vec::new();
                            for arg_vid in args {
                                let v = get_var_value(builder, var_map, *arg_vid, &mut value_stack)?;
                                arg_vals.push(v);
                            }
                            let sig_ref = builder.import_signature(builder.func.signature.clone());
                            let callee_ref = builder
                                .import_function(cl::ir::ExtFuncData {
                                    name: cl::ir::ExternalName::user(UserExternalNameRef::from_u32(*callee_id)),
                                    signature: sig_ref,
                                    colocated: false,
                                });
                            let call_inst = builder.ins().call(callee_ref, &arg_vals);
                            let results = builder.inst_results(call_inst);
                            if !results.is_empty() {
                                value_stack.push(results[0]);
                            }
                        }
                    }
                }
                CallIndirect(_func_ptr_var, args) => {
                    // 间接调用: 暂不支持，生成 trap
                    let _arg_count = args.len();
                    builder.ins().trap(cl::ir::TrapCode::UnreachableCodeReached);
                }

                // ---- 返回 ----
                Return(ret_val) => {
                    if let Some(vid) = ret_val {
                        let v = get_var_value(builder, var_map, *vid, &mut value_stack)?;
                        builder.ins().return_(&[v]);
                    } else {
                        builder.ins().return_(&[]);
                    }
                    // return_ 是块终结指令，封闭当前块并切换到新块
                    let current_block = builder.current_block().unwrap();
                    builder.seal_block(current_block);
                    let next_block = builder.create_block();
                    builder.switch_to_block(next_block);
                }

                // ---- 数据结构 (简化: 指针占位) ----
                MakeStruct(_, _) => {
                    let r = builder.ins().iconst(types::I64, 0);
                    value_stack.push(r);
                }
                GetField(_, _) => {
                    let _ = value_stack.pop();
                    let r = builder.ins().iconst(types::I64, 0);
                    value_stack.push(r);
                }
                SetField(_, _, _) => {
                    let _ = value_stack.pop();
                }
                MakeArray(_, _) | MakeMap(_) => {
                    let r = builder.ins().iconst(types::I64, 0);
                    value_stack.push(r);
                }
                IndexGet(_, _) => {
                    let _ = value_stack.pop();
                    let _ = value_stack.pop();
                    let r = builder.ins().iconst(types::I64, 0);
                    value_stack.push(r);
                }
                IndexSet(_, _, _) => {
                    let _ = value_stack.pop();
                    let _ = value_stack.pop();
                    let _ = value_stack.pop();
                }

                // ---- 所有权/Memory (AOT 模式静默忽略) ----
                Alloc(_) => {
                    let r = builder.ins().iconst(types::I64, 0);
                    value_stack.push(r);
                }
                Free(_) | OwnershipMove(_) | Borrow(_) | Deref(_) | AliveCheck(_) => {
                    let _ = value_stack.pop();
                }

                // ---- 栈操作 ----
                Dup => {
                    if let Some(&top) = value_stack.last() {
                        value_stack.push(top);
                    }
                }
                Pop => {
                    value_stack.pop();
                }
            }
        }

        // 确保所有基本块都被封闭 (seal)
        if let Some(current_block) = builder.current_block() {
            builder.seal_block(current_block);
        }

        Ok(())
}

// ==================== 辅助函数 ====================

/// 获取二元操作的操作数值
fn binary_operands(
    builder: &mut clf::FunctionBuilder,
    var_map: &HashMap<VarId, clf::Variable>,
    a: VarId,
    b: VarId,
    stack: &mut Vec<cl::ir::Value>,
) -> Result<(cl::ir::Value, cl::ir::Value), String> {
    let va = if let Some(var) = var_map.get(&a) {
        builder.use_var(*var)
    } else if let Some(v) = stack.pop() {
        v
    } else {
        return Err(format!("Binary op: unknown operand var {}", a));
    };
    let vb = if let Some(var) = var_map.get(&b) {
        builder.use_var(*var)
    } else if let Some(v) = stack.pop() {
        v
    } else {
        return Err(format!("Binary op: unknown operand var {}", b));
    };
    Ok((va, vb))
}

/// 获取指定变量的 CLIF Value
fn get_var_value(
    builder: &mut clf::FunctionBuilder,
    var_map: &HashMap<VarId, clf::Variable>,
    vid: VarId,
    stack: &mut Vec<cl::ir::Value>,
) -> Result<cl::ir::Value, String> {
    if let Some(var) = var_map.get(&vid) {
        Ok(builder.use_var(*var))
    } else if let Some(v) = stack.pop() {
        Ok(v)
    } else {
        Err(format!("Unknown variable {}", vid))
    }
}

// ==================== 顶层编译入口 (供 vxlinker 调用) ====================

/// 将 TypeModule 编译为目标平台原生对象文件
///
/// # 参数
/// - `type_module`: TypeIR 模块
/// - `target_triple`: 目标 triple (如 "x86_64-unknown-linux-gnu")
///   传 `None` 则自动检测宿主架构
///
/// # 返回
/// 原生对象文件字节 (ELF/Mach-O/PE 格式)
pub fn compile_type_module(
    type_module: &TypeModule,
    target_triple: Option<&str>,
) -> Result<Vec<u8>, String> {
    let mut backend = match target_triple {
        Some(t) => AotBackend::for_target(t)?,
        None => AotBackend::host_native()?,
    };

    backend.compile_module(type_module)
}

// ==================== 测试 ====================

#[cfg(test)]
mod tests {
    use super::*;
    use crate::type_ir::*;

    fn make_simple_module() -> TypeModule {
        let mut module = TypeModule::new();
        let mut func = TypeFunction::new("square", 0);
        func.params.push(("x".to_string(), Type::Int));
        func.return_type = Type::Int;
        func.has_return = true;
        let vid = func.add_local(Type::Int);
        func.body.push(TypedInstruction::LoadVar(0)); // load param
        func.body.push(TypedInstruction::LoadVar(0)); // load param again
        func.body.push(TypedInstruction::I32Mul(0, 0)); // multiply
        func.body.push(TypedInstruction::Return(Some(0)));
        module.functions.push(func);
        module.function_map.insert(0, "square".to_string());
        module
    }

    #[test]
    fn test_host_native_compile() {
        let module = make_simple_module();
        let result = compile_type_module(&module, None);
        assert!(
            result.is_ok(),
            "Host AOT compilation failed: {:?}",
            result.err()
        );
        let obj = result.unwrap();
        assert!(!obj.is_empty(), "Object file should not be empty");
        println!("Host AOT: compiled {} bytes object file", obj.len());
    }

    #[test]
    fn test_arithmetic_only() {
        let mut module = TypeModule::new();
        let mut func = TypeFunction::new("add_two", 1);
        func.params.push(("x".to_string(), Type::Int));
        func.return_type = Type::Int;
        func.has_return = true;
        func.body.push(TypedInstruction::ConstInt(2));
        func.body.push(TypedInstruction::I32Add(0, 0)); // x + 2
        func.body.push(TypedInstruction::Return(Some(0)));
        module.functions.push(func);
        module.function_map.insert(1, "add_two".to_string());

        let result = compile_type_module(&module, None);
        assert!(result.is_ok());
    }
}
