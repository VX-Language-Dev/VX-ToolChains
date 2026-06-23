use crate::parser::Expr;
use crate::OpCode;
use crate::compiler_bytecode::{BytecodeArg, ConstantValue};
use crate::compiler_core::{Compiler, KnownType};

impl Compiler {
    pub fn compile_expr(&mut self, e: &Expr) -> Result<(), String> {
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
                self.push_stack_type(KnownType::Nil);
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
                    let slot = self.allocate_slot(name);
                    self.emit(OpCode::LoadVar, BytecodeArg::Int(slot as i32));
                    self.push_stack_type(var_type);
                }
            },
            Expr::BinaryOp(op, left, right, _, _) => {
                self.compile_expr(left)?;
                self.compile_expr(right)?;
                let right_type = self.pop_stack_type();
                let left_type = self.pop_stack_type();
                let oc = match self.binary_op_specialized(op, left_type, right_type) {
                    Some(oc) => oc,
                    None => match op.as_ref() {
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
                        _ => return Err(format!("VX Error: 未知的二元操作符: {}", op)),
                    },
                };
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
                self.compile_expr(operand)?;
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
                            self.compile_expr(a)?;
                        }
                        self.emit(op, BytecodeArg::None);
                        return Ok(());
                    }
                }

                if let Expr::PropertyAccess(obj, prop, _, _) = callee.as_ref() {
                    self.compile_expr(obj)?;
                    let idx = self.add_const(ConstantValue::String(prop.clone())) as i32;
                    self.emit(OpCode::LoadConst, BytecodeArg::Int(idx));
                    for a in args {
                        self.compile_expr(a)?;
                    }
                    self.emit(OpCode::Call, BytecodeArg::Int((1 + args.len()) as i32));
                } else if let Expr::Identifier(name, _, _) = callee.as_ref() {
                    // 普通函数/内建函数调用 (如 out/len/str/push)：
                    // 将函数名作为字符串常量推入栈，VM 在 function_map/builtins 中查找
                    let idx = self.add_const(ConstantValue::String(name.clone())) as i32;
                    self.emit(OpCode::LoadConst, BytecodeArg::Int(idx));
                    for a in args {
                        self.compile_expr(a)?;
                    }
                    self.emit(OpCode::Call, BytecodeArg::Int(args.len() as i32));
                    self.push_stack_type(KnownType::Unknown);
                } else {
                    // 表达式返回值是函数引用
                    self.compile_expr(callee)?;
                    for a in args {
                        self.compile_expr(a)?;
                    }
                    self.emit(OpCode::Call, BytecodeArg::Int(args.len() as i32));
                    self.push_stack_type(KnownType::Unknown);
                }
            }
            Expr::IndexAccess(obj, index, _, _) => {
                self.compile_expr(obj)?;
                self.compile_expr(index)?;
                self.emit(OpCode::IndexGet, BytecodeArg::None);
            }
            Expr::PropertyAccess(obj, prop, _, _) => {
                self.compile_expr(obj)?;
                self.emit(OpCode::PropertyGet, BytecodeArg::String(prop.clone()));
            }
            Expr::ArrayLiteral(elements, _, _) => {
                for x in elements {
                    self.compile_expr(x)?;
                }
                self.emit(OpCode::MakeArray, BytecodeArg::Int(elements.len() as i32));
                self.push_stack_type(KnownType::Array);
            }
            Expr::MapLiteral(pairs, _, _) => {
                for (k, v) in pairs {
                    self.compile_expr(k)?;
                    self.compile_expr(v)?;
                }
                self.emit(OpCode::MakeMap, BytecodeArg::Int(pairs.len() as i32));
                self.push_stack_type(KnownType::Map);
            }
            Expr::NewExpr(type_name, _, args, _, _) => {
                let idx = self.add_const(ConstantValue::String(type_name.clone())) as i32;
                self.emit(OpCode::LoadConst, BytecodeArg::Int(idx));
                for a in args {
                    self.compile_expr(a)?;
                }
                self.emit(OpCode::Call, BytecodeArg::Int(args.len() as i32));
                self.push_stack_type(KnownType::Instance);
            }
            // NewzExpr 已裁减 → 由 NewExpr + zero:true 扩展或 std::mem::zeroed<T>() 替代
            // 编译器将 newz Foo(args) 展开为 new (Foo) { args..., zero: true } 或调用 zeroed 内建
            // 运行时不再存在 NewzExpr AST 变体, 此处 match 由穷尽性保证不可达
            Expr::MoveExpr(target, _, _) => {
                self.compile_expr(target)?;
                self.emit(OpCode::OwnershipMove, BytecodeArg::None);
            }
            Expr::AddressOf(operand, _, _) => {
                self.compile_expr(operand)?;
                self.emit(OpCode::BorrowCheck, BytecodeArg::None);
                self.emit(OpCode::AddressOf, BytecodeArg::None);
                self.push_stack_type(KnownType::Pointer);
            }
            Expr::Deref(operand, _, _) => {
                self.compile_expr(operand)?;
                self.emit(OpCode::AliveCheck, BytecodeArg::None);
                self.emit(OpCode::Deref, BytecodeArg::None);
                self.push_stack_type(KnownType::Instance);
            }
            Expr::PointerMember(obj, member, _, _) => {
                self.compile_expr(obj)?;
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
            | Expr::TypeExpr(..)
            | Expr::ExprStmt(..)
            | Expr::VarDecl(..)
            | Expr::Assign(..)
            | Expr::IfStmt(..)
            | Expr::MatchStmt(..)
            | Expr::WhileStmt(..)
            | Expr::ForStmt(..)
            | Expr::BreakStmt(..)
            | Expr::ContinueStmt(..)
            | Expr::ReturnStmt(..) => {}
            // 以下变体已从其 AST/解析层裁减:
            //   NewzExpr → mem::zeroed<T>() 标准库调用
            //   FreeStmt → mem::free(ptr) 标准库调用
            //   VectorLiteral → 数组字面量自动转为 std::Vec<T>
            
            // 宏节点在编译前已被展开，此处不应出现
            Expr::MacroDef(..) | Expr::MacroCall(..) => {
                // 理论上不可达，因为宏在expand_macros阶段已被处理
                // 如果到达这里，说明有未展开的宏，抛出错误
                return Err("VX Error: 未展开的宏节点".to_string());
            }
        }
        Ok(())
    }
    pub fn compile_assign(&mut self, target: &Expr, op: &str, value: &Expr) -> Result<(), String> {
        if op == "=" {
            match target {
                Expr::Identifier(name, _, _) => {
                    self.compile_expr(value)?;
                    let value_type = self.pop_stack_type();
                    self.set_var_type(name, value_type);
                    let slot = self.allocate_slot(name);
                    self.emit(OpCode::StoreVar, BytecodeArg::Int(slot as i32));
                }
                Expr::IndexAccess(obj, index, _, _) => {
                    self.compile_expr(value)?;
                    self.compile_expr(obj)?;
                    self.compile_expr(index)?;
                    self.emit(OpCode::IndexSet, BytecodeArg::None);
                }
                Expr::PropertyAccess(obj, prop, _, _) => {
                    self.compile_expr(value)?;
                    self.compile_expr(obj)?;
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
                    let slot = self.allocate_slot(name);
                    self.emit(OpCode::LoadVar, BytecodeArg::Int(slot as i32));
                    let var_type = self.get_var_type(name);
                    self.push_stack_type(var_type);
                    self.compile_expr(value)?;
                    let value_type = self.pop_stack_type();
                    let oc = match self.binary_op_specialized(bin_op, var_type, value_type) {
                        Some(oc) => oc,
                        None => match bin_op {
                            "+" => OpCode::BinaryAdd,
                            "-" => OpCode::BinarySub,
                            "*" => OpCode::BinaryMul,
                            "/" => OpCode::BinaryDiv,
                            "%" => OpCode::BinaryMod,
                            "^" => OpCode::BinaryPow,
                            _ => return Err(format!("VX Error: 未知的二元操作符: {}", bin_op)),
                        },
                    };
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
                    let slot2 = self.allocate_slot(name);
                    self.emit(OpCode::StoreVar, BytecodeArg::Int(slot2 as i32));
                }
                Expr::IndexAccess(obj, index, _, _) => {
                    self.compile_expr(obj)?;
                    self.compile_expr(index)?;
                    self.emit(OpCode::IndexGet, BytecodeArg::None);
                    self.compile_expr(value)?;
                    let oc = match bin_op {
                        "+" => OpCode::BinaryAdd,
                        "-" => OpCode::BinarySub,
                        "*" => OpCode::BinaryMul,
                        "/" => OpCode::BinaryDiv,
                        "%" => OpCode::BinaryMod,
                        "^" => OpCode::BinaryPow,
                        _ => return Err(format!("VX Error: 未知的二元操作符: {}", bin_op)),
                    };
                    self.emit(oc, BytecodeArg::None);
                    let tmp = format!("__asg_v_{}", self.instructions.len());
                    let tmp_slot = self.allocate_slot(&tmp);
                    self.emit(OpCode::StoreVar, BytecodeArg::Int(tmp_slot as i32));
                    self.compile_expr(obj)?;
                    self.compile_expr(index)?;
                    let tmp_slot2 = self.allocate_slot(&tmp);
                    self.emit(OpCode::LoadVar, BytecodeArg::Int(tmp_slot2 as i32));
                    self.emit(OpCode::IndexSet, BytecodeArg::None);
                }
                Expr::PropertyAccess(obj, prop, _, _) => {
                    self.compile_expr(obj)?;
                    self.emit(OpCode::PropertyGet, BytecodeArg::String(prop.clone()));
                    self.compile_expr(value)?;
                    let oc = match bin_op {
                        "+" => OpCode::BinaryAdd,
                        "-" => OpCode::BinarySub,
                        "*" => OpCode::BinaryMul,
                        "/" => OpCode::BinaryDiv,
                        "%" => OpCode::BinaryMod,
                        "^" => OpCode::BinaryPow,
                        _ => return Err(format!("VX Error: 未知的二元操作符: {}", bin_op)),
                    };
                    self.emit(oc, BytecodeArg::None);
                    let tmp = format!("__asg_v_{}", self.instructions.len());
                    let tmp_slot = self.allocate_slot(&tmp);
                    self.emit(OpCode::StoreVar, BytecodeArg::Int(tmp_slot as i32));
                    self.compile_expr(obj)?;
                    self.emit(OpCode::PropertySet, BytecodeArg::String(prop.clone()));
                    self.emit(OpCode::Pop, BytecodeArg::None);
                }
                _ => {}
            }
        }
        Ok(())
    }

}
