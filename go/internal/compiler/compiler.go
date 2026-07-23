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

	for _, instr := range funcIR.Instructions {
		bytecodeInstr, err := c.compileInstruction(instr)
		if err != nil {
			return compiler_bytecode.BytecodeFunction{}, err
		}
		instructions = append(instructions, bytecodeInstr)
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

func (c *Compiler) compileInstruction(instr typeir.TypedInstruction) (compiler_bytecode.Instruction, error) {
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
	case typeir.F64Eq:
		return compiler_bytecode.NewInstruction(opcode.EqFloat), nil

	case typeir.Return:
		return compiler_bytecode.NewInstruction(opcode.Return), nil

	case typeir.Jump:
		if len(instr.Args) > 0 {
			idx := c.resolveLabelIndex(instr.Args[0])
			return compiler_bytecode.NewInstructionWithArg(opcode.Jump, compiler_bytecode.NewIntArg(int32(idx))), nil
		}

	case typeir.JumpIfFalse:
		if len(instr.Args) > 0 {
			idx := c.resolveLabelIndex(instr.Args[0])
			return compiler_bytecode.NewInstructionWithArg(opcode.JumpIfFalse, compiler_bytecode.NewIntArg(int32(idx))), nil
		}

	case typeir.JumpIfTrue:
		if len(instr.Args) > 0 {
			idx := c.resolveLabelIndex(instr.Args[0])
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

	case typeir.Noop:
		return compiler_bytecode.NewInstruction(opcode.Pop), nil

	case typeir.Pop:
		return compiler_bytecode.NewInstruction(opcode.Pop), nil
	case typeir.Dup:
		return compiler_bytecode.NewInstruction(opcode.Dup), nil
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
	for i, g := range c.Module.Globals {
		if g.Name == name {
			return i
		}
	}
	return 0
}

func (c *Compiler) resolveLabelIndex(label string) int {
	return 0
}
