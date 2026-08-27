// ==================== 编译器字节码格式 ====================
// 注意：此处的字节码仅为编译器内部用于推导 TypeIR 的临时表示，
// 不再作为可持久化或执行的输出格式。
package compiler_bytecode

import (
	"fmt"

	"github.com/vx-lang/vxc/internal/opcode"
	"github.com/vx-lang/vxc/internal/typeir"
)

// ==================== BytecodeArg ====================

// BytecodeArgKind 表示 BytecodeArg 的变体类型。
type BytecodeArgKind int

const (
	ArgNone        BytecodeArgKind = iota // 无参数
	ArgInt                                // i32 整数值
	ArgString                             // UTF-8 字符串
	ArgImportTuple                        // 导入元组 (a, b?, c?)
)

// ImportTuple 表示导入指令的三元组参数。
type ImportTuple struct {
	A string // 模块名
	B string // 可选的第二项（可能为空）
	C string // 可选的第三项（可能为空）
}

// BytecodeArg 表示字节码指令的参数。
// 使用 tagged struct 模式（Kind 字段 + 值字段）替代 Zig 的 union(enum)。
type BytecodeArg struct {
	Kind BytecodeArgKind
	Int  int32
	Str  string
	Tup  ImportTuple
}

// NoneArg 表示无参数的 BytecodeArg。
var NoneArg = BytecodeArg{Kind: ArgNone}

// NewIntArg 创建一个 Int 类型的 BytecodeArg。
func NewIntArg(v int32) BytecodeArg {
	return BytecodeArg{Kind: ArgInt, Int: v}
}

// NewStringArg 创建一个 String 类型的 BytecodeArg。
func NewStringArg(s string) BytecodeArg {
	return BytecodeArg{Kind: ArgString, Str: s}
}

// NewImportTupleArg 创建一个 ImportTuple 类型的 BytecodeArg。
func NewImportTupleArg(a, b, c string) BytecodeArg {
	return BytecodeArg{Kind: ArgImportTuple, Tup: ImportTuple{A: a, B: b, C: c}}
}

// ToVmArgs 转换为 VM 参数表示。
// 返回 (optional int, optional string) 元组。
// 调用者负责释放返回的字符串（如有）。
func (arg BytecodeArg) ToVmArgs() (*int32, string) {
	switch arg.Kind {
	case ArgNone:
		return nil, ""
	case ArgInt:
		v := arg.Int
		return &v, ""
	case ArgString:
		return nil, arg.Str
	case ArgImportTuple:
		t := arg.Tup
		b := ""
		if t.B != "" {
			b = t.B
		}
		c := ""
		if t.C != "" {
			c = t.C
		}
		result := fmt.Sprintf("%s,%s,%s", b, c, t.A)
		return nil, result
	default:
		panic(fmt.Sprintf("unknown BytecodeArgKind: %d", arg.Kind))
	}
}

// ==================== Instruction ====================

// Instruction 表示一条字节码指令。
type Instruction struct {
	Op  opcode.OpCode
	Arg BytecodeArg
}

// NewInstruction 创建一条无参数的指令。
func NewInstruction(op opcode.OpCode) Instruction {
	return Instruction{Op: op, Arg: NoneArg}
}

// NewInstructionWithArg 创建一条带参数的指令。
func NewInstructionWithArg(op opcode.OpCode, arg BytecodeArg) Instruction {
	return Instruction{Op: op, Arg: arg}
}

// ==================== BytecodeFunction ====================

// BytecodeFunction 表示一个编译后的字节码函数。
type BytecodeFunction struct {
	Name         string
	Instructions []Instruction
	NumParams    int
	HasReturn    bool
	ParamNames   []string
	// 参数类型（VX 静态类型，以 TypeIR Typ 表示）
	ParamTypes []typeir.Typ
}

// ==================== ConstantValue ====================

// ConstantValueKind 表示常量值的变体类型。
type ConstantValueKind int

const (
	ConstNil    ConstantValueKind = iota // nil 常量
	ConstBool                            // bool 值
	ConstInt                             // i64 整数值
	ConstFloat                           // f64 浮点值
	ConstString                          // UTF-8 字符串
)

// ConstantValue 表示一个编译时常量值。
// 使用 tagged struct 模式替代 Zig 的 union(enum)。
type ConstantValue struct {
	Kind  ConstantValueKind
	Bool  bool
	Int   int64
	Float float64
	Str   string
}

// NilValue 表示 Nil 常量。
var NilValue = ConstantValue{Kind: ConstNil}

// NewBoolConst 创建一个 Bool 常量。
func NewBoolConst(v bool) ConstantValue {
	return ConstantValue{Kind: ConstBool, Bool: v}
}

// NewIntConst 创建一个 Int 常量。
func NewIntConst(v int64) ConstantValue {
	return ConstantValue{Kind: ConstInt, Int: v}
}

// NewFloatConst 创建一个 Float 常量。
func NewFloatConst(v float64) ConstantValue {
	return ConstantValue{Kind: ConstFloat, Float: v}
}

// NewStringConst 创建一个 String 常量。
func NewStringConst(s string) ConstantValue {
	return ConstantValue{Kind: ConstString, Str: s}
}

// ==================== StructEntry / ClassEntry ====================

// StructEntry 表示一个结构体定义条目。
type StructEntry struct {
	Name   string
	Fields []string
}

// ClassEntry 表示一个类定义条目。
type ClassEntry struct {
	Name   string
	Fields []string
}

// ==================== CompiledModule ====================

// CompiledModule 表示一个完整的编译模块。
type CompiledModule struct {
	Functions    []BytecodeFunction
	Constants    []ConstantValue
	Structs      []StructEntry
	Classes      []ClassEntry
	TypeIRData   string
	TargetTriple string
	// 外部依赖信息，用于静态链接时的动态库链接
	ExternalDeps []string
}

// String 返回 ConstantValueKind 的字符串名称。
func (k ConstantValueKind) String() string {
	switch k {
	case ConstNil:
		return "Nil"
	case ConstBool:
		return "Bool"
	case ConstInt:
		return "Int"
	case ConstFloat:
		return "Float"
	case ConstString:
		return "String"
	default:
		return fmt.Sprintf("ConstantValueKind(%d)", k)
	}
}

// String 返回 BytecodeArgKind 的字符串名称。
func (k BytecodeArgKind) String() string {
	switch k {
	case ArgNone:
		return "None"
	case ArgInt:
		return "Int"
	case ArgString:
		return "String"
	case ArgImportTuple:
		return "ImportTuple"
	default:
		return fmt.Sprintf("BytecodeArgKind(%d)", k)
	}
}
