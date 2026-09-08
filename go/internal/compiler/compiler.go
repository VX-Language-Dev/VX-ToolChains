package compiler

import (
	"fmt"
	"strconv"

	"github.com/vx-lang/vxc/internal/compiler_bytecode"
	"github.com/vx-lang/vxc/internal/opcode"
	"github.com/vx-lang/vxc/internal/typeir"
)

// CompilationResult 编译结果
type CompilationResult struct {
	Functions []compiler_bytecode.BytecodeFunction
	Constants []compiler_bytecode.ConstantValue
}

// Compiler VX 编译器
type Compiler struct {
	Module    *typeir.TypeIRModule
	constants []compiler_bytecode.ConstantValue
	constMap  map[string]int
	functions []compiler_bytecode.BytecodeFunction
}

// NewCompiler 创建新的编译器实例
func NewCompiler(module *typeir.TypeIRModule) *Compiler {
	return &Compiler{
		Module:    module,
		constants: make([]compiler_bytecode.ConstantValue, 0),
		constMap:  make(map[string]int),
		functions: make([]compiler_bytecode.BytecodeFunction, 0),
	}
}

// Compile 执行编译
func (c *Compiler) Compile() (*CompilationResult, error) {
	if c.Module == nil {
		return nil, fmt.Errorf("module is nil")
	}

	c.constants = make([]compiler_bytecode.ConstantValue, 0)
	c.constMap = make(map[string]int)

	for _, funcIR := range c.Module.Functions {
		bytecodeFunc, err := c.compileFunction(funcIR)
		if err != nil {
			return nil, err
		}
		c.functions = append(c.functions, bytecodeFunc)
	}

	return &CompilationResult{
		Functions: c.functions,
		Constants: c.constants,
	}, nil
}

func (c *Compiler) compileFunction(funcIR typeir.FunctionIR) (compiler_bytecode.BytecodeFunction, error) {
	var instructions []compiler_bytecode.Instruction
	var paramNames []string
	var paramTypes []typeir.Typ

	for _, param := range funcIR.Params {
		paramNames = append(paramNames, param.Name)
		paramTypes = append(paramTypes, param.Type)
	}

	// First pass: collect label positions so jump targets can be resolved.
	// Noop instructions with a label arg are emitted by typeirgen.emitLabel.
	labelPositions := make(map[string]int)
	for _, instr := range funcIR.Instructions {
		if instr.Op == typeir.Noop && len(instr.Args) > 0 {
			// The label name is the arg; record the current instruction index
			// so jump instructions can resolve it. We use a placeholder
			// index that will be corrected in the second pass.
			labelPositions[instr.Args[0]] = -1
		}
	}

	// Second pass: emit instructions, tracking positions and resolving labels.
	pos := 0
	for _, instr := range funcIR.Instructions {
		// Record label positions before emitting the instruction.
		if instr.Op == typeir.Noop && len(instr.Args) > 0 {
			labelPositions[instr.Args[0]] = pos
			// Skip emitting Noop — it's just a label marker.
			continue
		}
		bytecodeInstr, err := c.compileInstruction(instr, labelPositions)
		if err != nil {
			return compiler_bytecode.BytecodeFunction{}, err
		}
		instructions = append(instructions, bytecodeInstr)
		pos++
	}

	hasReturn := len(instructions) > 0
	if hasReturn {
		lastOp := instructions[len(instructions)-1].Op
		hasReturn = lastOp == opcode.Return
	}

	return compiler_bytecode.BytecodeFunction{
		Name:         funcIR.Name,
		Instructions: instructions,
		NumParams:    len(funcIR.Params),
		HasReturn:    hasReturn,
		ParamNames:   paramNames,
		ParamTypes:   paramTypes,
	}, nil
}

func (c *Compiler) compileInstruction(instr typeir.TypedInstruction, labelPositions map[string]int) (compiler_bytecode.Instruction, error) {
	switch instr.Op {
	case typeir.ConstInt:
		if len(instr.Args) > 0 {
			val, err := strconv.ParseInt(instr.Args[0], 10, 64)
			if err == nil {
				idx := c.addConstant(compiler_bytecode.NewIntConst(val))
				return compiler_bytecode.NewInstructionWithArg(opcode.LoadConst, compiler_bytecode.NewIntArg(int32(idx))), nil
			}
		}

	case typeir.ConstFloat:
		if len(instr.Args) > 0 {
			val, err := strconv.ParseFloat(instr.Args[0], 64)
			if err == nil {
				idx := c.addConstant(compiler_bytecode.NewFloatConst(val))
				return compiler_bytecode.NewInstructionWithArg(opcode.LoadConst, compiler_bytecode.NewIntArg(int32(idx))), nil
			}
		}

	case typeir.ConstBool:
		if len(instr.Args) > 0 {
			val := instr.Args[0] == "true"
			idx := c.addConstant(compiler_bytecode.NewBoolConst(val))
			return compiler_bytecode.NewInstructionWithArg(opcode.LoadConst, compiler_bytecode.NewIntArg(int32(idx))), nil
		}

	case typeir.ConstString:
		if len(instr.Args) > 0 {
			idx := c.addConstant(compiler_bytecode.NewStringConst(instr.Args[0]))
			return compiler_bytecode.NewInstructionWithArg(opcode.LoadConst, compiler_bytecode.NewIntArg(int32(idx))), nil
		}

	case typeir.ConstNil:
		return compiler_bytecode.NewInstruction(opcode.LoadNil), nil

	case typeir.LoadVar:
		if len(instr.Args) > 0 {
			idx := c.resolveVarIndex(instr.Args[0])
			return compiler_bytecode.NewInstructionWithArg(opcode.LoadVar, compiler_bytecode.NewIntArg(int32(idx))), nil
		}

	case typeir.StoreVar:
		if len(instr.Args) > 0 {
			idx := c.resolveVarIndex(instr.Args[0])
			return compiler_bytecode.NewInstructionWithArg(opcode.StoreVar, compiler_bytecode.NewIntArg(int32(idx))), nil
		}

	case typeir.I32Add:
		return compiler_bytecode.NewInstruction(opcode.AddInt), nil
	case typeir.I32Sub:
		return compiler_bytecode.NewInstruction(opcode.SubInt), nil
	case typeir.I32Mul:
		return compiler_bytecode.NewInstruction(opcode.MulInt), nil
	case typeir.I32Div:
		return compiler_bytecode.NewInstruction(opcode.DivInt), nil
	case typeir.I32Mod:
		return compiler_bytecode.NewInstruction(opcode.ModInt), nil
	case typeir.I64Add:
		return compiler_bytecode.NewInstruction(opcode.AddInt), nil
	case typeir.I64Sub:
		return compiler_bytecode.NewInstruction(opcode.SubInt), nil
	case typeir.I64Mul:
		return compiler_bytecode.NewInstruction(opcode.MulInt), nil
	case typeir.I64Div:
		return compiler_bytecode.NewInstruction(opcode.DivInt), nil

	case typeir.F64Add:
		return compiler_bytecode.NewInstruction(opcode.AddFloat), nil
	case typeir.F64Sub:
		return compiler_bytecode.NewInstruction(opcode.SubFloat), nil
	case typeir.F64Mul:
		return compiler_bytecode.NewInstruction(opcode.MulFloat), nil
	case typeir.F64Div:
		return compiler_bytecode.NewInstruction(opcode.DivFloat), nil

	case typeir.BoolAnd:
		return compiler_bytecode.NewInstruction(opcode.And), nil
	case typeir.BoolOr:
		return compiler_bytecode.NewInstruction(opcode.Or), nil
	case typeir.BoolNot:
		return compiler_bytecode.NewInstruction(opcode.Not), nil

	case typeir.I32Eq:
		return compiler_bytecode.NewInstruction(opcode.EqInt), nil
	case typeir.I32Neq:
		return compiler_bytecode.NewInstruction(opcode.NeqInt), nil
	case typeir.F64Eq:
		return compiler_bytecode.NewInstruction(opcode.EqFloat), nil
	case typeir.I32Lt:
		return compiler_bytecode.NewInstruction(opcode.LtInt), nil
	case typeir.I32Gt:
		return compiler_bytecode.NewInstruction(opcode.GtInt), nil
	case typeir.I32Le:
		return compiler_bytecode.NewInstruction(opcode.LeInt), nil
	case typeir.I32Ge:
		return compiler_bytecode.NewInstruction(opcode.GeInt), nil
	case typeir.F64Lt:
		return compiler_bytecode.NewInstruction(opcode.LtFloat), nil
	case typeir.F64Gt:
		return compiler_bytecode.NewInstruction(opcode.GtFloat), nil
	case typeir.F64Le:
		return compiler_bytecode.NewInstruction(opcode.LeFloat), nil
	case typeir.F64Ge:
		return compiler_bytecode.NewInstruction(opcode.GeFloat), nil

	case typeir.I32And:
		return compiler_bytecode.NewInstruction(opcode.And), nil
	case typeir.I32Or:
		return compiler_bytecode.NewInstruction(opcode.Or), nil
	case typeir.I32Neg:
		return compiler_bytecode.NewInstruction(opcode.NegInt), nil
	case typeir.F64Neg:
		return compiler_bytecode.NewInstruction(opcode.NegFloat), nil
	case typeir.StrEq:
		return compiler_bytecode.NewInstruction(opcode.StrEq), nil
	case typeir.NeqInt:
		return compiler_bytecode.NewInstruction(opcode.NeqInt), nil

	case typeir.Return:
		return compiler_bytecode.NewInstruction(opcode.Return), nil
	case typeir.ReturnVoid:
		return compiler_bytecode.NewInstruction(opcode.Return), nil

	case typeir.Jump:
		if len(instr.Args) > 0 {
			idx := c.resolveLabelIndex(instr.Args[0], labelPositions)
			return compiler_bytecode.NewInstructionWithArg(opcode.Jump, compiler_bytecode.NewIntArg(int32(idx))), nil
		}

	case typeir.JumpIfFalse:
		if len(instr.Args) > 0 {
			idx := c.resolveLabelIndex(instr.Args[0], labelPositions)
			return compiler_bytecode.NewInstructionWithArg(opcode.JumpIfFalse, compiler_bytecode.NewIntArg(int32(idx))), nil
		}

	case typeir.JumpIfTrue:
		if len(instr.Args) > 0 {
			idx := c.resolveLabelIndex(instr.Args[0], labelPositions)
			return compiler_bytecode.NewInstructionWithArg(opcode.JumpIfTrue, compiler_bytecode.NewIntArg(int32(idx))), nil
		}

	case typeir.Call:
		if len(instr.Args) > 0 {
			return compiler_bytecode.NewInstructionWithArg(opcode.Call, compiler_bytecode.NewStringArg(instr.Args[0])), nil
		}

	case typeir.Newz:
		return compiler_bytecode.NewInstruction(opcode.Newz), nil
	case typeir.Free:
		return compiler_bytecode.NewInstruction(opcode.Free), nil
	case typeir.OwnershipMove:
		return compiler_bytecode.NewInstruction(opcode.OwnershipMove), nil
	case typeir.ScopeDrop:
		return compiler_bytecode.NewInstruction(opcode.ScopeDrop), nil
	case typeir.BorrowCheck:
		return compiler_bytecode.NewInstruction(opcode.BorrowCheck), nil
	case typeir.AliveCheck:
		return compiler_bytecode.NewInstruction(opcode.AliveCheck), nil

	case typeir.Noop:
		return compiler_bytecode.NewInstruction(opcode.Noop), nil

	case typeir.Pop:
		return compiler_bytecode.NewInstruction(opcode.Pop), nil
	case typeir.Dup:
		return compiler_bytecode.NewInstruction(opcode.Dup), nil

	case typeir.Halt:
		return compiler_bytecode.NewInstruction(opcode.Halt), nil
	case typeir.Import:
		return compiler_bytecode.NewInstruction(opcode.Import), nil

	case typeir.PropertyGet:
		return compiler_bytecode.NewInstruction(opcode.PropertyGet), nil
	case typeir.PointerMember:
		return compiler_bytecode.NewInstruction(opcode.PointerMember), nil
	case typeir.IndexGet:
		return compiler_bytecode.NewInstruction(opcode.IndexGet), nil
	case typeir.IndexSet:
		return compiler_bytecode.NewInstruction(opcode.IndexSet), nil
	case typeir.FieldLoad:
		return compiler_bytecode.NewInstruction(opcode.LoadField), nil
	case typeir.FieldStore:
		return compiler_bytecode.NewInstruction(opcode.StoreField), nil

	case typeir.ArrayNew:
		return compiler_bytecode.NewInstruction(opcode.MakeArray), nil
	case typeir.MapNew:
		return compiler_bytecode.NewInstruction(opcode.MakeMap), nil
	case typeir.StructNew:
		return compiler_bytecode.NewInstruction(opcode.MakeStruct), nil

	case typeir.AddressOf:
		return compiler_bytecode.NewInstruction(opcode.AddressOf), nil
	case typeir.Deref:
		return compiler_bytecode.NewInstruction(opcode.Deref), nil

	case typeir.SysArgv:
		return compiler_bytecode.NewInstruction(opcode.SysArgv), nil
	case typeir.System:
		return compiler_bytecode.NewInstruction(opcode.System), nil
	case typeir.FileRead:
		return compiler_bytecode.NewInstruction(opcode.FileRead), nil
	case typeir.FileWrite:
		return compiler_bytecode.NewInstruction(opcode.FileWrite), nil
	case typeir.FileExists:
		return compiler_bytecode.NewInstruction(opcode.FileExists), nil

	case typeir.StrConcat:
		return compiler_bytecode.NewInstruction(opcode.StrConcat), nil
	case typeir.ArrayConcat:
		return compiler_bytecode.NewInstruction(opcode.ArrayConcat), nil
	}

	return compiler_bytecode.NewInstruction(opcode.Halt), fmt.Errorf("unsupported opcode: %s", instr.Op)
}

func (c *Compiler) addConstant(val compiler_bytecode.ConstantValue) int {
	key := fmt.Sprintf("%v:%v", val.Kind, val.Int)
	if val.Kind == compiler_bytecode.ConstString {
		key = fmt.Sprintf("%v:%v", val.Kind, val.Str)
	} else if val.Kind == compiler_bytecode.ConstFloat {
		key = fmt.Sprintf("%v:%v", val.Kind, val.Float)
	} else if val.Kind == compiler_bytecode.ConstBool {
		key = fmt.Sprintf("%v:%v", val.Kind, val.Bool)
	}

	if idx, ok := c.constMap[key]; ok {
		return idx
	}

	idx := len(c.constants)
	c.constants = append(c.constants, val)
	c.constMap[key] = idx
	return idx
}

func (c *Compiler) resolveVarIndex(name string) int {
	// TypeIRgen emits numeric IDs (0, 1, 2, ...) as LoadVar/StoreVar args,
	// not variable names. Parse the numeric ID directly.
	if idx, err := strconv.Atoi(name); err == nil {
		return idx
	}
	// Fall back to name-based lookup for globals (stable indices).
	for i, g := range c.Module.Globals {
		if g.Name == name {
			return i
		}
	}
	return 0
}

func (c *Compiler) resolveLabelIndex(label string, positions map[string]int) int {
	if pos, ok := positions[label]; ok && pos >= 0 {
		return pos
	}
	// Unknown label: default to end of function (safe fallback).
	return -1
}
