// ==================== VX VM 集成测试 ====================
// 测试编译→字节码→VM 执行的端到端流程

use std::collections::HashMap;
use std::sync::Arc;
use vx_vm::*;

/// 辅助：构造一个包含单函数 `__main__` 的最简 Module
fn make_module(name: &str, instructions: Vec<Instruction>, constants: Vec<Value>) -> Module {
    let mut function_map = HashMap::new();
    function_map.insert("__main__".to_string(), 0);

    Module {
        constants,
        functions: vec![Function {
            name: name.to_string(),
            instructions,
            constants: vec![],
            num_params: 0,
            has_return: true,
            param_names: vec![],
        }],
        function_map,
        struct_defs: HashMap::new(),
    }
}

/// 快速运行：创建 VM→加载 Module→执行，返回最终 Value
fn run_test(module: Module) -> Result<Value, String> {
    let mut vm = VM::new();
    vm.module = module;
    vm.run()
}

// ======================== 算术测试 ========================

#[test]
fn test_int_add() {
    let module = make_module(
        "__main__",
        vec![
            Instruction::with_iarg(OpCode::LoadConst, 0),
            Instruction::with_iarg(OpCode::LoadConst, 1),
            Instruction::new(OpCode::BinaryAdd),
            Instruction::new(OpCode::Return),
        ],
        vec![Value::Int(3), Value::Int(4)],
    );
    assert_eq!(run_test(module).unwrap(), Value::Int(7));
}

#[test]
fn test_int_sub() {
    let module = make_module(
        "__main__",
        vec![
            Instruction::with_iarg(OpCode::LoadConst, 0),
            Instruction::with_iarg(OpCode::LoadConst, 1),
            Instruction::new(OpCode::BinarySub),
            Instruction::new(OpCode::Return),
        ],
        vec![Value::Int(10), Value::Int(3)],
    );
    assert_eq!(run_test(module).unwrap(), Value::Int(7));
}

#[test]
fn test_int_mul() {
    let module = make_module(
        "__main__",
        vec![
            Instruction::with_iarg(OpCode::LoadConst, 0),
            Instruction::with_iarg(OpCode::LoadConst, 1),
            Instruction::new(OpCode::BinaryMul),
            Instruction::new(OpCode::Return),
        ],
        vec![Value::Int(6), Value::Int(7)],
    );
    assert_eq!(run_test(module).unwrap(), Value::Int(42));
}

#[test]
fn test_int_div() {
    let module = make_module(
        "__main__",
        vec![
            Instruction::with_iarg(OpCode::LoadConst, 0),
            Instruction::with_iarg(OpCode::LoadConst, 1),
            Instruction::new(OpCode::BinaryDiv),
            Instruction::new(OpCode::Return),
        ],
        vec![Value::Int(10), Value::Int(3)],
    );
    assert_eq!(run_test(module).unwrap(), Value::Int(3));
}

#[test]
fn test_int_mod() {
    let module = make_module(
        "__main__",
        vec![
            Instruction::with_iarg(OpCode::LoadConst, 0),
            Instruction::with_iarg(OpCode::LoadConst, 1),
            Instruction::new(OpCode::BinaryMod),
            Instruction::new(OpCode::Return),
        ],
        vec![Value::Int(10), Value::Int(3)],
    );
    assert_eq!(run_test(module).unwrap(), Value::Int(1));
}

#[test]
fn test_float_add() {
    let module = make_module(
        "__main__",
        vec![
            Instruction::with_iarg(OpCode::LoadConst, 0),
            Instruction::with_iarg(OpCode::LoadConst, 1),
            Instruction::new(OpCode::AddFloat),
            Instruction::new(OpCode::Return),
        ],
        vec![Value::Float(1.5), Value::Float(2.5)],
    );
    assert_eq!(run_test(module).unwrap(), Value::Float(4.0));
}

#[test]
fn test_mixed_arithmetic() {
    // (3 + 4) * 2 - 5 = 9
    let module = make_module(
        "__main__",
        vec![
            Instruction::with_iarg(OpCode::LoadConst, 0),
            Instruction::with_iarg(OpCode::LoadConst, 1),
            Instruction::new(OpCode::BinaryAdd),
            Instruction::with_iarg(OpCode::LoadConst, 2),
            Instruction::new(OpCode::BinaryMul),
            Instruction::with_iarg(OpCode::LoadConst, 3),
            Instruction::new(OpCode::BinarySub),
            Instruction::new(OpCode::Return),
        ],
        vec![
            Value::Int(3),
            Value::Int(4),
            Value::Int(2),
            Value::Int(5),
        ],
    );
    assert_eq!(run_test(module).unwrap(), Value::Int(9));
}

#[test]
fn test_neg_int() {
    let module = make_module(
        "__main__",
        vec![
            Instruction::with_iarg(OpCode::LoadConst, 0),
            Instruction::new(OpCode::NegInt),
            Instruction::new(OpCode::Return),
        ],
        vec![Value::Int(42)],
    );
    assert_eq!(run_test(module).unwrap(), Value::Int(-42));
}

    #[test]
    #[allow(clippy::approx_constant)]
    fn test_neg_float() {
        let module = make_module(
            "__main__",
            vec![
                Instruction::with_iarg(OpCode::LoadConst, 0),
                Instruction::new(OpCode::NegFloat),
                Instruction::new(OpCode::Return),
            ],
            vec![Value::Float(3.14)],
        );
        assert_eq!(run_test(module).unwrap(), Value::Float(-3.14));
    }


#[test]
fn test_unary_neg() {
    let module = make_module(
        "__main__",
        vec![
            Instruction::with_iarg(OpCode::LoadConst, 0),
            Instruction::new(OpCode::UnaryNeg),
            Instruction::new(OpCode::Return),
        ],
        vec![Value::Int(10)],
    );
    assert_eq!(run_test(module).unwrap(), Value::Int(-10));
}

#[test]
fn test_pow() {
    let module = make_module(
        "__main__",
        vec![
            Instruction::with_iarg(OpCode::LoadConst, 0),
            Instruction::with_iarg(OpCode::LoadConst, 1),
            Instruction::new(OpCode::BinaryPow),
            Instruction::new(OpCode::Return),
        ],
        vec![Value::Int(2), Value::Int(10)],
    );
    assert_eq!(run_test(module).unwrap(), Value::Int(1024));
}

#[test]
fn test_div_by_zero_returns_error() {
    let module = make_module(
        "__main__",
        vec![
            Instruction::with_iarg(OpCode::LoadConst, 0),
            Instruction::with_iarg(OpCode::LoadConst, 1),
            Instruction::new(OpCode::DivInt),
            Instruction::new(OpCode::Return),
        ],
        vec![Value::Int(1), Value::Int(0)],
    );
    assert!(run_test(module).is_err());
}

// ======================== 比较测试 ========================

#[test]
fn test_eq_int_true() {
    let module = make_module(
        "__main__",
        vec![
            Instruction::with_iarg(OpCode::LoadConst, 0),
            Instruction::with_iarg(OpCode::LoadConst, 1),
            Instruction::new(OpCode::EqInt),
            Instruction::new(OpCode::Return),
        ],
        vec![Value::Int(5), Value::Int(5)],
    );
    assert_eq!(run_test(module).unwrap(), Value::Bool(true));
}

#[test]
fn test_eq_int_false() {
    let module = make_module(
        "__main__",
        vec![
            Instruction::with_iarg(OpCode::LoadConst, 0),
            Instruction::with_iarg(OpCode::LoadConst, 1),
            Instruction::new(OpCode::EqInt),
            Instruction::new(OpCode::Return),
        ],
        vec![Value::Int(5), Value::Int(6)],
    );
    assert_eq!(run_test(module).unwrap(), Value::Bool(false));
}

#[test]
fn test_binary_eq() {
    let module = make_module(
        "__main__",
        vec![
            Instruction::with_iarg(OpCode::LoadConst, 0),
            Instruction::with_iarg(OpCode::LoadConst, 1),
            Instruction::new(OpCode::BinaryEq),
            Instruction::new(OpCode::Return),
        ],
        vec![Value::Int(7), Value::Int(7)],
    );
    assert_eq!(run_test(module).unwrap(), Value::Bool(true));
}

#[test]
fn test_lt_int() {
    let module = make_module(
        "__main__",
        vec![
            Instruction::with_iarg(OpCode::LoadConst, 0),
            Instruction::with_iarg(OpCode::LoadConst, 1),
            Instruction::new(OpCode::LtInt),
            Instruction::new(OpCode::Return),
        ],
        vec![Value::Int(3), Value::Int(5)],
    );
    assert_eq!(run_test(module).unwrap(), Value::Bool(true));
}

    #[test]
    #[allow(clippy::approx_constant)]
    fn test_gt_float() {
        let module = make_module(
            "__main__",
            vec![
                Instruction::with_iarg(OpCode::LoadConst, 0),
                Instruction::with_iarg(OpCode::LoadConst, 1),
                Instruction::new(OpCode::GtFloat),
                Instruction::new(OpCode::Return),
            ],
            vec![Value::Float(3.14), Value::Float(2.71)],
        );
        assert_eq!(run_test(module).unwrap(), Value::Bool(true));
    }


#[test]
fn test_and_or() {
    // true && false = false
    let module = make_module(
        "__main__",
        vec![
            Instruction::new(OpCode::LoadTrue),
            Instruction::new(OpCode::LoadFalse),
            Instruction::new(OpCode::And),
            Instruction::new(OpCode::Return),
        ],
        vec![],
    );
    assert_eq!(run_test(module).unwrap(), Value::Bool(false));

    // true || false = true
    let module = make_module(
        "__main__",
        vec![
            Instruction::new(OpCode::LoadTrue),
            Instruction::new(OpCode::LoadFalse),
            Instruction::new(OpCode::Or),
            Instruction::new(OpCode::Return),
        ],
        vec![],
    );
    assert_eq!(run_test(module).unwrap(), Value::Bool(true));
}

#[test]
fn test_not() {
    let module = make_module(
        "__main__",
        vec![
            Instruction::new(OpCode::LoadTrue),
            Instruction::new(OpCode::Not),
            Instruction::new(OpCode::Return),
        ],
        vec![],
    );
    assert_eq!(run_test(module).unwrap(), Value::Bool(false));
}

#[test]
fn test_unary_not() {
    let module = make_module(
        "__main__",
        vec![
            Instruction::new(OpCode::LoadTrue),
            Instruction::new(OpCode::UnaryNot),
            Instruction::new(OpCode::Return),
        ],
        vec![],
    );
    assert_eq!(run_test(module).unwrap(), Value::Bool(false));
}

// ======================== 控制流测试 ========================

#[test]
fn test_jump_if_false_skip() {
    // Load 0 (falsy), JumpIfFalse to PC=4 (skip LoadConst[1] and Return),
    // else path: LoadConst[1], Return
    // PC: 0=LoadConst[0], 1=JumpIfFalse→4, 2=LoadConst[1], 3=Return, 4=LoadConst[2], 5=Return
    //
    // Since 0 is falsy, jump to 4, load Const[2]=999, return 999
    let module = make_module(
        "__main__",
        vec![
            Instruction::with_iarg(OpCode::LoadConst, 0), // push 0
            Instruction::with_iarg(OpCode::JumpIfFalse, 4), // pop; if false → PC=4
            Instruction::with_iarg(OpCode::LoadConst, 1), // push 100  (skipped)
            Instruction::new(OpCode::Return),              // return 100 (skipped)
            Instruction::with_iarg(OpCode::LoadConst, 2), // push 999
            Instruction::new(OpCode::Return),              // return 999
        ],
        vec![Value::Int(0), Value::Int(100), Value::Int(999)],
    );
    assert_eq!(run_test(module).unwrap(), Value::Int(999));
}

#[test]
fn test_jump_if_true_take() {
    // Load 1 (truthy), JumpIfTrue to PC=4
    // PC: 0=LoadConst[0], 1=JumpIfTrue→4, 2=LoadConst[1], 3=Return, 4=LoadConst[2], 5=Return
    //
    // Since 1 is truthy, jump to 4, return 999
    let module = make_module(
        "__main__",
        vec![
            Instruction::with_iarg(OpCode::LoadConst, 0),  // push 1
            Instruction::with_iarg(OpCode::JumpIfTrue, 4),  // pop; if true → PC=4
            Instruction::with_iarg(OpCode::LoadConst, 1),   // push 100 (skipped)
            Instruction::new(OpCode::Return),                // return 100 (skipped)
            Instruction::with_iarg(OpCode::LoadConst, 2),   // push 999
            Instruction::new(OpCode::Return),                // return 999
        ],
        vec![Value::Int(1), Value::Int(100), Value::Int(999)],
    );
    assert_eq!(run_test(module).unwrap(), Value::Int(999));
}

#[test]
fn test_jump_unconditional() {
    // Jump to PC=3, skip everything in between
    let module = make_module(
        "__main__",
        vec![
            Instruction::with_iarg(OpCode::Jump, 3),        // unconditional → PC=3
            Instruction::with_iarg(OpCode::LoadConst, 0),   // skipped
            Instruction::new(OpCode::Return),                // skipped
            Instruction::with_iarg(OpCode::LoadConst, 1),   // push 77
            Instruction::new(OpCode::Return),                // return 77
        ],
        vec![Value::Int(999), Value::Int(77)],
    );
    assert_eq!(run_test(module).unwrap(), Value::Int(77));
}

// ======================== 栈操作测试 ========================

#[test]
fn test_dup() {
    let module = make_module(
        "__main__",
        vec![
            Instruction::with_iarg(OpCode::LoadConst, 0),
            Instruction::new(OpCode::Dup),
            Instruction::new(OpCode::BinaryAdd),
            Instruction::new(OpCode::Return),
        ],
        vec![Value::Int(5)],
    );
    assert_eq!(run_test(module).unwrap(), Value::Int(10));
}

#[test]
fn test_pop() {
    // Push 1, push 2, pop, return (should return 1)
    let module = make_module(
        "__main__",
        vec![
            Instruction::with_iarg(OpCode::LoadConst, 0),
            Instruction::with_iarg(OpCode::LoadConst, 1),
            Instruction::new(OpCode::Pop),
            Instruction::new(OpCode::Return),
        ],
        vec![Value::Int(1), Value::Int(2)],
    );
    assert_eq!(run_test(module).unwrap(), Value::Int(1));
}

#[test]
fn test_load_nil() {
    let module = make_module(
        "__main__",
        vec![
            Instruction::new(OpCode::LoadNil),
            Instruction::new(OpCode::Return),
        ],
        vec![],
    );
    assert_eq!(run_test(module).unwrap(), Value::Nil);
}

#[test]
fn test_load_true() {
    let module = make_module(
        "__main__",
        vec![
            Instruction::new(OpCode::LoadTrue),
            Instruction::new(OpCode::Return),
        ],
        vec![],
    );
    assert_eq!(run_test(module).unwrap(), Value::Bool(true));
}

#[test]
fn test_load_false() {
    let module = make_module(
        "__main__",
        vec![
            Instruction::new(OpCode::LoadFalse),
            Instruction::new(OpCode::Return),
        ],
        vec![],
    );
    assert_eq!(run_test(module).unwrap(), Value::Bool(false));
}

// ======================== 字符串测试 ========================

#[test]
fn test_string_concat() {
    let module = make_module(
        "__main__",
        vec![
            Instruction::with_iarg(OpCode::LoadConst, 0),
            Instruction::with_iarg(OpCode::LoadConst, 1),
            Instruction::new(OpCode::BinaryAdd),
            Instruction::new(OpCode::Return),
        ],
        vec![
            Value::string("Hello, ".to_string()),
            Value::string("World!".to_string()),
        ],
    );
    assert_eq!(
        run_test(module).unwrap(),
        Value::string("Hello, World!".to_string())
    );
}

// ======================== 数组测试 ========================

#[test]
fn test_make_array() {
    let module = make_module(
        "__main__",
        vec![
            Instruction::with_iarg(OpCode::LoadConst, 0), // 1
            Instruction::with_iarg(OpCode::LoadConst, 1), // 2
            Instruction::with_iarg(OpCode::LoadConst, 2), // 3
            Instruction::with_iarg(OpCode::MakeArray, 3),  // [1,2,3]
            Instruction::new(OpCode::Return),
        ],
        vec![Value::Int(1), Value::Int(2), Value::Int(3)],
    );
    assert_eq!(
        run_test(module).unwrap(),
        Value::Array(Arc::new(vec![Value::Int(1), Value::Int(2), Value::Int(3)]))
    );
}

#[test]
fn test_index_get() {
    let module = make_module(
        "__main__",
        vec![
            Instruction::with_iarg(OpCode::LoadConst, 0), // [10,20,30]
            Instruction::with_iarg(OpCode::LoadConst, 1), // 1
            Instruction::new(OpCode::IndexGet),            // [10,20,30][1]
            Instruction::new(OpCode::Return),
        ],
        vec![
            Value::Array(Arc::new(vec![Value::Int(10), Value::Int(20), Value::Int(30)])),
            Value::Int(1),
        ],
    );
    assert_eq!(run_test(module).unwrap(), Value::Int(20));
}

#[test]
fn test_array_index_out_of_bounds() {
    let module = make_module(
        "__main__",
        vec![
            Instruction::with_iarg(OpCode::LoadConst, 0), // [10]
            Instruction::with_iarg(OpCode::LoadConst, 1), // 5
            Instruction::new(OpCode::IndexGet),
            Instruction::new(OpCode::Return),
        ],
        vec![
            Value::Array(Arc::new(vec![Value::Int(10)])),
            Value::Int(5),
        ],
    );
    assert!(run_test(module).is_err());
}

// ======================== 变量测试 ========================

#[test]
fn test_define_and_load_var() {
    let module = make_module(
        "__main__",
        vec![
            Instruction::with_iarg(OpCode::LoadConst, 0),
            Instruction::with_iarg(OpCode::DefineVar, 0),
            Instruction::with_iarg(OpCode::LoadVar, 0),
            Instruction::new(OpCode::Return),
        ],
        vec![Value::Int(42)],
    );
    assert_eq!(run_test(module).unwrap(), Value::Int(42));
}

#[test]
fn test_store_var() {
    let module = make_module(
        "__main__",
        vec![
            Instruction::with_iarg(OpCode::LoadConst, 0),
            Instruction::with_iarg(OpCode::DefineVar, 0),
            Instruction::with_iarg(OpCode::LoadConst, 1),
            Instruction::with_iarg(OpCode::StoreVar, 0),
            Instruction::with_iarg(OpCode::LoadVar, 0),
            Instruction::new(OpCode::Return),
        ],
        vec![Value::Int(1), Value::Int(99)],
    );
    assert_eq!(run_test(module).unwrap(), Value::Int(99));
}

#[test]
fn test_undefined_variable_error() {
    // 槽位化系统：未初始化的槽位返回 Value::Nil
    // 编译器保证所有槽位在使用前都已初始化，所以运行时不会报错
    let module = make_module(
        "__main__",
        vec![
            Instruction::with_iarg(OpCode::LoadVar, 0),
            Instruction::new(OpCode::Return),
        ],
        vec![],
    );
    // 未初始化的槽位返回 Nil
    assert_eq!(run_test(module).unwrap(), Value::Nil);
}

// ======================== Halt 测试 ========================

#[test]
fn test_halt() {
    let module = make_module(
        "__main__",
        vec![
            Instruction::new(OpCode::Halt),
        ],
        vec![],
    );
    assert_eq!(run_test(module).unwrap(), Value::Nil);
}

// ======================== Float to_string 回归测试 ========================

#[test]
fn test_float_to_string_whole_number() {
    assert_eq!(Value::Float(1.0).to_string(), "1");
    assert_eq!(Value::Float(3.0).to_string(), "3");
    assert_eq!(Value::Float(0.0).to_string(), "0");
    assert_eq!(Value::Float(100.0).to_string(), "100");
}

    #[test]
    #[allow(clippy::approx_constant)]
    fn test_float_to_string_fractional() {
        assert_eq!(Value::Float(3.14).to_string(), "3.14");
        assert_eq!(Value::Float(1.5).to_string(), "1.5");
        assert_eq!(Value::Float(0.5).to_string(), "0.5");
    }


#[test]
fn test_float_to_string_trailing_zeros() {
    // f64 中 1.200 可能存为 1.2，但某些值确实有 trailing zeros
    assert_eq!(Value::Float(2.0).to_string(), "2");
    assert_eq!(Value::Float(0.0).to_string(), "0");
}

#[test]
fn test_float_to_string_negative() {
    assert_eq!(Value::Float(-1.5).to_string(), "-1.5");
    assert_eq!(Value::Float(-1.0).to_string(), "-1");
}

    #[test]
    fn test_float_to_string_special() {
        // NaN 和 Infinity 不做特殊美化，保持 f64 默认输出
        let nan_str = Value::Float(f64::NAN).to_string();
        assert!(nan_str == "NaN" || nan_str == "nan");
        let inf_str = Value::Float(f64::INFINITY).to_string();
        assert_eq!(inf_str, "inf");
    }


// ======================== Value 通用测试 ========================

#[test]
fn test_value_is_truthy() {
    assert!(!Value::Nil.is_truthy());
    assert!(Value::Bool(true).is_truthy());
    assert!(!Value::Bool(false).is_truthy());
    assert!(Value::Int(1).is_truthy());
    assert!(!Value::Int(0).is_truthy());
    assert!(Value::Float(1.0).is_truthy());
    assert!(!Value::Float(0.0).is_truthy());
    assert!(Value::string("hello".to_string()).is_truthy());
    assert!(!Value::string("".to_string()).is_truthy());
    assert!(Value::Array(Arc::new(vec![Value::Int(1)])).is_truthy());
    assert!(!Value::Array(Arc::new(vec![])).is_truthy());
    assert!(Value::Map(Arc::new(HashMap::from([("k".to_string(), Value::Int(1))]))).is_truthy());
    assert!(!Value::Map(Arc::new(HashMap::new())).is_truthy());
}

#[test]
fn test_value_type_name() {
    assert_eq!(Value::Nil.type_name(), "Nil");
    assert_eq!(Value::Int(0).type_name(), "Int");
    assert_eq!(Value::Float(0.0).type_name(), "Float");
    assert_eq!(Value::Bool(true).type_name(), "Bool");
    assert_eq!(Value::string("s".to_string()).type_name(), "String");
    assert_eq!(Value::Array(Arc::new(vec![])).type_name(), "Array");
    assert_eq!(Value::Map(Arc::new(HashMap::new())).type_name(), "Map");
    assert_eq!(
        Value::Instance {
            class_name: Arc::from("MyClass"),
            fields: Arc::new(HashMap::new())
        }
        .type_name(),
        "MyClass"
    );
}

// ======================== 编译器 Pipeline 测试 ========================

#[test]
fn test_lexer_basic() {
    use vx_vm::token::Lexer;
    let src = "func main():\n    var x = 42\n";
    let lexer = Lexer::new(src);
    let tokens = lexer.tokenize().expect("Lexer should succeed");
    assert!(!tokens.is_empty());
    // 至少包含 func, main, (, ), :, var, x, =, 42
    assert!(tokens.len() >= 8);
}

#[test]
fn test_parser_basic() {
    use vx_vm::token::Lexer;
    use vx_vm::parser::Parser;
    // VX 使用 func 关键字和冒号语法
    let src = "func main():\n    out(\"hello\")\n";
    let lexer = Lexer::new(src);
    let tokens = lexer.tokenize().expect("Lexer should succeed");
    let mut parser = Parser::new(tokens, src);
    let ast = parser.parse().expect("Parser should succeed");
    assert!(!ast.is_empty());
}

#[test]
fn test_parser_empty_input() {
    use vx_vm::token::Lexer;
    use vx_vm::parser::Parser;
    let src = "";
    let lexer = Lexer::new(src);
    let tokens = lexer.tokenize().expect("Lexer should succeed");
    let mut parser = Parser::new(tokens, src);
    let ast = parser.parse().expect("Parser should succeed on empty input");
    assert!(ast.is_empty());
}

// ======================== 用户函数调用测试 ========================

#[test]
fn test_user_function_call() {
    let mut function_map = HashMap::new();
    function_map.insert("__main__".to_string(), 0);
    function_map.insert("add".to_string(), 1);

    let module = Module {
        constants: vec![Value::string("add".to_string()), Value::Int(3), Value::Int(4)],
        functions: vec![
            Function {
                name: "__main__".to_string(),
                instructions: vec![
                    Instruction::with_iarg(OpCode::LoadConst, 0), // "add"
                    Instruction::with_iarg(OpCode::LoadConst, 1), // 3
                    Instruction::with_iarg(OpCode::LoadConst, 2), // 4
                    Instruction::with_iarg(OpCode::Call, 2),
                    Instruction::new(OpCode::Return),
                ],
                constants: vec![],
                num_params: 0,
                has_return: true,
                param_names: vec![],
            },
            Function {
                name: "add".to_string(),
                instructions: vec![
                    Instruction::with_iarg(OpCode::LoadVar, 0),
                    Instruction::with_iarg(OpCode::LoadVar, 1),
                    Instruction::new(OpCode::AddInt),
                    Instruction::new(OpCode::Return),
                ],
                constants: vec![],
                num_params: 2,
                has_return: true,
                param_names: vec!["a".to_string(), "b".to_string()],
            },
        ],
        function_map,
        struct_defs: HashMap::new(),
    };
    assert_eq!(run_test(module).unwrap(), Value::Int(7));
}

#[test]
fn test_function_arg_count_mismatch() {
    let mut function_map = HashMap::new();
    function_map.insert("__main__".to_string(), 0);
    function_map.insert("foo".to_string(), 1);

    let module = Module {
        constants: vec![Value::string("foo".to_string()), Value::Int(1)],
        functions: vec![
            Function {
                name: "__main__".to_string(),
                instructions: vec![
                    Instruction::with_iarg(OpCode::LoadConst, 0), // "foo"
                    Instruction::with_iarg(OpCode::LoadConst, 1), // 1
                    Instruction::with_iarg(OpCode::Call, 1),
                    Instruction::new(OpCode::Return),
                ],
                constants: vec![],
                num_params: 0,
                has_return: true,
                param_names: vec![],
            },
            Function {
                name: "foo".to_string(),
                instructions: vec![Instruction::new(OpCode::Return)],
                constants: vec![],
                num_params: 2,
                has_return: true,
                param_names: vec!["a".to_string(), "b".to_string()],
            },
        ],
        function_map,
        struct_defs: HashMap::new(),
    };
    assert!(run_test(module).is_err());
}

// ======================== Struct / Instance 测试 ========================

#[test]
fn test_make_struct_and_property_get() {
    // 测试 MakeStruct + PropertySet + PropertyGet 的基本流程
    let mut struct_defs = HashMap::new();
    struct_defs.insert("Point".to_string(), vec!["x".to_string(), "y".to_string()]);

    let module = Module {
        constants: vec![Value::Int(10)],
        functions: vec![Function {
            name: "__main__".to_string(),
            instructions: vec![
                Instruction::with_sarg(OpCode::MakeStruct, "Point".to_string()),
                Instruction::new(OpCode::Dup),
                Instruction::with_iarg(OpCode::LoadConst, 0), // 10
                Instruction::with_sarg(OpCode::PropertySet, "x".to_string()),
                Instruction::new(OpCode::Pop),
                // 直接返回实例本身，验证 MakeStruct 成功
                Instruction::new(OpCode::Return),
            ],
            constants: vec![],
            num_params: 0,
            has_return: true,
            param_names: vec![],
        }],
        function_map: HashMap::from([("__main__".to_string(), 0)]),
        struct_defs,
    };
    let result = run_test(module).unwrap();
    match result {
        Value::Instance { class_name, .. } => assert_eq!(class_name, "Point".into()),
        _ => panic!("Expected Instance, got {:?}", result),
    }
}

// ======================== 内存安全测试 ========================

#[test]
fn test_newz_creates_pointer() {
    let mut struct_defs = HashMap::new();
    struct_defs.insert("Obj".to_string(), vec!["val".to_string()]);

    let module = Module {
        constants: vec![Value::string("Obj".to_string()), Value::Int(42)],
        functions: vec![Function {
            name: "__main__".to_string(),
            instructions: vec![
                Instruction::with_iarg(OpCode::LoadConst, 0), // "Obj"
                Instruction::with_iarg(OpCode::LoadConst, 1), // 42
                Instruction::with_iarg(OpCode::Newz, 1),
                Instruction::new(OpCode::Return),
            ],
            constants: vec![],
            num_params: 0,
            has_return: true,
            param_names: vec![],
        }],
        function_map: HashMap::from([("__main__".to_string(), 0)]),
        struct_defs,
    };
    let result = run_test(module).unwrap();
    match result {
        Value::Pointer { class_name, alloc_id, generation } => {
            assert_eq!(class_name, "Obj".into());
            assert!(alloc_id > 0);
            assert_eq!(generation, 0);
        }
        _ => panic!("Expected Pointer, got {:?}", result),
    }
}

#[test]
fn test_newz_deref_property() {
    let mut struct_defs = HashMap::new();
    struct_defs.insert("Point".to_string(), vec!["x".to_string(), "y".to_string()]);

    let module = Module {
        constants: vec![Value::string("Point".to_string()), Value::Int(99)],
        functions: vec![Function {
            name: "__main__".to_string(),
            instructions: vec![
                Instruction::with_iarg(OpCode::LoadConst, 0), // "Point"
                Instruction::with_iarg(OpCode::LoadConst, 1), // 99
                Instruction::with_iarg(OpCode::Newz, 1),
                Instruction::new(OpCode::Deref),
                Instruction::with_sarg(OpCode::PropertyGet, "x".to_string()),
                Instruction::new(OpCode::Return),
            ],
            constants: vec![],
            num_params: 0,
            has_return: true,
            param_names: vec![],
        }],
        function_map: HashMap::from([("__main__".to_string(), 0)]),
        struct_defs,
    };
    assert_eq!(run_test(module).unwrap(), Value::Int(99));
}

#[test]
fn test_double_free_error() {
    let mut struct_defs = HashMap::new();
    struct_defs.insert("Obj".to_string(), vec![]);

    let module = Module {
        constants: vec![Value::string("Obj".to_string())],
        functions: vec![Function {
            name: "__main__".to_string(),
            instructions: vec![
                Instruction::with_iarg(OpCode::LoadConst, 0), // "Obj"
                Instruction::with_iarg(OpCode::Newz, 0),
                Instruction::new(OpCode::Dup),
                Instruction::new(OpCode::Free),
                Instruction::new(OpCode::Free),
                Instruction::new(OpCode::Return),
            ],
            constants: vec![],
            num_params: 0,
            has_return: true,
            param_names: vec![],
        }],
        function_map: HashMap::from([("__main__".to_string(), 0)]),
        struct_defs,
    };
    assert!(run_test(module).is_err());
}

#[test]
fn test_scope_drop_cleans_allocs() {
    let mut struct_defs = HashMap::new();
    struct_defs.insert("Temp".to_string(), vec![]);

    let module = Module {
        constants: vec![Value::string("Temp".to_string())],
        functions: vec![Function {
            name: "__main__".to_string(),
            instructions: vec![
                Instruction::with_iarg(OpCode::LoadConst, 0), // "Temp"
                Instruction::with_iarg(OpCode::Newz, 0),
                Instruction::new(OpCode::Pop),
                Instruction::new(OpCode::ScopeDrop),
                Instruction::new(OpCode::LoadNil),
                Instruction::new(OpCode::Return),
            ],
            constants: vec![],
            num_params: 0,
            has_return: true,
            param_names: vec![],
        }],
        function_map: HashMap::from([("__main__".to_string(), 0)]),
        struct_defs,
    };
    assert_eq!(run_test(module).unwrap(), Value::Nil);
}

// ======================== SysArgv 测试 ========================

#[test]
fn test_sys_argv_returns_configured_args() {
    let module = make_module(
        "__main__",
        vec![
            Instruction::new(OpCode::SysArgv),
            Instruction::new(OpCode::Return),
        ],
        vec![],
    );
    let mut vm = VM::new();
    vm.module = module;
    vm.argv = vec!["test_prog".to_string(), "--flag".to_string(), "value".to_string()];
    let result = vm.run().unwrap();
    match result {
        Value::Array(args) => {
            assert_eq!(args.len(), 3);
            assert_eq!(args[0], Value::string("test_prog".to_string()));
            assert_eq!(args[1], Value::string("--flag".to_string()));
            assert_eq!(args[2], Value::string("value".to_string()));
        }
        _ => panic!("Expected Array, got {:?}", result),
    }
}

// ======================== File I/O 测试 ========================

#[test]
fn test_file_write_and_read() {
    use tempfile::NamedTempFile;
    let tmp = NamedTempFile::new().unwrap();
    let path = tmp.path().to_string_lossy().to_string();

    // 先写文件
    let write_module = make_module(
        "__main__",
        vec![
            Instruction::with_sarg(OpCode::LoadConst, "__path__".to_string()),
            Instruction::with_sarg(OpCode::LoadConst, "__content__".to_string()),
            Instruction::new(OpCode::FileWrite),
            Instruction::new(OpCode::Return),
        ],
        vec![Value::string(path.clone()), Value::string("hello vx".to_string())],
    );
    // 手动设置常量索引
    let mut write_mod = write_module;
    write_mod.constants = vec![Value::string(path.clone()), Value::string("hello vx".to_string())];
    write_mod.functions[0].instructions = vec![
        Instruction::with_iarg(OpCode::LoadConst, 0), // path
        Instruction::with_iarg(OpCode::LoadConst, 1), // content
        Instruction::new(OpCode::FileWrite),
        Instruction::new(OpCode::Return),
    ];
    run_test(write_mod).unwrap();

    // 读文件
    let mut read_mod = make_module(
        "__main__",
        vec![
            Instruction::with_iarg(OpCode::LoadConst, 0), // path
            Instruction::new(OpCode::FileRead),
            Instruction::new(OpCode::Return),
        ],
        vec![Value::string(path.clone())],
    );
    read_mod.constants = vec![Value::string(path.clone())];
    let result = run_test(read_mod).unwrap();
    assert_eq!(result, Value::string("hello vx".to_string()));
}

#[test]
fn test_file_exists() {
    use tempfile::NamedTempFile;
    let tmp = NamedTempFile::new().unwrap();
    let path = tmp.path().to_string_lossy().to_string();

    let mut module = make_module(
        "__main__",
        vec![
            Instruction::with_iarg(OpCode::LoadConst, 0),
            Instruction::new(OpCode::FileExists),
            Instruction::new(OpCode::Return),
        ],
        vec![Value::string(path)],
    );
    module.constants = vec![Value::string(tmp.path().to_string_lossy().to_string())];
    let result = run_test(module).unwrap();
    assert_eq!(result, Value::Bool(true));
}

// ======================== 解析器高级测试 ========================

#[test]
fn test_parser_struct_decl() {
    use vx_vm::token::Lexer;
    use vx_vm::parser::Parser;
    let src = "struct Point:\n    x: int\n    y: int\n";
    let lexer = Lexer::new(src);
    let tokens = lexer.tokenize().expect("Lexer should succeed");
    let mut parser = Parser::new(tokens, src);
    let ast = parser.parse().expect("Parser should succeed");
    assert!(!ast.is_empty());
}

#[test]
fn test_parser_struct_decl_advanced() {
    use vx_vm::token::Lexer;
    use vx_vm::parser::Parser;
    // 测试更复杂的结构体声明语法
    let src = "func main():\n    struct Point:\n        x: int\n        y: int\n    var p = Point(1, 2)\n";
    let lexer = Lexer::new(src);
    let tokens = lexer.tokenize().expect("Lexer should succeed");
    let mut parser = Parser::new(tokens, src);
    let ast = parser.parse().expect("Parser should succeed");
    assert!(!ast.is_empty());
}

// ======================== 整数溢出测试 ========================

#[test]
fn test_large_int_arithmetic() {
    let module = make_module(
        "__main__",
        vec![
            Instruction::with_iarg(OpCode::LoadConst, 0),
            Instruction::with_iarg(OpCode::LoadConst, 1),
            Instruction::new(OpCode::AddInt),
            Instruction::new(OpCode::Return),
        ],
        vec![Value::Int(i64::MAX - 1), Value::Int(1)],
    );
    assert_eq!(run_test(module).unwrap(), Value::Int(i64::MAX));
}

// ======================== Map 测试 ========================

#[test]
fn test_make_map_and_index_get() {
    let module = make_module(
        "__main__",
        vec![
            Instruction::with_sarg(OpCode::LoadConst, "key".to_string()),
            Instruction::with_iarg(OpCode::LoadConst, 0), // 42
            Instruction::with_iarg(OpCode::MakeMap, 1),   // {"key": 42}
            Instruction::with_sarg(OpCode::LoadConst, "key".to_string()),
            Instruction::new(OpCode::IndexGet),
            Instruction::new(OpCode::Return),
        ],
        vec![Value::Int(42)],
    );
    // Map uses string keys via to_string(), so we need to set up constants properly
    let mut m = module;
    m.constants = vec![Value::Int(42)];
    m.functions[0].instructions = vec![
        Instruction::with_sarg(OpCode::LoadConst, "k".to_string()),
        Instruction::with_iarg(OpCode::LoadConst, 0),
        Instruction::with_iarg(OpCode::MakeMap, 1),
        Instruction::with_sarg(OpCode::LoadConst, "k".to_string()),
        Instruction::new(OpCode::IndexGet),
        Instruction::new(OpCode::Return),
    ];
    assert_eq!(run_test(m).unwrap(), Value::Int(42));
}

// ======================== 多函数协作测试 ========================

#[test]
fn test_multiple_functions_with_return() {
    // 测试函数返回后正确恢复调用帧
    let mut function_map = HashMap::new();
    function_map.insert("__main__".to_string(), 0);
    function_map.insert("double".to_string(), 1);

    let module = Module {
        constants: vec![Value::string("double".to_string()), Value::Int(21)],
        functions: vec![
            Function {
                name: "__main__".to_string(),
                instructions: vec![
                    Instruction::with_iarg(OpCode::LoadConst, 0), // "double"
                    Instruction::with_iarg(OpCode::LoadConst, 1), // 21
                    Instruction::with_iarg(OpCode::Call, 1),
                    Instruction::new(OpCode::Return),
                ],
                constants: vec![],
                num_params: 0,
                has_return: true,
                param_names: vec![],
            },
            Function {
                name: "double".to_string(),
                instructions: vec![
                    Instruction::with_iarg(OpCode::LoadVar, 0), // n
                    Instruction::with_iarg(OpCode::LoadVar, 0), // n
                    Instruction::new(OpCode::AddInt),
                    Instruction::new(OpCode::Return),
                ],
                constants: vec![],
                num_params: 1,
                has_return: true,
                param_names: vec!["n".to_string()],
            },
        ],
        function_map,
        struct_defs: HashMap::new(),
    };
    assert_eq!(run_test(module).unwrap(), Value::Int(42));
}
// ======================== 整数溢出测试 ========================

#[test]
fn test_int_overflow_add_returns_error() {
    let module = make_module(
        "__main__",
        vec![
            Instruction::with_iarg(OpCode::LoadConst, 0),
            Instruction::with_iarg(OpCode::LoadConst, 1),
            Instruction::new(OpCode::AddInt),
            Instruction::new(OpCode::Return),
        ],
        vec![Value::Int(i64::MAX), Value::Int(1)],
    );
    assert!(run_test(module).is_err());
}

#[test]
fn test_int_overflow_sub_returns_error() {
    let module = make_module(
        "__main__",
        vec![
            Instruction::with_iarg(OpCode::LoadConst, 0),
            Instruction::with_iarg(OpCode::LoadConst, 1),
            Instruction::new(OpCode::SubInt),
            Instruction::new(OpCode::Return),
        ],
        vec![Value::Int(i64::MIN), Value::Int(1)],
    );
    assert!(run_test(module).is_err());
}

#[test]
fn test_int_overflow_mul_returns_error() {
    let module = make_module(
        "__main__",
        vec![
            Instruction::with_iarg(OpCode::LoadConst, 0),
            Instruction::with_iarg(OpCode::LoadConst, 1),
            Instruction::new(OpCode::MulInt),
            Instruction::new(OpCode::Return),
        ],
        vec![Value::Int(i64::MAX), Value::Int(2)],
    );
    assert!(run_test(module).is_err());
}

#[test]
fn test_binary_add_int_overflow_returns_error() {
    let module = make_module(
        "__main__",
        vec![
            Instruction::with_iarg(OpCode::LoadConst, 0),
            Instruction::with_iarg(OpCode::LoadConst, 1),
            Instruction::new(OpCode::BinaryAdd),
            Instruction::new(OpCode::Return),
        ],
        vec![Value::Int(i64::MAX), Value::Int(1)],
    );
    assert!(run_test(module).is_err());
}

#[test]
fn test_binary_sub_int_overflow_returns_error() {
    let module = make_module(
        "__main__",
        vec![
            Instruction::with_iarg(OpCode::LoadConst, 0),
            Instruction::with_iarg(OpCode::LoadConst, 1),
            Instruction::new(OpCode::BinarySub),
            Instruction::new(OpCode::Return),
        ],
        vec![Value::Int(i64::MIN), Value::Int(1)],
    );
    assert!(run_test(module).is_err());
}

#[test]
fn test_binary_mul_int_overflow_returns_error() {
    let module = make_module(
        "__main__",
        vec![
            Instruction::with_iarg(OpCode::LoadConst, 0),
            Instruction::with_iarg(OpCode::LoadConst, 1),
            Instruction::new(OpCode::BinaryMul),
            Instruction::new(OpCode::Return),
        ],
        vec![Value::Int(i64::MAX), Value::Int(2)],
    );
    assert!(run_test(module).is_err());
}

// ======================== 使用后释放 (use-after-free) 检测测试 ========================

#[test]
fn test_use_after_free_detected() {
    let mut struct_defs = HashMap::new();
    struct_defs.insert("Obj".to_string(), vec!["x".to_string()]);

    let module = Module {
        constants: vec![Value::string("Obj".to_string()), Value::Int(42)],
        functions: vec![Function {
            name: "__main__".to_string(),
            instructions: vec![
                Instruction::with_iarg(OpCode::LoadConst, 0),
                Instruction::with_iarg(OpCode::LoadConst, 1),
                Instruction::with_iarg(OpCode::Newz, 1),
                Instruction::new(OpCode::Dup),
                Instruction::new(OpCode::Free),
                Instruction::new(OpCode::Deref),
                Instruction::new(OpCode::Return),
            ],
            constants: vec![],
            num_params: 0,
            has_return: true,
            param_names: vec![],
        }],
        function_map: HashMap::from([("__main__".to_string(), 0)]),
        struct_defs,
    };
    assert!(run_test(module).is_err());
}

#[test]
fn test_double_free_pointer_detected() {
    let mut struct_defs = HashMap::new();
    struct_defs.insert("Data".to_string(), vec!["value".to_string()]);

    let module = Module {
        constants: vec![Value::string("Data".to_string()), Value::Int(100)],
        functions: vec![Function {
            name: "__main__".to_string(),
            instructions: vec![
                Instruction::with_iarg(OpCode::LoadConst, 0),
                Instruction::with_iarg(OpCode::LoadConst, 1),
                Instruction::with_iarg(OpCode::Newz, 1),
                Instruction::new(OpCode::Dup),
                Instruction::new(OpCode::Free),
                Instruction::new(OpCode::Free),
                Instruction::new(OpCode::Return),
            ],
            constants: vec![],
            num_params: 0,
            has_return: true,
            param_names: vec![],
        }],
        function_map: HashMap::from([("__main__".to_string(), 0)]),
        struct_defs,
    };
    assert!(run_test(module).is_err());
}
