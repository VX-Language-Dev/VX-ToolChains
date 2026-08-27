package typeir

import "fmt"

// Typ is the type system tag. Primitive types are represented by baseTyp;
// composite types use the concrete types defined below.
type Typ interface {
	typ()
}

// baseTyp implements the primitive Typ tags.
type baseTyp int

func (baseTyp) typ() {}

const (
	Void baseTyp = iota
	Int
	Float
	Bool
	String
	Struct_
	Array
	Map_
	Func_
	Pointer
	Generic
	Unknown
)

// StructField represents a field in a struct type definition.
type StructField struct {
	Name      string
	FieldType Typ
}

// StructTyp represents a struct or class type with its fields.
type StructTyp struct {
	Name   string
	Fields []StructField
}

func (StructTyp) typ() {}

// ArrayTyp represents an array type. Elem is nil when the element type is unknown.
type ArrayTyp struct {
	Elem Typ
}

func (ArrayTyp) typ() {}

// MapTyp represents a map type. Key/Value are nil when unknown.
type MapTyp struct {
	Key   Typ
	Value Typ
}

func (MapTyp) typ() {}

// PointerTyp represents a pointer type. To is nil when unknown.
type PointerTyp struct {
	To Typ
}

func (PointerTyp) typ() {}

// GenericTyp represents a generic instantiation.
type GenericTyp struct {
	Name string
	Args []Typ
}

func (GenericTyp) typ() {}

// FuncTyp represents a function type.
type FuncTyp struct {
	Params []Typ
	Return Typ // nil if unknown / not specified
}

func (FuncTyp) typ() {}

// TypToString returns the string representation of a Typ.
func TypToString(t Typ) string {
	if t == nil {
		return "unknown"
	}
	switch x := t.(type) {
	case baseTyp:
		switch x {
		case Void:
			return "void"
		case Int:
			return "int"
		case Float:
			return "float"
		case Bool:
			return "bool"
		case String:
			return "string"
		case Struct_:
			return "struct"
		case Array:
			return "array"
		case Map_:
			return "map"
		case Func_:
			return "func"
		case Pointer:
			return "pointer"
		case Generic:
			return "generic"
		case Unknown:
			return "unknown"
		default:
			return "unknown"
		}
	case StructTyp:
		return x.Name
	case ArrayTyp:
		return "array"
	case MapTyp:
		return "map"
	case PointerTyp:
		return "pointer"
	case GenericTyp:
		return x.Name
	case FuncTyp:
		return "func"
	default:
		return "unknown"
	}
}

// TypesEqual performs a structural equality check on two Typ values.
func TypesEqual(a, b Typ) bool {
	if a == nil || b == nil {
		return a == b
	}
	switch at := a.(type) {
	case baseTyp:
		bt, ok := b.(baseTyp)
		return ok && at == bt
	case StructTyp:
		bt, ok := b.(StructTyp)
		if !ok || at.Name != bt.Name || len(at.Fields) != len(bt.Fields) {
			return false
		}
		for i := range at.Fields {
			if at.Fields[i].Name != bt.Fields[i].Name ||
				!TypesEqual(at.Fields[i].FieldType, bt.Fields[i].FieldType) {
				return false
			}
		}
		return true
	case ArrayTyp:
		bt, ok := b.(ArrayTyp)
		if !ok {
			return false
		}
		if at.Elem == nil || bt.Elem == nil {
			return at.Elem == bt.Elem
		}
		return TypesEqual(at.Elem, bt.Elem)
	case MapTyp:
		bt, ok := b.(MapTyp)
		if !ok {
			return false
		}
		if !typeOptionEqual(at.Key, bt.Key) || !typeOptionEqual(at.Value, bt.Value) {
			return false
		}
		return true
	case PointerTyp:
		bt, ok := b.(PointerTyp)
		if !ok {
			return false
		}
		if at.To == nil || bt.To == nil {
			return at.To == bt.To
		}
		return TypesEqual(at.To, bt.To)
	case GenericTyp:
		bt, ok := b.(GenericTyp)
		if !ok || at.Name != bt.Name || len(at.Args) != len(bt.Args) {
			return false
		}
		for i := range at.Args {
			if !TypesEqual(at.Args[i], bt.Args[i]) {
				return false
			}
		}
		return true
	case FuncTyp:
		bt, ok := b.(FuncTyp)
		if !ok || len(at.Params) != len(bt.Params) {
			return false
		}
		for i := range at.Params {
			if !TypesEqual(at.Params[i], bt.Params[i]) {
				return false
			}
		}
		if !typeOptionEqual(at.Return, bt.Return) {
			return false
		}
		return true
	default:
		return false
	}
}

func typeOptionEqual(a, b Typ) bool {
	if a == nil || b == nil {
		return a == b
	}
	return TypesEqual(a, b)
}

// IsNumeric reports whether t is Int or Float.
func IsNumeric(t Typ) bool {
	switch x := t.(type) {
	case baseTyp:
		return x == Int || x == Float
	default:
		return false
	}
}

// TypedInstruction represents a typed instruction with its opcode, arguments, and type annotations.
type TypedInstruction struct {
	Op    Opcode
	Args  []string
	Types []Typ
}

// TypeIRModule represents a complete TypeIR module (used by compiler/typeirgen).
type TypeIRModule struct {
	Name        string
	Functions   []FunctionIR
	Globals     []GlobalIR
	StructTypes []StructTypeDef
	Linkage     map[uint32]LinkageEntry
}

// GlobalIR represents a global variable in the intermediate representation.
type GlobalIR struct {
	Name  string
	Type  Typ
	Value string
}

// Opcode represents the operation code for typed instructions.
type Opcode int

const (
	// 常量指令
	ConstInt Opcode = iota
	ConstFloat
	ConstBool
	ConstString
	ConstNil
	// 变量指令
	LoadVar
	StoreVar
	// 算术/逻辑/比较指令
	I32Add
	I32Sub
	I32Mul
	I32Div
	I32Mod
	I64Add
	I64Sub
	I64Mul
	I64Div
	F64Add
	F64Sub
	F64Mul
	F64Div
	BoolAnd
	BoolOr
	BoolNot
	I32Eq
	I32Neq
	I32Lt
	I32Gt
	I32Le
	I32Ge
	F64Eq
	F64Lt
	F64Gt
	F64Le
	F64Ge
	F64Ne
	StrEq
	NeqInt
	// 位运算/逻辑
	I32And
	I32Or
	I32Neg
	F64Neg
	// 内存/所有权
	Newz
	Free
	OwnershipMove
	ScopeDrop
	BorrowCheck
	AliveCheck
	// 控制流
	Jump
	JumpIfFalse
	JumpIfTrue
	// 栈操作
	Dup
	Pop
	// 调用
	Call
	Return
	ReturnVoid
	// 数据结构
	StructNew
	FieldLoad
	FieldStore
	ArrayNew
	MapNew
	IndexGet
	IndexSet
	// 指针
	AddressOf
	Deref
	PointerMember
	// I/O
	SysArgv
	System
	FileRead
	FileWrite
	FileExists
	// 其他
	Halt
	PropertyGet
	Import
	StrConcat
	ArrayConcat
	Noop
)

// String returns the short string representation of an Opcode.
func (o Opcode) String() string {
	switch o {
	case ConstInt:
		return "ci"
	case ConstFloat:
		return "cf"
	case ConstBool:
		return "cb"
	case ConstString:
		return "cs"
	case ConstNil:
		return "cn"
	case LoadVar:
		return "lv"
	case StoreVar:
		return "sv"
	case I32Add:
		return "i32a"
	case I32Sub:
		return "i32s"
	case I32Mul:
		return "i32m"
	case I32Div:
		return "i32d"
	case I32Mod:
		return "i32mo"
	case I64Add:
		return "i64a"
	case I64Sub:
		return "i64s"
	case I64Mul:
		return "i64m"
	case I64Div:
		return "i64d"
	case F64Add:
		return "f64a"
	case F64Sub:
		return "f64s"
	case F64Mul:
		return "f64m"
	case F64Div:
		return "f64d"
	case BoolAnd:
		return "ba"
	case BoolOr:
		return "bo"
	case BoolNot:
		return "bn"
	case I32Eq:
		return "i32e"
	case I32Neq:
		return "i32n"
	case I32Lt:
		return "i32l"
	case I32Gt:
		return "i32g"
	case I32Le:
		return "i32le"
	case I32Ge:
		return "i32ge"
	case F64Eq:
		return "f64e"
	case F64Lt:
		return "f64l"
	case F64Gt:
		return "f64g"
	case F64Le:
		return "f64le"
	case F64Ge:
		return "f64ge"
	case F64Ne:
		return "f64ne"
	case I32And:
		return "i32and"
	case I32Or:
		return "i32or"
	case I32Neg:
		return "i32neg"
	case F64Neg:
		return "f64neg"
	case StrEq:
		return "ste"
	case NeqInt:
		return "nei"
	case Newz:
		return "nw"
	case Free:
		return "fr"
	case OwnershipMove:
		return "om"
	case ScopeDrop:
		return "sd"
	case BorrowCheck:
		return "bc"
	case AliveCheck:
		return "ac"
	case Jump:
		return "jp"
	case JumpIfFalse:
		return "jf"
	case JumpIfTrue:
		return "jt"
	case Dup:
		return "dp"
	case Pop:
		return "pp"
	case Call:
		return "cl"
	case Return:
		return "rt"
	case ReturnVoid:
		return "rtv"
	case StructNew:
		return "sn"
	case FieldLoad:
		return "fl"
	case FieldStore:
		return "fs"
	case ArrayNew:
		return "an"
	case MapNew:
		return "mn"
	case IndexGet:
		return "ig"
	case IndexSet:
		return "is"
	case AddressOf:
		return "ao"
	case Deref:
		return "dr"
	case PointerMember:
		return "pm"
	case SysArgv:
		return "sa"
	case System:
		return "sy"
	case FileRead:
		return "frd"
	case FileWrite:
		return "fw"
	case FileExists:
		return "fe"
	case Halt:
		return "ht"
	case PropertyGet:
		return "pg"
	case Import:
		return "im"
	case ArrayConcat:
		return "ac"
	case Noop:
		return "np"
	default:
		return fmt.Sprintf("opcode(%d)", o)
	}
}

// TypeModule represents a deserialized TypeIR module with linkage information.
type TypeModule struct {
	Name        string
	Functions   []FunctionIR
	Globals     []GlobalIR
	StructTypes []StructTypeDef
	Linkage     map[uint32]LinkageEntry
}
