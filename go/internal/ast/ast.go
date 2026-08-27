package ast

// Pos represents a source position.
type Pos struct {
	Line int
	Col  int
}

// Expr is the interface for all AST expression nodes.
type Expr interface {
	exprNode()
	Pos() Pos
}

// TypeExpr represents a type expression.
type TypeExpr struct {
	Name        string
	GenericArgs []TypeExpr
	Pointer     int // number of * levels (Pointer wrapping)
}

// ParamDef defines a function parameter.
type ParamDef struct {
	Name      string
	ParamType TypeExpr
}

// FieldDef defines a struct field.
type FieldDef struct {
	Name      string
	FieldType TypeExpr
}

// ClassFieldDef defines a class field with visibility.
type ClassFieldDef struct {
	Name       string
	FieldType  TypeExpr
	Visibility string
}

// EnumVariant defines an enum variant.
type EnumVariant struct {
	Name  string
	Value *int64
}

// UnionField defines a union field.
type UnionField struct {
	Name      string
	FieldType TypeExpr
}

// MatchArm defines a match arm.
type MatchArm struct {
	Pattern Expr
	Body    []Expr
}

// ElifBranch defines an elif branch.
type ElifBranch struct {
	Condition Expr
	Body      []Expr
}

// MapPair defines a key-value pair for map literals.
type MapPair struct {
	Key   Expr
	Value Expr
}

// --- Expression implementations ---

// IntLiteral implements Expr.
type IntLiteral struct {
	Value int64
	Loc   Pos
}

func (e *IntLiteral) exprNode() {}
func (e *IntLiteral) Pos() Pos  { return e.Loc }

// FloatLiteral implements Expr.
type FloatLiteral struct {
	Value float64
	Loc   Pos
}

func (e *FloatLiteral) exprNode() {}
func (e *FloatLiteral) Pos() Pos  { return e.Loc }

// StringLiteral implements Expr.
type StringLiteral struct {
	Value string
	Loc   Pos
}

func (e *StringLiteral) exprNode() {}
func (e *StringLiteral) Pos() Pos  { return e.Loc }

// BoolLiteral implements Expr.
type BoolLiteral struct {
	Value bool
	Loc   Pos
}

func (e *BoolLiteral) exprNode() {}
func (e *BoolLiteral) Pos() Pos  { return e.Loc }

// NilLiteral implements Expr.
type NilLiteral struct {
	Loc Pos
}

func (e *NilLiteral) exprNode() {}
func (e *NilLiteral) Pos() Pos  { return e.Loc }

// Identifier implements Expr.
type Identifier struct {
	Name string
	Loc  Pos
}

func (e *Identifier) exprNode() {}
func (e *Identifier) Pos() Pos  { return e.Loc }

// ArrayLiteral implements Expr.
type ArrayLiteral struct {
	Elements []Expr
	Loc      Pos
}

func (e *ArrayLiteral) exprNode() {}
func (e *ArrayLiteral) Pos() Pos  { return e.Loc }

// MapLiteral implements Expr.
type MapLiteral struct {
	Pairs []MapPair
	Loc   Pos
}

func (e *MapLiteral) exprNode() {}
func (e *MapLiteral) Pos() Pos  { return e.Loc }

// AddressOf implements Expr (&expr or &mut expr).
type AddressOf struct {
	Expr  Expr
	IsMut bool
	Loc   Pos
}

func (e *AddressOf) exprNode() {}
func (e *AddressOf) Pos() Pos  { return e.Loc }

// Deref implements Expr (*expr).
type Deref struct {
	Expr Expr
	Loc  Pos
}

func (e *Deref) exprNode() {}
func (e *Deref) Pos() Pos  { return e.Loc }

// PointerMember implements Expr (expr->member).
type PointerMember struct {
	Expr   Expr
	Member string
	Loc    Pos
}

func (e *PointerMember) exprNode() {}
func (e *PointerMember) Pos() Pos  { return e.Loc }

// BinaryOp implements Expr.
type BinaryOp struct {
	Op    string
	Left  Expr
	Right Expr
	Loc   Pos
}

func (e *BinaryOp) exprNode() {}
func (e *BinaryOp) Pos() Pos  { return e.Loc }

// UnaryOp implements Expr.
type UnaryOp struct {
	Op   string
	Expr Expr
	Loc  Pos
}

func (e *UnaryOp) exprNode() {}
func (e *UnaryOp) Pos() Pos  { return e.Loc }

// IndexAccess implements Expr (expr[index]).
type IndexAccess struct {
	Target Expr
	Index  Expr
	Loc    Pos
}

func (e *IndexAccess) exprNode() {}
func (e *IndexAccess) Pos() Pos  { return e.Loc }

// PropertyAccess implements Expr (expr.prop).
type PropertyAccess struct {
	Target Expr
	Prop   string
	Loc    Pos
}

func (e *PropertyAccess) exprNode() {}
func (e *PropertyAccess) Pos() Pos  { return e.Loc }

// CallExpr implements Expr (func(args)).
type CallExpr struct {
	Callee Expr
	Args   []Expr
	Loc    Pos
}

func (e *CallExpr) exprNode() {}
func (e *CallExpr) Pos() Pos  { return e.Loc }

// VarDecl implements Expr.
type VarDecl struct {
	Name  string
	Type  *TypeExpr
	Init  Expr
	IsMut bool
	Loc   Pos
}

func (e *VarDecl) exprNode() {}
func (e *VarDecl) Pos() Pos  { return e.Loc }

// Assign implements Expr.
type Assign struct {
	Target Expr
	Op     string
	Value  Expr
	Loc    Pos
}

func (e *Assign) exprNode() {}
func (e *Assign) Pos() Pos  { return e.Loc }

// IfStmt implements Expr.
type IfStmt struct {
	Condition    Expr
	ThenBranch   []Expr
	ElifBranches []ElifBranch
	ElseBranch   []Expr
	Loc          Pos
}

func (e *IfStmt) exprNode() {}
func (e *IfStmt) Pos() Pos  { return e.Loc }

// MatchStmt implements Expr.
type MatchStmt struct {
	Target Expr
	Arms   []MatchArm
	Loc    Pos
}

func (e *MatchStmt) exprNode() {}
func (e *MatchStmt) Pos() Pos  { return e.Loc }

// WhileStmt implements Expr.
type WhileStmt struct {
	Condition Expr
	Body      []Expr
	Loc       Pos
}

func (e *WhileStmt) exprNode() {}
func (e *WhileStmt) Pos() Pos  { return e.Loc }

// ForStmt implements Expr.
type ForStmt struct {
	VarName  string
	Iterable Expr
	Body     []Expr
	Loc      Pos
}

func (e *ForStmt) exprNode() {}
func (e *ForStmt) Pos() Pos  { return e.Loc }

// LoopStmt implements Expr.
type LoopStmt struct {
	Label string
	Body  []Expr
	Loc   Pos
}

func (e *LoopStmt) exprNode() {}
func (e *LoopStmt) Pos() Pos  { return e.Loc }

// BreakStmt implements Expr.
type BreakStmt struct {
	Label string
	Loc   Pos
}

func (e *BreakStmt) exprNode() {}
func (e *BreakStmt) Pos() Pos  { return e.Loc }

// ContinueStmt implements Expr.
type ContinueStmt struct {
	Label string
	Loc   Pos
}

func (e *ContinueStmt) exprNode() {}
func (e *ContinueStmt) Pos() Pos  { return e.Loc }

// DeferStmt implements Expr (defer expr).
type DeferStmt struct {
	Expr Expr
	Loc  Pos
}

func (e *DeferStmt) exprNode() {}
func (e *DeferStmt) Pos() Pos  { return e.Loc }

// ErrDeferStmt implements Expr (errdefer expr) — runs only on error return.
type ErrDeferStmt struct {
	Expr Expr
	Loc  Pos
}

func (e *ErrDeferStmt) exprNode() {}
func (e *ErrDeferStmt) Pos() Pos  { return e.Loc }

// FuncDecl implements Expr.
type FuncDecl struct {
	Name          string
	GenericParams []string
	Params        []ParamDef
	ReturnType    *TypeExpr
	Body          []Expr
	Loc           Pos
}

func (e *FuncDecl) exprNode() {}
func (e *FuncDecl) Pos() Pos  { return e.Loc }

// ReturnStmt implements Expr.
type ReturnStmt struct {
	Value Expr
	Loc   Pos
}

func (e *ReturnStmt) exprNode() {}
func (e *ReturnStmt) Pos() Pos  { return e.Loc }

// StructDecl implements Expr.
type StructDecl struct {
	Name          string
	GenericParams []string
	Fields        []FieldDef
	Methods       []Expr
	Loc           Pos
}

func (e *StructDecl) exprNode() {}
func (e *StructDecl) Pos() Pos  { return e.Loc }

// ClassDecl implements Expr.
type ClassDecl struct {
	Name          string
	GenericParams []string
	Fields        []ClassFieldDef
	Methods       []Expr
	Parent        string
	Interfaces    []string
	Loc           Pos
}

func (e *ClassDecl) exprNode() {}
func (e *ClassDecl) Pos() Pos  { return e.Loc }

// EnumDecl implements Expr.
type EnumDecl struct {
	Name     string
	Variants []EnumVariant
	Loc      Pos
}

func (e *EnumDecl) exprNode() {}
func (e *EnumDecl) Pos() Pos  { return e.Loc }

// UnionDecl implements Expr.
type UnionDecl struct {
	Name   string
	Fields []UnionField
	Loc    Pos
}

func (e *UnionDecl) exprNode() {}
func (e *UnionDecl) Pos() Pos  { return e.Loc }

// NewExpr implements Expr.
type NewExpr struct {
	TypeName   string
	Positional []Expr
	Named      []Expr
	Loc        Pos
}

func (e *NewExpr) exprNode() {}
func (e *NewExpr) Pos() Pos  { return e.Loc }

// MoveExpr implements Expr.
type MoveExpr struct {
	Expr Expr
	Loc  Pos
}

func (e *MoveExpr) exprNode() {}
func (e *MoveExpr) Pos() Pos  { return e.Loc }

// ExprStmt wraps an expression as a statement.
type ExprStmt struct {
	Expr Expr
	Loc  Pos
}

func (e *ExprStmt) exprNode() {}
func (e *ExprStmt) Pos() Pos  { return e.Loc }

// ImportStmt implements Expr.
type ImportStmt struct {
	Path  string
	Alias string
	Dirs  []string
	Loc   Pos
}

func (e *ImportStmt) exprNode() {}
func (e *ImportStmt) Pos() Pos  { return e.Loc }

// ExternDecl implements Expr.
type ExternDecl struct {
	Name          string
	GenericParams []string
	Params        []ParamDef
	ReturnType    *TypeExpr
	Loc           Pos
}

func (e *ExternDecl) exprNode() {}
func (e *ExternDecl) Pos() Pos  { return e.Loc }

// MacroDef implements Expr.
type MacroDef struct {
	Name   string
	Params []string
	Body   []Expr
	Loc    Pos
}

func (e *MacroDef) exprNode() {}
func (e *MacroDef) Pos() Pos  { return e.Loc }

// MacroCall implements Expr.
type MacroCall struct {
	Name string
	Args []Expr
	Loc  Pos
}

func (e *MacroCall) exprNode() {}
func (e *MacroCall) Pos() Pos  { return e.Loc }
