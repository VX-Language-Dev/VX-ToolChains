package lexer

import (
	"testing"

	"github.com/vx-lang/vxc/internal/token"
)

func TestLexerEdgeCases(t *testing.T) {
	tests := []struct {
		name     string
		input    string
		expected []struct {
			kind  token.TokenKind
			value string
		}
	}{
		{
			name:  "empty input",
			input: "",
			expected: []struct {
				kind  token.TokenKind
				value string
			}{
				{token.EOF, ""},
			},
		},
		{
			name:  "whitespace only",
			input: "   \t\n  ",
			expected: []struct {
				kind  token.TokenKind
				value string
			}{
				{token.Newline, ""},
				{token.EOF, ""},
			},
		},
		{
			name:  "single identifier",
			input: "hello",
			expected: []struct {
				kind  token.TokenKind
				value string
			}{
				{token.Ident, "hello"},
				{token.Newline, ""},
				{token.EOF, ""},
			},
		},
		{
			name:  "negative number",
			input: "-42",
			expected: []struct {
				kind  token.TokenKind
				value string
			}{
				{token.Minus, "-"},
				{token.Int, "42"},
				{token.Newline, ""},
				{token.EOF, ""},
			},
		},
		{
			name:  "hexadecimal literal",
			input: "0xFF",
			expected: []struct {
				kind  token.TokenKind
				value string
			}{
				{token.Int, "0xFF"},
				{token.Newline, ""},
				{token.EOF, ""},
			},
		},
		{
			name:  "binary literal",
			input: "0b1010",
			expected: []struct {
				kind  token.TokenKind
				value string
			}{
				{token.Int, "0b1010"},
				{token.Newline, ""},
				{token.EOF, ""},
			},
		},
		{
			name:  "octal literal",
			input: "0o777",
			expected: []struct {
				kind  token.TokenKind
				value string
			}{
				{token.Int, "0o777"},
				{token.Newline, ""},
				{token.EOF, ""},
			},
		},
	}

	for _, tt := range tests {
		t.Run(tt.name, func(t *testing.T) {
			l := NewLexer(tt.input)
			for i, expected := range tt.expected {
				tok := l.NextToken()
				if tok.Kind != expected.kind {
					t.Errorf("test[%d] - kind wrong: expected=%v, got=%v (value=%q)",
						i, expected.kind, tok.Kind, tok.Value)
				}
				if tok.Value != expected.value {
					t.Errorf("test[%d] - value wrong: expected=%q, got=%q",
						i, expected.value, tok.Value)
				}
			}
		})
	}
}

func TestLexerStringEscapes(t *testing.T) {
	input := `"hello\\world" "tab\there" "newline\nhere" "quote\"here" "null\x00here"`
	l := NewLexer(input)

	expectedValues := []string{
		`hello\world`,
		"tab\there",
		"newline\nhere",
		`quote"here`,
		"null\x00here",
	}

	for i, expected := range expectedValues {
		tok := l.NextToken()
		if tok.Kind != token.String {
			t.Errorf("test[%d] - kind wrong: expected=String, got=%v", i, tok.Kind)
		}
		if tok.Value != expected {
			t.Errorf("test[%d] - value wrong: expected=%q, got=%q", i, expected, tok.Value)
		}
	}
}

func TestLexerComments(t *testing.T) {
	input := `# This is a comment
x = 42 # inline comment
# Another comment`
	l := NewLexer(input)

	tokens := l.TokenizeAll()

	// Should have: IDENT(x), ASSIGN, INT(42), NEWLINE, EOF
	// Comments should be skipped
	foundIdent := false
	foundAssign := false
	foundInt := false

	for _, tok := range tokens {
		switch tok.Kind {
		case token.Ident:
			if tok.Value == "x" {
				foundIdent = true
			}
		case token.Assign:
			foundAssign = true
		case token.Int:
			if tok.Value == "42" {
				foundInt = true
			}
		case token.Comment:
			t.Error("comments should be skipped")
		}
	}

	if !foundIdent {
		t.Error("expected identifier 'x'")
	}
	if !foundAssign {
		t.Error("expected assignment operator")
	}
	if !foundInt {
		t.Error("expected integer '42'")
	}
}

func TestLexerIndentation(t *testing.T) {
	input := `if true:
    x = 1
    y = 2
z = 3
`
	l := NewLexer(input)
	tokens := l.TokenizeAll()

	hasIndent := false
	hasDedent := false

	for _, tok := range tokens {
		switch tok.Kind {
		case token.Indent:
			hasIndent = true
		case token.Dedent:
			hasDedent = true
		}
	}

	if !hasIndent {
		t.Error("expected INDENT tokens")
	}
	if !hasDedent {
		t.Error("expected DEDENT tokens")
	}
}

func TestLexerComplexExpression(t *testing.T) {
	input := `result = (a + b) * c / d ^ e % f`
	l := NewLexer(input)
	tokens := l.TokenizeAll()

	expectedKinds := []token.TokenKind{
		token.Ident,
		token.Assign,
		token.LParen,
		token.Ident,
		token.Plus,
		token.Ident,
		token.RParen,
		token.Star,
		token.Ident,
		token.Slash,
		token.Ident,
		token.Power,
		token.Ident,
		token.Percent,
		token.Ident,
		token.Newline,
		token.EOF,
	}

	if len(tokens) != len(expectedKinds) {
		t.Fatalf("expected %d tokens, got %d", len(expectedKinds), len(tokens))
	}

	for i, expected := range expectedKinds {
		if tokens[i].Kind != expected {
			t.Errorf("token[%d] - kind wrong: expected=%v, got=%v (value=%q)",
				i, expected, tokens[i].Kind, tokens[i].Value)
		}
	}
}

func TestLexerFunctionDefinition(t *testing.T) {
	input := `func add(a: int, b: int) -> int:
    return a + b
`
	l := NewLexer(input)
	tokens := l.TokenizeAll()

	expectedSequence := []struct {
		kind  token.TokenKind
		value string
	}{
		{token.Func, "func"},
		{token.Ident, "add"},
		{token.LParen, "("},
		{token.Ident, "a"},
		{token.Colon, ":"},
		{token.IntT, "int"},
		{token.Comma, ","},
		{token.Ident, "b"},
		{token.Colon, ":"},
		{token.IntT, "int"},
		{token.RParen, ")"},
		{token.Arrow, "->"},
		{token.IntT, "int"},
		{token.Colon, ":"},
		{token.Newline, ""},
		{token.Indent, ""},
		{token.Return, "return"},
		{token.Ident, "a"},
		{token.Plus, "+"},
		{token.Ident, "b"},
		{token.Newline, ""},
		{token.Dedent, ""},
		{token.EOF, ""},
	}

	if len(tokens) != len(expectedSequence) {
		t.Fatalf("expected %d tokens, got %d", len(expectedSequence), len(tokens))
	}

	for i, expected := range expectedSequence {
		if tokens[i].Kind != expected.kind {
			t.Errorf("token[%d] - kind wrong: expected=%v, got=%v (value=%q)",
				i, expected.kind, tokens[i].Kind, tokens[i].Value)
		}
		if tokens[i].Value != expected.value {
			t.Errorf("token[%d] - value wrong: expected=%q, got=%q",
				i, expected.value, tokens[i].Value)
		}
	}
}

func TestLexerTokenPosition(t *testing.T) {
	input := `hello
world
`
	l := NewLexer(input)

	tok1 := l.NextToken()
	if tok1.Line != 1 || tok1.Col != 1 {
		t.Errorf("first token position wrong: expected (1,1), got (%d,%d)", tok1.Line, tok1.Col)
	}

	tok2 := l.NextToken()
	if tok2.Line != 1 || tok2.Col != 6 {
		t.Errorf("second token position wrong: expected (1,6), got (%d,%d)", tok2.Line, tok2.Col)
	}
}
