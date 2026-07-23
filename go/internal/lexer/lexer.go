package lexer

import (
	"strings"

	"github.com/vx-lang/vxc/internal/token"
)

// Lexer implements a hand-written lexer for the vx language.
type Lexer struct {
	input string
	pos   int
	line  int
	col   int

	prevLine int // line of the current character (for token positioning)
	prevCol  int // col of the current character
	ch       byte

	// Indentation handling
	indentStack   []int
	pendingTokens []token.Token
	atLineStart   bool
	parenLevel    int

	// String buffer
	buf strings.Builder
}

// NewLexer creates a new Lexer for the given input string.
func NewLexer(input string) *Lexer {
	l := &Lexer{
		input:       input,
		line:        1,
		col:         0,
		indentStack: []int{0},
		atLineStart: true,
	}
	l.readChar()
	return l
}

// readChar advances one character and updates position tracking.
func (l *Lexer) readChar() {
	if l.pos >= len(l.input) {
		l.ch = 0
		return
	}
	l.ch = l.input[l.pos]
	l.prevLine = l.line
	l.prevCol = l.col
	l.pos++

	if l.ch == '\n' {
		l.line++
		l.col = 0
	} else {
		l.col++
	}
}

// peekChar returns the next character without advancing.
func (l *Lexer) peekChar() byte {
	if l.pos >= len(l.input) {
		return 0
	}
	return l.input[l.pos]
}

// skipWhitespace skips spaces, tabs, and carriage returns (not newlines).
func (l *Lexer) skipWhitespace() {
	for l.ch == ' ' || l.ch == '\t' || l.ch == '\r' {
		l.readChar()
	}
}

// readIdentifier reads a complete identifier (letters, digits, underscores).
func (l *Lexer) readIdentifier() string {
	l.buf.Reset()
	for isLetter(l.ch) || isDigit(l.ch) || l.ch == '_' {
		l.buf.WriteByte(l.ch)
		l.readChar()
	}
	return l.buf.String()
}

// readNumber reads a numeric literal and returns (literal, isFloat).
// Float detection: digits followed by '.' and at least one digit.
// Float may also include an optional exponent.
func (l *Lexer) readNumber() (string, bool) {
	l.buf.Reset()
	for isDigit(l.ch) {
		l.buf.WriteByte(l.ch)
		l.readChar()
	}

	isFloat := false
	if l.ch == '.' && isDigit(l.peekChar()) {
		isFloat = true
		l.buf.WriteByte(l.ch)
		l.readChar()
		for isDigit(l.ch) {
			l.buf.WriteByte(l.ch)
			l.readChar()
		}
		// Optional exponent
		if l.ch == 'e' || l.ch == 'E' {
			l.buf.WriteByte(l.ch)
			l.readChar()
			if l.ch == '+' || l.ch == '-' {
				l.buf.WriteByte(l.ch)
				l.readChar()
			}
			for isDigit(l.ch) {
				l.buf.WriteByte(l.ch)
				l.readChar()
			}
		}
	}

	return l.buf.String(), isFloat
}

// readString reads a string literal until the closing quote.
// The opening quote has already been consumed.
// Returns the string content (without quotes), with escape sequences resolved.
func (l *Lexer) readString(quote byte) string {
	l.buf.Reset()
	for {
		if l.ch == quote {
			l.readChar() // consume closing quote
			break
		}
		if l.ch == '\\' {
			l.readChar() // consume backslash
			switch l.ch {
			case 'n':
				l.buf.WriteByte('\n')
			case 't':
				l.buf.WriteByte('\t')
			case 'r':
				l.buf.WriteByte('\r')
			case '\\':
				l.buf.WriteByte('\\')
			case '"':
				l.buf.WriteByte('"')
			case '\'':
				l.buf.WriteByte('\'')
			default:
				l.buf.WriteByte('\\')
				if l.ch != 0 {
					l.buf.WriteByte(l.ch)
				}
			}
			if l.ch != 0 {
				l.readChar()
			}
		} else if l.ch == 0 || l.ch == '\n' {
			// Unterminated string — stop and return what we have
			break
		} else {
			l.buf.WriteByte(l.ch)
			l.readChar()
		}
	}
	return l.buf.String()
}

// skipToRealToken scans from the current position, skipping whitespace,
// empty lines, and comments, until the first real (non-whitespace, non-comment)
// character is found. Returns the indentation level (column of the first real
// character). After this call, l.ch points at the first real character
// (or 0 if EOF was reached).
func (l *Lexer) skipToRealToken() int {
	indent := 0
	for {
		for l.ch == ' ' {
			indent++
			l.readChar()
		}
		for l.ch == '\t' {
			indent += 4
			l.readChar()
		}

		// Comment line — skip rest of line and reset indent
		if l.ch == '#' {
			for l.ch != 0 && l.ch != '\n' && l.ch != '\r' {
				l.readChar()
			}
			indent = 0
			continue
		}

		// Empty line
		if l.ch == '\n' {
			l.readChar()
			indent = 0
			continue
		}
		if l.ch == '\r' {
			l.readChar()
			if l.ch == '\n' {
				l.readChar()
			}
			indent = 0
			continue
		}

		break
	}
	return indent
}

// handleIndent compares the given indent level with the indent stack and
// generates INDENT/DEDENT tokens as needed, appending them to pendingTokens.
func (l *Lexer) handleIndent(indent int) {
	top := l.indentStack[len(l.indentStack)-1]

	if indent > top {
		l.indentStack = append(l.indentStack, indent)
		l.pendingTokens = append(l.pendingTokens, token.Token{
			Kind: token.Indent, Value: "", Line: l.prevLine, Col: l.prevCol,
		})
	} else if indent < top {
		for len(l.indentStack) > 1 && l.indentStack[len(l.indentStack)-1] > indent {
			l.indentStack = l.indentStack[:len(l.indentStack)-1]
			l.pendingTokens = append(l.pendingTokens, token.Token{
				Kind: token.Dedent, Value: "", Line: l.prevLine, Col: l.prevCol,
			})
		}
		if l.indentStack[len(l.indentStack)-1] != indent {
			l.pendingTokens = append(l.pendingTokens, token.Token{
				Kind: token.Illegal, Value: "indentation mismatch",
				Line: l.prevLine, Col: l.prevCol,
			})
		}
	}
	// If equal, nothing to do
}

// NextToken returns the next token from the input.
func (l *Lexer) NextToken() token.Token {
	// 1. Drain pending tokens (INDENT/DEDENT from indentation handling)
	if len(l.pendingTokens) > 0 {
		tok := l.pendingTokens[0]
		l.pendingTokens = l.pendingTokens[1:]
		return tok
	}

	// 2. Handle indentation if we are at the start of a line
	if l.atLineStart {
		l.atLineStart = false
		indent := l.skipToRealToken()

		// If we reached EOF while scanning for indentation, emit final DEDENTs
		if l.ch == 0 {
			for len(l.indentStack) > 1 {
				l.indentStack = l.indentStack[:len(l.indentStack)-1]
				l.pendingTokens = append(l.pendingTokens, token.Token{
					Kind: token.Dedent, Value: "", Line: l.prevLine, Col: l.prevCol,
				})
			}
			if len(l.pendingTokens) > 0 {
				// Queue EOF after all final DEDENTs
				l.pendingTokens = append(l.pendingTokens, token.Token{
					Kind: token.EOF, Value: "", Line: l.prevLine, Col: l.prevCol,
				})
				tok := l.pendingTokens[0]
				l.pendingTokens = l.pendingTokens[1:]
				return tok
			}
			return token.Token{Kind: token.EOF, Value: "", Line: l.prevLine, Col: l.prevCol}
		}

		l.handleIndent(indent)
		if len(l.pendingTokens) > 0 {
			tok := l.pendingTokens[0]
			l.pendingTokens = l.pendingTokens[1:]
			return tok
		}
	}

	// 3. Skip inline whitespace (spaces, tabs, carriage returns)
	l.skipWhitespace()

	// 4. Save token start position
	tokLine := l.prevLine
	tokCol := l.prevCol

	// 5. EOF
	if l.ch == 0 {
		// If we're not at line start, emit a NEWLINE first (file didn't end
		// with a newline, but the parser expects NEWLINE-terminated lines).
		if !l.atLineStart {
			l.pendingTokens = append(l.pendingTokens, token.Token{
				Kind: token.Newline, Value: "", Line: l.prevLine, Col: l.prevCol,
			})
		}
		// Emit remaining DEDENTs to balance the stack
		for len(l.indentStack) > 1 {
			l.indentStack = l.indentStack[:len(l.indentStack)-1]
			l.pendingTokens = append(l.pendingTokens, token.Token{
				Kind: token.Dedent, Value: "", Line: l.prevLine, Col: l.prevCol,
			})
		}
		if len(l.pendingTokens) > 0 {
			l.pendingTokens = append(l.pendingTokens, token.Token{
				Kind: token.EOF, Value: "", Line: l.prevLine, Col: l.prevCol,
			})
			tok := l.pendingTokens[0]
			l.pendingTokens = l.pendingTokens[1:]
			return tok
		}
		return token.Token{Kind: token.EOF, Value: "", Line: l.prevLine, Col: l.prevCol}
	}

	// 6. Dispatch based on current character
	switch l.ch {
	case '\n':
		l.readChar()
		// Inside parentheses, newlines are ignored
		if l.parenLevel > 0 {
			return l.NextToken()
		}
		l.atLineStart = true
		return token.Token{Kind: token.Newline, Value: "", Line: tokLine, Col: tokCol}

	case '\r':
		l.readChar()
		if l.ch == '\n' {
			l.readChar()
		}
		if l.parenLevel > 0 {
			return l.NextToken()
		}
		l.atLineStart = true
		return token.Token{Kind: token.Newline, Value: "", Line: tokLine, Col: tokCol}

	case '"':
		l.readChar() // consume opening quote
		content := l.readString('"')
		return token.Token{Kind: token.String, Value: content, Line: tokLine, Col: tokCol}

	case '\'':
		l.readChar() // consume opening quote
		content := l.readString('\'')
		return token.Token{Kind: token.String, Value: content, Line: tokLine, Col: tokCol}

	case '+':
		l.readChar()
		if l.ch == '=' {
			l.readChar()
			return token.Token{Kind: token.PlusAssign, Value: "+=", Line: tokLine, Col: tokCol}
		}
		return token.Token{Kind: token.Plus, Value: "+", Line: tokLine, Col: tokCol}

	case '-':
		l.readChar()
		if l.ch == '=' {
			l.readChar()
			return token.Token{Kind: token.MinusAssign, Value: "-=", Line: tokLine, Col: tokCol}
		}
		if l.ch == '>' {
			l.readChar()
			return token.Token{Kind: token.Arrow, Value: "->", Line: tokLine, Col: tokCol}
		}
		return token.Token{Kind: token.Minus, Value: "-", Line: tokLine, Col: tokCol}

	case '*':
		l.readChar()
		if l.ch == '=' {
			l.readChar()
			return token.Token{Kind: token.StarAssign, Value: "*=", Line: tokLine, Col: tokCol}
		}
		return token.Token{Kind: token.Star, Value: "*", Line: tokLine, Col: tokCol}

	case '/':
		l.readChar()
		if l.ch == '=' {
			l.readChar()
			return token.Token{Kind: token.SlashAssign, Value: "/=", Line: tokLine, Col: tokCol}
		}
		return token.Token{Kind: token.Slash, Value: "/", Line: tokLine, Col: tokCol}

	case '%':
		l.readChar()
		if l.ch == '=' {
			l.readChar()
			return token.Token{Kind: token.PercentAssign, Value: "%=", Line: tokLine, Col: tokCol}
		}
		return token.Token{Kind: token.Percent, Value: "%", Line: tokLine, Col: tokCol}

	case '^':
		l.readChar()
		if l.ch == '=' {
			l.readChar()
			return token.Token{Kind: token.PowerAssign, Value: "^=", Line: tokLine, Col: tokCol}
		}
		return token.Token{Kind: token.Power, Value: "^", Line: tokLine, Col: tokCol}

	case '=':
		l.readChar()
		if l.ch == '=' {
			l.readChar()
			return token.Token{Kind: token.Eq, Value: "==", Line: tokLine, Col: tokCol}
		}
		return token.Token{Kind: token.Assign, Value: "=", Line: tokLine, Col: tokCol}

	case '!':
		l.readChar()
		if l.ch == '=' {
			l.readChar()
			return token.Token{Kind: token.Ne, Value: "!=", Line: tokLine, Col: tokCol}
		}
		return token.Token{Kind: token.Not, Value: "!", Line: tokLine, Col: tokCol}

	case '<':
		l.readChar()
		if l.ch == '=' {
			l.readChar()
			return token.Token{Kind: token.Le, Value: "<=", Line: tokLine, Col: tokCol}
		}
		return token.Token{Kind: token.Lt, Value: "<", Line: tokLine, Col: tokCol}

	case '>':
		l.readChar()
		if l.ch == '=' {
			l.readChar()
			return token.Token{Kind: token.Ge, Value: ">=", Line: tokLine, Col: tokCol}
		}
		return token.Token{Kind: token.Gt, Value: ">", Line: tokLine, Col: tokCol}

	case '&':
		l.readChar()
		if l.ch == '&' {
			l.readChar()
			return token.Token{Kind: token.And, Value: "&&", Line: tokLine, Col: tokCol}
		}
		return token.Token{Kind: token.Ampersand, Value: "&", Line: tokLine, Col: tokCol}

	case '|':
		l.readChar()
		if l.ch == '|' {
			l.readChar()
			return token.Token{Kind: token.Or, Value: "||", Line: tokLine, Col: tokCol}
		}
		return token.Token{Kind: token.Or, Value: "|", Line: tokLine, Col: tokCol}

	case '(':
		l.readChar()
		l.parenLevel++
		return token.Token{Kind: token.LParen, Value: "(", Line: tokLine, Col: tokCol}

	case ')':
		l.readChar()
		l.parenLevel--
		return token.Token{Kind: token.RParen, Value: ")", Line: tokLine, Col: tokCol}

	case '[':
		l.readChar()
		return token.Token{Kind: token.LBracket, Value: "[", Line: tokLine, Col: tokCol}

	case ']':
		l.readChar()
		return token.Token{Kind: token.RBracket, Value: "]", Line: tokLine, Col: tokCol}

	case '{':
		l.readChar()
		return token.Token{Kind: token.LBrace, Value: "{", Line: tokLine, Col: tokCol}

	case '}':
		l.readChar()
		return token.Token{Kind: token.RBrace, Value: "}", Line: tokLine, Col: tokCol}

	case ':':
		l.readChar()
		return token.Token{Kind: token.Colon, Value: ":", Line: tokLine, Col: tokCol}

	case ',':
		l.readChar()
		return token.Token{Kind: token.Comma, Value: ",", Line: tokLine, Col: tokCol}

	case '.':
		l.readChar()
		return token.Token{Kind: token.Dot, Value: ".", Line: tokLine, Col: tokCol}

	case '#':
		if l.peekChar() != 0 && l.peekChar() != '\n' && l.peekChar() != '\r' && l.peekChar() != ' ' && l.peekChar() != '\t' {
			l.readChar()
			return token.Token{Kind: token.Hash, Value: "#", Line: tokLine, Col: tokCol}
		}
		// Comment — skip to end of line
		for l.ch != 0 && l.ch != '\n' && l.ch != '\r' {
			l.readChar()
		}
		return l.NextToken()
	}

	// 7. Identifiers and keywords
	if isLetter(l.ch) || l.ch == '_' {
		savedLine := l.prevLine
		savedCol := l.prevCol
		ident := l.readIdentifier()
		if kind, ok := token.LookupKeyword(ident); ok {
			return token.Token{Kind: kind, Value: ident, Line: savedLine, Col: savedCol}
		}
		return token.Token{Kind: token.Ident, Value: ident, Line: savedLine, Col: savedCol}
	}

	// 8. Numbers
	if isDigit(l.ch) {
		savedLine := l.prevLine
		savedCol := l.prevCol
		lit, isFloat := l.readNumber()
		if isFloat {
			return token.Token{Kind: token.Float, Value: lit, Line: savedLine, Col: savedCol}
		}
		return token.Token{Kind: token.Int, Value: lit, Line: savedLine, Col: savedCol}
	}

	// 9. Unexpected character
	ch := l.ch
	l.readChar()
	return token.Token{
		Kind: token.Illegal, Value: string(ch),
		Line: tokLine, Col: tokCol,
	}
}

// TokenizeAll collects all remaining tokens until EOF.
func (l *Lexer) TokenizeAll() []token.Token {
	var tokens []token.Token
	for {
		tok := l.NextToken()
		tokens = append(tokens, tok)
		if tok.Kind == token.EOF {
			break
		}
	}
	return tokens
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

func isLetter(ch byte) bool {
	return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z')
}

func isDigit(ch byte) bool {
	return ch >= '0' && ch <= '9'
}
