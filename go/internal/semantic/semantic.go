package semantic

import (
	"fmt"
	"strings"

	"github.com/vx-lang/vxc/internal/ast"
	"github.com/vx-lang/vxc/internal/typeir"
)

// Pos represents a source position.
type Pos struct {
	Line, Col int
}

// SymEntry records information about a variable symbol.
type SymEntry struct {
	Name   string
	Type   typeir.Typ
	IsMut  bool
	IsHeap bool
}

// Scope is a single lexical scope.
type Scope struct {
	Names  map[string]SymEntry
	Parent *Scope
}

// FuncEntry records information about a function.
type FuncEntry struct {
	Name       string
	Params     []ParamEntry
	ReturnType typeir.Typ
	IsExtern   bool
}

// ParamEntry records a function parameter.
type ParamEntry struct {
	Name string
	Type typeir.Typ
}

// StructEntry records information about a struct type.
type StructEntry struct {
	Name   string
	Fields []FieldEntry
}

// FieldEntry records a struct/class field.
type FieldEntry struct {
	Name      string
	FieldType typeir.Typ
}

// ClassEntry records information about a class type.
type ClassEntry struct {
	Name       string
	Fields     []FieldEntry
	Parent     string
	Interfaces []string
}

// EnumEntry records information about an enum type.
type EnumEntry struct {
	Name     string
	Variants []VariantEntry
}

// VariantEntry records a single enum variant.
type VariantEntry struct {
	Name  string
	Value int64
}

// SymbolTable holds all symbols collected during analysis.
type SymbolTable struct {
	Scopes    []*Scope
	Functions map[string]FuncEntry
	Structs   map[string]StructEntry
	Classes   map[string]ClassEntry
	Enums     map[string]EnumEntry
	Errors    []SemanticError
}

// SemanticError records an error message and its source position.
type SemanticError struct {
	Msg string
	Pos Pos
}

// PushScope creates a new child scope.
func (st *SymbolTable) PushScope() {
	parent := (*Scope)(nil)
	if len(st.Scopes) > 0 {
		parent = st.Scopes[len(st.Scopes)-1]
	}
	st.Scopes = append(st.Scopes, &Scope{
		Names:  make(map[string]SymEntry),
		Parent: parent,
	})
}

// PopScope removes the current scope.
func (st *SymbolTable) PopScope() {
	if len(st.Scopes) == 0 {
		return
	}
	st.Scopes = st.Scopes[:len(st.Scopes)-1]
}

// LookupSym searches for a symbol in the current scope and its ancestors.
func (st *SymbolTable) LookupSym(name string) (SymEntry, bool) {
	for i := len(st.Scopes) - 1; i >= 0; i-- {
		if entry, ok := st.Scopes[i].Names[name]; ok {
			return entry, true
		}
	}
	return SymEntry{}, false
}

// DeclareSym adds a symbol to the current scope.
func (st *SymbolTable) DeclareSym(name string, entry SymEntry) {
	if len(st.Scopes) == 0 {
		st.PushScope()
	}
	st.Scopes[len(st.Scopes)-1].Names[name] = entry
}

// AddError records a semantic error.
func (st *SymbolTable) AddError(msg string, pos Pos) {
	st.Errors = append(st.Errors, SemanticError{Msg: msg, Pos: pos})
}

// AnalyzeProgram performs semantic analysis on a complete program.
func AnalyzeProgram(program []ast.Expr) *SymbolTable {
	st := &SymbolTable{
		Scopes:    []*Scope{{Names: make(map[string]SymEntry)}},
		Functions: make(map[string]FuncEntry),
		Structs:   make(map[string]StructEntry),
		Classes:   make(map[string]ClassEntry),
		Enums:     make(map[string]EnumEntry),
	}

	collectDecls(st, program)
	checkFuncBody(st, program)
	return st
}

// ==================== Type Resolution ====================

func resolveTypeExpr(st *SymbolTable, te ast.TypeExpr) typeir.Typ {
	var base typeir.Typ
	name := te.Name

	if strings.HasSuffix(name, "[]") {
		innerName := strings.TrimSuffix(name, "[]")
		inner := ast.TypeExpr{Name: innerName}
		base = typeir.ArrayTyp{Elem: resolveTypeExpr(st, inner)}
	} else {
		switch name {
		case "void":
			base = typeir.Void
		case "int":
			base = typeir.Int
		case "float", "double":
			base = typeir.Float
		case "bool":
			base = typeir.Bool
		case "string":
			base = typeir.String
		default:
			if len(te.GenericArgs) > 0 {
				args := make([]typeir.Typ, len(te.GenericArgs))
				for i, a := range te.GenericArgs {
					args[i] = resolveTypeExpr(st, a)
				}
				base = typeir.GenericTyp{Name: name, Args: args}
			} else if se, ok := st.Structs[name]; ok {
				base = structEntryToType(se)
			} else if ce, ok := st.Classes[name]; ok {
				base = classEntryToType(ce)
			} else {
				base = typeir.Unknown
			}
		}
	}

	for i := 0; i < te.Pointer; i++ {
		base = typeir.PointerTyp{To: base}
	}
	return base
}

func structEntryToType(se StructEntry) typeir.Typ {
	fields := make([]typeir.StructField, len(se.Fields))
	for i, f := range se.Fields {
		fields[i] = typeir.StructField{Name: f.Name, FieldType: f.FieldType}
	}
	return typeir.StructTyp{Name: se.Name, Fields: fields}
}

func classEntryToType(ce ClassEntry) typeir.Typ {
	fields := make([]typeir.StructField, len(ce.Fields))
	for i, f := range ce.Fields {
		fields[i] = typeir.StructField{Name: f.Name, FieldType: f.FieldType}
	}
	return typeir.StructTyp{Name: ce.Name, Fields: fields}
}

// ==================== Expression Type Inference ====================

func inferExprType(st *SymbolTable, e ast.Expr) typeir.Typ {
	switch x := e.(type) {
	case *ast.IntLiteral:
		return typeir.Int
	case *ast.FloatLiteral:
		return typeir.Float
	case *ast.StringLiteral:
		return typeir.String
	case *ast.BoolLiteral:
		return typeir.Bool
	case *ast.NilLiteral:
		return typeir.Unknown
	case *ast.Identifier:
		if entry, ok := st.LookupSym(x.Name); ok {
			return entry.Type
		}
		return typeir.Unknown
	case *ast.ArrayLiteral:
		var elem typeir.Typ
		if len(x.Elements) > 0 {
			elem = inferExprType(st, x.Elements[0])
		}
		return typeir.ArrayTyp{Elem: elem}
	case *ast.MapLiteral:
		var key, value typeir.Typ
		if len(x.Pairs) > 0 {
			key = inferExprType(st, x.Pairs[0].Key)
			value = inferExprType(st, x.Pairs[0].Value)
		}
		return typeir.MapTyp{Key: key, Value: value}
	case *ast.BinaryOp:
		return inferBinaryOpType(st, x)
	case *ast.UnaryOp:
		return inferUnaryOpType(st, x)
	case *ast.CallExpr:
		if ident, ok := x.Callee.(*ast.Identifier); ok {
			if fn, ok := st.Functions[ident.Name]; ok {
				return fn.ReturnType
			}
		}
		return typeir.Unknown
	case *ast.IndexAccess:
		targetType := inferExprType(st, x.Target)
		if at, ok := targetType.(typeir.ArrayTyp); ok {
			if at.Elem == nil {
				return typeir.Unknown
			}
			return at.Elem
		}
		return typeir.Unknown
	case *ast.PropertyAccess:
		targetType := inferExprType(st, x.Target)
		stt, ok := targetType.(typeir.StructTyp)
		if !ok {
			return typeir.Unknown
		}
		for _, f := range stt.Fields {
			if f.Name == x.Prop {
				return f.FieldType
			}
		}
		return typeir.Unknown
	default:
		return typeir.Unknown
	}
}

func inferBinaryOpType(st *SymbolTable, x *ast.BinaryOp) typeir.Typ {
	lt := inferExprType(st, x.Left)
	rt := inferExprType(st, x.Right)

	switch x.Op {
	case "+":
		if typeir.TypesEqual(lt, rt) {
			switch lt {
			case typeir.Int, typeir.Float, typeir.String:
				return lt
			}
		}
		return typeir.Unknown
	case "-", "*", "/", "%":
		if typeir.TypesEqual(lt, rt) && typeir.IsNumeric(lt) {
			return lt
		}
		return typeir.Unknown
	case "==", "!=", "<", ">", "<=", ">=":
		return typeir.Bool
	case "&&", "||":
		return typeir.Bool
	default:
		return typeir.Unknown
	}
}

func inferUnaryOpType(st *SymbolTable, x *ast.UnaryOp) typeir.Typ {
	t := inferExprType(st, x.Expr)
	switch x.Op {
	case "-":
		if typeir.IsNumeric(t) {
			return t
		}
		return typeir.Unknown
	case "!":
		return typeir.Bool
	default:
		return typeir.Unknown
	}
}

// ==================== Declaration Collection ====================

func collectDecls(st *SymbolTable, program []ast.Expr) {
	for _, e := range program {
		switch d := e.(type) {
		case *ast.FuncDecl:
			st.Functions[d.Name] = FuncEntry{
				Name:       d.Name,
				Params:     paramsToEntries(st, d.Params),
				ReturnType: resolveOptionalType(st, d.ReturnType),
				IsExtern:   false,
			}
		case *ast.ExternDecl:
			st.Functions[d.Name] = FuncEntry{
				Name:       d.Name,
				Params:     paramsToEntries(st, d.Params),
				ReturnType: resolveOptionalType(st, d.ReturnType),
				IsExtern:   true,
			}
		case *ast.StructDecl:
			st.Structs[d.Name] = StructEntry{
				Name:   d.Name,
				Fields: fieldsToEntries(st, d.Fields),
			}
		case *ast.ClassDecl:
			st.Classes[d.Name] = ClassEntry{
				Name:       d.Name,
				Fields:     classFieldsToEntries(st, d.Fields),
				Parent:     d.Parent,
				Interfaces: d.Interfaces,
			}
		case *ast.EnumDecl:
			variants := make([]VariantEntry, len(d.Variants))
			for i, v := range d.Variants {
				val := int64(0)
				if v.Value != nil {
					val = *v.Value
				}
				variants[i] = VariantEntry{Name: v.Name, Value: val}
			}
			st.Enums[d.Name] = EnumEntry{Name: d.Name, Variants: variants}
		case *ast.ImportStmt:
			// Import statements are recorded at the build/package level.
			// The semantic analyzer simply acknowledges them as valid top-level declarations.
		}
	}
}

func paramsToEntries(st *SymbolTable, params []ast.ParamDef) []ParamEntry {
	entries := make([]ParamEntry, len(params))
	for i, p := range params {
		entries[i] = ParamEntry{Name: p.Name, Type: resolveTypeExpr(st, p.ParamType)}
	}
	return entries
}

func fieldsToEntries(st *SymbolTable, fields []ast.FieldDef) []FieldEntry {
	entries := make([]FieldEntry, len(fields))
	for i, f := range fields {
		entries[i] = FieldEntry{Name: f.Name, FieldType: resolveTypeExpr(st, f.FieldType)}
	}
	return entries
}

func classFieldsToEntries(st *SymbolTable, fields []ast.ClassFieldDef) []FieldEntry {
	entries := make([]FieldEntry, len(fields))
	for i, f := range fields {
		entries[i] = FieldEntry{Name: f.Name, FieldType: resolveTypeExpr(st, f.FieldType)}
	}
	return entries
}

func resolveOptionalType(st *SymbolTable, te *ast.TypeExpr) typeir.Typ {
	if te == nil {
		return typeir.Void
	}
	return resolveTypeExpr(st, *te)
}

// ==================== Function Body Checking ====================

func checkFuncBody(st *SymbolTable, program []ast.Expr) {
	for _, e := range program {
		if f, ok := e.(*ast.FuncDecl); ok {
			st.PushScope()
			for _, p := range f.Params {
				st.DeclareSym(p.Name, SymEntry{
					Name:   p.Name,
					Type:   resolveTypeExpr(st, p.ParamType),
					IsMut:  false,
					IsHeap: false,
				})
			}
			for _, stmt := range f.Body {
				checkStmt(st, stmt)
			}
			st.PopScope()
		}
	}
}

func checkStmt(st *SymbolTable, s ast.Expr) {
	switch x := s.(type) {
	case *ast.VarDecl:
		initType := inferExprType(st, x.Init)
		var declType typeir.Typ
		if x.Type != nil {
			declType = resolveTypeExpr(st, *x.Type)
		} else {
			declType = initType
		}
		st.DeclareSym(x.Name, SymEntry{
			Name:   x.Name,
			Type:   declType,
			IsMut:  x.IsMut,
			IsHeap: false,
		})
	case *ast.Assign:
		targetType := inferExprType(st, x.Target)
		valueType := inferExprType(st, x.Value)
		if !typeir.TypesEqual(targetType, valueType) &&
			targetType != typeir.Unknown && valueType != typeir.Unknown {
			st.AddError(
				fmt.Sprintf("Type mismatch: cannot assign %s to %s",
					typeir.TypToString(valueType), typeir.TypToString(targetType)),
				Pos{Line: x.Pos().Line, Col: x.Pos().Col},
			)
		}
	case *ast.IfStmt:
		for _, stmt := range x.ThenBranch {
			checkStmt(st, stmt)
		}
		for _, eb := range x.ElifBranches {
			for _, stmt := range eb.Body {
				checkStmt(st, stmt)
			}
		}
		for _, stmt := range x.ElseBranch {
			checkStmt(st, stmt)
		}
	case *ast.WhileStmt:
		for _, stmt := range x.Body {
			checkStmt(st, stmt)
		}
	case *ast.ForStmt:
		for _, stmt := range x.Body {
			checkStmt(st, stmt)
		}
	case *ast.LoopStmt:
		for _, stmt := range x.Body {
			checkStmt(st, stmt)
		}
	case *ast.MatchStmt:
		for _, arm := range x.Arms {
			for _, stmt := range arm.Body {
				checkStmt(st, stmt)
			}
		}
	case *ast.ExprStmt:
		inferExprType(st, x.Expr)
	case *ast.FuncDecl:
		checkFuncBody(st, []ast.Expr{x})
	case *ast.ReturnStmt:
		// Deliberately left empty: return type checking is not implemented yet.
	case *ast.ImportStmt:
		// ImportStmt is valid only at the top level; silently accept if encountered inside a function.
	}
}
