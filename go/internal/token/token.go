package token

import (
	"fmt"
	"strconv"
	"strings"
	"unicode/utf8"
)

// ==================== 错误处理 ====================

// Severity 诊断严重程度
type Severity int

const (
	SeverityError Severity = iota
	SeverityWarning
	SeverityNote
	SeverityHelp
)

func (s Severity) String() string {
	switch s {
	case SeverityError:
		return "error"
	case SeverityWarning:
		return "warning"
	case SeverityNote:
		return "note"
	case SeverityHelp:
		return "help"
	default:
		return "unknown"
	}
}

// VXErrorKind 错误/诊断类别
type VXErrorKind int

const (
	LexicalKind VXErrorKind = iota
	SyntaxKind
	TypeKind
	OwnershipKind
	CompileKind
	LinkKind
	IoKind
	OtherKind
)

// Code 返回对应的错误代码字符串
func (k VXErrorKind) Code() string {
	switch k {
	case LexicalKind:
		return "E0001"
	case SyntaxKind:
		return "E0002"
	case TypeKind:
		return "E0003"
	case OwnershipKind:
		return "E0004"
	case CompileKind:
		return "E0005"
	case LinkKind:
		return "E0006"
	case IoKind:
		return "E0007"
	case OtherKind:
		return "E9999"
	default:
		return "E9999"
	}
}

func (k VXErrorKind) String() string {
	return k.Code()
}

// VXSpan 源代码位置区间（结构化 span）
type VXSpan struct {
	StartLine int
	StartCol  int
	EndLine   int
	EndCol    int
}

// NewSpan 创建一个区间 span
func NewSpan(startLine, startCol, endLine, endCol int) VXSpan {
	return VXSpan{
		StartLine: startLine,
		StartCol:  startCol,
		EndLine:   endLine,
		EndCol:    endCol,
	}
}

// Point 创建一个单点 span
func Point(line, col int) VXSpan {
	return NewSpan(line, col, line, col)
}

// VXRelatedInfo 相关位置信息（用于辅助诊断）
type VXRelatedInfo struct {
	Span    VXSpan
	Message string
}

// VXError VX 结构化错误类型
//
// 注意：此结构体的字段包含完整的字符串副本。
type VXError struct {
	Msg      string
	Line     int
	Col      int
	EndLine  int
	EndCol   int
	Source   string
	Severity Severity
	Kind     VXErrorKind
	Code     string
	Related  []VXRelatedInfo
}

// NewVXError 构造一个编译错误，默认位于单点 span
func NewVXError(msg string, line, col int) VXError {
	return VXError{
		Msg:      msg,
		Line:     line,
		Col:      col,
		EndLine:  line,
		EndCol:   col,
		Severity: SeverityError,
		Kind:     CompileKind,
		Code:     CompileKind.Code(),
	}
}

// WithSpan 指定完整的 span 区间
func (e VXError) WithSpan(span VXSpan) VXError {
	e.Line = span.StartLine
	e.Col = span.StartCol
	e.EndLine = span.EndLine
	e.EndCol = span.EndCol
	return e
}

// WithSource 指定源码上下文
func (e VXError) WithSource(source string) VXError {
	e.Source = source
	return e
}

// WithSeverity 指定严重程度
func (e VXError) WithSeverity(severity Severity) VXError {
	e.Severity = severity
	return e
}

// WithKind 指定错误类别和代码
func (e VXError) WithKind(kind VXErrorKind) VXError {
	e.Kind = kind
	e.Code = kind.Code()
	return e
}

// WithRelated 添加相关位置信息
func (e VXError) WithRelated(related []VXRelatedInfo) VXError {
	e.Related = related
	return e
}

// Message 返回可读字符串（不含源码上下文）
func (e VXError) Message() string {
	return e.Msg
}

// String 格式化输出，类似 Rust 的 Display 实现
func (e VXError) String() string {
	var sb strings.Builder
	sb.WriteString(fmt.Sprintf("VX %s [%s] [line %d, col %d]: %s",
		e.Severity, e.Kind, e.Line, e.Col, e.Msg))
	if e.Source != "" && e.Line > 0 {
		lines := strings.Split(e.Source, "\n")
		if e.Line <= len(lines) {
			lineStr := lines[e.Line-1]
			underlineLen := 1
			if e.EndLine == e.Line && e.EndCol >= e.Col {
				underlineLen = e.EndCol - e.Col
				if underlineLen < 1 {
					underlineLen = 1
				}
			}
			sb.WriteString("\n")
			sb.WriteString(fmt.Sprintf("%d | %s", e.Line, lineStr))
			sb.WriteString("\n | ")
			pad := 0
			if e.Col > 0 {
				pad = e.Col - 1
			}
			sb.WriteString(strings.Repeat(" ", pad))
			sb.WriteString(strings.Repeat("^", underlineLen))
		}
	}
	return sb.String()
}

// NewVXLexError 创建 VXError（替代 vx_error! 宏的构造功能）
// 返回一个 Lexer 类型的编译错误，用于词法分析阶段。
func NewVXLexError(msg string, line, col int, source string) VXError {
	return VXError{
		Msg:      msg,
		Line:     line,
		Col:      col,
		EndLine:  line,
		EndCol:   col,
		Source:   source,
		Severity: SeverityError,
		Kind:     LexicalKind,
		Code:     LexicalKind.Code(),
	}
}

// ==================== Token 类型 ====================

// TokenType 词法单元类型
type TokenType int

const (
	// 控制流
	If TokenType = iota
	Elif
	Else
	For
	While
	Break
	Continue
	// 函数
	Func
	Return
	// 循环
	Loop
	Match
	// 所有权
	Move
	Mut
	// 字面量
	Int
	Float
	String
	Identifier
	// 运算符
	Plus
	Minus
	Star
	Slash
	Percent
	Power
	Eq
	Ne
	Lt
	Gt
	Le
	Ge
	Assign
	PlusAssign
	MinusAssign
	StarAssign
	SlashAssign
	PercentAssign
	PowerAssign
	Ampersand
	Arrow
	// 逻辑运算符 (自举兼容: and/or/not 关键字 + 符号形式 &&/||/! 并存)
	And
	Or
	Not
	// 分隔符
	LParen
	RParen
	LBracket
	RBracket
	LBrace
	RBrace
	Colon
	Semicolon
	Comma
	Dot
	Newline
	Indent
	Dedent
	EOF
	// 布尔/零值
	True
	False
	Nil
	// 迭代/导入
	In
	Import
	As
	// 原生标量类型 (硬件基础类型, 必留)
	IntT
	FloatT
	DoubleT
	VarT
	BoolT
	VoidT
	// 复合类型声明
	Struct
	Class
	Enum
	Union
	// 内存分配/构造
	New
	// 宏系统
	Macro
	Hash
	// 外部/FFI
	Extern
	// 非法字符
	Illegal
)

// 向后兼容别名
const Ident = Identifier

// Token 词法单元
type Token struct {
	Kind  TokenType
	Value string
	Line  int
	Col   int
}

// ==================== 关键字表 ====================

// KeywordEntry 关键字条目类型
type KeywordEntry struct {
	Name string
	Kind TokenType
}

// KeywordEntries 关键字常量表（与 Zig 版 KEYWORDS 完全对应）
var KeywordEntries = []KeywordEntry{
	// 22 核心骨架关键字 (底层 OpCode 绑定, 永久保留)
	{Name: "if", Kind: If},
	{Name: "else", Kind: Else},
	{Name: "elif", Kind: Elif},
	{Name: "for", Kind: For},
	{Name: "while", Kind: While},
	{Name: "break", Kind: Break},
	{Name: "continue", Kind: Continue},
	{Name: "func", Kind: Func},
	{Name: "return", Kind: Return},
	{Name: "loop", Kind: Loop},
	{Name: "match", Kind: Match},
	{Name: "true", Kind: True},
	{Name: "false", Kind: False},
	{Name: "nil", Kind: Nil},
	{Name: "in", Kind: In},
	{Name: "import", Kind: Import},
	{Name: "as", Kind: As},
	{Name: "var", Kind: VarT},
	{Name: "struct", Kind: Struct},
	{Name: "class", Kind: Class},
	{Name: "enum", Kind: Enum},
	{Name: "union", Kind: Union},
	{Name: "new", Kind: New},
	{Name: "move", Kind: Move},
	{Name: "mut", Kind: Mut},
	{Name: "macro", Kind: Macro},
	{Name: "extern", Kind: Extern},
	// 5 原生标量类型 (硬件基础类型, 保留)
	{Name: "int", Kind: IntT},
	{Name: "float", Kind: FloatT},
	{Name: "double", Kind: DoubleT},
	{Name: "bool", Kind: BoolT},
	{Name: "void", Kind: VoidT},
	// 逻辑运算符关键字 (与符号形式 &&/||/! 并存, 自举代码大量使用)
	{Name: "and", Kind: And},
	{Name: "or", Kind: Or},
	{Name: "not", Kind: Not},
}

// keywordMap 编译期关键字查找表
var keywordMap map[string]TokenType

// Keywords 关键字查找表（向后兼容，供 lexer 等模块直接使用 map 查找）
var Keywords map[string]TokenType

// keywordKindSet 提供按 TokenType 快速判断是否为关键字的能力
var keywordKindSet map[TokenType]bool

func init() {
	keywordMap = make(map[string]TokenType, len(KeywordEntries))
	Keywords = make(map[string]TokenType, len(KeywordEntries))
	keywordKindSet = make(map[TokenType]bool, len(KeywordEntries))
	for _, kw := range KeywordEntries {
		keywordMap[kw.Name] = kw.Kind
		Keywords[kw.Name] = kw.Kind
		keywordKindSet[kw.Kind] = true
	}
}

// LookupKeyword looks up a keyword by name and returns its kind.
func LookupKeyword(name string) (TokenType, bool) {
	kind, ok := keywordMap[name]
	return kind, ok
}

// IsKeywordKind 判断给定的 TokenType 是否为关键字类型。
func IsKeywordKind(kind TokenType) bool {
	return keywordKindSet[kind]
}

// ==================== 向后兼容 ====================

// TokenKind 是 TokenType 的别名，保持与旧代码的兼容性
type TokenKind = TokenType

// ==================== 词法分析器 ====================

// Lexer 词法分析器
type Lexer struct {
	source string
	// 下一个待消费字符在 source 中的字节偏移
	pos  int
	line int
	// 当前字符的列号（1-based）。换行后重置为 1。
	col                 int
	tokens              []Token
	indentStack         []int
	continuationPending bool
	// 最近一次 Lexer 错误（词法错误时设置）
	lastError *VXError
}

// NewLexer 创建一个新的词法分析器
func NewLexer(source string) *Lexer {
	return &Lexer{
		source:      source,
		pos:         0,
		line:        1,
		col:         1,
		tokens:      nil,
		indentStack: []int{0},
	}
}

// setError 设置词法错误（使用静态消息）
func (l *Lexer) setError(msg string) {
	l.lastError = &VXError{
		Msg:      msg,
		Line:     l.line,
		Col:      l.col,
		EndLine:  l.line,
		EndCol:   l.col,
		Source:   l.source,
		Severity: SeverityError,
		Kind:     LexicalKind,
		Code:     "E0001",
	}
}

// setErrorFmt 设置词法错误（使用格式化消息）
func (l *Lexer) setErrorFmt(format string, args ...any) {
	l.setError(fmt.Sprintf(format, args...))
}

// peek 返回从 pos 起第 offset 个 Unicode 码点，不会越界。
// 对于超出末尾的情况返回 0（与旧的字节语义保持一致）。
func (l *Lexer) peek(offset int) rune {
	remaining := l.source[l.pos:]
	i := 0
	for i <= offset {
		if len(remaining) == 0 {
			return 0
		}
		r, size := utf8.DecodeRuneInString(remaining)
		if r == utf8.RuneError && size == 1 {
			r = rune(remaining[0])
		}
		if i == offset {
			return r
		}
		remaining = remaining[size:]
		i++
	}
	return 0
}

// advance 消费一个 Unicode 码点并返回它，同时更新 pos（字节偏移）、line、col。
func (l *Lexer) advance() rune {
	c := l.peek(0)
	if c == 0 {
		return 0
	}
	_, size := utf8.DecodeRuneInString(l.source[l.pos:])
	l.pos += size
	if c == '\n' {
		l.line++
		l.col = 1
	} else {
		l.col++
	}
	return c
}

func (l *Lexer) skipWhitespace() {
	for {
		c := l.peek(0)
		if c != ' ' && c != '\t' && c != '\r' && c != '\u3000' {
			break
		}
		l.advance()
	}
}

func (l *Lexer) readString(q rune) (string, error) {
	// 注意：此处使用字节级操作，因为字符串内容本质上是字节序列
	l.advance() // 消费开引号
	var sb strings.Builder
	for {
		c := l.peek(0)
		if c == q || c == 0 {
			break
		}
		if c == '\\' {
			l.advance()
			e := l.advance()
			switch e {
			case 'n':
				sb.WriteByte('\n')
			case 't':
				sb.WriteByte('\t')
			case 'r':
				sb.WriteByte('\r')
			case '"':
				sb.WriteByte('"')
			case '\'':
				sb.WriteByte('\'')
			case '\\':
				sb.WriteByte('\\')
			default:
				sb.WriteByte(byte(e))
			}
		} else {
			// 将 Unicode 码点编码为 UTF-8 字节
			var buf [4]byte
			n := utf8.EncodeRune(buf[:], c)
			sb.Write(buf[:n])
			l.advance()
		}
	}
	if l.peek(0) != q {
		l.setError("未闭合字符串")
		return "", fmt.Errorf("未闭合字符串")
	}
	l.advance()
	return sb.String(), nil
}

func (l *Lexer) readNumber() Token {
	sl := l.line
	sc := l.col
	var sb strings.Builder
	isFloat := false

	for l.peek(0) >= '0' && l.peek(0) <= '9' {
		sb.WriteRune(l.advance())
	}
	if l.peek(0) == '.' && l.peek(1) >= '0' && l.peek(1) <= '9' {
		isFloat = true
		sb.WriteRune(l.advance()) // '.'
		for l.peek(0) >= '0' && l.peek(0) <= '9' {
			sb.WriteRune(l.advance())
		}
	}
	if l.peek(0) == 'e' || l.peek(0) == 'E' {
		isFloat = true
		sb.WriteRune(l.advance()) // 'e'/'E'
		if l.peek(0) == '+' || l.peek(0) == '-' {
			sb.WriteRune(l.advance())
		}
		for l.peek(0) >= '0' && l.peek(0) <= '9' {
			sb.WriteRune(l.advance())
		}
	}
	// 标准化数字表示
	var val string
	if isFloat {
		n, err := strconv.ParseFloat(sb.String(), 64)
		if err != nil {
			n = 0.0
		}
		val = fmt.Sprintf("%g", n)
	} else {
		n, err := strconv.ParseInt(sb.String(), 10, 64)
		if err != nil {
			n = 0
		}
		val = fmt.Sprintf("%d", n)
	}
	kind := Int
	if isFloat {
		kind = Float
	}
	return Token{
		Kind:  kind,
		Value: val,
		Line:  sl,
		Col:   sc,
	}
}

func (l *Lexer) readIdentifier() Token {
	sl := l.line
	sc := l.col
	var sb strings.Builder
	for {
		c := l.peek(0)
		if c == 0 {
			break
		}
		isAsciiAlnum := c < 128 && ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'))
		isCJK := c >= 0x4E00 && c <= 0x9FFF
		if !isAsciiAlnum && c != '_' && !isCJK {
			break
		}
		// 复制原始字节
		_, size := utf8.DecodeRuneInString(l.source[l.pos:])
		sb.WriteString(l.source[l.pos : l.pos+size])
		l.advance()
	}
	kind := Identifier
	if k, ok := keywordMap[sb.String()]; ok {
		kind = k
	}
	return Token{
		Kind:  kind,
		Value: sb.String(),
		Line:  sl,
		Col:   sc,
	}
}

func (l *Lexer) handleIndent() error {
	for {
		if l.peek(0) != '\n' && len(l.tokens) == 0 {
			return nil
		}
		for l.peek(0) == '\n' {
			l.advance()
		}
		if l.peek(0) == 0 {
			return nil
		}
		indent := 0
		for l.peek(0) == ' ' || l.peek(0) == '\u3000' {
			indent++
			l.advance()
		}
		for l.peek(0) == '\t' {
			indent += 4
			l.advance()
		}
		// 跳过空注释行
		if l.peek(0) == '#' {
			next := l.peek(1)
			if next == ' ' || next == '\t' || next == '\n' || next == 0 {
				for l.peek(0) != '\n' && l.peek(0) != 0 {
					l.advance()
				}
				continue
			}
		}
		last := 0
		if len(l.indentStack) > 0 {
			last = l.indentStack[len(l.indentStack)-1]
		}
		if indent > last {
			if l.continuationPending {
				l.continuationPending = false
			} else {
				l.indentStack = append(l.indentStack, indent)
				l.tokens = append(l.tokens, Token{
					Kind:  Indent,
					Value: fmt.Sprintf("%d", indent),
					Line:  l.line,
					Col:   l.col,
				})
			}
		} else if indent < last {
			l.continuationPending = false
			for len(l.indentStack) > 0 {
				stackTop := l.indentStack[len(l.indentStack)-1]
				if indent >= stackTop {
					break
				}
				l.indentStack = l.indentStack[:len(l.indentStack)-1]
				l.tokens = append(l.tokens, Token{
					Kind:  Dedent,
					Value: "",
					Line:  l.line,
					Col:   l.col,
				})
			}
			if len(l.indentStack) > 0 {
				lastStack := l.indentStack[len(l.indentStack)-1]
				if indent != lastStack {
					l.setError("缩进不匹配")
					return fmt.Errorf("缩进不匹配")
				}
			} else {
				l.setError("缩进栈为空")
				return fmt.Errorf("缩进栈为空")
			}
		} else {
			l.continuationPending = false
		}
		return nil
	}
}

// isContinuationOperator 判断某类 token 是否可作为行尾续行运算符
func isContinuationOperator(kind TokenType) bool {
	switch kind {
	case Or, And, Plus, Minus, Star, Slash, Percent, Power, Comma:
		return true
	default:
		return false
	}
}

// Tokenize 对源代码进行词法分析，返回 token 列表
func (l *Lexer) Tokenize() ([]Token, error) {
	for l.pos < len(l.source) {
		l.skipWhitespace()
		sl := l.line
		sc := l.col
		c := l.peek(0)

		if c == '\n' || c == '\r' {
			if c == '\r' && l.peek(1) == '\n' {
				l.advance()
			}
			prevKind := EOF
			if len(l.tokens) > 0 {
				prevKind = l.tokens[len(l.tokens)-1].Kind
			}
			l.tokens = append(l.tokens, Token{
				Kind:  Newline,
				Value: "",
				Line:  sl,
				Col:   sc,
			})
			l.continuationPending = isContinuationOperator(prevKind)
			l.advance()
			if err := l.handleIndent(); err != nil {
				return nil, err
			}
			continue
		}

		if c == '#' {
			next := l.peek(1)
			if next == ' ' || next == '\t' || next == '\n' || next == 0 {
				// 空注释行，跳过整行
				for l.peek(0) != '\n' && l.peek(0) != 0 {
					l.advance()
				}
				continue
			}
			// Hash 作为宏系统 token
			l.advance()
			l.tokens = append(l.tokens, Token{
				Kind:  Hash,
				Value: "#",
				Line:  sl,
				Col:   sc,
			})
			continue
		}

		if c == '"' || c == '\'' {
			val, err := l.readString(c)
			if err != nil {
				return nil, err
			}
			l.tokens = append(l.tokens, Token{
				Kind:  String,
				Value: val,
				Line:  sl,
				Col:   sc,
			})
			continue
		}

		if c >= '0' && c <= '9' {
			l.tokens = append(l.tokens, l.readNumber())
			continue
		}

		if (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' || (c >= 0x4E00 && c <= 0x9FFF) {
			l.tokens = append(l.tokens, l.readIdentifier())
			continue
		}

		// 双字符运算符
		n := l.peek(1)
		handled := true
		switch c {
		case '=':
			if n == '=' {
				l.advance()
				l.advance()
				l.tokens = append(l.tokens, Token{Kind: Eq, Value: "==", Line: sl, Col: sc})
			} else {
				handled = false
			}
		case '!':
			if n == '=' {
				l.advance()
				l.advance()
				l.tokens = append(l.tokens, Token{Kind: Ne, Value: "!=", Line: sl, Col: sc})
			} else {
				handled = false
			}
		case '<':
			if n == '=' {
				l.advance()
				l.advance()
				l.tokens = append(l.tokens, Token{Kind: Le, Value: "<=", Line: sl, Col: sc})
			} else {
				handled = false
			}
		case '>':
			if n == '=' {
				l.advance()
				l.advance()
				l.tokens = append(l.tokens, Token{Kind: Ge, Value: ">=", Line: sl, Col: sc})
			} else {
				handled = false
			}
		case '+':
			if n == '=' {
				l.advance()
				l.advance()
				l.tokens = append(l.tokens, Token{Kind: PlusAssign, Value: "+=", Line: sl, Col: sc})
			} else {
				handled = false
			}
		case '-':
			switch n {
			case '=':
				l.advance()
				l.advance()
				l.tokens = append(l.tokens, Token{Kind: MinusAssign, Value: "-=", Line: sl, Col: sc})
			case '>':
				l.advance()
				l.advance()
				l.tokens = append(l.tokens, Token{Kind: Arrow, Value: "->", Line: sl, Col: sc})
			default:
				handled = false
			}
		case '*':
			if n == '=' {
				l.advance()
				l.advance()
				l.tokens = append(l.tokens, Token{Kind: StarAssign, Value: "*=", Line: sl, Col: sc})
			} else {
				handled = false
			}
		case '/':
			if n == '=' {
				l.advance()
				l.advance()
				l.tokens = append(l.tokens, Token{Kind: SlashAssign, Value: "/=", Line: sl, Col: sc})
			} else {
				handled = false
			}
		case '%':
			if n == '=' {
				l.advance()
				l.advance()
				l.tokens = append(l.tokens, Token{Kind: PercentAssign, Value: "%=", Line: sl, Col: sc})
			} else {
				handled = false
			}
		case '^':
			if n == '=' {
				l.advance()
				l.advance()
				l.tokens = append(l.tokens, Token{Kind: PowerAssign, Value: "^=", Line: sl, Col: sc})
			} else {
				handled = false
			}
		case '&':
			if n == '&' {
				l.advance()
				l.advance()
				l.tokens = append(l.tokens, Token{Kind: And, Value: "&&", Line: sl, Col: sc})
			} else {
				handled = false
			}
		case '|':
			if n == '|' {
				l.advance()
				l.advance()
				l.tokens = append(l.tokens, Token{Kind: Or, Value: "||", Line: sl, Col: sc})
			} else {
				handled = false
			}
		default:
			handled = false
		}
		if handled {
			continue
		}

		// 单字符运算符/分隔符
		var kind TokenType
		valid := true
		switch c {
		case '+':
			kind = Plus
		case '-':
			kind = Minus
		case '*':
			kind = Star
		case '/':
			kind = Slash
		case '%':
			kind = Percent
		case '^':
			kind = Power
		case '<':
			kind = Lt
		case '>':
			kind = Gt
		case '=':
			kind = Assign
		case '!':
			kind = Not
		case '&':
			kind = Ampersand
		case '#':
			kind = Hash
		case '(':
			kind = LParen
		case ')':
			kind = RParen
		case '[':
			kind = LBracket
		case ']':
			kind = RBracket
		case '{':
			kind = LBrace
		case '}':
			kind = RBrace
		case ':':
			kind = Colon
		case ';':
			kind = Semicolon
		case ',':
			kind = Comma
		case '.':
			kind = Dot
		default:
			valid = false
		}

		if valid {
			l.advance()
			// 对单字符 token，value 只用于存储原始字符的 UTF-8 表示
			var buf [4]byte
			n := utf8.EncodeRune(buf[:], c)
			l.tokens = append(l.tokens, Token{
				Kind:  kind,
				Value: string(buf[:n]),
				Line:  sl,
				Col:   sc,
			})
		} else {
			// 不在单字符符号表中，作为非法字符报错
			l.setErrorFmt("非法字符: U+%04X", uint32(c))
			return nil, fmt.Errorf("非法字符: U+%04X", uint32(c))
		}
	}

	// 闭合所有缩进层级
	for len(l.indentStack) > 1 {
		l.indentStack = l.indentStack[:len(l.indentStack)-1]
		l.tokens = append(l.tokens, Token{
			Kind:  Dedent,
			Value: "",
			Line:  l.line,
			Col:   l.col,
		})
	}

	l.tokens = append(l.tokens, Token{
		Kind:  EOF,
		Value: "",
		Line:  l.line,
		Col:   l.col,
	})

	// 将 tokens 的所有权转移给调用者
	result := l.tokens
	l.tokens = nil
	return result, nil
}
