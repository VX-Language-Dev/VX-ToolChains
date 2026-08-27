// ==================== VX 字节码优化器 ====================
//
// opt_level 1-10:
//
//	Debug (1-4):   L1=无, L2=常量折叠, L3=+死代码消除, L4=+常量传播
//	Release (5-7): L5=+分支折叠, L6=+死块消除, L7=+多趟
//	Super  (8-10): L8=+强化传播, L9=+强化DCE, L10=极致(16趟)
package optimize

import (
	"fmt"
	"math"

	"github.com/vx-lang/vxc/internal/compiler_bytecode"
	"github.com/vx-lang/vxc/internal/opcode"
)

// ==================== 统计 ====================

// OptStats 记录字节码优化过程中收集的统计信息。
type OptStats struct {
	Folds   uint32
	Dead    uint32
	Prop    uint32
	Branch  uint32
	Unreach uint32
	Passes  uint32
}

// Total 返回所有优化动作的总次数。
func (s OptStats) Total() uint32 {
	return s.Folds + s.Dead + s.Prop + s.Branch + s.Unreach
}

// FormatStats 使用 fmt.Sprintf 格式化统计信息。
func FormatStats(s OptStats) string {
	return fmt.Sprintf(
		"folds=%d dead=%d prop=%d branch=%d unreach=%d passes=%d total=%d",
		s.Folds, s.Dead, s.Prop, s.Branch, s.Unreach, s.Passes, s.Total(),
	)
}

// ==================== 工具函数 ====================

func argInt(a compiler_bytecode.BytecodeArg) (int32, bool) {
	if a.Kind != compiler_bytecode.ArgInt {
		return 0, false
	}
	return a.Int, true
}

func removeAt(ins *[]compiler_bytecode.Instruction, i int) {
	*ins = append((*ins)[:i], (*ins)[i+1:]...)
}

func addConst(pool *[]compiler_bytecode.ConstantValue, v compiler_bytecode.ConstantValue) int {
	idx := len(*pool)
	*pool = append(*pool, v)
	return idx
}

func getConst(pool []compiler_bytecode.ConstantValue, a compiler_bytecode.BytecodeArg) (compiler_bytecode.ConstantValue, bool) {
	raw, ok := argInt(a)
	if !ok || raw < 0 {
		return compiler_bytecode.NilValue, false
	}
	i := int(raw)
	if i >= len(pool) {
		return compiler_bytecode.NilValue, false
	}
	return pool[i], true
}

func isBinary(op opcode.OpCode) bool {
	switch op {
	case opcode.BinaryAdd,
		opcode.BinarySub,
		opcode.BinaryMul,
		opcode.BinaryDiv,
		opcode.BinaryMod,
		opcode.BinaryPow,
		opcode.BinaryEq,
		opcode.BinaryNe,
		opcode.BinaryLt,
		opcode.BinaryGt,
		opcode.BinaryLe,
		opcode.BinaryGe,
		opcode.BinaryAnd,
		opcode.BinaryOr,
		opcode.AddInt,
		opcode.AddFloat,
		opcode.SubInt,
		opcode.SubFloat,
		opcode.MulInt,
		opcode.MulFloat,
		opcode.DivInt,
		opcode.DivFloat,
		opcode.ModInt,
		opcode.EqInt,
		opcode.EqFloat,
		opcode.LtInt,
		opcode.LtFloat,
		opcode.GtInt,
		opcode.GtFloat,
		opcode.LeInt,
		opcode.LeFloat,
		opcode.GeInt,
		opcode.GeFloat,
		opcode.And,
		opcode.Or:
		return true
	default:
		return false
	}
}

func isEffectFree(op opcode.OpCode) bool {
	switch op {
	case opcode.LoadConst,
		opcode.LoadNil,
		opcode.LoadTrue,
		opcode.LoadFalse,
		opcode.LoadVar,
		opcode.UnaryNeg,
		opcode.UnaryNot,
		opcode.NegInt,
		opcode.NegFloat,
		opcode.Not,
		opcode.BinaryAdd,
		opcode.BinarySub,
		opcode.BinaryMul,
		opcode.BinaryDiv,
		opcode.BinaryMod,
		opcode.BinaryPow,
		opcode.BinaryEq,
		opcode.BinaryNe,
		opcode.BinaryLt,
		opcode.BinaryGt,
		opcode.BinaryLe,
		opcode.BinaryGe,
		opcode.BinaryAnd,
		opcode.BinaryOr,
		opcode.AddInt,
		opcode.AddFloat,
		opcode.SubInt,
		opcode.SubFloat,
		opcode.MulInt,
		opcode.MulFloat,
		opcode.DivInt,
		opcode.DivFloat,
		opcode.ModInt,
		opcode.EqInt,
		opcode.EqFloat,
		opcode.LtInt,
		opcode.LtFloat,
		opcode.GtInt,
		opcode.GtFloat,
		opcode.LeInt,
		opcode.LeFloat,
		opcode.GeInt,
		opcode.GeFloat,
		opcode.And,
		opcode.Or,
		opcode.Dup:
		return true
	default:
		return false
	}
}

func isTerm(op opcode.OpCode) bool {
	switch op {
	case opcode.Return, opcode.Halt, opcode.Jump, opcode.JumpIfFalse, opcode.JumpIfTrue, opcode.Break, opcode.Continue:
		return true
	default:
		return false
	}
}

// ==================== 常量运算 ====================

func saturatingMulInt64(a, b int64) int64 {
	if a == 0 || b == 0 {
		return 0
	}
	result := a * b
	if a == result/b {
		return result
	}
	if (a < 0) == (b < 0) {
		return math.MaxInt64
	}
	return math.MinInt64
}

func evalIntBin(l, r int64, op opcode.OpCode) (compiler_bytecode.ConstantValue, bool) {
	switch op {
	case opcode.BinaryAdd, opcode.AddInt:
		return compiler_bytecode.NewIntConst(l + r), true
	case opcode.BinarySub, opcode.SubInt:
		return compiler_bytecode.NewIntConst(l - r), true
	case opcode.BinaryMul, opcode.MulInt:
		return compiler_bytecode.NewIntConst(saturatingMulInt64(l, r)), true
	case opcode.BinaryDiv, opcode.DivInt:
		if r == 0 {
			return compiler_bytecode.NilValue, false
		}
		return compiler_bytecode.NewIntConst(l / r), true
	case opcode.BinaryMod, opcode.ModInt:
		if r == 0 {
			return compiler_bytecode.NilValue, false
		}
		return compiler_bytecode.NewIntConst(l % r), true
	case opcode.BinaryPow:
		if r < 0 {
			return compiler_bytecode.NilValue, false
		}
		b := l
		e := uint64(r)
		var res int64 = 1
		for e > 0 {
			if e&1 == 1 {
				res = saturatingMulInt64(res, b)
			}
			e >>= 1
			if e > 0 {
				b = saturatingMulInt64(b, b)
			}
		}
		return compiler_bytecode.NewIntConst(res), true
	case opcode.BinaryEq, opcode.EqInt:
		return compiler_bytecode.NewBoolConst(l == r), true
	case opcode.BinaryNe:
		return compiler_bytecode.NewBoolConst(l != r), true
	case opcode.BinaryLt, opcode.LtInt:
		return compiler_bytecode.NewBoolConst(l < r), true
	case opcode.BinaryGt, opcode.GtInt:
		return compiler_bytecode.NewBoolConst(l > r), true
	case opcode.BinaryLe, opcode.LeInt:
		return compiler_bytecode.NewBoolConst(l <= r), true
	case opcode.BinaryGe, opcode.GeInt:
		return compiler_bytecode.NewBoolConst(l >= r), true
	default:
		return compiler_bytecode.NilValue, false
	}
}

func evalFloatBin(l, r float64, op opcode.OpCode) (compiler_bytecode.ConstantValue, bool) {
	switch op {
	case opcode.BinaryAdd, opcode.AddFloat:
		return compiler_bytecode.NewFloatConst(l + r), true
	case opcode.BinarySub, opcode.SubFloat:
		return compiler_bytecode.NewFloatConst(l - r), true
	case opcode.BinaryMul, opcode.MulFloat:
		return compiler_bytecode.NewFloatConst(l * r), true
	case opcode.BinaryDiv, opcode.DivFloat:
		if r == 0.0 {
			return compiler_bytecode.NilValue, false
		}
		return compiler_bytecode.NewFloatConst(l / r), true
	case opcode.BinaryEq, opcode.EqFloat:
		return compiler_bytecode.NewBoolConst(l == r), true
	case opcode.BinaryNe:
		return compiler_bytecode.NewBoolConst(l != r), true
	case opcode.BinaryLt, opcode.LtFloat:
		return compiler_bytecode.NewBoolConst(l < r), true
	case opcode.BinaryGt, opcode.GtFloat:
		return compiler_bytecode.NewBoolConst(l > r), true
	case opcode.BinaryLe, opcode.LeFloat:
		return compiler_bytecode.NewBoolConst(l <= r), true
	case opcode.BinaryGe, opcode.GeFloat:
		return compiler_bytecode.NewBoolConst(l >= r), true
	default:
		return compiler_bytecode.NilValue, false
	}
}

func evalBoolBin(l, r bool, op opcode.OpCode) (compiler_bytecode.ConstantValue, bool) {
	switch op {
	case opcode.BinaryAnd, opcode.And:
		return compiler_bytecode.NewBoolConst(l && r), true
	case opcode.BinaryOr, opcode.Or:
		return compiler_bytecode.NewBoolConst(l || r), true
	default:
		return compiler_bytecode.NilValue, false
	}
}

// ==================== 主入口 ====================

// OptimizeBytecode 是字节码优化的主入口。
// 根据 optLevel 选择启用哪些优化趟，并返回收集到的统计信息。
func OptimizeBytecode(
	optLevel uint8,
	ins *[]compiler_bytecode.Instruction,
	constants *[]compiler_bytecode.ConstantValue,
) (OptStats, error) {
	if optLevel <= 1 {
		return OptStats{}, nil
	}

	maxPass := uint32(1)
	if optLevel >= 10 {
		maxPass = 16
	} else if optLevel >= 7 {
		maxPass = 8
	}

	var total OptStats
	changed := true

	for changed && total.Passes < maxPass {
		changed = false

		s := constantFold(ins, constants)
		if s.Folds > 0 {
			changed = true
		}
		total.Folds += s.Folds

		if optLevel >= 3 {
			s = deadCodeElimination(ins)
			if s.Dead > 0 {
				changed = true
			}
			total.Dead += s.Dead
		}

		if optLevel >= 4 {
			s = constantPropagation(ins, constants)
			if s.Prop > 0 {
				changed = true
			}
			total.Prop += s.Prop
		}

		if optLevel >= 5 {
			s = branchFold(ins, constants)
			if s.Branch+s.Unreach > 0 {
				changed = true
			}
			total.Branch += s.Branch
			total.Unreach += s.Unreach
		}

		if optLevel >= 6 {
			s = deadBlockElimination(ins)
			if s.Unreach > 0 {
				changed = true
			}
			total.Unreach += s.Unreach
		}

		total.Passes++
		if optLevel < 7 {
			break
		}
	}

	return total, nil
}

// ==================== Pass 1: 常量折叠 ====================

func constantFold(
	ins *[]compiler_bytecode.Instruction,
	constants *[]compiler_bytecode.ConstantValue,
) OptStats {
	var stats OptStats
	i := 0
	for i < len(*ins) {
		a := (*ins)[i].Op

		// LoadConst + LoadConst + BinaryOp → folded (需要 3 条指令)
		if i+2 < len(*ins) && a == opcode.LoadConst && (*ins)[i+1].Op == opcode.LoadConst && isBinary((*ins)[i+2].Op) {
			c0, ok0 := getConst(*constants, (*ins)[i].Arg)
			c1, ok1 := getConst(*constants, (*ins)[i+1].Arg)
			if ok0 && ok1 {
				if result, ok := foldBin(c0, c1, (*ins)[i+2].Op); ok {
					idx := addConst(constants, result)
					(*ins)[i] = compiler_bytecode.NewInstructionWithArg(opcode.LoadConst, compiler_bytecode.NewIntArg(int32(idx)))
					removeAt(ins, i+1)
					removeAt(ins, i+1)
					stats.Folds++
					continue
				}
			}
		}

		// LoadConst + UnaryNeg → folded
		if i+1 < len(*ins) && a == opcode.LoadConst && (*ins)[i+1].Op == opcode.UnaryNeg {
			c0, ok := getConst(*constants, (*ins)[i].Arg)
			if ok {
				var result compiler_bytecode.ConstantValue
				hasResult := false
				switch c0.Kind {
				case compiler_bytecode.ConstInt:
					result = compiler_bytecode.NewIntConst(-c0.Int)
					hasResult = true
				case compiler_bytecode.ConstFloat:
					result = compiler_bytecode.NewFloatConst(-c0.Float)
					hasResult = true
				}
				if hasResult {
					idx := addConst(constants, result)
					(*ins)[i] = compiler_bytecode.NewInstructionWithArg(opcode.LoadConst, compiler_bytecode.NewIntArg(int32(idx)))
					removeAt(ins, i+1)
					stats.Folds++
					continue
				}
			}
		}

		// LoadConst + UnaryNot → folded
		if i+1 < len(*ins) && a == opcode.LoadConst && (*ins)[i+1].Op == opcode.UnaryNot {
			c0, ok := getConst(*constants, (*ins)[i].Arg)
			if ok && c0.Kind == compiler_bytecode.ConstBool {
				idx := addConst(constants, compiler_bytecode.NewBoolConst(!c0.Bool))
				(*ins)[i] = compiler_bytecode.NewInstructionWithArg(opcode.LoadConst, compiler_bytecode.NewIntArg(int32(idx)))
				removeAt(ins, i+1)
				stats.Folds++
				continue
			}
		}

		i++
	}
	return stats
}

func foldBin(c0, c1 compiler_bytecode.ConstantValue, op opcode.OpCode) (compiler_bytecode.ConstantValue, bool) {
	switch c0.Kind {
	case compiler_bytecode.ConstInt:
		switch c1.Kind {
		case compiler_bytecode.ConstInt:
			return evalIntBin(c0.Int, c1.Int, op)
		case compiler_bytecode.ConstFloat:
			return evalFloatBin(float64(c0.Int), c1.Float, op)
		}
	case compiler_bytecode.ConstFloat:
		switch c1.Kind {
		case compiler_bytecode.ConstInt:
			return evalFloatBin(c0.Float, float64(c1.Int), op)
		case compiler_bytecode.ConstFloat:
			return evalFloatBin(c0.Float, c1.Float, op)
		}
	case compiler_bytecode.ConstBool:
		if c1.Kind == compiler_bytecode.ConstBool {
			return evalBoolBin(c0.Bool, c1.Bool, op)
		}
	}
	return compiler_bytecode.NilValue, false
}

// ==================== Pass 2: 死代码消除 ====================

func deadCodeElimination(ins *[]compiler_bytecode.Instruction) OptStats {
	var stats OptStats
	i := 0
	for i < len(*ins) {
		// 效率自由指令 + Pop → 移除 Pair
		if (*ins)[i].Op == opcode.Pop && i > 0 {
			prev := (*ins)[i-1]
			if isEffectFree(prev.Op) {
				removeAt(ins, i-1)
				removeAt(ins, i-1)
				stats.Dead++
				if i >= 2 {
					i -= 2
				} else {
					i = 0
				}
				continue
			}
		}
		// Pop + Pop → Pop
		if (*ins)[i].Op == opcode.Pop && i+1 < len(*ins) && (*ins)[i+1].Op == opcode.Pop {
			removeAt(ins, i+1)
			stats.Dead++
			continue
		}
		i++
	}
	return stats
}

// ==================== Pass 3: 常量传播 ====================

func constantPropagation(
	ins *[]compiler_bytecode.Instruction,
	constants *[]compiler_bytecode.ConstantValue,
) OptStats {
	_ = constants
	var stats OptStats
	// 使用 map 追踪 slot 的常量值，无 slot 数量限制
	slotVal := make(map[uint32]uint32)
	slotType := make(map[uint32]uint8)

	i := 0
	for i < len(*ins) {
		op := (*ins)[i].Op
		switch op {
		case opcode.DefineVar, opcode.StoreVar:
			sid, ok := argInt((*ins)[i].Arg)
			if !ok {
				i++
				continue
			}
			if sid < 0 {
				i++
				continue
			}
			s := uint32(sid)
			// 前一条指令是否是 LoadConst?
			if i > 0 && (*ins)[i-1].Op == opcode.LoadConst {
				ci, ok := argInt((*ins)[i-1].Arg)
				if ok && ci >= 0 {
					curType := slotType[s]
					curVal := slotVal[s]
					if curType == 1 && curVal != uint32(ci) {
						slotType[s] = 2 // 多次不同赋值 → unknown
					} else {
						slotType[s] = 1
						slotVal[s] = uint32(ci)
					}
				} else {
					slotType[s] = 2
				}
			} else {
				slotType[s] = 2
			}
		case opcode.LoadVar:
			sid, ok := argInt((*ins)[i].Arg)
			if !ok {
				i++
				continue
			}
			if sid < 0 {
				i++
				continue
			}
			s := uint32(sid)
			if slotType[s] == 1 {
				if v, ok := slotVal[s]; ok {
					(*ins)[i] = compiler_bytecode.NewInstructionWithArg(opcode.LoadConst, compiler_bytecode.NewIntArg(int32(v)))
					stats.Prop++
				}
			}
		// 循环/跳转重置所有 slot 追踪
		case opcode.Jump, opcode.JumpIfFalse, opcode.JumpIfTrue, opcode.Continue, opcode.Break, opcode.Iterate, opcode.Next:
			for k := range slotType {
				slotType[k] = 2
			}
		}
		i++
	}
	return stats
}

// ==================== Pass 4: 分支折叠 ====================

func branchFold(
	ins *[]compiler_bytecode.Instruction,
	constants *[]compiler_bytecode.ConstantValue,
) OptStats {
	var stats OptStats
	i := 0
	for i+1 < len(*ins) {
		a := (*ins)[i].Op
		b := (*ins)[i+1].Op

		// LoadTrue + JumpIfFalse → 跳过测试 (条件始终 true)
		if a == opcode.LoadTrue && b == opcode.JumpIfFalse {
			removeAt(ins, i)
			removeAt(ins, i)
			stats.Branch++
			continue
		}
		// LoadFalse + JumpIfTrue → 跳过测试 (条件始终 false)
		if a == opcode.LoadFalse && b == opcode.JumpIfTrue {
			removeAt(ins, i)
			removeAt(ins, i)
			stats.Branch++
			continue
		}
		// LoadTrue + JumpIfTrue → 无条件 Jump
		if a == opcode.LoadTrue && b == opcode.JumpIfTrue {
			(*ins)[i] = compiler_bytecode.NewInstructionWithArg(opcode.Jump, (*ins)[i+1].Arg)
			removeAt(ins, i+1)
			stats.Branch++
			continue
		}
		// LoadFalse + JumpIfFalse → 无条件 Jump
		if a == opcode.LoadFalse && b == opcode.JumpIfFalse {
			(*ins)[i] = compiler_bytecode.NewInstructionWithArg(opcode.Jump, (*ins)[i+1].Arg)
			removeAt(ins, i+1)
			stats.Branch++
			continue
		}
		// LoadConst(bool) + JumpIfFalse/JumpIfTrue → 折叠常量条件
		if a == opcode.LoadConst {
			cv, ok := getConst(*constants, (*ins)[i].Arg)
			if ok && cv.Kind == compiler_bytecode.ConstBool {
				if b == opcode.JumpIfFalse {
					if !cv.Bool {
						// 条件 false → 直接跳转
						(*ins)[i] = compiler_bytecode.NewInstructionWithArg(opcode.Jump, (*ins)[i+1].Arg)
						removeAt(ins, i+1)
					} else {
						// 条件 true → 移除测试
						removeAt(ins, i)
						removeAt(ins, i)
					}
					stats.Branch++
					continue
				}
				if b == opcode.JumpIfTrue {
					if cv.Bool {
						(*ins)[i] = compiler_bytecode.NewInstructionWithArg(opcode.Jump, (*ins)[i+1].Arg)
						removeAt(ins, i+1)
					} else {
						removeAt(ins, i)
						removeAt(ins, i)
					}
					stats.Branch++
					continue
				}
			}
		}
		i++
	}
	return stats
}

// ==================== Pass 5: 死代码块消除 ====================

func deadBlockElimination(ins *[]compiler_bytecode.Instruction) OptStats {
	var stats OptStats
	i := 0
	for i < len(*ins) {
		op := (*ins)[i].Op

		// Jump(target) 且 target == i+1 → 移除冗余跳转
		if op == opcode.Jump || op == opcode.JumpIfFalse || op == opcode.JumpIfTrue {
			t, ok := argInt((*ins)[i].Arg)
			if !ok {
				i++
				continue
			}
			if t >= 0 && int(t) == i+1 {
				removeAt(ins, i)
				stats.Unreach++
				continue
			}
		}

		// 终结指令后紧跟的不可达代码 → 移除
		if isTerm(op) && i+1 < len(*ins) {
			j := i + 1
			for j < len(*ins) && !isLabelLike((*ins)[j].Op) {
				j++
			}
			dead := j - i - 1
			if dead > 0 {
				for k := 0; k < dead; k++ {
					removeAt(ins, i+1)
				}
				stats.Unreach += uint32(dead)
				continue
			}
		}

		i++
	}
	return stats
}

// 指令是否可能是跳转目标 (label-like): 变量定义/赋值/内置/调用/内存操作等
func isLabelLike(op opcode.OpCode) bool {
	switch op {
	case opcode.DefineVar,
		opcode.StoreVar,
		opcode.StoreField,
		opcode.IndexSet,
		opcode.PropertySet,
		opcode.SysArgv,
		opcode.System,
		opcode.FileRead,
		opcode.FileWrite,
		opcode.FileExists,
		opcode.Call,
		opcode.Import,
		opcode.MakeFunction,
		opcode.MakeStruct,
		opcode.MakeClass,
		opcode.MakeEnum,
		opcode.MakeUnion,
		opcode.MakeArray,
		opcode.MakeMap,
		opcode.New,
		opcode.Newz,
		opcode.Free,
		opcode.OwnershipMove,
		opcode.ScopeDrop,
		opcode.BorrowCheck,
		opcode.AliveCheck,
		opcode.Iterate,
		opcode.Next:
		return true
	default:
		return false
	}
}
