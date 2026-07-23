package hover

import (
	"fmt"
	"strings"

	"github.com/vx-lang/vxc/internal/ast"
	"github.com/vx-lang/vxc/internal/lsp/types"
	"github.com/vx-lang/vxc/internal/token"
)

func Hover(astNodes []ast.Expr, tokens []token.Token, source string, position types.Position) (*types.Hover, error) {
	_ = source
	line := int(position.Line) + 1
	col := int(position.Character) + 1

	tok := findTokenAt(tokens, line, col)
	if tok == nil {
		return nil, nil
	}
	rng := tokenRange(tok)

	if isKeywordToken(tok.Kind) {
		if h := keywordHover(tok.Kind); h != nil {
			return &types.Hover{
				Contents: types.HoverContents{Scalar: h},
				Range:    rng,
			}, nil
		}
	}

	if tok.Kind == token.Identifier {
		if h := identifierHover(astNodes, *tok); h != nil {
			return &types.Hover{
				Contents: types.HoverContents{Scalar: h},
				Range:    rng,
			}, nil
		}
	}

	return nil, nil
}

func identifierHover(astNodes []ast.Expr, tok token.Token) *types.MarkupContent {
	for _, node := range astNodes {
		switch n := node.(type) {
		case *ast.FuncDecl:
			if n.Name == tok.Value {
				var paramsList []struct{ Name, Type string }
				for _, p := range n.Params {
					paramsList = append(paramsList, struct{ Name, Type string }{p.Name, p.ParamType.Name})
				}
				var retType *string
				if n.ReturnType != nil {
					rt := n.ReturnType.Name
					retType = &rt
				}
				sig := formatFuncSignature(n.Name, paramsList, retType)
				return &types.MarkupContent{
					Kind:  types.MarkupKindMarkdown,
					Value: sig,
				}
			}

		case *ast.StructDecl:
			if n.Name == tok.Value {
				var buf strings.Builder
				buf.WriteString("struct ")
				buf.WriteString(n.Name)
				if len(n.GenericParams) > 0 {
					buf.WriteString("<")
					for i, gp := range n.GenericParams {
						if i > 0 {
							buf.WriteString(", ")
						}
						buf.WriteString(gp)
					}
					buf.WriteString(">")
				}
				buf.WriteString(" {\n")
				for _, f := range n.Fields {
					buf.WriteString("    ")
					buf.WriteString(f.Name)
					buf.WriteString(": ")
					buf.WriteString(f.FieldType.Name)
					buf.WriteString("\n")
				}
				buf.WriteString("}")
				value := buf.String()
				return &types.MarkupContent{
					Kind:  types.MarkupKindMarkdown,
					Value: value,
				}
			}

		case *ast.ClassDecl:
			if n.Name == tok.Value {
				var buf strings.Builder
				buf.WriteString("class ")
				buf.WriteString(n.Name)
				buf.WriteString(" {\n")
				for _, f := range n.Fields {
					buf.WriteString("    ")
					buf.WriteString(f.Visibility)
					buf.WriteString(" ")
					buf.WriteString(f.Name)
					buf.WriteString(": ")
					buf.WriteString(f.FieldType.Name)
					buf.WriteString("\n")
				}
				buf.WriteString("}")
				value := buf.String()
				return &types.MarkupContent{
					Kind:  types.MarkupKindMarkdown,
					Value: value,
				}
			}

		case *ast.EnumDecl:
			if n.Name == tok.Value {
				var buf strings.Builder
				buf.WriteString("enum ")
				buf.WriteString(n.Name)
				buf.WriteString(" {\n")
				for _, v := range n.Variants {
					buf.WriteString("    ")
					buf.WriteString(v.Name)
					buf.WriteString(" = ")
					if v.Value != nil {
						buf.WriteString(fmt.Sprintf("%d", *v.Value))
					}
					buf.WriteString("\n")
				}
				buf.WriteString("}")
				value := buf.String()
				return &types.MarkupContent{
					Kind:  types.MarkupKindMarkdown,
					Value: value,
				}
			}

		case *ast.VarDecl:
			if n.Name == tok.Value {
				var buf strings.Builder
				if n.IsMut {
					buf.WriteString("mut ")
				} else {
					buf.WriteString("var ")
				}
				buf.WriteString(n.Name)
				if n.Type != nil {
					buf.WriteString(": ")
					buf.WriteString(n.Type.Name)
				}
				value := buf.String()
				return &types.MarkupContent{
					Kind:  types.MarkupKindMarkdown,
					Value: value,
				}
			}

		case *ast.ForStmt:
			if n.VarName == tok.Value {
				var buf strings.Builder
				buf.WriteString("var ")
				buf.WriteString(n.VarName)
				buf.WriteString(" (loop variable)")
				value := buf.String()
				return &types.MarkupContent{
					Kind:  types.MarkupKindMarkdown,
					Value: value,
				}
			}
		}
	}
	return nil
}

func findTokenAt(tokens []token.Token, line, col int) *token.Token {
	for i := range tokens {
		t := &tokens[i]
		if t.Line == line && t.Col <= col && col < t.Col+len(t.Value) {
			return t
		}
	}
	return nil
}

func tokenRange(t *token.Token) *types.Range {
	line0 := uint32(t.Line - 1)
	if t.Line == 0 {
		line0 = 0
	}
	col0 := uint32(t.Col - 1)
	if t.Col == 0 {
		col0 = 0
	}
	return &types.Range{
		Start: types.Position{Line: line0, Character: col0},
		End:   types.Position{Line: line0, Character: col0 + uint32(len(t.Value))},
	}
}

func isKeywordToken(kind token.TokenType) bool {
	switch kind {
	case token.If, token.Elif, token.Else, token.For, token.While, token.Break, token.Continue,
		token.Func, token.Return, token.Import, token.As, token.Struct, token.Class, token.Enum,
		token.Union, token.New, token.Move, token.IntT, token.FloatT, token.DoubleT, token.VarT,
		token.BoolT, token.VoidT, token.And, token.Or, token.Not, token.In, token.True, token.False,
		token.Nil, token.Loop, token.Match, token.Extern, token.Macro:
		return true
	default:
		return false
	}
}

func keywordHover(kind token.TokenType) *types.MarkupContent {
	var desc string
	switch kind {
	case token.If:
		desc = "**if 条件判断**\n\n根据条件执行不同分支。\n\n```vx\nif age >= 18:\n    out(\"adult\")\nelif age >= 12:\n    out(\"teen\")\nelse:\n    out(\"child\")\n```"
	case token.For:
		desc = "**for 循环**\n\n遍历集合或范围。\n\n```vx\nfor i in 0..10:\n    out(i)\n\nfor item in list:\n    out(item)\n```"
	case token.While:
		desc = "**while 循环**\n\n条件为真时重复执行。\n\n```vx\nmut count: int = 0\nwhile count < 10:\n    count = count + 1\n```"
	case token.Func:
		desc = "**func 函数声明**\n\n声明一个函数，必须显式类型标注。\n\n```vx\nfunc add(a: int, b: int) -> int:\n    return a + b\n```"
	case token.Return:
		desc = "**return 函数返回**\n\n从函数返回一个值。\n\n```vx\nfunc square(x: int) -> int:\n    return x * x\n```"
	case token.Import:
		desc = "**import 导入模块**\n\n导入外部模块。\n\n```vx\nimport std\nimport math as m\n```"
	case token.Struct:
		desc = "**struct 结构体**\n\n声明一个结构体类型。\n\n```vx\nstruct Point:\n    x: float\n    y: float\n```"
	case token.Class:
		desc = "**class 类**\n\n声明一个类类型，支持方法。\n\n```vx\nclass Animal:\n    name: string\n    func speak():\n        out(name)\n```"
	case token.Enum:
		desc = "**enum 枚举**\n\n声明一个枚举类型。\n\n```vx\nenum Color:\n    Red = 0\n    Green = 1\n    Blue = 2\n```"
	case token.Union:
		desc = "**union 联合体**\n\n声明一个联合体类型。\n\n```vx\nunion Value:\n    int_val: int\n    float_val: float\n```"
	case token.Mut:
		desc = "**mut 可变变量**\n\n声明一个可变变量。\n\n```vx\nmut x: int = 1\nx = 2  // 合法\n```"
	case token.Move:
		desc = "**move 所有权转移**\n\n转移值的所有权。\n\n```vx\na: string = \"hello\"\nb: string = move a  // a 不再可用\n```"
	case token.New:
		desc = "**new 创建实例**\n\n创建一个类的实例。\n\n```vx\np: Point = new Point(1.0, 2.0)\n```"
	case token.IntT:
		desc = "**int 整数类型**\n\n64位有符号整数。\n\n```vx\nx: int = 42\ny: int = -100\n```"
	case token.FloatT:
		desc = "**float 单精度浮点**\n\n32位单精度浮点数。\n\n```vx\npi: float = 3.14\n```"
	case token.DoubleT:
		desc = "**double 双精度浮点**\n\n64位双精度浮点数。\n\n```vx\npi: double = 3.14159265358979\n```"
	case token.BoolT:
		desc = "**bool 布尔类型**\n\ntrue 或 false。\n\n```vx\nflag: bool = true\n```"
	case token.VoidT:
		desc = "**void 空类型**\n\n表示无返回值。\n\n```vx\nfunc log(message: string) -> void:\n    out(message)\n```"
	case token.True:
		desc = "**true 布尔值真**\n\n布尔字面量 true。与 `false` 相对。"
	case token.False:
		desc = "**false 布尔值假**\n\n布尔字面量 false。与 `true` 相对。"
	case token.Nil:
		desc = "**nil 空值**\n\n表示空值或未初始化状态。\n\n```vx\nx: ?int = nil\n```"
	case token.Break:
		desc = "**break 跳出循环**\n\n跳出当前循环。\n\n```vx\nfor i in 0..10:\n    if i == 5:\n        break\n```"
	case token.Continue:
		desc = "**continue 跳过本次**\n\n跳过当前循环迭代。\n\n```vx\nfor i in 0..10:\n    if i % 2 == 0:\n        continue\n    out(i)\n```"
	case token.Loop:
		desc = "**loop 无限循环**\n\n创建一个无限循环。\n\n```vx\nloop:\n    // 需要 break 退出\n```"
	case token.Match:
		desc = "**match 模式匹配**\n\n基于值进行模式匹配。\n\n```vx\nmatch color:\n    Color.Red => out(\"red\")\n    Color.Green => out(\"green\")\n    _ => out(\"other\")\n```"
	case token.Extern:
		desc = "**extern 外部函数**\n\n声明一个外部函数。\n\n```vx\nextern func printf(fmt: string, ...) -> int\n```"
	case token.Macro:
		desc = "**macro 编译时宏**\n\n声明一个编译时宏。\n\n```vx\nmacro assert(cond: bool):\n    if not cond:\n        panic(\"assertion failed\")\n```"
	case token.And:
		desc = "**and 逻辑与**\n\n逻辑与运算符。\n\n```vx\nif a > 0 and b > 0:\n    // 两者都为真\n```"
	case token.Or:
		desc = "**or 逻辑或**\n\n逻辑或运算符。\n\n```vx\nif a > 0 or b > 0:\n    // 至少一个为真\n```"
	case token.Not:
		desc = "**not 逻辑非**\n\n逻辑非运算符。\n\n```vx\nif not flag:\n    // flag 为 false 时执行\n```"
	case token.In:
		desc = "**in 成员检查**\n\n检查值是否在集合中。\n\n```vx\nif x in list:\n    out(\"found\")\n```"
	default:
		return nil
	}

	return &types.MarkupContent{
		Kind:  types.MarkupKindMarkdown,
		Value: desc,
	}
}

func formatFuncSignature(name string, params []struct{ Name, Type string }, retType *string) string {
	var result strings.Builder
	result.WriteString("func ")
	result.WriteString(name)
	result.WriteString("(")

	for i, param := range params {
		if i > 0 {
			result.WriteString(", ")
		}
		result.WriteString(param.Name)
		result.WriteString(": ")
		result.WriteString(param.Type)
	}

	result.WriteString(")")

	if retType != nil {
		result.WriteString(" -> ")
		result.WriteString(*retType)
	}

	return result.String()
}
