// Package llvmir translates VX TypeIR to LLVM IR text.
package llvmir

import (
	"fmt"
	"strconv"
	"strings"

	"github.com/vx-lang/vxc/internal/typeir"
)

// Generate produces LLVM IR text for the given TypeModule, targeting the specified triple.
func Generate(module *typeir.TypeModule, targetTriple string) (string, error) {
	var b strings.Builder

	// Module header
	fmt.Fprintf(&b, "; LLVM IR generated from VXOBJ TypeIR\n")
	b.WriteString(fmt.Sprintf("target triple = \"%s\"\n", targetTriple))
	b.WriteString("target datalayout = \"e-m:e-p270:32:32-p271:32:32-p272:64:64-i64:64-f80:128-n8:16:32:64-S128\"\n")
	b.WriteString("\n")

	// External function declarations
	for _, fn := range module.Functions {
		linkage, ok := module.Linkage[fn.ID]
		if ok && linkage.IsExternal {
			b.WriteString(generateExternDecl(&fn, linkage.Name))
			b.WriteString("\n")
		}
	}

	// Auto-declare builtin/unknown callees that are not defined in this module.
	// These are referenced via Call instructions with a ";name" argument.
	definedFuncs := make(map[string]bool)
	for _, fn := range module.Functions {
		definedFuncs[fn.Name] = true
	}
	declaredExterns := make(map[string]bool)
	for _, fn := range module.Functions {
		linkage, ok := module.Linkage[fn.ID]
		if ok && linkage.IsExternal {
			declaredExterns[linkage.Name] = true
		}
	}
	for _, fn := range module.Functions {
		for _, inst := range fn.Body {
			if inst.Op != typeir.Call {
				continue
			}
			for _, a := range inst.Args[1:] {
				if strings.HasPrefix(a, ";") {
					name := a[1:]
					if name != "" && !definedFuncs[name] && !declaredExterns[name] && !declaredExterns["@"+name] {
						// Declare as a varargs function so any argument types
						// emitted at the call site are accepted.
						b.WriteString(fmt.Sprintf("declare void @%s(...)\n", name))
						declaredExterns[name] = true
					}
					break
				}
			}
		}
	}

	// Collect and emit string constants as global variables.
	// Each distinct string literal gets a stable index so the getelementptr
	// references in function bodies resolve to a defined global.
	strIndex := make(map[string]int)
	for _, fn := range module.Functions {
		for _, inst := range fn.Body {
			if inst.Op == typeir.ConstString && len(inst.Args) > 0 {
				if _, ok := strIndex[inst.Args[0]]; !ok {
					strIndex[inst.Args[0]] = len(strIndex)
				}
			}
		}
	}
	for s, idx := range strIndex {
		escaped := escapeLLVMString(s)
		n := len(s) + 1
		b.WriteString(fmt.Sprintf("@\"str.%d\" = private unnamed_addr constant [%d x i8] c\"%s\\00\"\n", idx, n, escaped))
	}
	if len(strIndex) > 0 {
		b.WriteString("\n")
	}

	// Function definitions
	for _, fn := range module.Functions {
		linkage, ok := module.Linkage[fn.ID]
		if ok && linkage.IsExternal {
			continue
		}
		fnIR, err := generateFunction(&fn, strIndex)
		if err != nil {
			return "", fmt.Errorf("function %s: %w", fn.Name, err)
		}
		b.WriteString(fnIR)
		b.WriteString("\n")
	}

	return b.String(), nil
}

func generateExternDecl(fn *typeir.FunctionIR, extName string) string {
	retType := llvmReturnType(fn)
	params := llvmParamList(fn.Params)
	return fmt.Sprintf("declare %s @%s(%s)", retType, extName, params)
}

func llvmReturnType(fn *typeir.FunctionIR) string {
	if fn.HasReturn || (fn.ReturnType != nil && typeir.TypeName(fn.ReturnType) != "void") {
		return llvmType(fn.ReturnType)
	}
	return "void"
}

func llvmType(t typeir.Typ) string {
	if t == nil {
		return "i32"
	}
	switch typeir.TypeName(t) {
	case "int", "bool":
		return "i32"
	case "float":
		return "double"
	case "void":
		return "void"
	case "string":
		return "i8*"
	default:
		return "i32"
	}
}

func llvmParamList(params []typeir.ParamIR) string {
	if len(params) == 0 {
		return ""
	}
	parts := make([]string, len(params))
	for i, p := range params {
		parts[i] = llvmType(p.Type) + " %_" + p.Name
	}
	return strings.Join(parts, ", ")
}

// generateFunction translates one FunctionIR to LLVM IR text.
func generateFunction(fn *typeir.FunctionIR, strIndex map[string]int) (string, error) {
	var b strings.Builder

	retType := llvmReturnType(fn)
	params := llvmParamList(fn.Params)
	b.WriteString(fmt.Sprintf("define %s @%s(%s) {\n", retType, fn.Name, params))

	// Check if function has jumps (needs basic blocks)
	hasJumps := false
	for _, inst := range fn.Body {
		if inst.Op == typeir.Jump || inst.Op == typeir.JumpIfFalse || inst.Op == typeir.JumpIfTrue {
			hasJumps = true
			break
		}
	}

	if hasJumps {
		generateAlloca(&b, fn)
		err := generateWithBlocks(&b, fn)
		if err != nil {
			return "", err
		}
	} else {
		// Generate locals alloca
		for _, inst := range fn.Body {
			if inst.Op == typeir.StoreVar || inst.Op == typeir.LoadVar {
				generateAlloca(&b, fn)
				break
			}
		}

		// Straight-line code with register stack
		gen := &ssaGen{buf: &b, locals: make(map[string]string), strIndex: strIndex}
		for _, inst := range fn.Body {
			if err := gen.emit(&inst); err != nil {
				return "", err
			}
		}
	}

	b.WriteString("}\n")
	return b.String(), nil
}

func generateAlloca(b *strings.Builder, fn *typeir.FunctionIR) {
	for vid, ty := range fn.LocalTypes {
		llvmTy := llvmType(ty)
		b.WriteString(fmt.Sprintf("  %%v%d = alloca %s, align 4\n", vid, llvmTy))
	}
}

// ssaGen generates LLVM IR for straight-line code (no jumps) using a virtual register stack.
type ssaGen struct {
	buf      *strings.Builder
	counter  int
	stack    []string          // virtual register stack
	types    []typeir.Typ      // type of each register on the stack (parallel to stack)
	locals   map[string]string // local variable name → SSA register
	strIndex map[string]int    // string literal value → global index
}

func (g *ssaGen) newReg() string {
	r := fmt.Sprintf("%%t%d", g.counter)
	g.counter++
	return r
}

func (g *ssaGen) push(r string, t typeir.Typ) {
	g.stack = append(g.stack, r)
	g.types = append(g.types, t)
}

func (g *ssaGen) pop() (string, typeir.Typ) {
	if len(g.stack) == 0 {
		return "%0", typeir.Unknown
	}
	r := g.stack[len(g.stack)-1]
	t := g.types[len(g.types)-1]
	g.stack = g.stack[:len(g.stack)-1]
	g.types = g.types[:len(g.types)-1]
	return r, t
}

func (g *ssaGen) popTwo() (string, string, typeir.Typ) {
	rhs, rt := g.pop()
	lhs, lt := g.pop()
	_ = rt
	return lhs, rhs, lt
}

func (g *ssaGen) emit(inst *typeir.TypedInstruction) error {
	op := inst.Op
	args := inst.Args

	switch op {
	case typeir.ConstInt:
		r := g.newReg()
		val := "0"
		if len(args) > 0 {
			val = args[0]
		}
		// Use add to materialize an i32 constant
		g.buf.WriteString(fmt.Sprintf("  %s = add i32 %s, 0\n", r, val))
		g.push(r, typeir.Int)

	case typeir.ConstFloat:
		r := g.newReg()
		val := "0.0"
		if len(args) > 0 {
			val = args[0]
		}
		g.buf.WriteString(fmt.Sprintf("  %s = fadd double %s, 0.0\n", r, val))
		g.push(r, typeir.Float)

	case typeir.ConstBool:
		r := g.newReg()
		val := "0"
		if len(args) > 0 && (args[0] == "1" || args[0] == "true") {
			val = "1"
		}
		g.buf.WriteString(fmt.Sprintf("  %s = add i32 %s, 0\n", r, val))
		g.push(r, typeir.Bool)

	case typeir.ConstString:
		r := g.newReg()
		str := ""
		if len(args) > 0 {
			str = args[0]
		}
		idx := 0
		if g.strIndex != nil {
			if i, ok := g.strIndex[str]; ok {
				idx = i
			}
		}
		n := len(str) + 1
		g.buf.WriteString(fmt.Sprintf("  %s = getelementptr [%d x i8], [%d x i8]* @\"str.%d\", i64 0, i64 0\n", r, n, n, idx))
		g.push(r, typeir.String)

	case typeir.ConstNil:
		r := g.newReg()
		g.buf.WriteString(fmt.Sprintf("  %s = inttoptr i64 0 to i8*\n", r))
		g.push(r, typeir.Unknown)

	case typeir.LoadVar:
		if len(args) == 0 {
			break
		}
		r := g.newReg()
		g.buf.WriteString(fmt.Sprintf("  %s = load i32, i32* %%v%s, align 4\n", r, args[0]))
		g.push(r, typeir.Int)

	case typeir.StoreVar:
		if len(args) == 0 {
			break
		}
		val, _ := g.pop()
		g.buf.WriteString(fmt.Sprintf("  store i32 %s, i32* %%v%s, align 4\n", val, args[0]))

	case typeir.I32Add:
		lhs, rhs, _ := g.popTwo()
		r := g.newReg()
		g.buf.WriteString(fmt.Sprintf("  %s = add i32 %s, %s\n", r, lhs, rhs))
		g.push(r, typeir.Int)
	case typeir.I32Sub:
		lhs, rhs, _ := g.popTwo()
		r := g.newReg()
		g.buf.WriteString(fmt.Sprintf("  %s = sub i32 %s, %s\n", r, lhs, rhs))
		g.push(r, typeir.Int)
	case typeir.I32Mul:
		lhs, rhs, _ := g.popTwo()
		r := g.newReg()
		g.buf.WriteString(fmt.Sprintf("  %s = mul i32 %s, %s\n", r, lhs, rhs))
		g.push(r, typeir.Int)
	case typeir.I32Div:
		lhs, rhs, _ := g.popTwo()
		r := g.newReg()
		g.buf.WriteString(fmt.Sprintf("  %s = sdiv i32 %s, %s\n", r, lhs, rhs))
		g.push(r, typeir.Int)

	case typeir.F64Add:
		lhs, rhs, _ := g.popTwo()
		r := g.newReg()
		g.buf.WriteString(fmt.Sprintf("  %s = fadd double %s, %s\n", r, lhs, rhs))
		g.push(r, typeir.Float)
	case typeir.F64Sub:
		lhs, rhs, _ := g.popTwo()
		r := g.newReg()
		g.buf.WriteString(fmt.Sprintf("  %s = fsub double %s, %s\n", r, lhs, rhs))
		g.push(r, typeir.Float)
	case typeir.F64Mul:
		lhs, rhs, _ := g.popTwo()
		r := g.newReg()
		g.buf.WriteString(fmt.Sprintf("  %s = fmul double %s, %s\n", r, lhs, rhs))
		g.push(r, typeir.Float)
	case typeir.F64Div:
		lhs, rhs, _ := g.popTwo()
		r := g.newReg()
		g.buf.WriteString(fmt.Sprintf("  %s = fdiv double %s, %s\n", r, lhs, rhs))
		g.push(r, typeir.Float)

	case typeir.I32Eq:
		lhs, rhs, _ := g.popTwo()
		r := g.newReg()
		r2 := g.newReg()
		g.buf.WriteString(fmt.Sprintf("  %s = icmp eq i32 %s, %s\n", r, lhs, rhs))
		g.buf.WriteString(fmt.Sprintf("  %s = zext i1 %s to i32\n", r2, r))
		g.push(r2, typeir.Bool)
	case typeir.I32Neq:
		lhs, rhs, _ := g.popTwo()
		r := g.newReg()
		r2 := g.newReg()
		g.buf.WriteString(fmt.Sprintf("  %s = icmp ne i32 %s, %s\n", r, lhs, rhs))
		g.buf.WriteString(fmt.Sprintf("  %s = zext i1 %s to i32\n", r2, r))
		g.push(r2, typeir.Bool)
	case typeir.I32Lt:
		lhs, rhs, _ := g.popTwo()
		r := g.newReg()
		r2 := g.newReg()
		g.buf.WriteString(fmt.Sprintf("  %s = icmp slt i32 %s, %s\n", r, lhs, rhs))
		g.buf.WriteString(fmt.Sprintf("  %s = zext i1 %s to i32\n", r2, r))
		g.push(r2, typeir.Bool)
	case typeir.I32Gt:
		lhs, rhs, _ := g.popTwo()
		r := g.newReg()
		r2 := g.newReg()
		g.buf.WriteString(fmt.Sprintf("  %s = icmp sgt i32 %s, %s\n", r, lhs, rhs))
		g.buf.WriteString(fmt.Sprintf("  %s = zext i1 %s to i32\n", r2, r))
		g.push(r2, typeir.Bool)
	case typeir.I32Le:
		lhs, rhs, _ := g.popTwo()
		r := g.newReg()
		r2 := g.newReg()
		g.buf.WriteString(fmt.Sprintf("  %s = icmp sle i32 %s, %s\n", r, lhs, rhs))
		g.buf.WriteString(fmt.Sprintf("  %s = zext i1 %s to i32\n", r2, r))
		g.push(r2, typeir.Bool)
	case typeir.I32Ge:
		lhs, rhs, _ := g.popTwo()
		r := g.newReg()
		r2 := g.newReg()
		g.buf.WriteString(fmt.Sprintf("  %s = icmp sge i32 %s, %s\n", r, lhs, rhs))
		g.buf.WriteString(fmt.Sprintf("  %s = zext i1 %s to i32\n", r2, r))
		g.push(r2, typeir.Bool)

	case typeir.I32And:
		lhs, rhs, _ := g.popTwo()
		r := g.newReg()
		g.buf.WriteString(fmt.Sprintf("  %s = and i32 %s, %s\n", r, lhs, rhs))
		g.push(r, typeir.Int)
	case typeir.I32Or:
		lhs, rhs, _ := g.popTwo()
		r := g.newReg()
		g.buf.WriteString(fmt.Sprintf("  %s = or i32 %s, %s\n", r, lhs, rhs))
		g.push(r, typeir.Int)

	case typeir.I32Neg:
		val, _ := g.pop()
		r := g.newReg()
		g.buf.WriteString(fmt.Sprintf("  %s = sub i32 0, %s\n", r, val))
		g.push(r, typeir.Int)
	case typeir.F64Neg:
		val, _ := g.pop()
		r := g.newReg()
		g.buf.WriteString(fmt.Sprintf("  %s = fsub double -0.0, %s\n", r, val))
		g.push(r, typeir.Float)
	case typeir.BoolNot:
		val, _ := g.pop()
		r := g.newReg()
		r2 := g.newReg()
		g.buf.WriteString(fmt.Sprintf("  %s = icmp eq i32 %s, 0\n", r, val))
		g.buf.WriteString(fmt.Sprintf("  %s = zext i1 %s to i32\n", r2, r))
		g.push(r2, typeir.Bool)

	case typeir.Dup:
		if len(g.stack) > 0 {
			top := g.stack[len(g.stack)-1]
			topT := g.types[len(g.types)-1]
			r := g.newReg()
			g.buf.WriteString(fmt.Sprintf("  %s = add i32 %s, 0\n", r, top))
			g.push(r, topT)
		}

	case typeir.Pop:
		if len(g.stack) > 0 {
			g.stack = g.stack[:len(g.stack)-1]
			g.types = g.types[:len(g.types)-1]
		}

	case typeir.Return:
		if len(args) > 0 && args[0] != "" {
			// Return with value from stack
			val, _ := g.pop()
			g.buf.WriteString(fmt.Sprintf("  ret i32 %s\n", val))
		} else {
			g.buf.WriteString("  ret void\n")
		}

	case typeir.ReturnVoid:
		g.buf.WriteString("  ret void\n")

	case typeir.Call:
		if len(args) == 0 {
			break
		}
		funcID := args[0]
		callee := ""
		// The first ';'-prefixed arg is the callee name; subsequent args are
		// the parameter type names (e.g. "string", "int").
		paramTypeNames := make([]string, 0)
		for _, a := range args[1:] {
			if strings.HasPrefix(a, ";") {
				if callee == "" {
					callee = a[1:]
				}
				continue
			}
			if callee != "" {
				// After the callee, remaining args are param type names.
				paramTypeNames = append(paramTypeNames, a)
			}
		}
		if callee == "" {
			callee = "@f" + funcID
		} else {
			callee = "@" + callee
		}

		// Collect call arguments by popping one value per declared param type.
		var callArgs []string
		var callTypes []typeir.Typ
		for range paramTypeNames {
			reg, t := g.pop()
			callArgs = append(callArgs, reg)
			callTypes = append(callTypes, t)
		}
		// Reverse so args appear in original left-to-right order.
		for i, j := 0, len(callArgs)-1; i < j; i, j = i+1, j-1 {
			callArgs[i], callArgs[j] = callArgs[j], callArgs[i]
			callTypes[i], callTypes[j] = callTypes[j], callTypes[i]
		}

		// Build param list with proper LLVM types.
		paramParts := make([]string, len(callArgs))
		for i, a := range callArgs {
			paramParts[i] = fmt.Sprintf("%s %s", llvmType(callTypes[i]), a)
		}

		if fn := findFuncByID(inst, funcID); fn != nil && fn.HasReturn {
			r := g.newReg()
			g.buf.WriteString(fmt.Sprintf("  %s = call i32 %s(%s)\n", r, callee, strings.Join(paramParts, ", ")))
			g.push(r, typeir.Int)
		} else {
			g.buf.WriteString(fmt.Sprintf("  call void %s(%s)\n", callee, strings.Join(paramParts, ", ")))
		}

	default:
		// For unsupported ops, emit a comment
		g.buf.WriteString(fmt.Sprintf("  ; unsupported op: %s %s\n", op, strings.Join(args, ", ")))
	}

	return nil
}

// findFuncByID looks up a function by ID from the instruction context.
// Since we don't have module access here, return nil (calls without return type assumed void).
func findFuncByID(inst *typeir.TypedInstruction, id string) *typeir.FunctionIR {
	return nil
}

// generateWithBlocks generates LLVM IR with basic blocks for functions that have jumps.
func generateWithBlocks(b *strings.Builder, fn *typeir.FunctionIR) error {
	// Discover jump targets
	targets := make(map[int]bool)
	targets[0] = true // entry block
	blockStarts := []int{0}

	for i, inst := range fn.Body {
		switch inst.Op {
		case typeir.Jump:
			if len(inst.Args) > 0 {
				if t, err := strconv.Atoi(inst.Args[0]); err == nil {
					if !targets[t] {
						targets[t] = true
						blockStarts = append(blockStarts, t)
					}
				}
			}
		case typeir.JumpIfFalse, typeir.JumpIfTrue:
			if len(inst.Args) >= 2 {
				if t, err := strconv.Atoi(inst.Args[1]); err == nil {
					if !targets[t] {
						targets[t] = true
						blockStarts = append(blockStarts, t)
					}
				}
			}
			// Fall-through also creates a block
			if i+1 < len(fn.Body) && !targets[i+1] {
				targets[i+1] = true
				blockStarts = append(blockStarts, i+1)
			}
		}
	}

	// Sort block starts
	sortInts(blockStarts)

	// Build block ranges
	type blockRange struct {
		start, end int
		name       string
	}
	var blocks []blockRange
	for idx, start := range blockStarts {
		end := len(fn.Body)
		if idx+1 < len(blockStarts) {
			end = blockStarts[idx+1]
		}
		name := fmt.Sprintf("bb%d", start)
		blocks = append(blocks, blockRange{start, end, name})
	}

	blockMap := make(map[int]string)
	for _, blk := range blocks {
		blockMap[blk.start] = blk.name
	}

	// Create alloca for stack-based execution
	b.WriteString(fmt.Sprintf("  %%vstack = alloca [256 x i64], align 8\n"))
	b.WriteString(fmt.Sprintf("  %%vsp = alloca i64, align 8\n"))
	b.WriteString("  store i64 0, i64* %vsp\n")

	// Emit each block
	for bi, blk := range blocks {
		if bi == 0 {
			fmt.Fprintf(b, "entry:\n")
		} else {
			b.WriteString(fmt.Sprintf("%s:\n", blk.name))
		}

		for i := blk.start; i < blk.end; i++ {
			inst := fn.Body[i]
			if err := emitStackInst(b, &inst, targetBlockName(inst, blockMap)); err != nil {
				return fmt.Errorf("inst %d: %w", i, err)
			}
		}
	}

	return nil
}

func targetBlockName(inst typeir.TypedInstruction, blockMap map[int]string) string {
	switch inst.Op {
	case typeir.Jump:
		if len(inst.Args) > 0 {
			if t, err := strconv.Atoi(inst.Args[0]); err == nil {
				if name, ok := blockMap[t]; ok {
					return name
				}
			}
		}
	case typeir.JumpIfFalse, typeir.JumpIfTrue:
		if len(inst.Args) >= 2 {
			if t, err := strconv.Atoi(inst.Args[1]); err == nil {
				if name, ok := blockMap[t]; ok {
					return name
				}
			}
		}
	}
	return ""
}

func emitStackInst(b *strings.Builder, inst *typeir.TypedInstruction, target string) error {
	op := inst.Op
	args := inst.Args

	switch op {
	case typeir.ConstInt:
		val := "0"
		if len(args) > 0 {
			val = args[0]
		}
		n := newStackCounter()
		b.WriteString(fmt.Sprintf("  %%c%d = add i32 %s, 0\n", n, val))
		b.WriteString(fmt.Sprintf("  %%sp%d = load i64, i64* %%vsp\n", n))
		b.WriteString(fmt.Sprintf("  %%p%d = getelementptr [256 x i64], [256 x i64]* %%vstack, i64 0, i64 %%sp%d\n", n, n))
		b.WriteString(fmt.Sprintf("  store i64 %%c%d, i64* %%p%d\n", n, n))
		b.WriteString(fmt.Sprintf("  %%nsp%d = add i64 %%sp%d, 1\n", n, n))
		b.WriteString(fmt.Sprintf("  store i64 %%nsp%d, i64* %%vsp\n", n))

	case typeir.I32Add:
		n := newStackCounter()
		fmt.Fprintf(b, "  ;; I32Add\n")
		b.WriteString(fmt.Sprintf("  %%a%d = call i64 @__vx_stack_pop()\n", n))
		b.WriteString(fmt.Sprintf("  %%b%d = call i64 @__vx_stack_pop()\n", n))
		b.WriteString(fmt.Sprintf("  %%c%d = add i64 %%a%d, %%b%d\n", n, n, n))
		b.WriteString(fmt.Sprintf("  call void @__vx_stack_push(i64 %%c%d)\n", n))

	case typeir.Return:
		if len(args) > 0 && args[0] != "" {
			n := newStackCounter()
			b.WriteString(fmt.Sprintf("  %%r%d = call i64 @__vx_stack_pop()\n", n))
			b.WriteString(fmt.Sprintf("  %%r%d = trunc i64 %%r%d to i32\n", n+1, n))
			b.WriteString(fmt.Sprintf("  ret i32 %%r%d\n", n+1))
		} else {
			b.WriteString("  ret void\n")
		}

	case typeir.ReturnVoid:
		b.WriteString("  ret void\n")

	case typeir.Jump:
		if target != "" {
			b.WriteString(fmt.Sprintf("  br label %%%s\n", target))
		}

	default:
		n := newStackCounter()
		b.WriteString(fmt.Sprintf("  ;; unsupported op: %s\n", op))
		// Push 0 as placeholder
		fmt.Fprintf(b, "  call void @__vx_stack_push(i64 0)\n")
		_ = n
	}

	return nil
}

var stackCounter int

func newStackCounter() int {
	stackCounter++
	return stackCounter
}

func sortInts(s []int) {
	for i := 0; i < len(s); i++ {
		for j := i + 1; j < len(s); j++ {
			if s[j] < s[i] {
				s[i], s[j] = s[j], s[i]
			}
		}
	}
}

// llvmTypeName returns LLVM type name for a primitive type string.
func llvmTypeName(ty string) string {
	switch ty {
	case "int", "bool":
		return "i32"
	case "float":
		return "double"
	case "void":
		return "void"
	default:
		return "i32"
	}
}

// escapeLLVMString escapes a Go string for use inside an LLVM IR `c"..."`
// string constant. Backslashes, double quotes, and non-printable characters
// are escaped using LLVM's \xx hex escapes.
func escapeLLVMString(s string) string {
	var b strings.Builder
	for _, r := range s {
		switch r {
		case '\\':
			b.WriteString("\\\\")
		case '"':
			b.WriteString("\\22")
		case '\n':
			b.WriteString("\\0A")
		case '\t':
			b.WriteString("\\09")
		case '\r':
			b.WriteString("\\0D")
		case 0:
			b.WriteString("\\00")
		default:
			if r < 0x20 || r == 0x7f {
				b.WriteString(fmt.Sprintf("\\%02X", r))
			} else {
				b.WriteRune(r)
			}
		}
	}
	return b.String()
}
