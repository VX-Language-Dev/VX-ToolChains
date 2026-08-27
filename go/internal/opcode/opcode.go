// OpCode 使用 explicit tag values 保证与字节码文件中的编码一致。
// `FromByte` 通过查找表实现，新增 OpCode 时只需在 const 块中添加变体并指定显式判别值即可。
// 无需手动维护冗长的 switch 表。
package opcode

import "errors"

// OpCode 使用 explicit tag values 保证与字节码文件中的编码一致。
type OpCode uint8

const (
	LoadConst     OpCode = 0x01
	LoadNil       OpCode = 0x02
	LoadTrue      OpCode = 0x03
	LoadFalse     OpCode = 0x04
	LoadVar       OpCode = 0x05
	StoreVar      OpCode = 0x06
	DefineVar     OpCode = 0x07
	Call          OpCode = 0x08
	Return        OpCode = 0x09
	MakeFunction  OpCode = 0x0A
	Jump          OpCode = 0x0B
	JumpIfFalse   OpCode = 0x0C
	JumpIfTrue    OpCode = 0x0D
	Break         OpCode = 0x0E
	Continue      OpCode = 0x0F
	BinaryAdd     OpCode = 0x10
	BinarySub     OpCode = 0x11
	BinaryMul     OpCode = 0x12
	BinaryDiv     OpCode = 0x13
	BinaryMod     OpCode = 0x14
	BinaryPow     OpCode = 0x15
	BinaryEq      OpCode = 0x16
	BinaryNe      OpCode = 0x17
	BinaryLt      OpCode = 0x18
	BinaryGt      OpCode = 0x19
	BinaryLe      OpCode = 0x1A
	BinaryGe      OpCode = 0x1B
	BinaryAnd     OpCode = 0x1C
	BinaryOr      OpCode = 0x1D
	UnaryNeg      OpCode = 0x1E
	UnaryNot      OpCode = 0x1F
	MakeStruct    OpCode = 0x20
	MakeClass     OpCode = 0x21
	MakeEnum      OpCode = 0x22
	MakeUnion     OpCode = 0x23
	LoadField     OpCode = 0x24
	StoreField    OpCode = 0x25
	MakeArray     OpCode = 0x26
	MakeMap       OpCode = 0x27
	IndexGet      OpCode = 0x28
	IndexSet      OpCode = 0x29
	PropertyGet   OpCode = 0x2A
	PropertySet   OpCode = 0x2B
	AddressOf     OpCode = 0x2C
	Deref         OpCode = 0x2D
	PointerMember OpCode = 0x2E
	Import        OpCode = 0x2F
	New           OpCode = 0x30
	Halt          OpCode = 0x31
	SysArgv       OpCode = 0x32
	System        OpCode = 0x33
	FileRead      OpCode = 0x34
	FileWrite     OpCode = 0x35
	FileExists    OpCode = 0x36
	Dup           OpCode = 0x37
	Pop           OpCode = 0x38
	// Memory Safety / Ownership
	Newz          OpCode = 0x39
	Free          OpCode = 0x3A
	OwnershipMove OpCode = 0x3B
	ScopeDrop     OpCode = 0x3C
	BorrowCheck   OpCode = 0x3D
	AliveCheck    OpCode = 0x3E
	// Specialized type ops (0x40-0x57)
	AddInt   OpCode = 0x40
	AddFloat OpCode = 0x41
	SubInt   OpCode = 0x42
	SubFloat OpCode = 0x43
	MulInt   OpCode = 0x44
	MulFloat OpCode = 0x45
	DivInt   OpCode = 0x46
	DivFloat OpCode = 0x47
	ModInt   OpCode = 0x48
	EqInt    OpCode = 0x49
	EqFloat  OpCode = 0x4A
	LtInt    OpCode = 0x4B
	LtFloat  OpCode = 0x4C
	GtInt    OpCode = 0x4D
	GtFloat  OpCode = 0x4E
	LeInt    OpCode = 0x4F
	LeFloat  OpCode = 0x50
	GeInt    OpCode = 0x51
	GeFloat  OpCode = 0x52
	NegInt   OpCode = 0x53
	NegFloat OpCode = 0x54
	Not      OpCode = 0x55
	And      OpCode = 0x56
	Or       OpCode = 0x57
	// Iterator ops (for-in support)
	Iterate OpCode = 0x60
	Next    OpCode = 0x61
)

// opCodeMax 最大 opcode 值 + 1，用于确定查找表大小。
const opCodeMax byte = 0x62 // 98

// ErrUnknownOpcode 表示无效或未定义的字节码。
var ErrUnknownOpcode = errors.New("unknown opcode")

// lookupTable 将 u8 字节码映射到 OpCode。
// 未使用的字节码位置为 0（FromByte 返回 error）。
var lookupTable [opCodeMax]OpCode

func init() {
	// 将所有已知 OpCode 填入查找表
	allOps := []OpCode{
		LoadConst, LoadNil, LoadTrue, LoadFalse,
		LoadVar, StoreVar, DefineVar,
		Call, Return, MakeFunction,
		Jump, JumpIfFalse, JumpIfTrue,
		Break, Continue,
		BinaryAdd, BinarySub, BinaryMul, BinaryDiv, BinaryMod, BinaryPow,
		BinaryEq, BinaryNe, BinaryLt, BinaryGt, BinaryLe, BinaryGe,
		BinaryAnd, BinaryOr,
		UnaryNeg, UnaryNot,
		MakeStruct, MakeClass, MakeEnum, MakeUnion,
		LoadField, StoreField,
		MakeArray, MakeMap,
		IndexGet, IndexSet,
		PropertyGet, PropertySet,
		AddressOf, Deref, PointerMember,
		Import, New, Halt,
		SysArgv, System,
		FileRead, FileWrite, FileExists,
		Dup, Pop,
		Newz, Free, OwnershipMove, ScopeDrop, BorrowCheck, AliveCheck,
		AddInt, AddFloat, SubInt, SubFloat, MulInt, MulFloat,
		DivInt, DivFloat, ModInt,
		EqInt, EqFloat, LtInt, LtFloat, GtInt, GtFloat,
		LeInt, LeFloat, GeInt, GeFloat,
		NegInt, NegFloat, Not, And, Or,
		Iterate, Next,
	}
	for _, op := range allOps {
		lookupTable[op] = op
	}
}

// FromByte 将 u8 字节码转换为 OpCode。
// 如果字节码无效或未定义，返回 ErrUnknownOpcode。
func FromByte(value byte) (OpCode, error) {
	if value >= opCodeMax {
		return 0, ErrUnknownOpcode
	}
	op := lookupTable[value]
	if op == 0 {
		return 0, ErrUnknownOpcode
	}
	return op, nil
}

// String 返回 OpCode 的字符串名称。
func (op OpCode) String() string {
	names := map[OpCode]string{
		LoadConst: "LoadConst", LoadNil: "LoadNil",
		LoadTrue: "LoadTrue", LoadFalse: "LoadFalse",
		LoadVar: "LoadVar", StoreVar: "StoreVar", DefineVar: "DefineVar",
		Call: "Call", Return: "Return", MakeFunction: "MakeFunction",
		Jump: "Jump", JumpIfFalse: "JumpIfFalse", JumpIfTrue: "JumpIfTrue",
		Break: "Break", Continue: "Continue",
		BinaryAdd: "BinaryAdd", BinarySub: "BinarySub",
		BinaryMul: "BinaryMul", BinaryDiv: "BinaryDiv",
		BinaryMod: "BinaryMod", BinaryPow: "BinaryPow",
		BinaryEq: "BinaryEq", BinaryNe: "BinaryNe",
		BinaryLt: "BinaryLt", BinaryGt: "BinaryGt",
		BinaryLe: "BinaryLe", BinaryGe: "BinaryGe",
		BinaryAnd: "BinaryAnd", BinaryOr: "BinaryOr",
		UnaryNeg: "UnaryNeg", UnaryNot: "UnaryNot",
		MakeStruct: "MakeStruct", MakeClass: "MakeClass",
		MakeEnum: "MakeEnum", MakeUnion: "MakeUnion",
		LoadField: "LoadField", StoreField: "StoreField",
		MakeArray: "MakeArray", MakeMap: "MakeMap",
		IndexGet: "IndexGet", IndexSet: "IndexSet",
		PropertyGet: "PropertyGet", PropertySet: "PropertySet",
		AddressOf: "AddressOf", Deref: "Deref", PointerMember: "PointerMember",
		Import: "Import", New: "New", Halt: "Halt",
		SysArgv: "SysArgv", System: "System",
		FileRead: "FileRead", FileWrite: "FileWrite", FileExists: "FileExists",
		Dup: "Dup", Pop: "Pop",
		Newz: "Newz", Free: "Free", OwnershipMove: "OwnershipMove",
		ScopeDrop: "ScopeDrop", BorrowCheck: "BorrowCheck", AliveCheck: "AliveCheck",
		AddInt: "AddInt", AddFloat: "AddFloat", SubInt: "SubInt", SubFloat: "SubFloat",
		MulInt: "MulInt", MulFloat: "MulFloat", DivInt: "DivInt", DivFloat: "DivFloat",
		ModInt: "ModInt",
		EqInt:  "EqInt", EqFloat: "EqFloat", LtInt: "LtInt", LtFloat: "LtFloat",
		GtInt: "GtInt", GtFloat: "GtFloat", LeInt: "LeInt", LeFloat: "LeFloat",
		GeInt: "GeInt", GeFloat: "GeFloat",
		NegInt: "NegInt", NegFloat: "NegFloat",
		Not: "Not", And: "And", Or: "Or",
		Iterate: "Iterate", Next: "Next",
	}
	if name, ok := names[op]; ok {
		return name
	}
	return "OpCode(unknown)"
}
