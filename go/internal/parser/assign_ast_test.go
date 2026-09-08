package parser_test

import (
	"fmt"
	"reflect"
	"testing"

	"github.com/vx-lang/vxc/internal/lexer"
	"github.com/vx-lang/vxc/internal/parser"
)

func TestAssignAST(t *testing.T) {
	src := `func main():
    var x = 0
    while x < 3:
        x = x + 1
    out(x)
`
	toks := lexer.NewLexer(src).TokenizeAll()
	program := parser.NewParser(toks).ParseProgram()
	walk(program[0], 0)
}

func walk(v interface{}, depth int) {
	rv := reflect.ValueOf(v)
	if rv.Kind() == reflect.Ptr {
		if rv.IsNil() {
			return
		}
		fmt.Printf("%*s%s\n", depth*2, "", reflect.TypeOf(v).Elem().Name())
		walk(rv.Elem().Interface(), depth+1)
		return
	}
	if rv.Kind() == reflect.Struct {
		fmt.Printf("%*s%s\n", depth*2, "", reflect.TypeOf(v).Name())
		for i := 0; i < rv.NumField(); i++ {
			f := rv.Field(i)
			if f.Kind() == reflect.Interface || f.Kind() == reflect.Ptr || f.Kind() == reflect.Slice {
				if f.Kind() == reflect.Slice {
					for j := 0; j < f.Len(); j++ {
						walk(f.Index(j).Interface(), depth+1)
					}
					continue
				}
				walk(f.Interface(), depth+1)
			}
		}
		return
	}
	if rv.Kind() == reflect.String {
		fmt.Printf("%*s= %q\n", depth*2, "", rv.String())
	}
}
