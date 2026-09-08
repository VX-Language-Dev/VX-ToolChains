package typeirgen

import (
	"fmt"
	"sort"
	"strconv"
	"strings"

	"github.com/vx-lang/vxc/internal/ast"
	"github.com/vx-lang/vxc/internal/typeir"
)

// Context simulates compilation state for one function.
type Context struct {
	Instructions []typeir.TypedInstruction
	Locals       map[string]typeir.Typ
	LocalIDs     map[string]uint32 // name -> numeric local ID
	NextLocalID  uint32
	Stack        []typeir.Typ
	LabelCounter int
	ReturnType   typeir.Typ          // return type of the function being compiled
	FuncReturns  map[string]bool     // function name -> has return value (non-void)
	breakLabels  []string            // loop break targets (innermost last)
	continueLabels []string          // loop continue targets (innermost last)
}

// Generator holds a generated TypeIR module.
type Generator struct {
	Module typeir.TypeIRModule
}

// NewContext creates a fresh compilation context.
func NewContext() *Context {
	return &Context{
		Instructions: []typeir.TypedInstruction{},
		Locals:       map[string]typeir.Typ{},
		LocalIDs:     map[string]uint32{},
		Stack:        []typeir.Typ{},
		LabelCounter: 0,
		FuncReturns:  map[string]bool{},
	}
}

// emit appends a typed instruction to the current function body.
func (c *Context) emit(op typeir.Opcode, args []string, types []typeir.Typ) {
	c.Instructions = append(c.Instructions, typeir.TypedInstruction{
		Op:    op,
		Args:  args,
		Types: types,
	})
}

// emit0 emits an instruction with no arguments or type annotations.
func (c *Context) emit0(op typeir.Opcode) {
	c.emit(op, nil, nil)
}

// pushStack records that a value of type t was pushed onto the operand stack.
func (c *Context) pushStack(t typeir.Typ) {
	c.Stack = append(c.Stack, t)
}

// popStack removes and returns the top type from the operand stack.
func (c *Context) popStack() typeir.Typ {
	if len(c.Stack) == 0 {
		return typeir.Unknown
	}
	t := c.Stack[len(c.Stack)-1]
	c.Stack = c.Stack[:len(c.Stack)-1]
	return t
}

// allocateLocal registers a local variable with the given name and type,
// assigning it a stable numeric ID used by LoadVar/StoreVar and the LLVM backend.
func (c *Context) allocateLocal(name string, t typeir.Typ) {
	c.Locals[name] = t
	if _, ok := c.LocalIDs[name]; !ok {
		c.LocalIDs[name] = c.NextLocalID
		c.NextLocalID++
	}
}

// localID returns the numeric ID for a local variable, allocating one if needed.
func (c *Context) localID(name string) uint32 {
	if id, ok := c.LocalIDs[name]; ok {
		return id
	}
	id := c.NextLocalID
	c.LocalIDs[name] = id
	c.NextLocalID++
	return id
}

// newLabel generates a unique textual label.
func (c *Context) newLabel() string {
	c.LabelCounter++
	return fmt.Sprintf("L%d", c.LabelCounter)
}

// compileBoolCond compiles a boolean condition expression with proper
// short-circuit semantics for && and ||. The result (0 or 1) is pushed onto
// the operand stack, consumed by the caller's JumpIfFalse. Non-short-circuit
// expressions are compiled as-is via CompileExpr.
//
// Each emitted JumpIfFalse/JumpIfTrue consumes one value from the operand
// stack, so the type stack must mirror that: popStack() accompanies every
// conditional jump. All paths leave exactly one result on the stack.
func compileBoolCond(c *Context, expr ast.Expr) {
	switch e := expr.(type) {
	case *ast.BinaryOp:
		switch e.Op {
		case "&&":
			compileBoolCond(c, e.Left)
			c.popStack()
			endLabel := c.newLabel()
			c.emit(typeir.JumpIfFalse, []string{endLabel}, nil)
			compileBoolCond(c, e.Right)
			c.popStack()
			c.emit(typeir.JumpIfFalse, []string{endLabel}, nil)
			c.emit(typeir.ConstInt, []string{"1"}, []typeir.Typ{typeir.Int})
			doneLabel := c.newLabel()
			c.emit(typeir.Jump, []string{doneLabel}, nil)
			c.emitLabel(endLabel)
			c.emit(typeir.ConstInt, []string{"0"}, []typeir.Typ{typeir.Int})
			c.emitLabel(doneLabel)
			return
		case "||":
			compileBoolCond(c, e.Left)
			c.popStack()
			trueLabel := c.newLabel()
			c.emit(typeir.JumpIfTrue, []string{trueLabel}, nil)
			compileBoolCond(c, e.Right)
			c.popStack()
			endLabel := c.newLabel()
			c.emit(typeir.JumpIfFalse, []string{endLabel}, nil)
			c.emit(typeir.ConstInt, []string{"1"}, []typeir.Typ{typeir.Int})
			c.emit(typeir.Jump, []string{endLabel}, nil)
			c.emitLabel(trueLabel)
			c.emit(typeir.ConstInt, []string{"1"}, []typeir.Typ{typeir.Int})
			c.emitLabel(endLabel)
			return
		}
	case *ast.UnaryOp:
		if e.Op == "!" {
			compileBoolCond(c, e.Expr)
			c.emit0(typeir.BoolNot)
			return
		}
	}
	CompileExpr(c, expr)
}

// pushLoopLabels records the current loop's continue/break targets so
// nested break/continue statements can jump to the right labels.
func (c *Context) pushLoopLabels(startLabel, endLabel string) {
	c.continueLabels = append(c.continueLabels, startLabel)
	c.breakLabels = append(c.breakLabels, endLabel)
}

func (c *Context) popLoopLabels() {
	if len(c.continueLabels) > 0 {
		c.continueLabels = c.continueLabels[:len(c.continueLabels)-1]
	}
	if len(c.breakLabels) > 0 {
		c.breakLabels = c.breakLabels[:len(c.breakLabels)-1]
	}
}

// emitLabel emits a Noop instruction carrying a label name.
func (c *Context) emitLabel(name string) {
	c.emit(typeir.Noop, []string{name}, nil)
}

// CompileExpr recursively compiles an expression and emits instructions.
func CompileExpr(c *Context, e ast.Expr) {
	switch expr := e.(type) {
	case *ast.IntLiteral:
		c.emit(typeir.ConstInt, []string{strconv.FormatInt(expr.Value, 10)}, []typeir.Typ{typeir.Int})
		c.pushStack(typeir.Int)

	case *ast.FloatLiteral:
		c.emit(typeir.ConstFloat, []string{strconv.FormatFloat(expr.Value, 'g', -1, 64)}, []typeir.Typ{typeir.Float})
		c.pushStack(typeir.Float)

	case *ast.BoolLiteral:
		v := "false"
		if expr.Value {
			v = "true"
		}
		c.emit(typeir.ConstBool, []string{v}, []typeir.Typ{typeir.Bool})
		c.pushStack(typeir.Bool)

	case *ast.StringLiteral:
		c.emit(typeir.ConstString, []string{expr.Value}, []typeir.Typ{typeir.String})
		c.pushStack(typeir.String)

	case *ast.NilLiteral:
		c.emit(typeir.ConstNil, nil, []typeir.Typ{typeir.Unknown})
		c.pushStack(typeir.Unknown)

	case *ast.Identifier:
		var t typeir.Typ = typeir.Unknown
		if lt, ok := c.Locals[expr.Name]; ok {
			t = lt
		}
		id := c.localID(expr.Name)
		c.emit(typeir.LoadVar, []string{strconv.Itoa(int(id))}, []typeir.Typ{t})
		c.pushStack(t)

	case *ast.BinaryOp:
		if expr.Op == "=" {
			// Assignment parsed as a binary expression: target = value.
			compileAssign(c, expr.Left, expr.Right)
			return
		}
		CompileExpr(c, expr.Left)
		leftTyp := c.popStack()
		CompileExpr(c, expr.Right)
		rightTyp := c.popStack()
		if expr.Op == "+" && (isStringType(leftTyp) || isStringType(rightTyp)) {
			// Implicit int/bool → string conversion in concatenation.
			// itoa consumes the top of the operand stack, so convert the
			// right operand first; converting the left operand requires
			// stashing the right value in a temp local.
			if !isStringType(rightTyp) && isIntOrBool(rightTyp) {
				c.emit(typeir.Call, []string{";itoa", typeir.TypToString(rightTyp)}, []typeir.Typ{typeir.Unknown})
				rightTyp = typeir.String
			}
			if !isStringType(leftTyp) && isIntOrBool(leftTyp) {
				tmpName := fmt.Sprintf("__concat_tmp_%d", len(c.LocalIDs))
				c.allocateLocal(tmpName, typeir.String)
				tmpID := c.localID(tmpName)
				c.emit(typeir.StoreVar, []string{strconv.Itoa(int(tmpID))}, []typeir.Typ{typeir.String})
				c.emit(typeir.Call, []string{";itoa", typeir.TypToString(leftTyp)}, []typeir.Typ{typeir.Unknown})
				c.emit(typeir.LoadVar, []string{strconv.Itoa(int(tmpID))}, []typeir.Typ{typeir.String})
				leftTyp = typeir.String
			}
		}
		emitBinaryOp(c, expr.Op, leftTyp, rightTyp)
		c.pushStack(resultTypeOfBinary(expr.Op, leftTyp, rightTyp))

	case *ast.UnaryOp:
		switch expr.Op {
		case "-":
			CompileExpr(c, expr.Expr)
			childTyp := c.popStack()
			if isFloatType(childTyp) {
				c.emit(typeir.ConstFloat, []string{"0"}, []typeir.Typ{typeir.Float})
				CompileExpr(c, expr.Expr)
				c.popStack()
				c.emit0(typeir.F64Sub)
				c.pushStack(typeir.Float)
			} else {
				c.emit(typeir.ConstInt, []string{"0"}, []typeir.Typ{typeir.Int})
				CompileExpr(c, expr.Expr)
				c.popStack()
				c.emit0(typeir.I32Sub)
				c.pushStack(typeir.Int)
			}
		case "!":
			CompileExpr(c, expr.Expr)
			c.popStack()
			c.emit0(typeir.BoolNot)
			c.pushStack(typeir.Bool)
		}

	case *ast.CallExpr:
		argTypes := make([]typeir.Typ, 0, len(expr.Args))
		for _, arg := range expr.Args {
			CompileExpr(c, arg)
			argTypes = append(argTypes, c.popStack())
		}
		funcName := ""
		if id, ok := expr.Callee.(*ast.Identifier); ok {
			funcName = id.Name
		}
		// Handle platform builtins that have dedicated opcodes.
		switch funcName {
		case "sys_argv":
			c.emit(typeir.SysArgv, nil, []typeir.Typ{typeir.Int})
			c.pushStack(typeir.Int)
			return
		case "system":
			c.emit(typeir.System, nil, []typeir.Typ{typeir.Int})
			c.pushStack(typeir.Int)
			return
		case "file_read":
			c.emit(typeir.FileRead, nil, []typeir.Typ{typeir.String})
			c.pushStack(typeir.String)
			return
		case "file_write":
			c.emit(typeir.FileWrite, nil, []typeir.Typ{typeir.Int})
			c.pushStack(typeir.Int)
			return
		case "file_exists":
			c.emit(typeir.FileExists, nil, []typeir.Typ{typeir.Bool})
			c.pushStack(typeir.Bool)
			return
		}
		// Implicit int/bool → string conversion for the output builtins.
		// The translator's @out wrapper takes i8* only, so non-string
		// arguments are converted via itoa (which pops the value and
		// pushes its string form back onto the operand stack).
		if (funcName == "out" || funcName == "println") && len(argTypes) == 1 && isIntOrBool(argTypes[0]) {
			c.emit(typeir.Call, []string{";itoa", typeir.TypToString(argTypes[0])}, []typeir.Typ{typeir.Unknown})
			argTypes[0] = typeir.String
		}
		// The LLVM IR backend expects the callee name prefixed with ';'
		// (e.g. ";out") so it can emit a direct call to @out instead of
		// treating funcName as a numeric function ID (which would produce
		// an undefined @f<name> reference). Argument type names are appended
		// after the callee so the backend knows how many args to pop and
		// their LLVM types.
		callArgs := []string{";" + funcName}
		for _, t := range argTypes {
			callArgs = append(callArgs, typeir.TypToString(t))
		}
		// The translator's stack builtins return their result via the
		// virtual stack; model their result types so downstream code
		// (implicit conversions, variable declarations) sees correct types.
		retTyp := typeir.Unknown
		switch funcName {
		case "len", "ord":
			retTyp = typeir.Int
		case "chr", "itoa":
			retTyp = typeir.String
		}
		// Void user functions (e.g. sknl(), nxt()) return nothing on the
		// operand stack, so the statement-level Pop must not fire for them.
		if retTyp == typeir.Unknown && funcName != "out" && funcName != "println" {
			if hasRet, ok := c.FuncReturns[funcName]; ok && !hasRet {
				retTyp = typeir.Void
			}
		}
		c.emit(typeir.Call, callArgs, []typeir.Typ{typeir.Unknown})
		if typeir.TypeName(retTyp) != "void" {
			c.pushStack(retTyp)
		}

	case *ast.VarDecl:
		CompileExpr(c, expr.Init)
		initTyp := c.popStack()
		typ := typeFromTypeExpr(expr.Type)
		if typeir.TypToString(typ) == "unknown" {
			typ = initTyp
		}
		c.allocateLocal(expr.Name, typ)
		id := c.localID(expr.Name)
		c.emit(typeir.StoreVar, []string{strconv.Itoa(int(id))}, []typeir.Typ{typ})

	case *ast.Assign:
		compileAssign(c, expr.Target, expr.Value)

	case *ast.IfStmt:
		compileBoolCond(c, expr.Condition)
		c.popStack()
		elseLabel := c.newLabel()
		endLabel := c.newLabel()
		c.emit(typeir.JumpIfFalse, []string{elseLabel}, nil)
		for _, s := range expr.ThenBranch {
			CompileExpr(c, s)
		}
		c.emit(typeir.Jump, []string{endLabel}, nil)

		currentElse := elseLabel
		for _, elif := range expr.ElifBranches {
			c.emitLabel(currentElse)
			nextElse := c.newLabel()
			compileBoolCond(c, elif.Condition)
			c.popStack()
			c.emit(typeir.JumpIfFalse, []string{nextElse}, nil)
			for _, s := range elif.Body {
				CompileExpr(c, s)
			}
			c.emit(typeir.Jump, []string{endLabel}, nil)
			currentElse = nextElse
		}

		c.emitLabel(currentElse)
		for _, s := range expr.ElseBranch {
			CompileExpr(c, s)
		}
		c.emitLabel(endLabel)

	case *ast.WhileStmt:
		startLabel := c.newLabel()
		endLabel := c.newLabel()
		c.emitLabel(startLabel)
		c.pushLoopLabels(startLabel, endLabel)
		compileBoolCond(c, expr.Condition)
		c.popStack()
		c.emit(typeir.JumpIfFalse, []string{endLabel}, nil)
		for _, s := range expr.Body {
			CompileExpr(c, s)
		}
		c.emit(typeir.Jump, []string{startLabel}, nil)
		c.popLoopLabels()
		c.emitLabel(endLabel)

	case *ast.ForStmt:
		// For-in loop: for VarName in Iterable { Body }
		// Step 1: evaluate the iterable and store it in a temp variable.
		CompileExpr(c, expr.Iterable)
		iterVar := fmt.Sprintf("__for_iter_%d", len(c.LocalIDs))
		c.allocateLocal(iterVar, typeir.Unknown)
		c.emit(typeir.StoreVar, []string{strconv.Itoa(int(c.localID(iterVar)))}, nil)

		// Step 2: compute the iterable's length via len() and store it.
		// Re-load the iterable so len() operates on the live value.
		c.emit(typeir.LoadVar, []string{strconv.Itoa(int(c.localID(iterVar)))}, nil)
		c.emit(typeir.Call, []string{";len", "string"}, []typeir.Typ{typeir.Int})
		lenVar := fmt.Sprintf("__for_len_%d", len(c.LocalIDs))
		c.allocateLocal(lenVar, typeir.Int)
		c.emit(typeir.StoreVar, []string{strconv.Itoa(int(c.localID(lenVar)))}, nil)

		// Step 3: initialize index to 0.
		idxVar := fmt.Sprintf("__for_idx_%d", len(c.LocalIDs))
		c.allocateLocal(idxVar, typeir.Int)
		c.emit(typeir.ConstInt, []string{"0"}, []typeir.Typ{typeir.Int})
		c.emit(typeir.StoreVar, []string{strconv.Itoa(int(c.localID(idxVar)))}, nil)

		startLabel := c.newLabel()
		endLabel := c.newLabel()
		c.emitLabel(startLabel)
		c.pushLoopLabels(startLabel, endLabel)

		// Step 4: check index < len(iterable)
		c.emit(typeir.LoadVar, []string{strconv.Itoa(int(c.localID(idxVar)))}, []typeir.Typ{typeir.Int})
		c.emit(typeir.LoadVar, []string{strconv.Itoa(int(c.localID(lenVar)))}, []typeir.Typ{typeir.Int})
		c.emit0(typeir.I32Lt)
		c.popStack()
		c.emit(typeir.JumpIfFalse, []string{endLabel}, nil)

		// Step 5: elem = iterable[index]
		c.emit(typeir.LoadVar, []string{strconv.Itoa(int(c.localID(iterVar)))}, nil)
		c.emit(typeir.LoadVar, []string{strconv.Itoa(int(c.localID(idxVar)))}, []typeir.Typ{typeir.Int})
		c.emit(typeir.IndexGet, nil, nil)

		// Step 6: store in loop variable VarName.
		c.allocateLocal(expr.VarName, typeir.Unknown)
		c.emit(typeir.StoreVar, []string{strconv.Itoa(int(c.localID(expr.VarName)))}, nil)

		// Step 7: execute loop body.
		for _, s := range expr.Body {
			CompileExpr(c, s)
		}

		// Step 8: index++ and loop back.
		c.emit(typeir.LoadVar, []string{strconv.Itoa(int(c.localID(idxVar)))}, []typeir.Typ{typeir.Int})
		c.emit(typeir.ConstInt, []string{"1"}, []typeir.Typ{typeir.Int})
		c.emit0(typeir.I32Add)
		c.popStack()
		c.emit(typeir.StoreVar, []string{strconv.Itoa(int(c.localID(idxVar)))}, nil)

		c.emit(typeir.Jump, []string{startLabel}, nil)
		c.popLoopLabels()
		c.emitLabel(endLabel)

	case *ast.ReturnStmt:
		if expr.Value != nil {
			CompileExpr(c, expr.Value)
			c.popStack()
			// Attach the function's return type so the LLVM backend can
			// emit the correct ret instruction even when the operand stack
			// type information is incomplete.
			c.emit(typeir.Return, []string{typeir.TypToString(c.ReturnType)}, nil)
		} else {
			c.emit0(typeir.ReturnVoid)
		}

	case *ast.BreakStmt:
		if len(c.breakLabels) > 0 {
			c.emit(typeir.Jump, []string{c.breakLabels[len(c.breakLabels)-1]}, nil)
		}

	case *ast.ContinueStmt:
		if len(c.continueLabels) > 0 {
			c.emit(typeir.Jump, []string{c.continueLabels[len(c.continueLabels)-1]}, nil)
		}

	case *ast.StructDecl:
		c.emit(typeir.StructNew, []string{expr.Name}, nil)

	case *ast.ClassDecl:
		c.emit(typeir.StructNew, []string{expr.Name}, nil)

	case *ast.EnumDecl:
		c.emit(typeir.StructNew, []string{expr.Name}, nil)

	case *ast.UnionDecl:
		c.emit(typeir.StructNew, []string{expr.Name}, nil)

	case *ast.NewExpr:
		for _, arg := range expr.Positional {
			CompileExpr(c, arg)
			c.popStack()
		}
		typ := typeir.StructTyp{Name: expr.TypeName}
		c.emit(typeir.Newz, []string{expr.TypeName}, []typeir.Typ{typ})
		c.pushStack(typ)

	case *ast.MoveExpr:
		CompileExpr(c, expr.Expr)
		c.popStack()
		c.emit0(typeir.OwnershipMove)

	case *ast.AddressOf:
		CompileExpr(c, expr.Expr)
		inner := c.popStack()
		typ := typeir.PointerTyp{To: inner}
		c.emit(typeir.AddressOf, nil, []typeir.Typ{typ})
		c.pushStack(typ)

	case *ast.Deref:
		CompileExpr(c, expr.Expr)
		inner := c.popStack()
		var typ typeir.Typ = typeir.Unknown
		if pt, ok := inner.(typeir.PointerTyp); ok {
			typ = pt.To
		}
		c.emit(typeir.Deref, nil, []typeir.Typ{typ})
		c.pushStack(typ)

	case *ast.IndexAccess:
		CompileExpr(c, expr.Target)
		targetTyp := c.popStack()
		CompileExpr(c, expr.Index)
		c.popStack()
		typ := typeir.Unknown
		indexArgs := []string(nil)
		if isStringType(targetTyp) {
			// s[i] yields a pointer to the i-th char — usable as a
			// 1-char string with ord(s[i]) and s[i] == 'c'. The "s" arg
			// marker survives VXOBJ serialization (Types are not serialized).
			typ = typeir.String
			indexArgs = []string{"s"}
		}
		c.emit(typeir.IndexGet, indexArgs, []typeir.Typ{typ})
		c.pushStack(typ)

	case *ast.PropertyAccess:
		CompileExpr(c, expr.Target)
		c.popStack()
		// Try to determine the struct type name from the target.
		structName := ""
		if id, ok := expr.Target.(*ast.Identifier); ok {
			if lt, ok := c.Locals[id.Name]; ok {
				if st, ok := lt.(typeir.StructTyp); ok {
					structName = st.Name
				}
			}
		}
		args := []string{expr.Prop}
		if structName != "" {
			args = append(args, structName)
		}
		typ := typeir.Unknown
		// Use FieldLoad instead of PropertyGet — FieldLoad is handled by
		// both the translator (LLVM IR generation) and the decompiler (vxde).
		c.emit(typeir.FieldLoad, args, []typeir.Typ{typ})
		c.pushStack(typ)

	case *ast.PointerMember:
		CompileExpr(c, expr.Expr)
		c.popStack()
		typ := typeir.Unknown
		c.emit(typeir.PointerMember, []string{expr.Member}, []typeir.Typ{typ})
		c.pushStack(typ)

	case *ast.ArrayLiteral:
		for _, el := range expr.Elements {
			CompileExpr(c, el)
			c.popStack()
		}
		typ := typeir.ArrayTyp{Elem: typeir.Unknown}
		c.emit(typeir.ArrayNew, []string{strconv.Itoa(len(expr.Elements))}, []typeir.Typ{typ})
		c.pushStack(typ)

	case *ast.MapLiteral:
		for _, pair := range expr.Pairs {
			CompileExpr(c, pair.Key)
			c.popStack()
			CompileExpr(c, pair.Value)
			c.popStack()
		}
		typ := typeir.MapTyp{Key: typeir.Unknown, Value: typeir.Unknown}
		c.emit(typeir.MapNew, []string{strconv.Itoa(len(expr.Pairs))}, []typeir.Typ{typ})
		c.pushStack(typ)

	case *ast.ExprStmt:
		CompileExpr(c, expr.Expr)
		if len(c.Stack) > 0 {
			c.popStack()
			c.emit0(typeir.Pop)
		}

	case *ast.ImportStmt:
		c.emit(typeir.Import, []string{expr.Path}, nil)
	}
}

// CompileStmt is a thin wrapper around CompileExpr.
func CompileStmt(c *Context, s ast.Expr) {
	CompileExpr(c, s)
}

// compileAssign compiles an assignment statement: target = value.
// The value expression leaves its result on the operand stack, which the
// emitted StoreVar / FieldStore / IndexSet instruction consumes.
//
// IMPORTANT: The value must remain on the operand stack so that FieldStore
// and IndexSet (which pop multiple operands from the stack) can consume it.
// We record the value's type via popStack() but immediately re-push it
// using a Const* instruction so the stack layout is preserved.
func compileAssign(c *Context, target, value ast.Expr) {
	// Compile the value expression — it pushes its result onto the stack.
	CompileExpr(c, value)
	// Record the value's type for the StoreVar/FieldStore/IndexSet type
	// annotation, then re-push it so it remains on the stack for the
	// target-specific instruction to consume.
	valueTyp := c.popStack()
	c.pushStack(valueTyp)

	targetName := ""
	if id, ok := target.(*ast.Identifier); ok {
		targetName = id.Name
	}
	// Resolve the declared type of the target variable (for StoreVar).
	typ := valueTyp
	if id, ok := target.(*ast.Identifier); ok {
		if lt, ok := c.Locals[id.Name]; ok {
			typ = lt
		} else {
			c.allocateLocal(id.Name, valueTyp)
		}
	}
	// Handle struct field assignment: p.x = val
	// Stack layout after CompileExpr(value) + CompileExpr(target):
	//   [value, structPtr]  (value pushed first, structPtr pushed second)
	// FieldStore in llvmir pops structPtr first, then value.
	if pa, ok := target.(*ast.PropertyAccess); ok {
		CompileExpr(c, pa.Target)
		// Do NOT pop the struct pointer — FieldStore consumes it from the
		// operand stack. The value remains on the stack from above.
		structName := ""
		if id, ok := pa.Target.(*ast.Identifier); ok {
			if lt, ok := c.Locals[id.Name]; ok {
				if st, ok := lt.(typeir.StructTyp); ok {
					structName = st.Name
				}
			}
		}
		args := []string{pa.Prop}
		if structName != "" {
			args = append(args, structName)
		}
		c.emit(typeir.FieldStore, args, []typeir.Typ{valueTyp})
		// Both operands consumed by FieldStore.
		c.popStack() // structPtr
		c.popStack() // value
		return
	}
	// Handle array index assignment: a[i] = val
	// Stack layout after CompileExpr(value) + CompileExpr(target) + CompileExpr(index):
	//   [value, index, ptr]  (value pushed first, ptr pushed last)
	// IndexSet in llvmir pops index first, then ptr, then value.
	if ia, ok := target.(*ast.IndexAccess); ok {
		CompileExpr(c, ia.Target)
		CompileExpr(c, ia.Index)
		// Do NOT pop — IndexSet consumes all three from the operand stack.
		c.emit(typeir.IndexSet, nil, []typeir.Typ{valueTyp})
		c.popStack() // index
		c.popStack() // ptr
		c.popStack() // value
		return
	}
	// Handle pointer dereference assignment: *p = val
	// Stack layout: [value, pointer]
	if dt, ok := target.(*ast.Deref); ok {
		CompileExpr(c, dt.Expr)
		// Emit a StoreVar-like instruction: store value at *pointer.
		// Use StoreVar with the pointer's local ID — but the pointer is
		// on the stack, not in a local. Use FieldStore with field 0 as
		// a store-through-pointer (struct field 0 of a zero-sized struct).
		// Simpler: emit a raw store by reusing the IndexSet pattern with
		// index 0 on the pointer itself.
		c.emit(typeir.IndexSet, nil, []typeir.Typ{valueTyp})
		c.popStack() // pointer
		c.popStack() // value
		return
	}
	if targetName == "" {
		// Unknown target type — still need to consume the value from
		// the operand stack to avoid stack corruption.
		c.popStack()
		c.emit0(typeir.Pop)
		return
	}
	id := c.localID(targetName)
	c.emit(typeir.StoreVar, []string{strconv.Itoa(int(id))}, []typeir.Typ{typ})
	// StoreVar consumes the value from the operand stack.
	c.popStack()
}

// CompileFunction compiles a single function declaration into FunctionIR.
// The globalTypes map provides types for global variables so that references
// to globals inside function bodies get correct LLVM IR types.
func CompileFunction(fn *ast.FuncDecl, globalTypes map[string]typeir.Typ) typeir.FunctionIR {
	return compileFunction(fn, globalTypes, map[string]bool{})
}

func compileFunction(fn *ast.FuncDecl, globalTypes map[string]typeir.Typ, funcReturns map[string]bool) typeir.FunctionIR {
	c := NewContext()
	c.FuncReturns = funcReturns
	// Globals are allocated first with stable, name-sorted IDs so the
	// LLVM backend can resolve them by ID (0..len(globals)-1).
	gNames := make([]string, 0, len(globalTypes))
	for name := range globalTypes {
		gNames = append(gNames, name)
	}
	sort.Strings(gNames)
	for _, name := range gNames {
		c.allocateLocal(name, globalTypes[name])
	}
	params := make([]typeir.ParamIR, 0, len(fn.Params))
	for _, p := range fn.Params {
		t := typeFromTypeExpr(&p.ParamType)
		params = append(params, typeir.ParamIR{Name: p.Name, Type: t})
		// Parameters arrive on the operand stack; store them into locals.
		c.allocateLocal(p.Name, t)
		id := c.localID(p.Name)
		c.emit(typeir.StoreVar, []string{strconv.Itoa(int(id))}, []typeir.Typ{t})
	}
	retType := typeFromTypeExpr(fn.ReturnType)
	if fn.ReturnType == nil {
		retType = typeir.Void
	}
	c.ReturnType = retType
	for _, stmt := range fn.Body {
		CompileStmt(c, stmt)
	}
	if len(c.Instructions) == 0 || !isReturnOpcode(c.Instructions[len(c.Instructions)-1].Op) {
		if typeir.TypeName(retType) != "void" {
			// Reaching the end of a non-void function without an explicit
			// return is undefined behavior; emit a default zero return so
			// the generated LLVM IR is well-formed.
			c.emit(typeir.ConstInt, []string{"0"}, nil)
			c.emit(typeir.Return, []string{typeir.TypToString(retType)}, nil)
		} else {
			c.emit0(typeir.ReturnVoid)
		}
	}
	hasReturn := typeir.TypeName(retType) != "void"
	localTypes := make(map[uint32]typeir.Typ, len(c.LocalIDs))
	for name, id := range c.LocalIDs {
		localTypes[id] = c.Locals[name]
	}
	return typeir.FunctionIR{
		Name:         fn.Name,
		ParamCount:   uint32(len(params)),
		HasReturn:    hasReturn,
		ReturnType:   retType,
		Params:       params,
		Instructions: c.Instructions,
		Body:         c.Instructions,
		Locals:       c.Locals,
		LocalTypes:   localTypes,
	}
}

// GenerateIR compiles a program slice into a TypeIRModule.
func GenerateIR(program []ast.Expr, moduleName string) *typeir.TypeIRModule {
	module := &typeir.TypeIRModule{
		Name:        moduleName,
		Functions:   []typeir.FunctionIR{},
		Globals:     []typeir.GlobalIR{},
		StructTypes: []typeir.StructTypeDef{},
		Linkage:     make(map[uint32]typeir.LinkageEntry),
	}
	funcID := uint32(0)
	globalTypes := make(map[string]typeir.Typ)
	for _, e := range program {
		if vd, ok := e.(*ast.VarDecl); ok {
			globalTypes[vd.Name] = typeFromTypeExpr(vd.Type)
		}
	}
	// Collect function return signatures so void calls (e.g. sknl(), nxt())
	// do not leave a phantom value on the operand stack.
	funcReturns := make(map[string]bool)
	for _, e := range program {
		switch fd := e.(type) {
		case *ast.FuncDecl:
			funcReturns[fd.Name] = fd.ReturnType != nil && fd.ReturnType.Name != "" && fd.ReturnType.Name != "void"
		case *ast.ExternDecl:
			funcReturns[fd.Name] = fd.ReturnType != nil && fd.ReturnType.Name != "" && fd.ReturnType.Name != "void"
		}
	}
	for _, e := range program {
		switch expr := e.(type) {
		case *ast.FuncDecl:
			fn := compileFunction(expr, globalTypes, funcReturns)
			fn.ID = funcID
			funcID++
			module.Functions = append(module.Functions, fn)
		case *ast.ExternDecl:
			fn := compileExternDecl(expr)
			fn.ID = funcID
			module.Linkage[funcID] = typeir.LinkageEntry{
				IsExternal: true,
				Name:       expr.Name,
			}
			funcID++
			module.Functions = append(module.Functions, fn)
		case *ast.VarDecl:
			module.Globals = append(module.Globals, typeir.GlobalIR{
				Name:  expr.Name,
				Type:  typeFromTypeExpr(expr.Type),
				Value: "",
			})
		case *ast.StructDecl:
			fields := make([]typeir.StructField, 0, len(expr.Fields))
			for _, f := range expr.Fields {
				var ft typeir.Typ = typeir.Unknown
				if f.FieldType.Name != "" {
					ft = typeFromTypeExpr(&f.FieldType)
				}
				fields = append(fields, typeir.StructField{Name: f.Name, FieldType: ft})
			}
			module.StructTypes = append(module.StructTypes, typeir.StructTypeDef{Name: expr.Name, Fields: fields})
		case *ast.ClassDecl:
			fields := make([]typeir.StructField, 0, len(expr.Fields))
			for _, f := range expr.Fields {
				var ft typeir.Typ = typeir.Unknown
				if f.FieldType.Name != "" {
					ft = typeFromTypeExpr(&f.FieldType)
				}
				fields = append(fields, typeir.StructField{Name: f.Name, FieldType: ft})
			}
			module.StructTypes = append(module.StructTypes, typeir.StructTypeDef{Name: expr.Name, Fields: fields})
		case *ast.EnumDecl:
			module.StructTypes = append(module.StructTypes, typeir.StructTypeDef{Name: expr.Name, Fields: []typeir.StructField{}})
		case *ast.UnionDecl:
			fields := make([]typeir.StructField, 0, len(expr.Fields))
			for _, f := range expr.Fields {
				var ft typeir.Typ = typeir.Unknown
				if f.FieldType.Name != "" {
					ft = typeFromTypeExpr(&f.FieldType)
				}
				fields = append(fields, typeir.StructField{Name: f.Name, FieldType: ft})
			}
			module.StructTypes = append(module.StructTypes, typeir.StructTypeDef{Name: expr.Name, Fields: fields})
		case *ast.ImportStmt:
			// Import statements are recorded as instructions during function compilation.
			// At the module level we simply acknowledge them.
		}
	}
	return module
}

// compileExternDecl builds a FunctionIR skeleton for an external function.
// The function has no body; its presence in the module plus a Linkage entry
// tells the translator to emit a `declare` instead of a `define`.
func compileExternDecl(decl *ast.ExternDecl) typeir.FunctionIR {
	params := make([]typeir.ParamIR, 0, len(decl.Params))
	for _, p := range decl.Params {
		params = append(params, typeir.ParamIR{
			Name: p.Name,
			Type: typeFromTypeExpr(&p.ParamType),
		})
	}
	retType := typeFromTypeExpr(decl.ReturnType)
	// Override return type for known C functions that return pointers.
	switch decl.Name {
	case "malloc", "calloc", "realloc":
		retType = typeir.PointerTyp{} // opaque pointer
	}
	hasReturn := retType != nil && typeir.TypeName(retType) != "void"
	return typeir.FunctionIR{
		Name:       decl.Name,
		ParamCount: uint32(len(params)),
		HasReturn:  hasReturn,
		ReturnType: retType,
		Params:     params,
		Body:       []typeir.TypedInstruction{},
		Locals:     map[string]typeir.Typ{},
		LocalTypes: map[uint32]typeir.Typ{},
	}
}

// typeFromTypeExpr converts an AST type expression to a TypeIR type.
func typeFromTypeExpr(te *ast.TypeExpr) typeir.Typ {
	if te == nil {
		return typeir.Unknown
	}
	var t typeir.Typ
	switch strings.ToLower(te.Name) {
	case "void":
		t = typeir.Void
	case "int", "i32", "i64":
		t = typeir.Int
	case "float", "f32", "f64":
		t = typeir.Float
	case "bool":
		t = typeir.Bool
	case "string", "str":
		t = typeir.String
	default:
		if len(te.GenericArgs) > 0 {
			args := make([]typeir.Typ, 0, len(te.GenericArgs))
			for _, a := range te.GenericArgs {
				args = append(args, typeFromTypeExpr(&a))
			}
			t = typeir.GenericTyp{Name: te.Name, Args: args}
		} else {
			t = typeir.StructTyp{Name: te.Name}
		}
	}
	if te.Pointer > 0 {
		for i := 0; i < te.Pointer; i++ {
			t = typeir.PointerTyp{To: t}
		}
	}
	return t
}

// emitBinaryOp emits the appropriate arithmetic/comparison/logical opcode.
func emitBinaryOp(c *Context, op string, leftTyp, rightTyp typeir.Typ) {
	isFloat := isFloatType(leftTyp) || isFloatType(rightTyp)
	isString := isStringType(leftTyp) || isStringType(rightTyp)
	switch op {
	case "+":
		if isFloat {
			c.emit0(typeir.F64Add)
		} else if isString {
			c.emit0(typeir.StrConcat)
		} else if isArrayType(leftTyp) || isArrayType(rightTyp) {
			// Array concatenation: use a dedicated ArrayConcat opcode
			// that handles array types correctly
			c.emit0(typeir.ArrayConcat)
		} else {
			c.emit0(typeir.I32Add)
		}
	case "-":
		if isFloat {
			c.emit0(typeir.F64Sub)
		} else {
			c.emit0(typeir.I32Sub)
		}
	case "*":
		if isFloat {
			c.emit0(typeir.F64Mul)
		} else {
			c.emit0(typeir.I32Mul)
		}
	case "/":
		if isFloat {
			c.emit0(typeir.F64Div)
		} else {
			c.emit0(typeir.I32Div)
		}
	case "%":
		c.emit0(typeir.I32Mod)
	case "==":
		if isFloat {
			c.emit0(typeir.F64Eq)
		} else if isString {
			c.emit0(typeir.StrEq)
		} else {
			c.emit0(typeir.I32Eq)
		}
	case "!=":
		if isString {
			c.emit0(typeir.StrEq)
			c.emit0(typeir.BoolNot)
		} else {
			c.emit0(typeir.I32Neq)
		}
	case "<":
		if isFloat {
			c.emit0(typeir.F64Lt)
		} else {
			c.emit0(typeir.I32Lt)
		}
	case ">":
		if isFloat {
			c.emit0(typeir.F64Gt)
		} else {
			c.emit0(typeir.I32Gt)
		}
	case "<=":
		if isFloat {
			c.emit0(typeir.F64Le)
		} else {
			c.emit0(typeir.I32Le)
		}
	case ">=":
		if isFloat {
			c.emit0(typeir.F64Ge)
		} else {
			c.emit0(typeir.I32Ge)
		}
	case "and", "&&":
		c.emit0(typeir.BoolAnd)
	case "or", "||":
		c.emit0(typeir.BoolOr)
	}
}

// resultTypeOfBinary returns the result type of a binary operation.
func resultTypeOfBinary(op string, leftTyp, rightTyp typeir.Typ) typeir.Typ {
	switch op {
	case "+", "-", "*", "/", "%":
		// String concatenation: "hello" + "world" → string
		if op == "+" && (isStringType(leftTyp) || isStringType(rightTyp)) {
			return typeir.String
		}
		if isFloatType(leftTyp) || isFloatType(rightTyp) {
			return typeir.Float
		}
		return typeir.Int
	case "==", "!=", "<", ">", "<=", ">=":
		return typeir.Bool
	case "and", "&&", "or", "||":
		return typeir.Bool
	}
	return typeir.Unknown
}

func isArrayType(t typeir.Typ) bool {
	return typeir.TypToString(t) == "array"
}

func isFloatType(t typeir.Typ) bool {
	return typeir.TypToString(t) == "float"
}

func isStringType(t typeir.Typ) bool {
	return typeir.TypToString(t) == "string"
}

// isIntOrBool reports whether t is an integer or boolean type.
// These can be implicitly converted to string via itoa in output and
// concatenation contexts.
func isIntOrBool(t typeir.Typ) bool {
	tn := typeir.TypToString(t)
	return tn == "int" || tn == "bool"
}

func isReturnOpcode(op typeir.Opcode) bool {
	return op == typeir.Return || op == typeir.ReturnVoid
}
