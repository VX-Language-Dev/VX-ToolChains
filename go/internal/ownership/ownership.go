package ownership

import (
	"fmt"

	"github.com/vx-lang/vxc/internal/ast"
)

// ValueState represents the ownership state of a variable.
type ValueState int

const (
	Owned ValueState = iota
	Moved
	Borrowed
	Freed
)

// BorrowKind represents the kind of borrow.
type BorrowKind int

const (
	ImmBorrow BorrowKind = iota
	MutBorrow
)

// BorrowEntry tracks an active borrow.
type BorrowEntry struct {
	Variable string
	Kind     BorrowKind
	Scope    *Scope
}

// Scope tracks variable ownership states at a given lexical scope.
type Scope struct {
	Variables map[string]ValueState
	IsMutable map[string]bool
	IsHeap    map[string]bool
	Borrows   []BorrowEntry
	Parent    *Scope
	types     map[string]string
}

// Checker is the ownership checker state.
type Checker struct {
	Scopes        []*Scope
	ActiveBorrows []BorrowEntry
	Errors        []OwnershipError
	Warnings      []OwnershipError
}

// OwnershipError represents an ownership or borrow violation.
type OwnershipError struct {
	Msg  string
	Line int
	Col  int
}

// NewChecker creates a new ownership checker with a global scope.
func NewChecker() *Checker {
	c := &Checker{}
	c.PushScope()
	return c
}

// PushScope enters a new lexical scope.
func (c *Checker) PushScope() {
	scope := &Scope{
		Variables: make(map[string]ValueState),
		IsMutable: make(map[string]bool),
		IsHeap:    make(map[string]bool),
		Borrows:   []BorrowEntry{},
		types:     make(map[string]string),
	}
	if len(c.Scopes) > 0 {
		scope.Parent = c.CurrentScope()
	}
	c.Scopes = append(c.Scopes, scope)
}

// PopScope exits the current scope, checking for leaked heap variables
// and clearing borrows owned by this scope.
func (c *Checker) PopScope() {
	if len(c.Scopes) == 0 {
		return
	}
	scope := c.Scopes[len(c.Scopes)-1]
	c.Scopes = c.Scopes[:len(c.Scopes)-1]

	for name, state := range scope.Variables {
		if scope.IsHeap[name] && state == Owned {
			c.AddWarning(
				fmt.Sprintf("堆变量 '%s' 在作用域结束时未被显式释放（内存泄漏），请调用 free(%s)", name, name),
				0, 0,
			)
		}
	}

	c.ClearBorrowsForScope(scope)
}

// CurrentScope returns the innermost active scope.
func (c *Checker) CurrentScope() *Scope {
	if len(c.Scopes) == 0 {
		return nil
	}
	return c.Scopes[len(c.Scopes)-1]
}

// Declare introduces a variable into the current scope.
func (c *Checker) Declare(name string, isMut, isHeap bool) {
	scope := c.CurrentScope()
	if scope == nil {
		return
	}
	scope.Variables[name] = Owned
	scope.IsMutable[name] = isMut
	scope.IsHeap[name] = isHeap
}

// setVarType records the type name of a variable in the current scope.
func (c *Checker) setVarType(name, typ string) {
	scope := c.CurrentScope()
	if scope == nil {
		return
	}
	if scope.types == nil {
		scope.types = make(map[string]string)
	}
	scope.types[name] = typ
}

// lookupType searches for a variable's type through the scope chain.
func (c *Checker) lookupType(name string) string {
	for i := len(c.Scopes) - 1; i >= 0; i-- {
		if t, ok := c.Scopes[i].types[name]; ok {
			return t
		}
	}
	return ""
}

// Lookup searches for a variable's state through the scope chain.
func (c *Checker) Lookup(name string) (ValueState, bool) {
	for i := len(c.Scopes) - 1; i >= 0; i-- {
		if state, ok := c.Scopes[i].Variables[name]; ok {
			return state, true
		}
	}
	return Owned, false
}

// UpdateState updates the state of a variable in the current or outer scope.
func (c *Checker) UpdateState(name string, state ValueState) {
	for i := len(c.Scopes) - 1; i >= 0; i-- {
		if _, ok := c.Scopes[i].Variables[name]; ok {
			c.Scopes[i].Variables[name] = state
			return
		}
	}
}

// IsMutable reports whether a variable was declared mutable.
func (c *Checker) IsMutable(name string) bool {
	for i := len(c.Scopes) - 1; i >= 0; i-- {
		if m, ok := c.Scopes[i].IsMutable[name]; ok {
			return m
		}
	}
	return false
}

// IsHeap reports whether a variable is heap-allocated.
func (c *Checker) IsHeap(name string) bool {
	for i := len(c.Scopes) - 1; i >= 0; i-- {
		if h, ok := c.Scopes[i].IsHeap[name]; ok {
			return h
		}
	}
	return false
}

// RecordBorrow registers an active borrow and marks the owner Borrowed.
func (c *Checker) RecordBorrow(variable string, kind BorrowKind) {
	scope := c.CurrentScope()
	entry := BorrowEntry{Variable: variable, Kind: kind, Scope: scope}
	c.ActiveBorrows = append(c.ActiveBorrows, entry)
	if scope != nil {
		scope.Borrows = append(scope.Borrows, entry)
	}
	c.UpdateState(variable, Borrowed)
}

// CheckBorrowConflict reports whether a new borrow violates aliasing XOR mutation.
// If a conflict exists, an error is recorded and true is returned.
func (c *Checker) CheckBorrowConflict(variable string, kind BorrowKind) bool {
	hasExisting := false
	for _, b := range c.ActiveBorrows {
		if b.Variable != variable {
			continue
		}
		hasExisting = true
		if kind == MutBorrow || b.Kind == MutBorrow {
			kindStr := "不可变"
			if kind == MutBorrow {
				kindStr = "可变"
			}
			c.AddError(
				fmt.Sprintf("变量 '%s' 无法创建%s借用（aliasing XOR mutation）", variable, kindStr),
				0, 0,
			)
			return true
		}
	}
	// A mutable borrow requires no existing borrows at all.
	if kind == MutBorrow && hasExisting {
		c.AddError(
			fmt.Sprintf("变量 '%s' 无法创建可变借用（aliasing XOR mutation）", variable),
			0, 0,
		)
		return true
	}
	return false
}

// ClearBorrowsForScope removes all borrows that originated in the given scope.
func (c *Checker) ClearBorrowsForScope(scope *Scope) {
	if scope == nil {
		return
	}
	filtered := c.ActiveBorrows[:0]
	for _, b := range c.ActiveBorrows {
		if b.Scope != scope {
			filtered = append(filtered, b)
		}
	}
	c.ActiveBorrows = filtered
}

// AddError records an ownership error.
func (c *Checker) AddError(msg string, line, col int) {
	c.Errors = append(c.Errors, OwnershipError{Msg: msg, Line: line, Col: col})
}

// AddWarning records an ownership warning.
func (c *Checker) AddWarning(msg string, line, col int) {
	c.Warnings = append(c.Warnings, OwnershipError{Msg: msg, Line: line, Col: col})
}

// IsCopyType returns true for types that are copied rather than moved.
func (c *Checker) IsCopyType(t string) bool {
	switch t {
	case "int", "float", "double", "bool", "string":
		return true
	}
	return false
}

// CheckExpr performs ownership and borrow checking on a single expression.
func (c *Checker) CheckExpr(e ast.Expr, line, col int) {
	if e == nil {
		return
	}
	pos := e.Pos()
	line = pos.Line
	col = pos.Col

	switch e := e.(type) {
	case *ast.VarDecl:
		isHeap := e.Type != nil && e.Type.Pointer > 0
		c.Declare(e.Name, e.IsMut, isHeap)
		if e.Type != nil {
			c.setVarType(e.Name, e.Type.Name)
		}
		c.CheckExpr(e.Init, line, col)

	case *ast.Identifier:
		state, found := c.Lookup(e.Name)
		if !found {
			c.AddError(fmt.Sprintf("未定义的变量 '%s'", e.Name), e.Loc.Line, e.Loc.Col)
			return
		}
		switch state {
		case Moved:
			c.AddError(fmt.Sprintf("变量 '%s' 的所有权已被转移（use-after-move）", e.Name), e.Loc.Line, e.Loc.Col)
		case Freed:
			c.AddError(fmt.Sprintf("变量 '%s' 已被释放（use-after-free/悬垂指针）", e.Name), e.Loc.Line, e.Loc.Col)
		case Borrowed:
			for _, b := range c.ActiveBorrows {
				if b.Variable == e.Name && b.Kind == MutBorrow {
					c.AddError(fmt.Sprintf("变量 '%s' 存在活跃可变借用，无法使用", e.Name), e.Loc.Line, e.Loc.Col)
					return
				}
			}
		}

	case *ast.Assign:
		if id, ok := e.Target.(*ast.Identifier); ok {
			if !c.IsMutable(id.Name) {
				c.AddError(fmt.Sprintf("变量 '%s' 不可变，无法赋值", id.Name), id.Loc.Line, id.Loc.Col)
			}
		}
		c.CheckExpr(e.Target, line, col)
		c.CheckExpr(e.Value, line, col)

	case *ast.MoveExpr:
		if id, ok := e.Expr.(*ast.Identifier); ok {
			state, found := c.Lookup(id.Name)
			if !found {
				c.AddError(fmt.Sprintf("未定义的变量 '%s'", id.Name), id.Loc.Line, id.Loc.Col)
				return
			}
			switch state {
			case Moved:
				c.AddError(fmt.Sprintf("变量 '%s' 所有权已转移，无法再次移动", id.Name), id.Loc.Line, id.Loc.Col)
				return
			case Freed:
				c.AddError(fmt.Sprintf("变量 '%s' 已被释放，无法移动", id.Name), id.Loc.Line, id.Loc.Col)
				return
			case Borrowed:
				for _, b := range c.ActiveBorrows {
					if b.Variable == id.Name {
						c.AddError(fmt.Sprintf("变量 '%s' 存在活跃借用，无法移动", id.Name), id.Loc.Line, id.Loc.Col)
						return
					}
				}
			}
			if c.IsCopyType(c.lookupType(id.Name)) {
				return
			}
			c.UpdateState(id.Name, Moved)
		} else {
			c.CheckExpr(e.Expr, line, col)
		}

	case *ast.AddressOf:
		c.CheckExpr(e.Expr, line, col)
		if id, ok := e.Expr.(*ast.Identifier); ok {
			kind := ImmBorrow
			if e.IsMut {
				kind = MutBorrow
			}
			if !c.CheckBorrowConflict(id.Name, kind) {
				c.RecordBorrow(id.Name, kind)
			}
		}

	case *ast.Deref:
		c.CheckExpr(e.Expr, line, col)

	case *ast.FuncDecl:
		c.PushScope()
		for _, p := range e.Params {
			isHeap := p.ParamType.Pointer > 0
			c.Declare(p.Name, false, isHeap)
			c.setVarType(p.Name, p.ParamType.Name)
		}
		c.checkExprs(e.Body)
		c.PopScope()

	case *ast.CallExpr:
		// The callee is a function name in this context; do not treat it as a
		// variable reference. If it is a method/closure expression, check the
		// underlying expression instead.
		if _, ok := e.Callee.(*ast.Identifier); !ok {
			c.CheckExpr(e.Callee, line, col)
		}
		for _, arg := range e.Args {
			c.CheckExpr(arg, line, col)
			if id, ok := arg.(*ast.Identifier); ok {
				if !c.IsCopyType(c.lookupType(id.Name)) {
					c.UpdateState(id.Name, Moved)
				}
			}
		}

	case *ast.IfStmt:
		c.CheckExpr(e.Condition, line, col)
		c.checkExprs(e.ThenBranch)
		for _, elif := range e.ElifBranches {
			c.CheckExpr(elif.Condition, line, col)
			c.checkExprs(elif.Body)
		}
		c.checkExprs(e.ElseBranch)

	case *ast.WhileStmt:
		c.CheckExpr(e.Condition, line, col)
		c.checkExprs(e.Body)

	case *ast.ForStmt:
		c.PushScope()
		c.Declare(e.VarName, false, false)
		c.CheckExpr(e.Iterable, line, col)
		c.checkExprs(e.Body)
		c.PopScope()

	case *ast.LoopStmt:
		c.checkExprs(e.Body)

	case *ast.MatchStmt:
		c.CheckExpr(e.Target, line, col)
		for _, arm := range e.Arms {
			c.checkExprs(arm.Body)
		}

	case *ast.ReturnStmt:
		c.CheckExpr(e.Value, line, col)

	case *ast.BreakStmt, *ast.ContinueStmt:
		// No ownership checks required.

	case *ast.BinaryOp:
		c.CheckExpr(e.Left, line, col)
		c.CheckExpr(e.Right, line, col)

	case *ast.UnaryOp:
		c.CheckExpr(e.Expr, line, col)

	case *ast.IndexAccess:
		c.CheckExpr(e.Target, line, col)
		c.CheckExpr(e.Index, line, col)

	case *ast.PropertyAccess:
		c.CheckExpr(e.Target, line, col)

	case *ast.PointerMember:
		c.CheckExpr(e.Expr, line, col)

	case *ast.NewExpr:
		for _, arg := range e.Positional {
			c.CheckExpr(arg, line, col)
		}
		for _, arg := range e.Named {
			c.CheckExpr(arg, line, col)
		}

	case *ast.ArrayLiteral:
		for _, el := range e.Elements {
			c.CheckExpr(el, line, col)
		}

	case *ast.MapLiteral:
		for _, pair := range e.Pairs {
			c.CheckExpr(pair.Key, line, col)
			c.CheckExpr(pair.Value, line, col)
		}

	case *ast.ExprStmt:
		c.CheckExpr(e.Expr, line, col)

	case *ast.MacroCall:
		for _, arg := range e.Args {
			c.CheckExpr(arg, line, col)
		}

	case *ast.StructDecl, *ast.ClassDecl, *ast.EnumDecl, *ast.UnionDecl, *ast.ExternDecl, *ast.MacroDef, *ast.ImportStmt:
		// Declarations and imports have no ownership checks at this level.

	default:
		// Literals (IntLiteral, FloatLiteral, StringLiteral, BoolLiteral, NilLiteral)
		// require no ownership checking.
	}
}

// checkExprs checks a list of expressions in order.
func (c *Checker) checkExprs(exprs []ast.Expr) {
	for _, e := range exprs {
		c.CheckExpr(e, 0, 0)
	}
}

// CheckProgram checks an entire program for ownership and borrow violations.
func CheckProgram(program []ast.Expr) *Checker {
	c := NewChecker()

	// First pass: collect top-level variable declarations.
	for _, e := range program {
		if vd, ok := e.(*ast.VarDecl); ok {
			isHeap := vd.Type != nil && vd.Type.Pointer > 0
			c.Declare(vd.Name, vd.IsMut, isHeap)
			if vd.Type != nil {
				c.setVarType(vd.Name, vd.Type.Name)
			}
		}
	}

	// Second pass: perform ownership checks.
	for _, e := range program {
		c.CheckExpr(e, 0, 0)
	}

	c.PopScope()
	return c
}
