package typeirgen

import (
	"fmt"
	"strconv"
	"strings"

	"github.com/vx-lang/vxc/internal/ast"
	"github.com/vx-lang/vxc/internal/typeir"
)

// Context simulates compilation state for one function.
type Context struct {
	Instructions []typeir.TypedInstruction
	Locals       map[string]typeir.Typ
	Stack        []typeir.Typ
	LabelCounter int
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
		Stack:        []typeir.Typ{},
		LabelCounter: 0,
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

// allocateLocal registers a local variable with the given name and type.
func (c *Context) allocateLocal(name string, t typeir.Typ) {
	c.Locals[name] = t
}

// newLabel generates a unique textual label.
func (c *Context) newLabel() string {
	c.LabelCounter++
	return fmt.Sprintf("L%d", c.LabelCounter)
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
		c.emit(typeir.LoadVar, []string{expr.Name}, []typeir.Typ{t})
		c.pushStack(t)

	case *ast.BinaryOp:
		CompileExpr(c, expr.Left)
		leftTyp := c.popStack()
		CompileExpr(c, expr.Right)
		rightTyp := c.popStack()
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
		c.emit(typeir.Call, callArgs, []typeir.Typ{typeir.Unknown})
		c.pushStack(typeir.Unknown)

	case *ast.VarDecl:
		CompileExpr(c, expr.Init)
		initTyp := c.popStack()
		typ := typeFromTypeExpr(expr.Type)
		if typeir.TypToString(typ) == "unknown" {
			typ = initTyp
		}
		c.emit(typeir.StoreVar, []string{expr.Name}, []typeir.Typ{typ})
		c.allocateLocal(expr.Name, typ)

	case *ast.Assign:
		CompileExpr(c, expr.Value)
		valueTyp := c.popStack()
		targetName := ""
		if id, ok := expr.Target.(*ast.Identifier); ok {
			targetName = id.Name
		}
		typ := valueTyp
		if id, ok := expr.Target.(*ast.Identifier); ok {
			if lt, ok := c.Locals[id.Name]; ok {
				typ = lt
			} else {
				c.allocateLocal(id.Name, valueTyp)
			}
		}
		c.emit(typeir.StoreVar, []string{targetName}, []typeir.Typ{typ})

	case *ast.IfStmt:
		CompileExpr(c, expr.Condition)
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
			CompileExpr(c, elif.Condition)
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
		CompileExpr(c, expr.Condition)
		c.popStack()
		c.emit(typeir.JumpIfFalse, []string{endLabel}, nil)
		for _, s := range expr.Body {
			CompileExpr(c, s)
		}
		c.emit(typeir.Jump, []string{startLabel}, nil)
		c.emitLabel(endLabel)

	case *ast.ForStmt:
		CompileExpr(c, expr.Iterable)
		c.popStack()
		startLabel := c.newLabel()
		endLabel := c.newLabel()
		c.emitLabel(startLabel)
		// Simplified loop: a placeholder condition that is always true.
		c.emit(typeir.JumpIfFalse, []string{endLabel}, nil)
		for _, s := range expr.Body {
			CompileExpr(c, s)
		}
		c.emit(typeir.Jump, []string{startLabel}, nil)
		c.emitLabel(endLabel)

	case *ast.ReturnStmt:
		if expr.Value != nil {
			CompileExpr(c, expr.Value)
			c.popStack()
			c.emit0(typeir.Return)
		} else {
			c.emit0(typeir.ReturnVoid)
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
		c.popStack()
		CompileExpr(c, expr.Index)
		c.popStack()
		typ := typeir.Unknown
		c.emit(typeir.IndexGet, nil, []typeir.Typ{typ})
		c.pushStack(typ)

	case *ast.PropertyAccess:
		CompileExpr(c, expr.Target)
		c.popStack()
		typ := typeir.Unknown
		c.emit(typeir.PropertyGet, []string{expr.Prop}, []typeir.Typ{typ})
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

// CompileFunction compiles a single function declaration into FunctionIR.
func CompileFunction(fn *ast.FuncDecl) typeir.FunctionIR {
	c := NewContext()
	params := make([]typeir.ParamIR, 0, len(fn.Params))
	for _, p := range fn.Params {
		t := typeFromTypeExpr(&p.ParamType)
		params = append(params, typeir.ParamIR{Name: p.Name, Type: t})
		// Parameters arrive on the operand stack; store them into locals.
		c.emit(typeir.StoreVar, []string{p.Name}, []typeir.Typ{t})
		c.allocateLocal(p.Name, t)
	}
	for _, stmt := range fn.Body {
		CompileStmt(c, stmt)
	}
	if len(c.Instructions) == 0 || !isReturnOpcode(c.Instructions[len(c.Instructions)-1].Op) {
		c.emit0(typeir.ReturnVoid)
	}
	return typeir.FunctionIR{
		Name:         fn.Name,
		Params:       params,
		ReturnType:   typeFromTypeExpr(fn.ReturnType),
		Instructions: c.Instructions,
		Locals:       c.Locals,
	}
}

// GenerateIR compiles a program slice into a TypeIRModule.
func GenerateIR(program []ast.Expr, moduleName string) *typeir.TypeIRModule {
	module := &typeir.TypeIRModule{
		Name:        moduleName,
		Functions:   []typeir.FunctionIR{},
		Globals:     []typeir.GlobalIR{},
		StructTypes: []typeir.StructTypeDef{},
	}
	for _, e := range program {
		switch expr := e.(type) {
		case *ast.FuncDecl:
			module.Functions = append(module.Functions, CompileFunction(expr))
		case *ast.VarDecl:
			module.Globals = append(module.Globals, typeir.GlobalIR{
				Name:  expr.Name,
				Type:  typeFromTypeExpr(expr.Type),
				Value: "",
			})
		case *ast.StructDecl:
			fields := make([]typeir.StructField, 0, len(expr.Fields))
			for _, f := range expr.Fields {
				fields = append(fields, typeir.StructField{Name: f.Name, FieldType: typeir.Unknown})
			}
			module.StructTypes = append(module.StructTypes, typeir.StructTypeDef{Name: expr.Name, Fields: fields})
		case *ast.ClassDecl:
			fields := make([]typeir.StructField, 0, len(expr.Fields))
			for _, f := range expr.Fields {
				fields = append(fields, typeir.StructField{Name: f.Name, FieldType: typeir.Unknown})
			}
			module.StructTypes = append(module.StructTypes, typeir.StructTypeDef{Name: expr.Name, Fields: fields})
		case *ast.EnumDecl:
			module.StructTypes = append(module.StructTypes, typeir.StructTypeDef{Name: expr.Name, Fields: []typeir.StructField{}})
		case *ast.UnionDecl:
			fields := make([]typeir.StructField, 0, len(expr.Fields))
			for _, f := range expr.Fields {
				fields = append(fields, typeir.StructField{Name: f.Name, FieldType: typeir.Unknown})
			}
			module.StructTypes = append(module.StructTypes, typeir.StructTypeDef{Name: expr.Name, Fields: fields})
		case *ast.ImportStmt:
			// Import statements are recorded as instructions during function compilation.
			// At the module level we simply acknowledge them.
		}
	}
	return module
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
		c.emit0(typeir.I32Neq)
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

func isFloatType(t typeir.Typ) bool {
	return typeir.TypToString(t) == "float"
}

func isStringType(t typeir.Typ) bool {
	return typeir.TypToString(t) == "string"
}

func isReturnOpcode(op typeir.Opcode) bool {
	return op == typeir.Return || op == typeir.ReturnVoid
}
