package macros

import (
	"fmt"

	"github.com/vx-lang/vxc/internal/ast"
	"github.com/vx-lang/vxc/internal/token"
)

// Macro 宏定义结构体
type Macro struct {
	Name   string
	Params []string
	Body   []ast.Expr
	Line   int
	Col    int
}

// cachedMacro 带缓存的宏对象（简化缓存实现）
type cachedMacro struct {
	macroObj       Macro
	expansionCache map[string][]ast.Expr
}

// MacroRegistry 宏注册表，管理所有宏的定义和展开
type MacroRegistry struct {
	macros        map[string]*cachedMacro
	ExpandCount   uint64
	CacheHitCount uint64
}

// macroError 包装 token.VXError 以适配 Go 的 error 接口
type macroError struct {
	err token.VXError
}

func (e *macroError) Error() string {
	return e.err.String()
}

// UnwrapVXError 返回底层的结构化错误（可选辅助）
func (e *macroError) UnwrapVXError() token.VXError {
	return e.err
}

func newMacroError(msg string, line, col int, kind token.VXErrorKind) error {
	return &macroError{err: token.NewVXError(msg, line, col).WithKind(kind)}
}

// NewMacroRegistry 创建新的宏注册表
func NewMacroRegistry() *MacroRegistry {
	return &MacroRegistry{
		macros:        make(map[string]*cachedMacro),
		ExpandCount:   0,
		CacheHitCount: 0,
	}
}

// RegisterMacro 注册宏定义
func (r *MacroRegistry) RegisterMacro(mac Macro) error {
	if _, exists := r.macros[mac.Name]; exists {
		return newMacroError(fmt.Sprintf("宏 '%s' 已经存在", mac.Name), mac.Line, mac.Col, token.CompileKind)
	}

	r.macros[mac.Name] = &cachedMacro{
		macroObj:       mac,
		expansionCache: make(map[string][]ast.Expr),
	}
	return nil
}

// HasMacro 检查宏是否存在
func (r *MacroRegistry) HasMacro(name string) bool {
	_, exists := r.macros[name]
	return exists
}

// ExpandMacro 展开宏调用
func (r *MacroRegistry) ExpandMacro(name string, args []ast.Expr) ([]ast.Expr, error) {
	r.ExpandCount++

	cached, exists := r.macros[name]
	if !exists {
		return nil, newMacroError(fmt.Sprintf("未找到宏: '%s'", name), 0, 0, token.CompileKind)
	}

	// 验证参数数量
	if len(args) != len(cached.macroObj.Params) {
		mac := cached.macroObj
		return nil, newMacroError(
			fmt.Sprintf("宏 '%s' 期望 %d 个参数，但提供了 %d 个", name, len(mac.Params), len(args)),
			mac.Line, mac.Col, token.CompileKind,
		)
	}

	// 简化缓存：跳过复杂缓存逻辑，始终展开
	// （保留 expansion_cache 字段用于 API 兼容性）

	// 创建参数绑定映射：param_name -> ast.Expr（借用 args）
	bindings := make(map[string]ast.Expr)
	for i, param := range cached.macroObj.Params {
		bindings[param] = args[i]
	}

	// 展开宏体，替换参数
	result := make([]ast.Expr, 0, len(cached.macroObj.Body))
	for _, expr := range cached.macroObj.Body {
		substituted, err := substituteParams(expr, bindings)
		if err != nil {
			return nil, err
		}
		result = append(result, substituted)
	}

	// 简化：不缓存结果
	return result, nil
}

// Stats 宏展开统计信息
type Stats struct {
	ExpandCount   uint64
	CacheHitCount uint64
	HitRate       float64
}

// GetStats 获取统计信息
func (r *MacroRegistry) GetStats() Stats {
	var hitRate float64
	if r.ExpandCount > 0 {
		hitRate = (float64(r.CacheHitCount) / float64(r.ExpandCount)) * 100.0
	}
	return Stats{
		ExpandCount:   r.ExpandCount,
		CacheHitCount: r.CacheHitCount,
		HitRate:       hitRate,
	}
}

// ExpandMacros 主入口点：对表达式列表先注册所有宏定义，再展开顶层宏调用
func ExpandMacros(stmts []ast.Expr) ([]ast.Expr, error) {
	registry := NewMacroRegistry()

	// 第一阶段：注册所有宏定义
	for _, stmt := range stmts {
		if macroDef, ok := stmt.(*ast.MacroDef); ok {
			macro := Macro{
				Name:   macroDef.Name,
				Params: macroDef.Params,
				Body:   macroDef.Body,
				Line:   macroDef.Loc.Line,
				Col:    macroDef.Loc.Col,
			}
			if err := registry.RegisterMacro(macro); err != nil {
				return nil, err
			}
		}
	}

	// 第二阶段：展开宏调用
	return expandTopLevel(registry, stmts)
}

func expandTopLevel(registry *MacroRegistry, exprs []ast.Expr) ([]ast.Expr, error) {
	result := make([]ast.Expr, 0, len(exprs))
	for _, expr := range exprs {
		if macroCall, ok := expr.(*ast.MacroCall); ok {
			expanded, err := registry.ExpandMacro(macroCall.Name, macroCall.Args)
			if err != nil {
				return nil, err
			}
			result = append(result, expanded...)
		} else {
			result = append(result, expr)
		}
	}
	return result, nil
}

// ==================== 辅助函数 ====================

// cloneExpr 递归克隆整个 Expr 树
func cloneExpr(expr ast.Expr) ast.Expr {
	switch v := expr.(type) {
	case *ast.IntLiteral:
		return &ast.IntLiteral{Value: v.Value, Loc: v.Loc}
	case *ast.FloatLiteral:
		return &ast.FloatLiteral{Value: v.Value, Loc: v.Loc}
	case *ast.StringLiteral:
		return &ast.StringLiteral{Value: v.Value, Loc: v.Loc}
	case *ast.BoolLiteral:
		return &ast.BoolLiteral{Value: v.Value, Loc: v.Loc}
	case *ast.NilLiteral:
		return &ast.NilLiteral{Loc: v.Loc}
	case *ast.Identifier:
		return &ast.Identifier{Name: v.Name, Loc: v.Loc}
	case *ast.ArrayLiteral:
		return &ast.ArrayLiteral{Elements: cloneExprList(v.Elements), Loc: v.Loc}
	case *ast.MapLiteral:
		pairs := make([]ast.MapPair, len(v.Pairs))
		for i, pair := range v.Pairs {
			pairs[i] = ast.MapPair{Key: cloneExpr(pair.Key), Value: cloneExpr(pair.Value)}
		}
		return &ast.MapLiteral{Pairs: pairs, Loc: v.Loc}
	case *ast.AddressOf:
		return &ast.AddressOf{Expr: cloneExpr(v.Expr), IsMut: v.IsMut, Loc: v.Loc}
	case *ast.Deref:
		return &ast.Deref{Expr: cloneExpr(v.Expr), Loc: v.Loc}
	case *ast.PointerMember:
		return &ast.PointerMember{Expr: cloneExpr(v.Expr), Member: v.Member, Loc: v.Loc}
	case *ast.BinaryOp:
		return &ast.BinaryOp{Op: v.Op, Left: cloneExpr(v.Left), Right: cloneExpr(v.Right), Loc: v.Loc}
	case *ast.UnaryOp:
		return &ast.UnaryOp{Op: v.Op, Expr: cloneExpr(v.Expr), Loc: v.Loc}
	case *ast.VarDecl:
		return &ast.VarDecl{
			Name:   v.Name,
			Type:   cloneTypeExprPtr(v.Type),
			Init:   cloneExpr(v.Init),
			IsMut:  v.IsMut,
			Loc:    v.Loc,
		}
	case *ast.Assign:
		return &ast.Assign{Target: cloneExpr(v.Target), Op: v.Op, Value: cloneExpr(v.Value), Loc: v.Loc}
	case *ast.IndexAccess:
		return &ast.IndexAccess{Target: cloneExpr(v.Target), Index: cloneExpr(v.Index), Loc: v.Loc}
	case *ast.PropertyAccess:
		return &ast.PropertyAccess{Target: cloneExpr(v.Target), Prop: v.Prop, Loc: v.Loc}
	case *ast.IfStmt:
		elifBranches := make([]ast.ElifBranch, len(v.ElifBranches))
		for i, elif := range v.ElifBranches {
			elifBranches[i] = ast.ElifBranch{
				Condition: cloneExpr(elif.Condition),
				Body:      cloneExprList(elif.Body),
			}
		}
		return &ast.IfStmt{
			Condition:    cloneExpr(v.Condition),
			ThenBranch:   cloneExprList(v.ThenBranch),
			ElifBranches: elifBranches,
			ElseBranch:   cloneExprList(v.ElseBranch),
			Loc:          v.Loc,
		}
	case *ast.MatchStmt:
		arms := make([]ast.MatchArm, len(v.Arms))
		for i, arm := range v.Arms {
			arms[i] = ast.MatchArm{Pattern: cloneExpr(arm.Pattern), Body: cloneExprList(arm.Body)}
		}
		return &ast.MatchStmt{Target: cloneExpr(v.Target), Arms: arms, Loc: v.Loc}
	case *ast.WhileStmt:
		return &ast.WhileStmt{Condition: cloneExpr(v.Condition), Body: cloneExprList(v.Body), Loc: v.Loc}
	case *ast.ForStmt:
		return &ast.ForStmt{VarName: v.VarName, Iterable: cloneExpr(v.Iterable), Body: cloneExprList(v.Body), Loc: v.Loc}
	case *ast.LoopStmt:
		return &ast.LoopStmt{Label: v.Label, Body: cloneExprList(v.Body), Loc: v.Loc}
	case *ast.BreakStmt:
		return &ast.BreakStmt{Label: v.Label, Loc: v.Loc}
	case *ast.ContinueStmt:
		return &ast.ContinueStmt{Label: v.Label, Loc: v.Loc}
	case *ast.FuncDecl:
		return &ast.FuncDecl{
			Name:          v.Name,
			GenericParams: cloneStringList(v.GenericParams),
			Params:        cloneParamDefs(v.Params),
			ReturnType:    cloneTypeExprPtr(v.ReturnType),
			Body:          cloneExprList(v.Body),
			Loc:           v.Loc,
		}
	case *ast.ReturnStmt:
		var value ast.Expr
		if v.Value != nil {
			value = cloneExpr(v.Value)
		}
		return &ast.ReturnStmt{Value: value, Loc: v.Loc}
	case *ast.CallExpr:
		return &ast.CallExpr{Callee: cloneExpr(v.Callee), Args: cloneExprList(v.Args), Loc: v.Loc}
	case *ast.StructDecl:
		return &ast.StructDecl{
			Name:          v.Name,
			GenericParams: cloneStringList(v.GenericParams),
			Fields:        cloneFieldDefs(v.Fields),
			Methods:       cloneExprList(v.Methods),
			Loc:           v.Loc,
		}
	case *ast.ClassDecl:
		return &ast.ClassDecl{
			Name:          v.Name,
			GenericParams: cloneStringList(v.GenericParams),
			Fields:        cloneClassFieldDefs(v.Fields),
			Methods:       cloneExprList(v.Methods),
			Parent:        v.Parent,
			Interfaces:    cloneStringList(v.Interfaces),
			Loc:           v.Loc,
		}
	case *ast.EnumDecl:
		variants := make([]ast.EnumVariant, len(v.Variants))
		for i, variant := range v.Variants {
			variants[i] = ast.EnumVariant{Name: variant.Name}
			if variant.Value != nil {
				val := *variant.Value
				variants[i].Value = &val
			}
		}
		return &ast.EnumDecl{Name: v.Name, Variants: variants, Loc: v.Loc}
	case *ast.UnionDecl:
		return &ast.UnionDecl{Name: v.Name, Fields: cloneUnionFields(v.Fields), Loc: v.Loc}
	case *ast.NewExpr:
		return &ast.NewExpr{
			TypeName:   v.TypeName,
			Positional: cloneExprList(v.Positional),
			Named:      cloneExprList(v.Named),
			Loc:        v.Loc,
		}
	case *ast.MoveExpr:
		return &ast.MoveExpr{Expr: cloneExpr(v.Expr), Loc: v.Loc}
	case *ast.ExprStmt:
		return &ast.ExprStmt{Expr: cloneExpr(v.Expr), Loc: v.Loc}
	case *ast.ImportStmt:
		return &ast.ImportStmt{Path: v.Path, Alias: v.Alias, Dirs: cloneStringList(v.Dirs), Loc: v.Loc}
	case *ast.ExternDecl:
		return &ast.ExternDecl{
			Name:          v.Name,
			GenericParams: cloneStringList(v.GenericParams),
			Params:        cloneParamDefs(v.Params),
			ReturnType:    cloneTypeExprPtr(v.ReturnType),
			Loc:           v.Loc,
		}
	case *ast.MacroDef:
		return &ast.MacroDef{Name: v.Name, Params: cloneStringList(v.Params), Body: cloneExprList(v.Body), Loc: v.Loc}
	case *ast.MacroCall:
		return &ast.MacroCall{Name: v.Name, Args: cloneExprList(v.Args), Loc: v.Loc}
	default:
		// 对于未识别的表达式类型，直接返回原值
		return expr
	}
}

// substituteParams 递归替换表达式中的参数引用
func substituteParams(expr ast.Expr, bindings map[string]ast.Expr) (ast.Expr, error) {
	switch v := expr.(type) {
	// 替换标识符（参数引用）
	case *ast.Identifier:
		if bound, ok := bindings[v.Name]; ok {
			return cloneExpr(bound), nil
		}
		// 非参数标识符，原样克隆
		return cloneExpr(expr), nil

	// 递归处理二元操作
	case *ast.BinaryOp:
		left, err := substituteParams(v.Left, bindings)
		if err != nil {
			return nil, err
		}
		right, err := substituteParams(v.Right, bindings)
		if err != nil {
			return nil, err
		}
		return &ast.BinaryOp{Op: v.Op, Left: left, Right: right, Loc: v.Loc}, nil

	// 递归处理一元操作
	case *ast.UnaryOp:
		e, err := substituteParams(v.Expr, bindings)
		if err != nil {
			return nil, err
		}
		return &ast.UnaryOp{Op: v.Op, Expr: e, Loc: v.Loc}, nil

	// 递归处理函数调用
	case *ast.CallExpr:
		callee, err := substituteParams(v.Callee, bindings)
		if err != nil {
			return nil, err
		}
		args := make([]ast.Expr, len(v.Args))
		for i, arg := range v.Args {
			sub, err := substituteParams(arg, bindings)
			if err != nil {
				return nil, err
			}
			args[i] = sub
		}
		return &ast.CallExpr{Callee: callee, Args: args, Loc: v.Loc}, nil

	// 递归处理变量声明（不替换类型标注）
	case *ast.VarDecl:
		init, err := substituteParams(v.Init, bindings)
		if err != nil {
			return nil, err
		}
		return &ast.VarDecl{
			Name:  v.Name,
			Type:  cloneTypeExprPtr(v.Type),
			Init:  init,
			IsMut: v.IsMut,
			Loc:   v.Loc,
		}, nil

	// 递归处理赋值语句
	case *ast.Assign:
		target, err := substituteParams(v.Target, bindings)
		if err != nil {
			return nil, err
		}
		value, err := substituteParams(v.Value, bindings)
		if err != nil {
			return nil, err
		}
		return &ast.Assign{Target: target, Op: v.Op, Value: value, Loc: v.Loc}, nil

	// 递归处理条件语句
	case *ast.IfStmt:
		condition, err := substituteParams(v.Condition, bindings)
		if err != nil {
			return nil, err
		}
		thenBranch := make([]ast.Expr, len(v.ThenBranch))
		for i, stmt := range v.ThenBranch {
			sub, err := substituteParams(stmt, bindings)
			if err != nil {
				return nil, err
			}
			thenBranch[i] = sub
		}
		elifBranches := make([]ast.ElifBranch, len(v.ElifBranches))
		for i, elif := range v.ElifBranches {
			cond, err := substituteParams(elif.Condition, bindings)
			if err != nil {
				return nil, err
			}
			body := make([]ast.Expr, len(elif.Body))
			for j, stmt := range elif.Body {
				sub, err := substituteParams(stmt, bindings)
				if err != nil {
					return nil, err
				}
				body[j] = sub
			}
			elifBranches[i] = ast.ElifBranch{Condition: cond, Body: body}
		}
		elseBranch := make([]ast.Expr, len(v.ElseBranch))
		for i, stmt := range v.ElseBranch {
			sub, err := substituteParams(stmt, bindings)
			if err != nil {
				return nil, err
			}
			elseBranch[i] = sub
		}
		return &ast.IfStmt{
			Condition:    condition,
			ThenBranch:   thenBranch,
			ElifBranches: elifBranches,
			ElseBranch:   elseBranch,
			Loc:          v.Loc,
		}, nil

	// 递归处理 while 循环
	case *ast.WhileStmt:
		condition, err := substituteParams(v.Condition, bindings)
		if err != nil {
			return nil, err
		}
		body := make([]ast.Expr, len(v.Body))
		for i, stmt := range v.Body {
			sub, err := substituteParams(stmt, bindings)
			if err != nil {
				return nil, err
			}
			body[i] = sub
		}
		return &ast.WhileStmt{Condition: condition, Body: body, Loc: v.Loc}, nil

	// 递归处理 for 循环（不替换循环变量名）
	case *ast.ForStmt:
		iterable, err := substituteParams(v.Iterable, bindings)
		if err != nil {
			return nil, err
		}
		body := make([]ast.Expr, len(v.Body))
		for i, stmt := range v.Body {
			sub, err := substituteParams(stmt, bindings)
			if err != nil {
				return nil, err
			}
			body[i] = sub
		}
		return &ast.ForStmt{VarName: v.VarName, Iterable: iterable, Body: body, Loc: v.Loc}, nil

	// 递归处理 match 语句
	case *ast.MatchStmt:
		target, err := substituteParams(v.Target, bindings)
		if err != nil {
			return nil, err
		}
		arms := make([]ast.MatchArm, len(v.Arms))
		for i, arm := range v.Arms {
			pattern, err := substituteParams(arm.Pattern, bindings)
			if err != nil {
				return nil, err
			}
			body := make([]ast.Expr, len(arm.Body))
			for j, stmt := range arm.Body {
				sub, err := substituteParams(stmt, bindings)
				if err != nil {
					return nil, err
				}
				body[j] = sub
			}
			arms[i] = ast.MatchArm{Pattern: pattern, Body: body}
		}
		return &ast.MatchStmt{Target: target, Arms: arms, Loc: v.Loc}, nil

	// 递归处理函数定义（不替换泛型参数、函数参数和返回类型）
	case *ast.FuncDecl:
		params := make([]ast.ParamDef, len(v.Params))
		for i, p := range v.Params {
			params[i] = ast.ParamDef{
				Name:      p.Name,
				ParamType: *cloneTypeExpr(&p.ParamType),
			}
		}
		body := make([]ast.Expr, len(v.Body))
		for i, stmt := range v.Body {
			sub, err := substituteParams(stmt, bindings)
			if err != nil {
				return nil, err
			}
			body[i] = sub
		}
		return &ast.FuncDecl{
			Name:          v.Name,
			GenericParams: cloneStringList(v.GenericParams),
			Params:        params,
			ReturnType:    cloneTypeExprPtr(v.ReturnType),
			Body:          body,
			Loc:           v.Loc,
		}, nil

	// 递归处理返回语句
	case *ast.ReturnStmt:
		var value ast.Expr
		if v.Value != nil {
			var err error
			value, err = substituteParams(v.Value, bindings)
			if err != nil {
				return nil, err
			}
		}
		return &ast.ReturnStmt{Value: value, Loc: v.Loc}, nil

	// 递归处理数组字面量
	case *ast.ArrayLiteral:
		elements := make([]ast.Expr, len(v.Elements))
		for i, elem := range v.Elements {
			sub, err := substituteParams(elem, bindings)
			if err != nil {
				return nil, err
			}
			elements[i] = sub
		}
		return &ast.ArrayLiteral{Elements: elements, Loc: v.Loc}, nil

	// 递归处理索引访问
	case *ast.IndexAccess:
		target, err := substituteParams(v.Target, bindings)
		if err != nil {
			return nil, err
		}
		index, err := substituteParams(v.Index, bindings)
		if err != nil {
			return nil, err
		}
		return &ast.IndexAccess{Target: target, Index: index, Loc: v.Loc}, nil

	// 递归处理属性访问（不替换属性名）
	case *ast.PropertyAccess:
		target, err := substituteParams(v.Target, bindings)
		if err != nil {
			return nil, err
		}
		return &ast.PropertyAccess{Target: target, Prop: v.Prop, Loc: v.Loc}, nil

	// 其他表达式类型原样克隆（字面量、声明等）
	default:
		return cloneExpr(expr), nil
	}
}

func cloneExprList(exprs []ast.Expr) []ast.Expr {
	result := make([]ast.Expr, len(exprs))
	for i, e := range exprs {
		result[i] = cloneExpr(e)
	}
	return result
}

func cloneStringList(list []string) []string {
	result := make([]string, len(list))
	copy(result, list)
	return result
}

func cloneTypeExpr(t *ast.TypeExpr) *ast.TypeExpr {
	if t == nil {
		return nil
	}
	args := make([]ast.TypeExpr, len(t.GenericArgs))
	for i, arg := range t.GenericArgs {
		args[i] = *cloneTypeExpr(&arg)
	}
	return &ast.TypeExpr{
		Name:        t.Name,
		GenericArgs: args,
		Pointer:     t.Pointer,
	}
}

func cloneTypeExprPtr(t *ast.TypeExpr) *ast.TypeExpr {
	return cloneTypeExpr(t)
}

func cloneParamDefs(params []ast.ParamDef) []ast.ParamDef {
	result := make([]ast.ParamDef, len(params))
	for i, p := range params {
		result[i] = ast.ParamDef{
			Name:      p.Name,
			ParamType: *cloneTypeExpr(&p.ParamType),
		}
	}
	return result
}

func cloneFieldDefs(fields []ast.FieldDef) []ast.FieldDef {
	result := make([]ast.FieldDef, len(fields))
	for i, f := range fields {
		result[i] = ast.FieldDef{
			Name:      f.Name,
			FieldType: *cloneTypeExpr(&f.FieldType),
		}
	}
	return result
}

func cloneClassFieldDefs(fields []ast.ClassFieldDef) []ast.ClassFieldDef {
	result := make([]ast.ClassFieldDef, len(fields))
	for i, f := range fields {
		result[i] = ast.ClassFieldDef{
			Name:       f.Name,
			FieldType:  *cloneTypeExpr(&f.FieldType),
			Visibility: f.Visibility,
		}
	}
	return result
}

func cloneUnionFields(fields []ast.UnionField) []ast.UnionField {
	result := make([]ast.UnionField, len(fields))
	for i, f := range fields {
		result[i] = ast.UnionField{
			Name:      f.Name,
			FieldType: *cloneTypeExpr(&f.FieldType),
		}
	}
	return result
}
