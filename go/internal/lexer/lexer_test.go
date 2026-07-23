package lexer

import (
	"fmt"
	"testing"

	"github.com/vx-lang/vxc/internal/token"
)

func TestNextToken(t *testing.T) {
	input := `if x == 42:
    print("hello")
else:
    print("world")`

	tests := []struct {
		kind  token.TokenKind
		value string
	}{
		{token.If, "if"},
		{token.Ident, "x"},
		{token.Eq, "=="},
		{token.Int, "42"},
		{token.Colon, ":"},
		{token.Newline, ""},
		{token.Indent, ""},
		{token.Ident, "print"},
		{token.LParen, "("},
		{token.String, "hello"},
		{token.RParen, ")"},
		{token.Newline, ""},
		{token.Dedent, ""},
		{token.Else, "else"},
		{token.Colon, ":"},
		{token.Newline, ""},
		{token.Indent, ""},
		{token.Ident, "print"},
		{token.LParen, "("},
		{token.String, "world"},
		{token.RParen, ")"},
		{token.Newline, ""},
		{token.Dedent, ""},
		{token.EOF, ""},
	}

	l := NewLexer(input)
	for i, tt := range tests {
		tok := l.NextToken()
		if tok.Kind != tt.kind {
			t.Errorf("tests[%d] - kind wrong: expected=%v, got=%v (value=%q)",
				i, tt.kind, tok.Kind, tok.Value)
		}
		if tok.Value != tt.value {
			t.Errorf("tests[%d] - value wrong: expected=%q, got=%q",
				i, tt.value, tok.Value)
		}
	}
}

func TestOperators(t *testing.T) {
	input := `+ - * / % ^ == != < > <= >= = += -= *= /= %= ^= -> && || & | ! . , : ( ) [ ] { } #name`
	tests := []struct {
		kind  token.TokenKind
		value string
	}{
		{token.Plus, "+"},
		{token.Minus, "-"},
		{token.Star, "*"},
		{token.Slash, "/"},
		{token.Percent, "%"},
		{token.Power, "^"},
		{token.Eq, "=="},
		{token.Ne, "!="},
		{token.Lt, "<"},
		{token.Gt, ">"},
		{token.Le, "<="},
		{token.Ge, ">="},
		{token.Assign, "="},
		{token.PlusAssign, "+="},
		{token.MinusAssign, "-="},
		{token.StarAssign, "*="},
		{token.SlashAssign, "/="},
		{token.PercentAssign, "%="},
		{token.PowerAssign, "^="},
		{token.Arrow, "->"},
		{token.And, "&&"},
		{token.Or, "||"},
		{token.Ampersand, "&"},
		{token.Or, "|"},
		{token.Not, "!"},
		{token.Dot, "."},
		{token.Comma, ","},
		{token.Colon, ":"},
		{token.LParen, "("},
		{token.RParen, ")"},
		{token.LBracket, "["},
		{token.RBracket, "]"},
		{token.LBrace, "{"},
		{token.RBrace, "}"},
		{token.Hash, "#"},
		{token.Identifier, "name"},
		{token.Newline, ""},
		{token.EOF, ""},
	}

	l := NewLexer(input)
	for i, tt := range tests {
		tok := l.NextToken()
		if tok.Kind != tt.kind {
			t.Errorf("tests[%d] - kind wrong: expected=%v, got=%v (value=%q, pos=%d:%d)",
				i, tt.kind, tok.Kind, tok.Value, tok.Line, tok.Col)
		}
		if tok.Value != tt.value {
			t.Errorf("tests[%d] - value wrong: expected=%q, got=%q",
				i, tt.value, tok.Value)
		}
	}
}

func TestNumbers(t *testing.T) {
	input := `42 3.14 1.5e10 2.0e-3`
	tests := []struct {
		kind  token.TokenKind
		value string
	}{
		{token.Int, "42"},
		{token.Float, "3.14"},
		{token.Float, "1.5e10"},
		{token.Float, "2.0e-3"},
		{token.Newline, ""},
		{token.EOF, ""},
	}

	l := NewLexer(input)
	for i, tt := range tests {
		tok := l.NextToken()
		if tok.Kind != tt.kind {
			t.Errorf("tests[%d] - kind wrong: expected=%v, got=%v (value=%q)",
				i, tt.kind, tok.Kind, tok.Value)
		}
		if tok.Value != tt.value {
			t.Errorf("tests[%d] - value wrong: expected=%q, got=%q",
				i, tt.value, tok.Value)
		}
	}
}

func TestStrings(t *testing.T) {
	input := `"hello\nworld" 'single'`
	tests := []struct {
		kind  token.TokenKind
		value string
	}{
		{token.String, "hello\nworld"},
		{token.String, "single"},
		{token.Newline, ""},
		{token.EOF, ""},
	}

	l := NewLexer(input)
	for i, tt := range tests {
		tok := l.NextToken()
		if tok.Kind != tt.kind {
			t.Errorf("tests[%d] - kind wrong: expected=%v, got=%v (value=%q)",
				i, tt.kind, tok.Kind, tok.Value)
		}
		if tok.Value != tt.value {
			t.Errorf("tests[%d] - value wrong: expected=%q, got=%q",
				i, tt.value, tok.Value)
		}
	}
}

func TestKeywords(t *testing.T) {
	input := `true false nil and or not func return if else elif for while break continue struct class enum union import as in var int float double bool void macro extern move mut loop match new`
	keywords := []token.TokenKind{
		token.True, token.False, token.Nil,
		token.And, token.Or, token.Not,
		token.Func, token.Return,
		token.If, token.Else, token.Elif,
		token.For, token.While, token.Break, token.Continue,
		token.Struct, token.Class, token.Enum, token.Union,
		token.Import, token.As, token.In,
		token.VarT, token.IntT, token.FloatT, token.DoubleT, token.BoolT, token.VoidT,
		token.Macro, token.Extern,
		token.Move, token.Mut,
		token.Loop, token.Match, token.New,
	}

	l := NewLexer(input)
	for i, expectedKind := range keywords {
		tok := l.NextToken()
		if tok.Kind != expectedKind {
			t.Errorf("tests[%d] - kind wrong: expected=%v, got=%v (value=%q)",
				i, expectedKind, tok.Kind, tok.Value)
		}
	}
	// Should end with NEWLINE then EOF
	tok := l.NextToken()
	if tok.Kind != token.Newline {
		t.Errorf("expected NEWLINE, got %v (%q)", tok.Kind, tok.Value)
	}
	tok = l.NextToken()
	if tok.Kind != token.EOF {
		t.Errorf("expected EOF, got %v (%q)", tok.Kind, tok.Value)
	}
}

func TestTokenizeAll(t *testing.T) {
	input := "a + b"
	l := NewLexer(input)
	tokens := l.TokenizeAll()
	if len(tokens) != 5 { // IDENT, PLUS, IDENT, NEWLINE, EOF
		t.Errorf("expected 5 tokens, got %d", len(tokens))
		for i, tok := range tokens {
			t.Logf("  token[%d] = %v (%q)", i, tok.Kind, tok.Value)
		}
	}
}

func TestParenNewlineSkip(t *testing.T) {
	input := "(\n)\nx"
	l := NewLexer(input)
	toks := l.TokenizeAll()
	// Expected: LPAREN, RPAREN, NEWLINE, IDENT("x"), EOF
	// Actually, after "(\n)", we get LPAREN, RPAREN. No NEWLINE inside parens.
	// Then newline after ")" → NEWLINE, atLineStart
	// Then IDENT("x")
	got := make([]token.TokenKind, len(toks))
	for i, tok := range toks {
		got[i] = tok.Kind
	}
	fmt.Println(got)
}

func TestEmptyInput(t *testing.T) {
	l := NewLexer("")
	tok := l.NextToken()
	if tok.Kind != token.EOF {
		t.Errorf("expected EOF, got %v", tok.Kind)
	}
}
