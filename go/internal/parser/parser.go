package parser

import (
	"fmt"
	"strconv"

	"github.com/vx-lang/vxc/internal/ast"
	"github.com/vx-lang/vxc/internal/token"
)

// Parser implements a recursive descent parser for the vx language.
type Parser struct {
	tokens []token.Token
	pos    int
	Errors []string
}

// NewParser creates a new Parser.
func NewParser(tokens []token.Token) *Parser {
	return &Parser{tokens: tokens, pos: 0}
}

// ---------------------------------------------------------------------------
// Token helpers
// ---------------------------------------------------------------------------

// currentToken returns the token at the current position.
func (p *Parser) currentToken() token.Token {
	if p.pos >= len(p.tokens) {
		return token.Token{Kind: token.EOF, Line: 0, Col: 0}
	}
	return p.tokens[p.pos]
}

// nextToken advances the position by one and returns the consumed token.
func (p *Parser) nextToken() token.Token {
	tok := p.currentToken()
	if p.pos < len(p.tokens) {
		p.pos++
	}
	return tok
}

// currentTokenIs checks whether the current token has the given kind.
func (p *Parser) currentTokenIs(kind token.TokenKind) bool {
	return p.currentToken().Kind == kind
}

// expect asserts the current token has the given kind and advances.
// If the assertion fails, an error is recorded and a synthetic token is returned.
func (p *Parser) expect(kind token.TokenKind) token.Token {
	tok := p.currentToken()
	if tok.Kind != kind {
		p.addError(fmt.Sprintf("expected %v, got %v (%q)", kind, tok.Kind, tok.Value))
		// Return a synthetic token so callers can proceed
		return token.Token{Kind: kind, Value: "", Line: tok.Line, Col: tok.Col}
	}
	p.pos++
	return tok
}

// addError records a parse error.
func (p *Parser) addError(msg string) {
	tok := p.currentToken()
	p.Errors = append(p.Errors, fmt.Sprintf("line %d:%d: %s", tok.Line, tok.Col, msg))
}

// acceptIdentOrKeyword consumes and returns the current token if it is an
// identifier or any keyword. This is used in contexts like member access
// (e.g. .union) where keywords are valid as method/property names.
func (p *Parser) acceptIdentOrKeyword() token.Token {
	tok := p.currentToken()
	if tok.Kind == token.Ident || token.IsKeywordKind(tok.Kind) {
		p.pos++
		return tok
	}
	return p.expect(token.Ident)
}

// syncToNextStatement advances tokens until a NEWLINE, DEDENT, or EOF is found.
func (p *Parser) syncToNextStatement() {
	for {
		switch p.currentToken().Kind {
		case token.Newline, token.Dedent, token.EOF:
			return
		}
		p.nextToken()
	}
}

// ---------------------------------------------------------------------------
// Entry point
// ---------------------------------------------------------------------------

// ParseProgram parses a complete program (stmts until EOF).
func (p *Parser) ParseProgram() []ast.Expr {
	stmts := p.parseStmts()
	return stmts
}

// ---------------------------------------------------------------------------
// Statement parsing
// ---------------------------------------------------------------------------

// parseStmts parses zero or more statements until EOF or DEDENT.
func (p *Parser) parseStmts() []ast.Expr {
	var stmts []ast.Expr
	for !p.currentTokenIs(token.EOF) && !p.currentTokenIs(token.Dedent) {
		stmt := p.parseStmt()
		if stmt != nil {
			stmts = append(stmts, stmt)
		}
	}
	return stmts
}

// parseStmt parses a single statement.
func (p *Parser) parseStmt() ast.Expr {
	// Blank line
	if p.currentTokenIs(token.Newline) {
		tok := p.nextToken()
		return &ast.ExprStmt{
			Expr: &ast.NilLiteral{Loc: ast.Pos{Line: tok.Line, Col: tok.Col}},
			Loc:  ast.Pos{Line: tok.Line, Col: tok.Col},
		}
	}

	// Check for compound statement keywords
	switch p.currentToken().Kind {
	case token.Func, token.Extern, token.Struct, token.Class,
		token.Enum, token.Union, token.Macro,
		token.If, token.Match, token.While, token.For, token.Loop:
		return p.parseCompoundStmt()
	default:
		stmt := p.parseSimpleStmt()
		// simple_stmt must be followed by NEWLINE (unless EOF/DEDENT)
		if !p.currentTokenIs(token.EOF) && !p.currentTokenIs(token.Dedent) {
			if p.currentTokenIs(token.Newline) {
				p.nextToken()
			} else {
				p.addError(fmt.Sprintf("expected NEWLINE after statement, got %v", p.currentToken().Kind))
				p.syncToNextStatement()
				if p.currentTokenIs(token.Newline) {
					p.nextToken()
				}
			}
		}
		return stmt
	}
}

// parseSimpleStmt parses a simple statement (expression, var_decl, return, etc.).
func (p *Parser) parseSimpleStmt() ast.Expr {
	tok := p.currentToken()
	switch tok.Kind {
	case token.Import:
		return p.parseImportStmt()
	case token.VarT, token.Mut:
		return p.parseVarDecl()
	case token.Return:
		return p.parseReturnStmt()
	case token.Break:
		return p.parseBreakStmt()
	case token.Continue:
		return p.parseContinueStmt()
	case token.Hash:
		return p.parseMacroCall()
	default:
		expr := p.parseExpr()
		return &ast.ExprStmt{
			Expr: expr,
			Loc:  expr.Pos(),
		}
	}
}

// parseCompoundStmt parses a compound statement.
func (p *Parser) parseCompoundStmt() ast.Expr {
	switch p.currentToken().Kind {
	case token.Func:
		return p.parseFuncDecl()
	case token.Extern:
		return p.parseExternDecl()
	case token.Struct:
		return p.parseStructDecl()
	case token.Class:
		return p.parseClassDecl()
	case token.Enum:
		return p.parseEnumDecl()
	case token.Union:
		return p.parseUnionDecl()
	case token.Macro:
		return p.parseMacroDef()
	case token.If:
		return p.parseIfStmt()
	case token.Match:
		return p.parseMatchStmt()
	case token.While:
		return p.parseWhileStmt()
	case token.For:
		return p.parseForStmt()
	case token.Loop:
		return p.parseLoopStmt()
	default:
		p.addError(fmt.Sprintf("unexpected token %v in compound statement", p.currentToken().Kind))
		return nil
	}
}

// parseBlock parses: NEWLINE INDENT stmts DEDENT
func (p *Parser) parseBlock() []ast.Expr {
	p.expect(token.Newline)
	p.expect(token.Indent)
	stmts := p.parseStmts()
	p.expect(token.Dedent)
	return stmts
}

// skipBlock consumes a full indented block without parsing contents.
// This is used for class/struct/enum/union bodies where the current
// parser does not fully support field declarations.
func (p *Parser) skipBlock() {
	if !p.currentTokenIs(token.Newline) {
		return
	}
	p.nextToken() // consume NEWLINE
	if !p.currentTokenIs(token.Indent) {
		return
	}
	p.nextToken() // consume INDENT
	depth := 1
	for depth > 0 {
		switch p.currentToken().Kind {
		case token.Indent:
			depth++
			p.nextToken()
		case token.Dedent:
			depth--
			if depth > 0 {
				p.nextToken()
			}
		case token.EOF:
			return
		default:
			p.nextToken()
		}
	}
	// Consume the matching DEDENT
	if p.currentTokenIs(token.Dedent) {
		p.nextToken()
	}
}

// ---------------------------------------------------------------------------
// Specific statement parsers
// ---------------------------------------------------------------------------

// parseVarDecl parses: VAR_T IDENT [: type_expr] ASSIGN expr
//
//	MUT IDENT [: type_expr] ASSIGN expr
func (p *Parser) parseVarDecl() ast.Expr {
	tok := p.currentToken()
	isMut := tok.Kind == token.VarT || tok.Kind == token.Mut
	p.nextToken() // consume VAR_T or MUT

	nameTok := p.acceptIdentOrKeyword()
	name := nameTok.Value

	var typeExpr *ast.TypeExpr
	if p.currentTokenIs(token.Colon) {
		p.nextToken() // consume COLON
		te := p.parseTypeExpr()
		typeExpr = &te
	}

	p.expect(token.Assign)
	init := p.parseExpr()

	return &ast.VarDecl{
		Name:  name,
		Type:  typeExpr,
		Init:  init,
		IsMut: isMut,
		Loc:   ast.Pos{Line: tok.Line, Col: tok.Col},
	}
}

// parseReturnStmt parses: RETURN [expr]
func (p *Parser) parseReturnStmt() ast.Expr {
	tok := p.nextToken() // consume RETURN

	var value ast.Expr
	if !p.currentTokenIs(token.Newline) &&
		!p.currentTokenIs(token.Dedent) &&
		!p.currentTokenIs(token.EOF) {
		value = p.parseExpr()
	}

	return &ast.ReturnStmt{
		Value: value,
		Loc:   ast.Pos{Line: tok.Line, Col: tok.Col},
	}
}

// parseBreakStmt parses: BREAK [IDENT]
func (p *Parser) parseBreakStmt() ast.Expr {
	tok := p.nextToken() // consume BREAK

	var label string
	if p.currentTokenIs(token.Ident) || token.IsKeywordKind(p.currentToken().Kind) {
		label = p.nextToken().Value
	}

	return &ast.BreakStmt{
		Label: label,
		Loc:   ast.Pos{Line: tok.Line, Col: tok.Col},
	}
}

// parseContinueStmt parses: CONTINUE [IDENT]
func (p *Parser) parseContinueStmt() ast.Expr {
	tok := p.nextToken() // consume CONTINUE

	var label string
	if p.currentTokenIs(token.Ident) || token.IsKeywordKind(p.currentToken().Kind) {
		label = p.nextToken().Value
	}

	return &ast.ContinueStmt{
		Label: label,
		Loc:   ast.Pos{Line: tok.Line, Col: tok.Col},
	}
}

// parseImportStmt parses: IMPORT import_path [AS IDENT]
func (p *Parser) parseImportStmt() ast.Expr {
	tok := p.nextToken() // consume IMPORT

	path := p.parseImportPath()

	var alias string
	if p.currentTokenIs(token.As) {
		p.nextToken() // consume AS
		aliasTok := p.acceptIdentOrKeyword()
		alias = aliasTok.Value
	}

	return &ast.ImportStmt{
		Path:  path,
		Alias: alias,
		Loc:   ast.Pos{Line: tok.Line, Col: tok.Col},
	}
}

// parseImportPath parses: IDENT {. IDENT}*
func (p *Parser) parseImportPath() string {
	tok := p.acceptIdentOrKeyword()
	path := tok.Value
	for p.currentTokenIs(token.Dot) {
		p.nextToken() // consume DOT
		ident := p.acceptIdentOrKeyword()
		path += "." + ident.Value
	}
	return path
}

// parseIfStmt parses: IF expr COLON block {ELIF expr COLON block}* [ELSE COLON block]
func (p *Parser) parseIfStmt() ast.Expr {
	tok := p.currentToken()
	p.nextToken() // consume IF

	condition := p.parseExpr()
	p.expect(token.Colon)
	thenBranch := p.parseBlock()

	var elifBranches []ast.ElifBranch
	for p.currentTokenIs(token.Elif) {
		p.nextToken() // consume ELIF
		elifCond := p.parseExpr()
		p.expect(token.Colon)
		elifBody := p.parseBlock()
		elifBranches = append(elifBranches, ast.ElifBranch{
			Condition: elifCond,
			Body:      elifBody,
		})
	}

	var elseBranch []ast.Expr
	if p.currentTokenIs(token.Else) {
		p.nextToken() // consume ELSE
		p.expect(token.Colon)
		elseBranch = p.parseBlock()
	}

	return &ast.IfStmt{
		Condition:    condition,
		ThenBranch:   thenBranch,
		ElifBranches: elifBranches,
		ElseBranch:   elseBranch,
		Loc:          ast.Pos{Line: tok.Line, Col: tok.Col},
	}
}

// parseMatchStmt parses: MATCH expr COLON NEWLINE INDENT match_arms DEDENT
func (p *Parser) parseMatchStmt() ast.Expr {
	tok := p.currentToken()
	p.nextToken() // consume MATCH

	target := p.parseExpr()
	p.expect(token.Colon)
	p.expect(token.Newline)
	p.expect(token.Indent)

	var arms []ast.MatchArm
	for !p.currentTokenIs(token.Dedent) && !p.currentTokenIs(token.EOF) {
		arm := p.parseMatchArm()
		arms = append(arms, arm)
	}
	p.expect(token.Dedent)

	return &ast.MatchStmt{
		Target: target,
		Arms:   arms,
		Loc:    ast.Pos{Line: tok.Line, Col: tok.Col},
	}
}

// parseMatchArm parses: expr COLON block
func (p *Parser) parseMatchArm() ast.MatchArm {
	pattern := p.parseExpr()
	p.expect(token.Colon)
	body := p.parseBlock()
	return ast.MatchArm{
		Pattern: pattern,
		Body:    body,
	}
}

// parseWhileStmt parses: WHILE expr COLON block
func (p *Parser) parseWhileStmt() ast.Expr {
	tok := p.currentToken()
	p.nextToken() // consume WHILE

	condition := p.parseExpr()
	p.expect(token.Colon)
	body := p.parseBlock()

	return &ast.WhileStmt{
		Condition: condition,
		Body:      body,
		Loc:       ast.Pos{Line: tok.Line, Col: tok.Col},
	}
}

// parseForStmt parses: FOR IDENT IN expr COLON block
func (p *Parser) parseForStmt() ast.Expr {
	tok := p.currentToken()
	p.nextToken() // consume FOR

	nameTok := p.acceptIdentOrKeyword()
	p.expect(token.In)
	iterable := p.parseExpr()
	p.expect(token.Colon)
	body := p.parseBlock()

	return &ast.ForStmt{
		VarName:  nameTok.Value,
		Iterable: iterable,
		Body:     body,
		Loc:      ast.Pos{Line: tok.Line, Col: tok.Col},
	}
}

// parseLoopStmt parses: LOOP [IDENT] COLON block
func (p *Parser) parseLoopStmt() ast.Expr {
	tok := p.currentToken()
	p.nextToken() // consume LOOP

	var label string
	if p.currentTokenIs(token.Ident) || token.IsKeywordKind(p.currentToken().Kind) {
		label = p.nextToken().Value
	}

	p.expect(token.Colon)
	body := p.parseBlock()

	return &ast.LoopStmt{
		Label: label,
		Body:  body,
		Loc:   ast.Pos{Line: tok.Line, Col: tok.Col},
	}
}

// parseFuncDecl parses:
//
//	FUNC IDENT LPAREN params RPAREN [ARROW type_expr] COLON block
func (p *Parser) parseFuncDecl() ast.Expr {
	tok := p.currentToken()
	p.nextToken() // consume FUNC

	nameTok := p.acceptIdentOrKeyword()
	p.expect(token.LParen)
	params := p.parseParams()
	p.expect(token.RParen)

	var returnType *ast.TypeExpr
	if p.currentTokenIs(token.Arrow) {
		p.nextToken() // consume ARROW
		rt := p.parseTypeExpr()
		returnType = &rt
	}

	p.expect(token.Colon)
	body := p.parseBlock()

	return &ast.FuncDecl{
		Name:       nameTok.Value,
		Params:     params,
		ReturnType: returnType,
		Body:       body,
		Loc:        ast.Pos{Line: tok.Line, Col: tok.Col},
	}
}

// parseExternDecl parses:
//
//	EXTERN IDENT LPAREN params RPAREN [ARROW type_expr] NEWLINE
func (p *Parser) parseExternDecl() ast.Expr {
	tok := p.currentToken()
	p.nextToken() // consume EXTERN

	nameTok := p.acceptIdentOrKeyword()
	p.expect(token.LParen)
	params := p.parseParams()
	p.expect(token.RParen)

	var returnType *ast.TypeExpr
	if p.currentTokenIs(token.Arrow) {
		p.nextToken() // consume ARROW
		rt := p.parseTypeExpr()
		returnType = &rt
	}

	// ExternDecl ends at NEWLINE (consumed by the caller in parseStmt)
	return &ast.ExternDecl{
		Name:       nameTok.Value,
		Params:     params,
		ReturnType: returnType,
		Loc:        ast.Pos{Line: tok.Line, Col: tok.Col},
	}
}

// parseStructDecl parses: STRUCT IDENT COLON block (fields/methods parsed as empty)
func (p *Parser) parseStructDecl() ast.Expr {
	tok := p.currentToken()
	p.nextToken() // consume STRUCT

	nameTok := p.acceptIdentOrKeyword()
	p.expect(token.Colon)
	_ = p.parseBlock() // consume block but ignore contents for now

	return &ast.StructDecl{
		Name: nameTok.Value,
		Loc:  ast.Pos{Line: tok.Line, Col: tok.Col},
	}
}

// parseClassDecl parses: CLASS IDENT COLON block (fields/methods parsed as empty)
func (p *Parser) parseClassDecl() ast.Expr {
	tok := p.currentToken()
	p.nextToken() // consume CLASS

	nameTok := p.acceptIdentOrKeyword()
	p.expect(token.Colon)
	_ = p.parseBlock() // consume block but ignore contents for now

	return &ast.ClassDecl{
		Name: nameTok.Value,
		Loc:  ast.Pos{Line: tok.Line, Col: tok.Col},
	}
}

// parseEnumDecl parses: ENUM IDENT COLON block (variants parsed as empty)
func (p *Parser) parseEnumDecl() ast.Expr {
	tok := p.currentToken()
	p.nextToken() // consume ENUM

	nameTok := p.acceptIdentOrKeyword()
	p.expect(token.Colon)
	_ = p.parseBlock() // consume block but ignore contents for now

	return &ast.EnumDecl{
		Name: nameTok.Value,
		Loc:  ast.Pos{Line: tok.Line, Col: tok.Col},
	}
}

// parseUnionDecl parses: UNION IDENT COLON block (fields parsed as empty)
func (p *Parser) parseUnionDecl() ast.Expr {
	tok := p.currentToken()
	p.nextToken() // consume UNION

	nameTok := p.acceptIdentOrKeyword()
	p.expect(token.Colon)
	_ = p.parseBlock() // consume block but ignore contents for now

	return &ast.UnionDecl{
		Name: nameTok.Value,
		Loc:  ast.Pos{Line: tok.Line, Col: tok.Col},
	}
}

// parseMacroDef parses: MACRO IDENT LPAREN ident_list RPAREN COLON block
func (p *Parser) parseMacroDef() ast.Expr {
	tok := p.currentToken()
	p.nextToken() // consume MACRO

	nameTok := p.acceptIdentOrKeyword()
	p.expect(token.LParen)
	params := p.parseIdentList()
	p.expect(token.RParen)
	p.expect(token.Colon)
	body := p.parseBlock()

	return &ast.MacroDef{
		Name:   nameTok.Value,
		Params: params,
		Body:   body,
		Loc:    ast.Pos{Line: tok.Line, Col: tok.Col},
	}
}

// parseMacroCall parses: HASH IDENT LPAREN expr_list RPAREN
func (p *Parser) parseMacroCall() ast.Expr {
	tok := p.nextToken() // consume HASH

	nameTok := p.acceptIdentOrKeyword()
	p.expect(token.LParen)
	args := p.parseArgs()
	p.expect(token.RParen)

	return &ast.MacroCall{
		Name: nameTok.Value,
		Args: args,
		Loc:  ast.Pos{Line: tok.Line, Col: tok.Col},
	}
}

// ---------------------------------------------------------------------------
// Helper parsers
// ---------------------------------------------------------------------------

// parseParams parses: [param_def {COMMA param_def}]
func (p *Parser) parseParams() []ast.ParamDef {
	var params []ast.ParamDef
	if p.currentTokenIs(token.RParen) {
		return params
	}
	params = append(params, p.parseParamDef())
	for p.currentTokenIs(token.Comma) {
		p.nextToken() // consume COMMA
		if p.currentTokenIs(token.RParen) {
			break
		}
		params = append(params, p.parseParamDef())
	}
	return params
}

// parseParamDef parses: IDENT COLON type_expr
func (p *Parser) parseParamDef() ast.ParamDef {
	nameTok := p.acceptIdentOrKeyword()
	p.expect(token.Colon)
	typ := p.parseTypeExpr()
	return ast.ParamDef{
		Name:      nameTok.Value,
		ParamType: typ,
	}
}

// parseIdentList parses: [IDENT {COMMA IDENT}]
func (p *Parser) parseIdentList() []string {
	var idents []string
	if p.currentTokenIs(token.RParen) {
		return idents
	}
	tok := p.acceptIdentOrKeyword()
	idents = append(idents, tok.Value)
	for p.currentTokenIs(token.Comma) {
		p.nextToken() // consume COMMA
		if p.currentTokenIs(token.RParen) {
			break
		}
		tok := p.acceptIdentOrKeyword()
		idents = append(idents, tok.Value)
	}
	return idents
}

// parseArgs parses: [expr {COMMA expr}]
func (p *Parser) parseArgs() []ast.Expr {
	var args []ast.Expr
	if p.currentTokenIs(token.RParen) {
		return args
	}
	args = append(args, p.parseExpr())
	for p.currentTokenIs(token.Comma) {
		p.nextToken() // consume COMMA
		if p.currentTokenIs(token.RParen) {
			break
		}
		args = append(args, p.parseExpr())
	}
	return args
}

// parseTypeExpr parses a type expression.
func (p *Parser) parseTypeExpr() ast.TypeExpr {
	var base ast.TypeExpr

	tok := p.currentToken()
	switch tok.Kind {
	case token.Ident:
		name := tok.Value
		p.nextToken()
		if p.currentTokenIs(token.Lt) {
			p.nextToken() // consume LT
			var args []ast.TypeExpr
			args = append(args, p.parseTypeExpr())
			for p.currentTokenIs(token.Comma) {
				p.nextToken() // consume COMMA
				args = append(args, p.parseTypeExpr())
			}
			p.expect(token.Gt)
			base = ast.TypeExpr{Name: name, GenericArgs: args}
		} else {
			base = ast.TypeExpr{Name: name}
		}
	case token.IntT:
		p.nextToken()
		base = ast.TypeExpr{Name: "int"}
	case token.FloatT:
		p.nextToken()
		base = ast.TypeExpr{Name: "float"}
	case token.DoubleT:
		p.nextToken()
		base = ast.TypeExpr{Name: "double"}
	case token.BoolT:
		p.nextToken()
		base = ast.TypeExpr{Name: "bool"}
	case token.VoidT:
		p.nextToken()
		base = ast.TypeExpr{Name: "void"}
	case token.VarT:
		p.nextToken()
		base = ast.TypeExpr{Name: "var"}
	case token.LParen:
		p.nextToken() // consume LPAREN
		base = p.parseTypeExpr()
		p.expect(token.RParen)
	default:
		p.addError(fmt.Sprintf("expected type, got %v (%q)", tok.Kind, tok.Value))
		return ast.TypeExpr{Name: "unknown"}
	}

	// Handle postfix STAR for pointer types: base*
	for p.currentTokenIs(token.Star) {
		p.nextToken()
		base.Pointer++
	}

	return base
}

// ---------------------------------------------------------------------------
// Expression parsing (precedence climbing)
// ---------------------------------------------------------------------------

// parseExpr is the top-level expression parser.
func (p *Parser) parseExpr() ast.Expr {
	return p.parseAssignment()
}

// parseAssignment handles: or_expr {ASSIGN_OP or_expr}
//
//	Right-associative.
func (p *Parser) parseAssignment() ast.Expr {
	left := p.parseOr()

	if p.isAssignmentOp(p.currentToken().Kind) {
		opTok := p.nextToken()
		right := p.parseAssignment() // right-associative
		return &ast.Assign{
			Target: left,
			Op:     opTok.Value,
			Value:  right,
			Loc:    left.Pos(),
		}
	}

	return left
}

// isAssignmentOp checks whether the token is an assignment operator.
func (p *Parser) isAssignmentOp(kind token.TokenKind) bool {
	switch kind {
	case token.Assign, token.PlusAssign, token.MinusAssign,
		token.StarAssign, token.SlashAssign, token.PercentAssign, token.PowerAssign:
		return true
	}
	return false
}

// parseOr handles: and_expr {OR and_expr}
func (p *Parser) parseOr() ast.Expr {
	left := p.parseAnd()

	for p.currentTokenIs(token.Or) {
		opTok := p.nextToken()
		right := p.parseAnd()
		left = &ast.BinaryOp{
			Op:    opTok.Value,
			Left:  left,
			Right: right,
			Loc:   left.Pos(),
		}
	}

	return left
}

// parseAnd handles: equality_expr {AND equality_expr}
func (p *Parser) parseAnd() ast.Expr {
	left := p.parseEquality()

	for p.currentTokenIs(token.And) {
		opTok := p.nextToken()
		right := p.parseEquality()
		left = &ast.BinaryOp{
			Op:    opTok.Value,
			Left:  left,
			Right: right,
			Loc:   left.Pos(),
		}
	}

	return left
}

// parseEquality handles: comparison_expr {EQ|NE comparison_expr}
func (p *Parser) parseEquality() ast.Expr {
	left := p.parseComparison()

	for p.currentTokenIs(token.Eq) || p.currentTokenIs(token.Ne) {
		opTok := p.nextToken()
		right := p.parseComparison()
		left = &ast.BinaryOp{
			Op:    opTok.Value,
			Left:  left,
			Right: right,
			Loc:   left.Pos(),
		}
	}

	return left
}

// parseComparison handles: additive_expr {LT|GT|LE|GE additive_expr}
func (p *Parser) parseComparison() ast.Expr {
	left := p.parseAdditive()

	for p.currentTokenIs(token.Lt) || p.currentTokenIs(token.Gt) ||
		p.currentTokenIs(token.Le) || p.currentTokenIs(token.Ge) {
		opTok := p.nextToken()
		right := p.parseAdditive()
		left = &ast.BinaryOp{
			Op:    opTok.Value,
			Left:  left,
			Right: right,
			Loc:   left.Pos(),
		}
	}

	return left
}

// parseAdditive handles: multiplicative_expr {PLUS|MINUS multiplicative_expr}
func (p *Parser) parseAdditive() ast.Expr {
	left := p.parseMultiplicative()

	for p.currentTokenIs(token.Plus) || p.currentTokenIs(token.Minus) {
		opTok := p.nextToken()
		right := p.parseMultiplicative()
		left = &ast.BinaryOp{
			Op:    opTok.Value,
			Left:  left,
			Right: right,
			Loc:   left.Pos(),
		}
	}

	return left
}

// parseMultiplicative handles: power_expr {STAR|SLASH|PERCENT power_expr}
func (p *Parser) parseMultiplicative() ast.Expr {
	left := p.parsePower()

	for p.currentTokenIs(token.Star) || p.currentTokenIs(token.Slash) || p.currentTokenIs(token.Percent) {
		opTok := p.nextToken()
		right := p.parsePower()
		left = &ast.BinaryOp{
			Op:    opTok.Value,
			Left:  left,
			Right: right,
			Loc:   left.Pos(),
		}
	}

	return left
}

// parsePower handles: unary_expr {POWER unary_expr}
//
//	Right-associative.
func (p *Parser) parsePower() ast.Expr {
	left := p.parseUnary()

	if p.currentTokenIs(token.Power) {
		opTok := p.nextToken()
		right := p.parsePower() // right-associative
		left = &ast.BinaryOp{
			Op:    opTok.Value,
			Left:  left,
			Right: right,
			Loc:   left.Pos(),
		}
	}

	return left
}

// parseUnary handles: postfix_expr | MINUS unary_expr | NOT unary_expr |
//
//	AMPERSAND unary_expr | AMPERSAND MUT unary_expr | STAR unary_expr | MOVE unary_expr
func (p *Parser) parseUnary() ast.Expr {
	tok := p.currentToken()

	switch tok.Kind {
	case token.Minus:
		p.nextToken()
		expr := p.parseUnary()
		return &ast.UnaryOp{
			Op:   "-",
			Expr: expr,
			Loc:  ast.Pos{Line: tok.Line, Col: tok.Col},
		}
	case token.Not:
		p.nextToken()
		expr := p.parseUnary()
		return &ast.UnaryOp{
			Op:   "!",
			Expr: expr,
			Loc:  ast.Pos{Line: tok.Line, Col: tok.Col},
		}
	case token.Ampersand:
		p.nextToken()
		isMut := false
		if p.currentTokenIs(token.Mut) {
			p.nextToken()
			isMut = true
		}
		expr := p.parseUnary()
		return &ast.AddressOf{
			Expr:  expr,
			IsMut: isMut,
			Loc:   ast.Pos{Line: tok.Line, Col: tok.Col},
		}
	case token.Star:
		p.nextToken()
		expr := p.parseUnary()
		return &ast.Deref{
			Expr: expr,
			Loc:  ast.Pos{Line: tok.Line, Col: tok.Col},
		}
	case token.Move:
		p.nextToken()
		expr := p.parseUnary()
		return &ast.MoveExpr{
			Expr: expr,
			Loc:  ast.Pos{Line: tok.Line, Col: tok.Col},
		}
	default:
		return p.parsePostfix()
	}
}

// parsePostfix handles: primary_expr {LPAREN args RPAREN | LBRACKET expr RBRACKET | DOT IDENT | ARROW IDENT}
func (p *Parser) parsePostfix() ast.Expr {
	left := p.parsePrimary()

	for {
		switch p.currentToken().Kind {
		case token.LParen:
			p.nextToken() // consume LPAREN
			args := p.parseArgs()
			p.expect(token.RParen)
			left = &ast.CallExpr{
				Callee: left,
				Args:   args,
				Loc:    left.Pos(),
			}
		case token.LBracket:
			p.nextToken() // consume LBRACKET
			index := p.parseExpr()
			p.expect(token.RBracket)
			left = &ast.IndexAccess{
				Target: left,
				Index:  index,
				Loc:    left.Pos(),
			}
		case token.Dot:
			p.nextToken() // consume DOT
			propTok := p.acceptIdentOrKeyword()
			left = &ast.PropertyAccess{
				Target: left,
				Prop:   propTok.Value,
				Loc:    left.Pos(),
			}
		case token.Arrow:
			p.nextToken() // consume ARROW
			memberTok := p.acceptIdentOrKeyword()
			left = &ast.PointerMember{
				Expr:   left,
				Member: memberTok.Value,
				Loc:    left.Pos(),
			}
		default:
			return left
		}
	}
}

// parsePrimary handles atomic expressions.
func (p *Parser) parsePrimary() ast.Expr {
	tok := p.currentToken()

	switch tok.Kind {
	case token.Int:
		p.nextToken()
		val, err := strconv.ParseInt(tok.Value, 10, 64)
		if err != nil {
			p.addError(fmt.Sprintf("invalid integer literal %q: %v", tok.Value, err))
			val = 0
		}
		return &ast.IntLiteral{
			Value: val,
			Loc:   ast.Pos{Line: tok.Line, Col: tok.Col},
		}

	case token.Float:
		p.nextToken()
		val, err := strconv.ParseFloat(tok.Value, 64)
		if err != nil {
			p.addError(fmt.Sprintf("invalid float literal %q: %v", tok.Value, err))
			val = 0.0
		}
		return &ast.FloatLiteral{
			Value: val,
			Loc:   ast.Pos{Line: tok.Line, Col: tok.Col},
		}

	case token.String:
		p.nextToken()
		return &ast.StringLiteral{
			Value: tok.Value,
			Loc:   ast.Pos{Line: tok.Line, Col: tok.Col},
		}

	case token.True:
		p.nextToken()
		return &ast.BoolLiteral{
			Value: true,
			Loc:   ast.Pos{Line: tok.Line, Col: tok.Col},
		}

	case token.False:
		p.nextToken()
		return &ast.BoolLiteral{
			Value: false,
			Loc:   ast.Pos{Line: tok.Line, Col: tok.Col},
		}

	case token.Nil:
		p.nextToken()
		return &ast.NilLiteral{
			Loc: ast.Pos{Line: tok.Line, Col: tok.Col},
		}

	case token.Ident:
		p.nextToken()
		return &ast.Identifier{
			Name: tok.Value,
			Loc:  ast.Pos{Line: tok.Line, Col: tok.Col},
		}

	case token.LParen:
		p.nextToken() // consume LPAREN
		expr := p.parseExpr()
		p.expect(token.RParen)
		return expr

	case token.LBracket:
		p.nextToken() // consume LBRACKET
		var elements []ast.Expr
		if !p.currentTokenIs(token.RBracket) {
			elements = append(elements, p.parseExpr())
			for p.currentTokenIs(token.Comma) {
				p.nextToken() // consume COMMA
				if p.currentTokenIs(token.RBracket) {
					break
				}
				elements = append(elements, p.parseExpr())
			}
		}
		p.expect(token.RBracket)
		return &ast.ArrayLiteral{
			Elements: elements,
			Loc:      ast.Pos{Line: tok.Line, Col: tok.Col},
		}

	case token.LBrace:
		p.nextToken() // consume LBRACE
		var pairs []ast.MapPair
		if !p.currentTokenIs(token.RBrace) {
			pairs = append(pairs, p.parseMapPair())
			for p.currentTokenIs(token.Comma) {
				p.nextToken() // consume COMMA
				if p.currentTokenIs(token.RBrace) {
					break
				}
				pairs = append(pairs, p.parseMapPair())
			}
		}
		p.expect(token.RBrace)
		return &ast.MapLiteral{
			Pairs: pairs,
			Loc:   ast.Pos{Line: tok.Line, Col: tok.Col},
		}

	case token.New:
		p.nextToken() // consume NEW
		typeNameTok := p.acceptIdentOrKeyword()
		p.expect(token.LParen)
		positional := p.parseArgs()
		p.expect(token.RParen)
		return &ast.NewExpr{
			TypeName:   typeNameTok.Value,
			Positional: positional,
			Loc:        ast.Pos{Line: tok.Line, Col: tok.Col},
		}

	default:
		p.addError(fmt.Sprintf("unexpected token %v (%q) in expression", tok.Kind, tok.Value))
		// Return a nil literal as error recovery
		return &ast.NilLiteral{
			Loc: ast.Pos{Line: tok.Line, Col: tok.Col},
		}
	}
}

// parseMapPair parses: expr COLON expr
func (p *Parser) parseMapPair() ast.MapPair {
	key := p.parseExpr()
	p.expect(token.Colon)
	value := p.parseExpr()
	return ast.MapPair{
		Key:   key,
		Value: value,
	}
}
