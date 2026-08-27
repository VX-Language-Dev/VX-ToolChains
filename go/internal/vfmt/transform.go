package vfmt

import (
	"fmt"
	"strings"

	"github.com/vx-lang/vxc/internal/ast"
)

// Rule defines a single migration rule.
type Rule struct {
	// Description of what this rule does
	Description string `toml:"description"`

	// Kind is the type of rule: "rename_func", "rename_method", "migrate_call",
	// "migrate_property", "migrate_import", "migrate_type"
	Kind string `toml:"kind"`

	// OldName is the name to match (function name, method name, import path, type name)
	OldName string `toml:"old_name"`

	// NewName is the replacement name
	NewName string `toml:"new_name"`

	// OldModule is the module/class the old function/method belongs to (optional)
	OldModule string `toml:"old_module"`

	// NewModule is the new module/class the function/method belongs to (optional)
	NewModule string `toml:"new_module"`

	// ArgMapping maps old argument indices to new positions (0-based).
	// e.g., [2, 0, 1] means new[0]=old[2], new[1]=old[0], new[2]=old[1]
	ArgMapping []int `toml:"arg_mapping"`

	// AddArgs specifies additional literal arguments to add (in order)
	AddArgs []string `toml:"add_args"`

	// RemoveArgs are indices of arguments to remove
	RemoveArgs []int `toml:"remove_args"`

	// AddImport specifies an import to add if not present
	AddImport string `toml:"add_import"`
}

// Config holds all migration rules.
type Config struct {
	Rules []Rule `toml:"rule"`
}

// Transformer applies migration rules to an AST.
type Transformer struct {
	config     Config
	ruleEngine *RuleEngine
}

// NewTransformer creates a new Transformer.
func NewTransformer(config Config) *Transformer {
	return &Transformer{
		config:     config,
		ruleEngine: NewRuleEngine(config.Rules),
	}
}

// Transform applies all migration rules to the AST and returns the transformed code.
func (t *Transformer) Transform(program []ast.Expr, source string) []ast.Expr {
	return t.ruleEngine.Transform(program)
}

// RuleEngine evaluates and applies rules to the AST.
type RuleEngine struct {
	rules []Rule
}

// NewRuleEngine creates a RuleEngine.
func NewRuleEngine(rules []Rule) *RuleEngine {
	return &RuleEngine{rules: rules}
}

// Transform walks the AST and applies matching rules.
func (re *RuleEngine) Transform(program []ast.Expr) []ast.Expr {
	var result []ast.Expr
	for _, stmt := range program {
		transformed := re.transformStmt(stmt)
		if transformed != nil {
			result = append(result, transformed)
		}
	}
	return result
}

func (re *RuleEngine) transformStmt(s ast.Expr) ast.Expr {
	if s == nil {
		return nil
	}

	switch x := s.(type) {
	case *ast.FuncDecl:
		return re.transformFuncDecl(x)

	case *ast.ExternDecl:
		return re.transformExternDecl(x)

	case *ast.StructDecl:
		return re.transformStructDecl(x)

	case *ast.ClassDecl:
		return re.transformClassDecl(x)

	case *ast.EnumDecl:
		return re.transformEnumDecl(x)

	case *ast.UnionDecl:
		return re.transformUnionDecl(x)

	case *ast.MacroDef:
		return re.transformMacroDef(x)

	case *ast.MacroCall:
		return x

	case *ast.IfStmt:
		return re.transformIfStmt(x)

	case *ast.MatchStmt:
		return re.transformMatchStmt(x)

	case *ast.WhileStmt:
		return re.transformWhileStmt(x)

	case *ast.ForStmt:
		return re.transformForStmt(x)

	case *ast.LoopStmt:
		return re.transformLoopStmt(x)

	case *ast.BreakStmt:
		return x

	case *ast.ContinueStmt:
		return x

	case *ast.ReturnStmt:
		return re.transformReturnStmt(x)

	case *ast.VarDecl:
		return re.transformVarDecl(x)

	case *ast.Assign:
		return re.transformAssign(x)

	case *ast.ImportStmt:
		return re.transformImportStmt(x)

	case *ast.ExprStmt:
		return re.transformExprStmt(x)

	case *ast.NewExpr:
		return x

	default:
		if expr, ok := s.(ast.Expr); ok {
			return re.transformExpr(expr)
		}
		return s
	}
}

func (re *RuleEngine) transformExpr(e ast.Expr) ast.Expr {
	if e == nil {
		return nil
	}

	switch x := e.(type) {
	case *ast.Identifier:
		return x

	case *ast.IntLiteral, *ast.FloatLiteral, *ast.StringLiteral:
		return x

	case *ast.BoolLiteral:
		return x

	case *ast.NilLiteral:
		return x

	case *ast.ArrayLiteral:
		for i, elem := range x.Elements {
			x.Elements[i] = re.transformExpr(elem)
		}
		return x

	case *ast.MapLiteral:
		for i, pair := range x.Pairs {
			x.Pairs[i].Key = re.transformExpr(pair.Key)
			x.Pairs[i].Value = re.transformExpr(pair.Value)
		}
		return x

	case *ast.AddressOf:
		x.Expr = re.transformExpr(x.Expr)
		return x

	case *ast.Deref:
		x.Expr = re.transformExpr(x.Expr)
		return x

	case *ast.PointerMember:
		x.Expr = re.transformExpr(x.Expr)
		return x

	case *ast.BinaryOp:
		x.Left = re.transformExpr(x.Left)
		x.Right = re.transformExpr(x.Right)
		return x

	case *ast.UnaryOp:
		x.Expr = re.transformExpr(x.Expr)
		return x

	case *ast.IndexAccess:
		x.Target = re.transformExpr(x.Target)
		x.Index = re.transformExpr(x.Index)
		return x

	case *ast.PropertyAccess:
		x.Target = re.transformExpr(x.Target)
		return re.applyPropertyRules(x)

	case *ast.CallExpr:
		x.Args = re.transformExprs(x.Args)
		x.Callee = re.transformExpr(x.Callee)
		return re.applyCallRules(x)

	case *ast.MoveExpr:
		x.Expr = re.transformExpr(x.Expr)
		return x

	case *ast.NewExpr:
		for i, arg := range x.Positional {
			x.Positional[i] = re.transformExpr(arg)
		}
		return x

	case *ast.MacroCall:
		for i, arg := range x.Args {
			x.Args[i] = re.transformExpr(arg)
		}
		return x

	default:
		return e
	}
}

func (re *RuleEngine) transformExprs(exprs []ast.Expr) []ast.Expr {
	for i, e := range exprs {
		exprs[i] = re.transformExpr(e)
	}
	return exprs
}

// ==================== Statement Transformers ====================

func (re *RuleEngine) transformFuncDecl(f *ast.FuncDecl) *ast.FuncDecl {
	// Check if the function itself should be renamed
	for _, rule := range re.rules {
		if rule.Kind == "rename_func" && f.Name == rule.OldName {
			f.Name = rule.NewName
		}
		if rule.Kind == "migrate_type" && f.ReturnType != nil {
			re.applyTypeRule(f.ReturnType, rule)
		}
	}
	// Transform body
	for i, stmt := range f.Body {
		f.Body[i] = re.transformStmt(stmt)
	}
	// Transform params
	for i, param := range f.Params {
		paramCopy := param
		for _, rule := range re.rules {
			if rule.Kind == "migrate_type" {
				re.applyTypeRule(&paramCopy.ParamType, rule)
			}
		}
		f.Params[i] = paramCopy
	}
	return f
}

func (re *RuleEngine) transformExternDecl(e *ast.ExternDecl) *ast.ExternDecl {
	for _, rule := range re.rules {
		if rule.Kind == "rename_func" && e.Name == rule.OldName {
			e.Name = rule.NewName
		}
		if rule.Kind == "migrate_type" && e.ReturnType != nil {
			re.applyTypeRule(e.ReturnType, rule)
		}
	}
	for i, param := range e.Params {
		paramCopy := param
		for _, rule := range re.rules {
			if rule.Kind == "migrate_type" {
				re.applyTypeRule(&paramCopy.ParamType, rule)
			}
		}
		e.Params[i] = paramCopy
	}
	return e
}

func (re *RuleEngine) transformStructDecl(s *ast.StructDecl) *ast.StructDecl {
	for _, rule := range re.rules {
		if rule.Kind == "migrate_type" && s.Name == rule.OldName {
			s.Name = rule.NewName
		}
	}
	for i, f := range s.Fields {
		fCopy := f
		for _, rule := range re.rules {
			if rule.Kind == "migrate_type" {
				re.applyTypeRule(&fCopy.FieldType, rule)
			}
		}
		s.Fields[i] = fCopy
	}
	for i, m := range s.Methods {
		s.Methods[i] = re.transformStmt(m)
	}
	return s
}

func (re *RuleEngine) transformClassDecl(c *ast.ClassDecl) *ast.ClassDecl {
	for _, rule := range re.rules {
		if rule.Kind == "migrate_type" && c.Name == rule.OldName {
			c.Name = rule.NewName
		}
	}
	for i, f := range c.Fields {
		fCopy := f
		for _, rule := range re.rules {
			if rule.Kind == "migrate_type" {
				re.applyTypeRule(&fCopy.FieldType, rule)
			}
		}
		c.Fields[i] = fCopy
	}
	for i, m := range c.Methods {
		c.Methods[i] = re.transformStmt(m)
	}
	return c
}

func (re *RuleEngine) transformEnumDecl(e *ast.EnumDecl) *ast.EnumDecl {
	for _, rule := range re.rules {
		if rule.Kind == "migrate_type" && e.Name == rule.OldName {
			e.Name = rule.NewName
		}
	}
	return e
}

func (re *RuleEngine) transformUnionDecl(u *ast.UnionDecl) *ast.UnionDecl {
	for _, rule := range re.rules {
		if rule.Kind == "migrate_type" && u.Name == rule.OldName {
			u.Name = rule.NewName
		}
	}
	for i, f := range u.Fields {
		for _, rule := range re.rules {
			if rule.Kind == "migrate_type" {
				re.applyTypeRule(&f.FieldType, rule)
			}
		}
		u.Fields[i] = f
	}
	return u
}

func (re *RuleEngine) transformMacroDef(m *ast.MacroDef) *ast.MacroDef {
	for i, stmt := range m.Body {
		m.Body[i] = re.transformStmt(stmt)
	}
	return m
}

func (re *RuleEngine) transformIfStmt(s *ast.IfStmt) *ast.IfStmt {
	s.Condition = re.transformExpr(s.Condition)
	for i, stmt := range s.ThenBranch {
		s.ThenBranch[i] = re.transformStmt(stmt)
	}
	for i, eb := range s.ElifBranches {
		eb.Condition = re.transformExpr(eb.Condition)
		for j, stmt := range eb.Body {
			eb.Body[j] = re.transformStmt(stmt)
		}
		s.ElifBranches[i] = eb
	}
	for i, stmt := range s.ElseBranch {
		s.ElseBranch[i] = re.transformStmt(stmt)
	}
	return s
}

func (re *RuleEngine) transformMatchStmt(s *ast.MatchStmt) *ast.MatchStmt {
	s.Target = re.transformExpr(s.Target)
	for i, arm := range s.Arms {
		arm.Pattern = re.transformExpr(arm.Pattern)
		for j, stmt := range arm.Body {
			arm.Body[j] = re.transformStmt(stmt)
		}
		s.Arms[i] = arm
	}
	return s
}

func (re *RuleEngine) transformWhileStmt(s *ast.WhileStmt) *ast.WhileStmt {
	s.Condition = re.transformExpr(s.Condition)
	for i, stmt := range s.Body {
		s.Body[i] = re.transformStmt(stmt)
	}
	return s
}

func (re *RuleEngine) transformForStmt(s *ast.ForStmt) *ast.ForStmt {
	s.Iterable = re.transformExpr(s.Iterable)
	for i, stmt := range s.Body {
		s.Body[i] = re.transformStmt(stmt)
	}
	return s
}

func (re *RuleEngine) transformLoopStmt(s *ast.LoopStmt) *ast.LoopStmt {
	for i, stmt := range s.Body {
		s.Body[i] = re.transformStmt(stmt)
	}
	return s
}

func (re *RuleEngine) transformReturnStmt(s *ast.ReturnStmt) *ast.ReturnStmt {
	if s.Value != nil {
		s.Value = re.transformExpr(s.Value)
	}
	return s
}

func (re *RuleEngine) transformVarDecl(d *ast.VarDecl) *ast.VarDecl {
	d.Init = re.transformExpr(d.Init)
	if d.Type != nil {
		for _, rule := range re.rules {
			if rule.Kind == "migrate_type" {
				re.applyTypeRule(d.Type, rule)
			}
		}
	}
	return d
}

func (re *RuleEngine) transformAssign(a *ast.Assign) *ast.Assign {
	a.Target = re.transformExpr(a.Target)
	a.Value = re.transformExpr(a.Value)
	return a
}

func (re *RuleEngine) transformImportStmt(s *ast.ImportStmt) *ast.ImportStmt {
	for _, rule := range re.rules {
		if rule.Kind == "migrate_import" {
			if s.Path == rule.OldName || strings.HasPrefix(s.Path, rule.OldName) {
				s.Path = strings.Replace(s.Path, rule.OldName, rule.NewName, 1)
			}
		}
	}
	return s
}

func (re *RuleEngine) transformExprStmt(s *ast.ExprStmt) *ast.ExprStmt {
	s.Expr = re.transformExpr(s.Expr)
	return s
}

// ==================== Call/Method/Property Rule Application ====================

func (re *RuleEngine) applyCallRules(call *ast.CallExpr) ast.Expr {
	// Check if callee is a simple identifier (function call) or a property access (method call)
	if ident, ok := call.Callee.(*ast.Identifier); ok {
		for _, rule := range re.rules {
			switch rule.Kind {
			case "rename_func", "migrate_call":
				if ident.Name == rule.OldName && (rule.Kind != "migrate_call" || rule.OldModule == "") {
					if rule.NewModule != "" {
						return &ast.CallExpr{
							Callee: &ast.PropertyAccess{
								Target: &ast.Identifier{Name: rule.NewModule, Loc: ident.Loc},
								Prop:   rule.NewName,
								Loc:    ident.Loc,
							},
							Args: re.mapArgs(call.Args, rule),
							Loc:  call.Loc,
						}
					}
					ident.Name = rule.NewName
					call.Args = re.mapArgs(call.Args, rule)
					return call
				}
			}
		}
	}

	if prop, ok := call.Callee.(*ast.PropertyAccess); ok {
		for _, rule := range re.rules {
			switch rule.Kind {
			case "rename_method", "migrate_call":
				if prop.Prop == rule.OldName {
					if rule.OldModule == "" || re.isTypeName(prop.Target, rule.OldModule) {
						if rule.NewModule != "" {
							newArgs := append([]ast.Expr{prop.Target}, re.mapArgs(call.Args, rule)...)
							return &ast.CallExpr{
								Callee: &ast.PropertyAccess{
									Target: &ast.Identifier{Name: rule.NewModule},
									Prop:   rule.NewName,
								},
								Args: newArgs,
								Loc:  call.Loc,
							}
						}
						prop.Prop = rule.NewName
						call.Args = re.mapArgs(call.Args, rule)
						return call
					}
				}
			}
		}
	}

	return call
}

func (re *RuleEngine) applyPropertyRules(prop *ast.PropertyAccess) ast.Expr {
	for _, rule := range re.rules {
		if rule.Kind == "migrate_property" {
			if prop.Prop == rule.OldName {
				if rule.OldModule == "" || re.isTypeName(prop.Target, rule.OldModule) {
					prop.Prop = rule.NewName
					return prop
				}
			}
		}
	}
	return prop
}

// ==================== Type Rule Application ====================

func (re *RuleEngine) applyTypeRule(t *ast.TypeExpr, rule Rule) {
	if rule.Kind == "migrate_type" && t.Name == rule.OldName {
		t.Name = rule.NewName
	}
	for i, arg := range t.GenericArgs {
		newArg := arg
		re.applyTypeRule(&newArg, rule)
		t.GenericArgs[i] = newArg
	}
}

// ==================== Helpers ====================

func (re *RuleEngine) mapArgs(args []ast.Expr, rule Rule) []ast.Expr {
	// Apply argument removal (high to low index to avoid shifting)
	removeSet := make(map[int]bool)
	for _, idx := range rule.RemoveArgs {
		removeSet[idx] = true
	}

	// Apply arg mapping if specified
	mapped := make([]ast.Expr, 0, len(args))
	if len(rule.ArgMapping) > 0 {
		for _, srcIdx := range rule.ArgMapping {
			if srcIdx >= 0 && srcIdx < len(args) && !removeSet[srcIdx] {
				mapped = append(mapped, args[srcIdx])
			} else {
				// Placeholder for skipped args
				mapped = append(mapped, &ast.NilLiteral{})
			}
		}
	} else {
		for i, arg := range args {
			if !removeSet[i] {
				mapped = append(mapped, arg)
			}
		}
	}

	// Add extra arguments
	for _, addArg := range rule.AddArgs {
		mapped = append(mapped, re.parseLiteralArg(addArg))
	}

	return mapped
}

// isTypeName checks if an expression is an identifier with the given name.
func (re *RuleEngine) isTypeName(e ast.Expr, name string) bool {
	if ident, ok := e.(*ast.Identifier); ok {
		return ident.Name == name
	}
	return false
}

// parseLiteralArg parses a string into an AST literal node.
func (re *RuleEngine) parseLiteralArg(s string) ast.Expr {
	switch {
	case s == "true":
		return &ast.BoolLiteral{Value: true}
	case s == "false":
		return &ast.BoolLiteral{Value: false}
	case s == "nil":
		return &ast.NilLiteral{}
	case len(s) > 1 && s[0] == '"' && s[len(s)-1] == '"':
		return &ast.StringLiteral{Value: s[1 : len(s)-1]}
	default:
		// Try as int
		var i int64
		if _, err := fmt.Sscanf(s, "%d", &i); err == nil {
			return &ast.IntLiteral{Value: i}
		}
		// Fallback to identifier reference
		return &ast.Identifier{Name: s}
	}
}

// ==================== Rule loading ====================

// LoadDefaultRules returns a default set of migration rules.
func LoadDefaultRules() Config {
	return Config{
		Rules: []Rule{},
	}
}

// LoadBuiltinRules provides example migration rules for API migration.
// Users can customize these via TOML config.
func LoadBuiltinRules() Config {
	return Config{
		Rules: []Rule{
			{
				Description: "Move std.io functions to std.sys",
				Kind:        "rename_func",
				OldName:     "out",
				NewModule:   "sys",
				NewName:     "print",
			},
			{
				Description: "Option.some() -> Option.Some() (renamed to PascalCase)",
				Kind:        "rename_method",
				OldName:     "some",
				NewName:     "Some",
			},
			{
				Description: "Option.none() -> Option.None()",
				Kind:        "rename_method",
				OldName:     "none",
				NewName:     "None",
			},
			{
				Description: "push() renamed to append()",
				Kind:        "rename_method",
				OldName:     "push",
				NewName:     "append",
			},
			{
				Description: "Import migration from old to new path",
				Kind:        "migrate_import",
				OldName:     "std.old_module",
				NewName:     "std.new_module",
			},
		},
	}
}

// ParseConfig parses a TOML config into a Config struct.
func ParseConfig(configData string) (Config, error) {
	// Simple line-based parser for TOML-like rule definitions.
	// For production, use github.com/BurntSushi/toml
	// Here we provide a minimal parser.
	cfg := Config{}
	lines := strings.Split(configData, "\n")
	var currentRule *Rule

	for _, line := range lines {
		line = strings.TrimSpace(line)
		if line == "" || strings.HasPrefix(line, "#") {
			continue
		}

		if line == "[[rule]]" {
			if currentRule != nil {
				cfg.Rules = append(cfg.Rules, *currentRule)
			}
			currentRule = &Rule{}
			continue
		}

		if currentRule == nil {
			continue
		}

		parts := strings.SplitN(line, "=", 2)
		if len(parts) != 2 {
			continue
		}

		key := strings.TrimSpace(parts[0])
		value := strings.TrimSpace(parts[1])
		// Remove quotes
		value = strings.Trim(value, "\"")
		value = strings.TrimSpace(value)

		switch key {
		case "description":
			currentRule.Description = value
		case "kind":
			currentRule.Kind = value
		case "old_name":
			currentRule.OldName = value
		case "new_name":
			currentRule.NewName = value
		case "old_module":
			currentRule.OldModule = value
		case "new_module":
			currentRule.NewModule = value
		case "add_import":
			currentRule.AddImport = value
		}
	}

	if currentRule != nil {
		cfg.Rules = append(cfg.Rules, *currentRule)
	}

	return cfg, nil
}

// ValidateConfig checks that the loaded config is valid.
func ValidateConfig(cfg Config) []string {
	var errors []string
	validKinds := map[string]bool{
		"rename_func":      true,
		"rename_method":    true,
		"migrate_call":     true,
		"migrate_property": true,
		"migrate_import":   true,
		"migrate_type":     true,
	}

	for i, rule := range cfg.Rules {
		if !validKinds[rule.Kind] {
			errors = append(errors, fmt.Sprintf("rule[%d]: unknown kind %q", i, rule.Kind))
		}
		if rule.OldName == "" && rule.Kind != "" {
			errors = append(errors, fmt.Sprintf("rule[%d]: old_name is required", i))
		}
		if rule.NewName == "" && rule.Kind != "" {
			errors = append(errors, fmt.Sprintf("rule[%d]: new_name is required", i))
		}
	}
	return errors
}

// ==================== Source-level transformations ====================

// FindAndAddImport checks if an import exists in the program and adds it if not.
func FindAndAddImport(program []ast.Expr, importPath string) []ast.Expr {
	for _, stmt := range program {
		if imp, ok := stmt.(*ast.ImportStmt); ok {
			if imp.Path == importPath {
				return program // Already exists
			}
		}
	}

	// Add import at the beginning
	newImport := &ast.ImportStmt{
		Path: importPath,
	}
	return append([]ast.Expr{newImport}, program...)
}


