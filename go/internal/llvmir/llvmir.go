// Package llvmir translates VX TypeIR to LLVM IR text.
package llvmir

import (
	"fmt"
	"sort"
	"strconv"
	"strings"

	"github.com/vx-lang/vxc/internal/typeir"
)

// ==================== Extern FFI type corrections ====================
//
// VX's extern FFI uses `int` for both true 32-bit integers and opaque
// pointer handles (FILE*, malloc'd buffers, etc.). On 64-bit targets
// this causes pointer truncation. These maps correct the LLVM types
// for well-known C runtime functions.

// ptrReturnFns lists extern C functions that return pointers (FILE*, void*).
var ptrReturnFns = map[string]bool{
	"fopen": true, "freopen": true, "tmpfile": true,
	"malloc": true, "calloc": true, "realloc": true, "strdup": true,
}

// ptrParamFns defines extern C functions with pointer parameters at
// specific indices (0-based, matching VX declaration order).
// These are typically FILE* handles passed as VX `int`.
var ptrParamFns = map[string]map[int]bool{
	"fputc":   {1: true}, // int fputc(int c, FILE *fh)
	"fgetc":   {0: true}, // int fgetc(FILE *fh)
	"fputs":   {1: true}, // int fputs(const char *s, FILE *fh)
	"fseek":   {0: true}, // int fseek(FILE *fh, long off, int whence)
	"ftell":   {0: true}, // long ftell(FILE *fh)
	"fclose":  {0: true}, // int fclose(FILE *fh)
	"fflush":  {0: true}, // int fflush(FILE *fh)
	"fprintf": {0: true}, // int fprintf(FILE *fh, const char *fmt, ...)
	"fscanf":  {0: true}, // int fscanf(FILE *fh, ...)
	"free":    {0: true}, // void free(void *ptr) — VX declares as int
	"puts":    {0: true}, // int puts(const char *s) — VX declares as int
}

// Generate produces LLVM IR text for the given TypeModule, targeting the specified triple.
func Generate(module *typeir.TypeModule, targetTriple string) (string, error) {
	var b strings.Builder

	// Module header
	fmt.Fprintf(&b, "; LLVM IR generated from VXOBJ TypeIR\n")
	b.WriteString(fmt.Sprintf("target triple = \"%s\"\n", targetTriple))
	b.WriteString("target datalayout = \"e-m:e-p270:32:32-p271:32:32-p272:64:64-i64:64-f80:128-n8:16:32:64-S128\"\n")
	b.WriteString("\n")

	// External function declarations (deduplicated by name for merged modules)
	declaredExternNames := make(map[string]bool)
	for _, fn := range module.Functions {
		linkage, ok := module.Linkage[fn.ID]
		if ok && linkage.IsExternal && !declaredExternNames[linkage.Name] {
			declaredExternNames[linkage.Name] = true
			b.WriteString(generateExternDecl(&fn, linkage.Name))
			b.WriteString("\n")
		}
	}

	// Struct type declarations (LLVM named struct types).
	for _, st := range module.StructTypes {
		b.WriteString(generateStructTypeDecl(&st))
	}
	if len(module.StructTypes) > 0 {
		b.WriteString("\n")
	}

	// Auto-declare builtin/unknown callees that are not defined in this module.
	// These are referenced via Call instructions with a ";name" argument.
	definedFuncs := make(map[string]bool)
	for _, fn := range module.Functions {
		definedFuncs[fn.Name] = true
	}
	declaredExterns := make(map[string]bool)
	// Track which externs were already declared in the extern-decl loop above.
	for _, fn := range module.Functions {
		linkage, ok := module.Linkage[fn.ID]
		if ok && linkage.IsExternal {
			declaredExterns[linkage.Name] = true
		}
	}
	// strcmp is used by the stack mode StrEq handler — skip if already declared.
	if !declaredExterns["strcmp"] {
		b.WriteString("declare i32 @strcmp(i8*, i8*)\n")
	}
	declaredExterns["@strcmp"] = true
	for _, fn := range module.Functions {
		for _, inst := range fn.Body {
			if inst.Op != typeir.Call {
				continue
			}
			for _, a := range inst.Args {
				if strings.HasPrefix(a, ";") {
					name := a[1:]
					if name != "" && !definedFuncs[name] && !declaredExterns[name] && !declaredExterns["@"+name] {
						declaredExterns[name] = true
						// Builtins (chr, len, ord) are implemented by the
						// translator's platform runtime, skip auto-declaration.
						if name == "chr" || name == "len" || name == "ord" || name == "itoa" {
							continue
						}
						// Skip `out`, `println`, `str`, `value_error` — the translator
						// provides these as platform builtins.
						if name == "out" || name == "println" || name == "str" || name == "value_error" {
							continue
						}
							// Standard C functions are declared by generateExternDecl
							if name == "malloc" || name == "calloc" || name == "realloc" || name == "free" || name == "c_free" || name == "c_realloc" {
								continue
							}
						// Declare as a varargs function so any argument types
						// emitted at the call site are accepted.
						b.WriteString(fmt.Sprintf("declare void @%s(...)\n", name))
					}
					break
				}
			}
		}
	}

	// Collect and emit string constants as global variables.
	// Each distinct string literal gets a stable index so the getelementptr
	// references in function bodies resolve to a defined global.
	strIndex := make(map[string]int)
	for _, fn := range module.Functions {
		for _, inst := range fn.Body {
			if inst.Op == typeir.ConstString && len(inst.Args) > 0 {
				if _, ok := strIndex[inst.Args[0]]; !ok {
					strIndex[inst.Args[0]] = len(strIndex)
				}
			}
		}
	}
	for s, idx := range strIndex {
		escaped := escapeLLVMString(s)
		n := len(s) + 1
		b.WriteString(fmt.Sprintf("@\"str.%d\" = private unnamed_addr constant [%d x i8] c\"%s\\00\"\n", idx, n, escaped))
	}
	if len(strIndex) > 0 {
		b.WriteString("\n")
	}

	// Global variables (shared across functions). typeirgen allocates
	// global IDs 0..len(globals)-1 in name-sorted order, so the sorted name
	// slice doubles as the ID→name index here.
	sort.Slice(module.Globals, func(i, j int) bool { return module.Globals[i].Name < module.Globals[j].Name })
	globals := make([]string, 0, len(module.Globals))
	for gi, g := range module.Globals {
		globals = append(globals, g.Name)
		tn := typeir.TypeName(g.Type)
		var llvmTy, init string
		switch tn {
		case "string":
			// String globals start as the empty string, not null, so
			// `var g_s: string = ""` behaves like "" (strcat/strlen safe).
			llvmTy = "i8*"
			gstr := fmt.Sprintf("@.str.glob%d", gi)
			b.WriteString(fmt.Sprintf("%s = private constant [1 x i8] c\"\\00\"\n", gstr))
			init = fmt.Sprintf("getelementptr ([1 x i8], [1 x i8]* %s, i64 0, i64 0)", gstr)
		case "float":
			llvmTy, init = "double", "0.0"
		default:
			// int/bool/unknown/struct: opaque i64 slot.
			llvmTy, init = "i64", "0"
		}
		b.WriteString(fmt.Sprintf("@g_%s = global %s %s\n", g.Name, llvmTy, init))
	}
	if len(module.Globals) > 0 {
		b.WriteString("\n")
	}

	// Function definitions
	funcMap := make(map[string]*typeir.FunctionIR, len(module.Functions))
	for i := range module.Functions {
		fn := &module.Functions[i]
		funcMap[strconv.Itoa(int(fn.ID))] = fn
		funcMap[fn.Name] = fn
	}
	for _, fn := range module.Functions {
		// For merged modules, function IDs may collide. Use the function body
		// to determine if it's an extern declaration (empty body) or a
		// user-defined function (non-empty body).
		isExtern := len(fn.Body) == 0
		if !isExtern {
			linkage, ok := module.Linkage[fn.ID]
			if ok && linkage.IsExternal {
				isExtern = true
			}
		}
		if isExtern {
			continue
		}
		fnIR, err := generateFunction(&fn, strIndex, funcMap, module.StructTypes, module.Linkage, globals)
		if err != nil {
			return "", fmt.Errorf("function %s: %w", fn.Name, err)
		}
		b.WriteString(fnIR)
		b.WriteString("\n")
	}

	return b.String(), nil
}

func generateExternDecl(fn *typeir.FunctionIR, extName string) string {
	// Known standard C functions — always emit their correct C ABI
	// signature regardless of what VX source types say.
	switch extName {
	case "malloc":
		return "declare i8* @malloc(i64)"
	case "free":
		return "declare void @free(i8*)"
	case "realloc":
		return "declare i8* @realloc(i8*, i64)"
	case "c_free":
		// Defined by cWrapperRuntime — skip extern declaration
		return ""
	case "c_realloc":
		// Defined by cWrapperRuntime — skip extern declaration
		return ""
	case "calloc":
		return "declare i8* @calloc(i64, i64)"
	case "memcpy":
		return "declare i8* @memcpy(i8*, i8*, i64)"
	case "memset":
		return "declare i8* @memset(i8*, i32, i64)"
	case "puts", "printf":
		return fmt.Sprintf("declare i32 @%s(...)", extName)
	case "out", "println":
		// The translator provides the full definition via outDecl();
		// skip the extern declaration to avoid duplicate declare/define conflict.
		return ""
	case "exit":
		return "declare void @exit(i32)"
	case "open":
		return "declare i32 @open(i8*, i32, ...)"
	case "read":
		return "declare i64 @read(i32, i8*, i64)"
	case "write":
		return "declare i64 @write(i32, i8*, i64)"
	case "close":
		return "declare i32 @close(i32)"
	case "lseek":
		return "declare i64 @lseek(i32, i64, i32)"
	case "fopen":
		return "declare i8* @fopen(i8*, i8*)"
	case "fclose":
		return "declare i32 @fclose(i8*)"
	case "fread":
		return "declare i64 @fread(i8*, i64, i64, i8*)"
	case "fwrite":
		return "declare i64 @fwrite(i8*, i64, i64, i8*)"
	case "fprintf":
		return "declare i32 @fprintf(i8*, i8*, ...)"
	case "strcmp":
		return "declare i32 @strcmp(i8*, i8*)"
	case "strlen":
		return "declare i64 @strlen(i8*)"
	}
	// General case: generate from VX function signature.
	retType := llvmReturnType(fn)
	if ptrReturnFns[extName] {
		retType = "i8*"
	}
	params := llvmParamList(fn.Params, extName, false)
	return fmt.Sprintf("declare %s @%s(%s)", retType, extName, params)
}

func llvmReturnType(fn *typeir.FunctionIR) string {
	if fn.HasReturn || (fn.ReturnType != nil && typeir.TypeName(fn.ReturnType) != "void") {
		// Struct types are heap-allocated and returned as i64 pointers.
		if _, isStruct := fn.ReturnType.(typeir.StructTyp); isStruct {
			return "i64"
		}
		tn := typeir.TypeName(fn.ReturnType)
		// Use i64 for int/bool returns to avoid truncating 64-bit pointers
		// and to match the i64 alloca used for local variables.
		if tn == "int" || tn == "bool" {
			return "i64"
		}
		return llvmType(fn.ReturnType)
	}
	return "void"
}

func llvmType(t typeir.Typ) string {
	if t == nil {
		return "i32"
	}
	switch typeir.TypeName(t) {
	case "int", "bool":
		return "i32"
	case "float":
		return "double"
	case "void":
		return "void"
	case "string":
		return "i8*"
	case "pointer":
		return "i64"
	default:
		// Handle struct types by name.
		if st, ok := t.(typeir.StructTyp); ok {
			return "%struct." + st.Name
		}
		// Pointer types: for struct pointers, use i64 (opaque pointer).
		if _, ok := t.(typeir.PointerTyp); ok {
			return "i64"
		}
		return "i32"
	}
}

// generateStructTypeDecl emits an LLVM named struct type declaration.
func generateStructTypeDecl(st *typeir.StructTypeDef) string {
	var b strings.Builder
	fmt.Fprintf(&b, "%%struct.%s = type { ", st.Name)
	// Use i64 for all struct fields to safely hold both ints and pointers.
	// This wastes some memory but avoids pointer-truncation bugs during
	// the bootstrapping phase. Can be optimized later.
	fieldTypes := make([]string, 0, len(st.Fields))
	for range st.Fields {
		fieldTypes = append(fieldTypes, "i64")
	}
	b.WriteString(strings.Join(fieldTypes, ", "))
	b.WriteString(" }\n")
	return b.String()
}

// findStructFieldIndex locates a field by name in a struct type definition.
// Returns the field index and its LLVM type string.
func findStructFieldIndex(st *typeir.StructTypeDef, fieldName string) (int, string, bool) {
	for i, f := range st.Fields {
		if f.Name == fieldName {
			return i, llvmType(f.FieldType), true
		}
	}
	return 0, "", false
}

// findStructType looks up a struct type by name in the given slice.
func findStructType(structTypes []typeir.StructTypeDef, name string) *typeir.StructTypeDef {
	for i := range structTypes {
		if structTypes[i].Name == name {
			return &structTypes[i]
		}
	}
	return nil
}

// llvmFieldGEP emits LLVM IR to compute a pointer to a struct field given
// a pointer to the struct, returning the field pointer register name.
func llvmFieldGEP(b *strings.Builder, n int, structPtr string, structName string, fieldIdx int, fieldLLVMType string) string {
	fmt.Fprintf(b, "  %%fld_ptr%d = getelementptr %%struct.%s, %%struct.%s* %s, i32 0, i32 %d\n",
		n, structName, structName, structPtr, fieldIdx)
	return fmt.Sprintf("%%fld_ptr%d", n)
}

func llvmParamList(params []typeir.ParamIR, fnName string, wideInt bool) string {
	if len(params) == 0 {
		return ""
	}
	parts := make([]string, len(params))
	for i, p := range params {
		var llvmTyp string
		if _, isStruct := p.Type.(typeir.StructTyp); isStruct {
			// Struct types are heap-allocated and passed as i64 pointers.
			llvmTyp = "i64"
		} else if pt, ok := p.Type.(typeir.PointerTyp); ok {
			// Pointer to struct: pass as i64 opaque pointer.
			if _, isStruct := pt.To.(typeir.StructTyp); isStruct {
				llvmTyp = "i64"
			} else {
				llvmTyp = "i32"
			}
		} else {
			llvmTyp = llvmType(p.Type)
			// Override i32→i8* for FILE* parameters declared as VX `int`.
			if llvmTyp == "i32" && ptrParamFns[fnName] != nil && ptrParamFns[fnName][i] {
				llvmTyp = "i8*"
			} else if wideInt && llvmTyp == "i32" {
				// User-defined functions receive int/bool params as i64 so
				// opaque handles (FILE*) pass through without truncation.
				llvmTyp = "i64"
			}
		}
		parts[i] = llvmTyp + " %_" + p.Name
	}
	return strings.Join(parts, ", ")
}

// generateFunction translates one FunctionIR to LLVM IR text.
func generateFunction(fn *typeir.FunctionIR, strIndex map[string]int, funcMap map[string]*typeir.FunctionIR, structTypes []typeir.StructTypeDef, linkage map[uint32]typeir.LinkageEntry, globals []string) (string, error) {
	var b strings.Builder

	retType := llvmReturnType(fn)
	params := llvmParamList(fn.Params, "", true)
	b.WriteString(fmt.Sprintf("define %s @%s(%s) {\n", retType, fn.Name, params))

	err := generateWithBlocks(&b, fn, strIndex, funcMap, structTypes, linkage, globals)
	if err != nil {
		return "", err
	}

	b.WriteString("}\n")
	return b.String(), nil
}

func generateAlloca(b *strings.Builder, fn *typeir.FunctionIR) {
	for vid, ty := range fn.LocalTypes {
		tn := typeir.TypeName(ty)
		var llvmTy string
		if tn == "string" {
			llvmTy = "i8*"
		} else if tn == "float" {
			llvmTy = "double"
		} else {
			// int/bool/unknown/struct: store as i64 so opaque handles
			// (FILE*, malloc'd buffers) pass through int variables
			// without truncation on 64-bit targets.
			llvmTy = "i64"
		}
		b.WriteString(fmt.Sprintf("  %%v%d = alloca %s, align 4\n", vid, llvmTy))
	}
}

// ssaGen generates LLVM IR for straight-line code (no jumps) using a virtual register stack.
type ssaGen struct {
	buf        *strings.Builder
	counter    int
	stack      []string                      // virtual register stack
	types      []typeir.Typ                  // type of each register on the stack (parallel to stack)
	locals     map[string]string             // local variable name → SSA register
	strIndex   map[string]int                // string literal value → global index
	hasReturn  bool                          // whether the current function returns a value
	funcMap    map[string]*typeir.FunctionIR // function lookup by ID or name
	localTypes map[uint32]typeir.Typ         // local variable types for type-safe load/store
	linkage    map[uint32]typeir.LinkageEntry // extern linkage info (to detect extern functions)
}

func (g *ssaGen) newReg() string {
	r := fmt.Sprintf("%%t%d", g.counter)
	g.counter++
	return r
}

func (g *ssaGen) push(r string, t typeir.Typ) {
	g.stack = append(g.stack, r)
	g.types = append(g.types, t)
}

func (g *ssaGen) pop() (string, typeir.Typ) {
	if len(g.stack) == 0 {
		return "%0", typeir.Unknown
	}
	r := g.stack[len(g.stack)-1]
	t := g.types[len(g.types)-1]
	g.stack = g.stack[:len(g.stack)-1]
	g.types = g.types[:len(g.types)-1]
	return r, t
}

func (g *ssaGen) popTwo() (string, string, typeir.Typ) {
	rhs, rt := g.pop()
	lhs, lt := g.pop()
	_ = rt
	return lhs, rhs, lt
}

func (g *ssaGen) emit(inst *typeir.TypedInstruction) error {
	op := inst.Op
	args := inst.Args

	switch op {
	case typeir.ConstInt:
		r := g.newReg()
		val := "0"
		if len(args) > 0 {
			val = args[0]
		}
		// Use i64 to match the i64 alloca and avoid pointer truncation.
		g.buf.WriteString(fmt.Sprintf("  %s = add i64 %s, 0\n", r, val))
		g.push(r, typeir.Int)

	case typeir.ConstFloat:
		r := g.newReg()
		val := "0.0"
		if len(args) > 0 {
			val = args[0]
		}
		g.buf.WriteString(fmt.Sprintf("  %s = fadd double %s, 0.0\n", r, val))
		g.push(r, typeir.Float)

	case typeir.ConstBool:
		r := g.newReg()
		val := "0"
		if len(args) > 0 && (args[0] == "1" || args[0] == "true") {
			val = "1"
		}
		g.buf.WriteString(fmt.Sprintf("  %s = add i64 %s, 0\n", r, val))
		g.push(r, typeir.Bool)

	case typeir.ConstString:
		r := g.newReg()
		str := ""
		if len(args) > 0 {
			str = args[0]
		}
		idx := 0
		if g.strIndex != nil {
			if i, ok := g.strIndex[str]; ok {
				idx = i
			}
		}
		n := len(str) + 1
		g.buf.WriteString(fmt.Sprintf("  %s = getelementptr [%d x i8], [%d x i8]* @\"str.%d\", i64 0, i64 0\n", r, n, n, idx))
		g.push(r, typeir.String)

	case typeir.ConstNil:
		r := g.newReg()
		g.buf.WriteString(fmt.Sprintf("  %s = inttoptr i64 0 to i8*\n", r))
		g.push(r, typeir.Unknown)

	case typeir.LoadVar:
		if len(args) == 0 {
			break
		}
		r := g.newReg()
		vid, _ := strconv.Atoi(args[0])
		typ, hasTyp := g.localTypes[uint32(vid)]
		if hasTyp && typeir.TypeName(typ) == "string" {
			g.buf.WriteString(fmt.Sprintf("  %s = load i8*, i8** %%v%s, align 8\n", r, args[0]))
			g.push(r, typeir.String)
		} else if hasTyp && typeir.TypeName(typ) == "float" {
			g.buf.WriteString(fmt.Sprintf("  %s = load double, double* %%v%s, align 8\n", r, args[0]))
			g.push(r, typeir.Float)
		} else {
			// Use i64 for int/bool/struct/pointer to avoid truncating
			// 64-bit pointers on arm64 (malloc returns i8* stored in int vars).
			g.buf.WriteString(fmt.Sprintf("  %s = load i64, i64* %%v%s, align 8\n", r, args[0]))
			g.push(r, typeir.Int)
		}

	case typeir.StoreVar:
		if len(args) == 0 {
			break
		}
		val, _ := g.pop()
		vid, _ := strconv.Atoi(args[0])
		typ, hasTyp := g.localTypes[uint32(vid)]
		if hasTyp && typeir.TypeName(typ) == "string" {
			g.buf.WriteString(fmt.Sprintf("  store i8* %s, i8** %%v%s, align 8\n", val, args[0]))
		} else if hasTyp && typeir.TypeName(typ) == "float" {
			g.buf.WriteString(fmt.Sprintf("  store double %s, double* %%v%s, align 8\n", val, args[0]))
		} else {
			// Use i64 for int/bool/struct/pointer to preserve full pointer width.
			g.buf.WriteString(fmt.Sprintf("  store i64 %s, i64* %%v%s, align 8\n", val, args[0]))
		}

	case typeir.I32Add:
		lhs, rhs, _ := g.popTwo()
		r := g.newReg()
		g.buf.WriteString(fmt.Sprintf("  %s = add i64 %s, %s\n", r, lhs, rhs))
		g.push(r, typeir.Int)
	case typeir.I32Sub:
		lhs, rhs, _ := g.popTwo()
		r := g.newReg()
		g.buf.WriteString(fmt.Sprintf("  %s = sub i64 %s, %s\n", r, lhs, rhs))
		g.push(r, typeir.Int)
	case typeir.I32Mul:
		lhs, rhs, _ := g.popTwo()
		r := g.newReg()
		g.buf.WriteString(fmt.Sprintf("  %s = mul i64 %s, %s\n", r, lhs, rhs))
		g.push(r, typeir.Int)
	case typeir.I32Div:
		lhs, rhs, _ := g.popTwo()
		r := g.newReg()
		g.buf.WriteString(fmt.Sprintf("  %s = sdiv i64 %s, %s\n", r, lhs, rhs))
		g.push(r, typeir.Int)

	case typeir.F64Add:
		lhs, rhs, _ := g.popTwo()
		r := g.newReg()
		g.buf.WriteString(fmt.Sprintf("  %s = fadd double %s, %s\n", r, lhs, rhs))
		g.push(r, typeir.Float)
	case typeir.F64Sub:
		lhs, rhs, _ := g.popTwo()
		r := g.newReg()
		g.buf.WriteString(fmt.Sprintf("  %s = fsub double %s, %s\n", r, lhs, rhs))
		g.push(r, typeir.Float)
	case typeir.F64Mul:
		lhs, rhs, _ := g.popTwo()
		r := g.newReg()
		g.buf.WriteString(fmt.Sprintf("  %s = fmul double %s, %s\n", r, lhs, rhs))
		g.push(r, typeir.Float)
	case typeir.F64Div:
		lhs, rhs, _ := g.popTwo()
		r := g.newReg()
		g.buf.WriteString(fmt.Sprintf("  %s = fdiv double %s, %s\n", r, lhs, rhs))
		g.push(r, typeir.Float)

	case typeir.I32Eq:
		lhs, rhs, _ := g.popTwo()
		r := g.newReg()
		r2 := g.newReg()
		g.buf.WriteString(fmt.Sprintf("  %s = icmp eq i64 %s, %s\n", r, lhs, rhs))
		g.buf.WriteString(fmt.Sprintf("  %s = zext i1 %s to i64\n", r2, r))
		g.push(r2, typeir.Bool)
	case typeir.I32Neq:
		lhs, rhs, _ := g.popTwo()
		r := g.newReg()
		r2 := g.newReg()
		g.buf.WriteString(fmt.Sprintf("  %s = icmp ne i64 %s, %s\n", r, lhs, rhs))
		g.buf.WriteString(fmt.Sprintf("  %s = zext i1 %s to i64\n", r2, r))
		g.push(r2, typeir.Bool)
	case typeir.I32Lt:
		lhs, rhs, _ := g.popTwo()
		r := g.newReg()
		r2 := g.newReg()
		g.buf.WriteString(fmt.Sprintf("  %s = icmp slt i64 %s, %s\n", r, lhs, rhs))
		g.buf.WriteString(fmt.Sprintf("  %s = zext i1 %s to i64\n", r2, r))
		g.push(r2, typeir.Bool)
	case typeir.I32Gt:
		lhs, rhs, _ := g.popTwo()
		r := g.newReg()
		r2 := g.newReg()
		g.buf.WriteString(fmt.Sprintf("  %s = icmp sgt i64 %s, %s\n", r, lhs, rhs))
		g.buf.WriteString(fmt.Sprintf("  %s = zext i1 %s to i64\n", r2, r))
		g.push(r2, typeir.Bool)
	case typeir.I32Le:
		lhs, rhs, _ := g.popTwo()
		r := g.newReg()
		r2 := g.newReg()
		g.buf.WriteString(fmt.Sprintf("  %s = icmp sle i64 %s, %s\n", r, lhs, rhs))
		g.buf.WriteString(fmt.Sprintf("  %s = zext i1 %s to i64\n", r2, r))
		g.push(r2, typeir.Bool)
	case typeir.I32Ge:
		lhs, rhs, _ := g.popTwo()
		r := g.newReg()
		r2 := g.newReg()
		g.buf.WriteString(fmt.Sprintf("  %s = icmp sge i64 %s, %s\n", r, lhs, rhs))
		g.buf.WriteString(fmt.Sprintf("  %s = zext i1 %s to i64\n", r2, r))
		g.push(r2, typeir.Bool)

	case typeir.I32And:
		lhs, rhs, _ := g.popTwo()
		r := g.newReg()
		g.buf.WriteString(fmt.Sprintf("  %s = and i64 %s, %s\n", r, lhs, rhs))
		g.push(r, typeir.Int)
	case typeir.I32Or:
		lhs, rhs, _ := g.popTwo()
		r := g.newReg()
		g.buf.WriteString(fmt.Sprintf("  %s = or i64 %s, %s\n", r, lhs, rhs))
		g.push(r, typeir.Int)

	case typeir.I32Neg:
		val, _ := g.pop()
		r := g.newReg()
		g.buf.WriteString(fmt.Sprintf("  %s = sub i64 0, %s\n", r, val))
		g.push(r, typeir.Int)
	case typeir.F64Neg:
		val, _ := g.pop()
		r := g.newReg()
		g.buf.WriteString(fmt.Sprintf("  %s = fsub double -0.0, %s\n", r, val))
		g.push(r, typeir.Float)
	case typeir.BoolNot:
		val, _ := g.pop()
		r := g.newReg()
		r2 := g.newReg()
		g.buf.WriteString(fmt.Sprintf("  %s = icmp eq i64 %s, 0\n", r, val))
		g.buf.WriteString(fmt.Sprintf("  %s = zext i1 %s to i64\n", r2, r))
		g.push(r2, typeir.Bool)

	case typeir.Dup:
		if len(g.stack) > 0 {
			top := g.stack[len(g.stack)-1]
			topT := g.types[len(g.types)-1]
			r := g.newReg()
			if typeir.TypeName(topT) == "string" {
				g.buf.WriteString(fmt.Sprintf("  %s = getelementptr i8, i8* %s, i64 0\n", r, top))
			} else if typeir.TypeName(topT) == "float" {
				g.buf.WriteString(fmt.Sprintf("  %s = fadd double %s, 0.0\n", r, top))
			} else {
				g.buf.WriteString(fmt.Sprintf("  %s = add i64 %s, 0\n", r, top))
			}
			g.push(r, topT)
		}

	case typeir.Pop:
		if len(g.stack) > 0 {
			g.stack = g.stack[:len(g.stack)-1]
			g.types = g.types[:len(g.types)-1]
		}

	case typeir.Return:
		if g.hasReturn {
			// Return with value from stack
			val, typ := g.pop()
			tn := typeir.TypeName(typ)
			if tn != "void" {
				// Use i64 for int/bool returns to match the function signature.
				retTy := "i64"
				if tn == "string" {
					retTy = "i8*"
				} else if tn == "float" {
					retTy = "double"
				}
				g.buf.WriteString(fmt.Sprintf("  ret %s %s\n", retTy, val))
			} else {
				g.buf.WriteString("  ret void\n")
			}
		} else {
			g.buf.WriteString("  ret void\n")
		}

	case typeir.ReturnVoid:
		g.buf.WriteString("  ret void\n")

	case typeir.Call:
		if len(args) == 0 {
			break
		}
		// Convention (matches vxc/typeirgen): args[0] is either:
		//   - a numeric funcID (when the callee is defined in this module), or
		//   - a callee marker like ";puts" (when calling an external function).
		// The remaining args are the parameter type names (e.g. "string").
		first := args[0]
		var funcID string
		var callee string
		paramTypeNames := make([]string, 0)
		if strings.HasPrefix(first, ";") {
			// External callee form: [";callee", paramType1, paramType2, ...]
			callee = first[1:]
			if len(args) > 1 {
				paramTypeNames = append(paramTypeNames, args[1:]...)
			}
		} else {
			// Numeric funcID form: [funcID, ";callee"?, paramType1, ...]
			funcID = first
			sawCallee := false
			for _, a := range args[1:] {
				if strings.HasPrefix(a, ";") {
					if !sawCallee {
						callee = a[1:]
						sawCallee = true
					}
					continue
				}
				if sawCallee {
					paramTypeNames = append(paramTypeNames, a)
				}
			}
		}
		if callee == "" {
			callee = "@f" + funcID
		} else {
			callee = "@" + callee
		}

		// Collect call arguments by popping one value per declared param type.
		var callArgs []string
		var callTypes []typeir.Typ
		for _, tName := range paramTypeNames {
			reg, t := g.pop()
			// If the popped register has unknown type, infer from the
			// declared parameter type so we emit the correct LLVM type.
			if typeir.TypeName(t) == "unknown" || typeir.TypeName(t) == "" {
				if pt, err := parseParamTypeName(tName); err == nil {
					t = pt
				}
			}
			callArgs = append(callArgs, reg)
			callTypes = append(callTypes, t)
		}
		// Reverse so args appear in original left-to-right order.
		for i, j := 0, len(callArgs)-1; i < j; i, j = i+1, j-1 {
			callArgs[i], callArgs[j] = callArgs[j], callArgs[i]
			callTypes[i], callTypes[j] = callTypes[j], callTypes[i]
		}

		// Build param list with proper LLVM types.
		// For extern functions, use the actual C ABI types from the declaration
		// (e.g., puts expects i8*, not i32).
		paramParts := make([]string, len(callArgs))
		lookupID := funcID
		if lookupID == "" {
			lookupID = strings.TrimPrefix(callee, "@")
		}
		calleeFn := g.findFunc(lookupID)
		for i, a := range callArgs {
			// Check if this is an extern function with known parameter types.
			if calleeFn != nil && i < len(calleeFn.Params) {
				paramType := calleeFn.Params[i].Type
				ptn := typeir.TypeName(paramType)
				if ptn == "string" {
					paramParts[i] = fmt.Sprintf("i8* %s", a)
					continue
				}
				// Also check ptrParamFns for int params that are actually pointers (e.g., free(ptr: int) → i8*).
				if ptn == "int" && ptrParamFns[lookupID] != nil && ptrParamFns[lookupID][i] {
					paramParts[i] = fmt.Sprintf("i8* %s", a)
					continue
				}
			}
			paramParts[i] = fmt.Sprintf("%s %s", llvmType(callTypes[i]), a)
		}
		if calleeFn != nil && calleeFn.HasReturn {
			// Determine the correct LLVM return type for the callee.
			retLLVMType := "i64"
			retTypeirType := typeir.Int
			if ptrReturnFns[lookupID] {
				retLLVMType = "i8*"
				retTypeirType = typeir.String
			} else if calleeFn.HasReturn && calleeFn.ReturnType != nil {
				rtn := typeir.TypeName(calleeFn.ReturnType)
				if rtn == "string" {
					retLLVMType = "i8*"
					retTypeirType = typeir.String
				} else if rtn == "float" {
					retLLVMType = "double"
					retTypeirType = typeir.Float
				}
			}
			// For extern functions, use i32 (C ABI). For user-defined, use i64.
			isExt := false
			if g.linkage != nil && calleeFn != nil {
				if le, ok := g.linkage[calleeFn.ID]; ok && le.IsExternal {
					isExt = true
				}
			}
			if isExt && retLLVMType == "i64" {
				retLLVMType = "i32"
			}
			r := g.newReg()
			g.buf.WriteString(fmt.Sprintf("  %s = call %s %s(%s)\n", r, retLLVMType, callee, strings.Join(paramParts, ", ")))
			g.push(r, retTypeirType)
		} else {
			g.buf.WriteString(fmt.Sprintf("  call void %s(%s)\n", callee, strings.Join(paramParts, ", ")))
		}

	// ==================== Platform operations ====================
	// These abstract operations are resolved by the translator into
	// platform-specific API calls. VXOBJ stores them as pure abstractions;
	// the translator provides the concrete platform implementation.

	case typeir.SysArgv:
		// __vx_platform_sysargv() -> i32 (argc)
		r := g.newReg()
		g.buf.WriteString(fmt.Sprintf("  %s = call i32 @__vx_platform_sysargv()\n", r))
		g.push(r, typeir.Int)

	case typeir.System:
		// __vx_platform_system(i8* cmd) -> i32
		cmd, _ := g.pop()
		r := g.newReg()
		g.buf.WriteString(fmt.Sprintf("  %s = call i32 @__vx_platform_system(i8* %s)\n", r, cmd))
		g.push(r, typeir.Int)

	case typeir.FileRead:
		// __vx_platform_fileread(i8* path) -> i8* (buffer)
		path, _ := g.pop()
		r := g.newReg()
		g.buf.WriteString(fmt.Sprintf("  %s = call i8* @__vx_platform_fileread(i8* %s)\n", r, path))
		g.push(r, typeir.String)

	case typeir.FileWrite:
		// __vx_platform_filewrite(i8* path, i8* data) -> i32
		data, _ := g.pop()
		path, _ := g.pop()
		r := g.newReg()
		g.buf.WriteString(fmt.Sprintf("  %s = call i32 @__vx_platform_filewrite(i8* %s, i8* %s)\n", r, path, data))
		g.push(r, typeir.Int)

	case typeir.FileExists:
		// __vx_platform_fileexists(i8* path) -> i32
		path, _ := g.pop()
		r := g.newReg()
		g.buf.WriteString(fmt.Sprintf("  %s = call i32 @__vx_platform_fileexists(i8* %s)\n", r, path))
		g.push(r, typeir.Bool)

	case typeir.Halt:
		g.buf.WriteString("  call void @__vx_platform_halt()\n")
		g.buf.WriteString("  unreachable\n")

	case typeir.I32Mod:
		lhs, rhs, _ := g.popTwo()
		r := g.newReg()
		g.buf.WriteString(fmt.Sprintf("  %s = srem i64 %s, %s\n", r, lhs, rhs))
		g.push(r, typeir.Int)

	default:
		// For unsupported ops, emit a comment
		g.buf.WriteString(fmt.Sprintf("  ; unsupported op: %s %s\n", op, strings.Join(args, ", ")))
	}

	return nil
}

// findFunc looks up a function by ID or name from the module function map.
func (g *ssaGen) findFunc(id string) *typeir.FunctionIR {
	if g.funcMap == nil {
		return nil
	}
	return g.funcMap[id]
}

// stackPop emits inline LLVM IR to pop an i64 from the local virtual stack.
// It returns the register name holding the popped value.
func stackPop(b *strings.Builder, counter *int, vsp string, vstack string) string {
	n := *counter
	*counter++
	b.WriteString(fmt.Sprintf("  %%sp_p%d = load i64, i64* %s\n", n, vsp))
	b.WriteString(fmt.Sprintf("  %%dec_p%d = add i64 %%sp_p%d, -1\n", n, n))
	b.WriteString(fmt.Sprintf("  store i64 %%dec_p%d, i64* %s\n", n, vsp))
	b.WriteString(fmt.Sprintf("  %%idx_p%d = getelementptr [256 x i64], [256 x i64]* %s, i64 0, i64 %%dec_p%d\n", n, vstack, n))
	b.WriteString(fmt.Sprintf("  %%val_p%d = load i64, i64* %%idx_p%d\n", n, n))
	return fmt.Sprintf("%%val_p%d", n)
}

// stackPush emits inline LLVM IR to push an i64 value onto the local virtual stack.
func stackPush(b *strings.Builder, counter *int, vsp string, vstack string, val string) string {
	n := *counter
	*counter++
	b.WriteString(fmt.Sprintf("  %%sp_q%d = load i64, i64* %s\n", n, vsp))
	b.WriteString(fmt.Sprintf("  %%idx_q%d = getelementptr [256 x i64], [256 x i64]* %s, i64 0, i64 %%sp_q%d\n", n, vstack, n))
	b.WriteString(fmt.Sprintf("  store i64 %s, i64* %%idx_q%d\n", val, n))
	b.WriteString(fmt.Sprintf("  %%inc_q%d = add i64 %%sp_q%d, 1\n", n, n))
	b.WriteString(fmt.Sprintf("  store i64 %%inc_q%d, i64* %s\n", n, vsp))
	return fmt.Sprintf("%%sp_q%d", n)
}

// generateWithBlocks generates LLVM IR with basic blocks for functions that have jumps.
func generateWithBlocks(b *strings.Builder, fn *typeir.FunctionIR, strIndex map[string]int, funcMap map[string]*typeir.FunctionIR, structTypes []typeir.StructTypeDef, linkage map[uint32]typeir.LinkageEntry, globals []string) error {
	// Phase 1: Build label→instructionIndex map from Noop instructions.
	// Noop instructions with a single non-empty arg carry label names
	// (emitted by emitLabel in typeirgen and emit_label in bootstrap_compiler).
	labelToIndex := make(map[string]int)
	for i, inst := range fn.Body {
		if inst.Op == typeir.Noop && len(inst.Args) > 0 && inst.Args[0] != "" {
			labelToIndex[inst.Args[0]] = i
		}
	}

	// Phase 2: Resolve a jump target arg to an instruction index.
	// Supports both conventions:
	//   - numeric string "8" → index 8 (used by some code paths)
	//   - label name "L1" → look up in labelToIndex
	resolveTarget := func(arg string) (int, bool) {
		if t, err := strconv.Atoi(arg); err == nil {
			return t, true
		}
		if idx, ok := labelToIndex[arg]; ok {
			return idx, true
		}
		return 0, false
	}

	// Discover jump targets
	targets := make(map[int]bool)
	targets[0] = true // entry block
	blockStarts := []int{0}

	for i, inst := range fn.Body {
		switch inst.Op {
		case typeir.Jump:
			if len(inst.Args) > 0 {
				if t, ok := resolveTarget(inst.Args[0]); ok {
					if !targets[t] {
						targets[t] = true
						blockStarts = append(blockStarts, t)
					}
				}
			}
		case typeir.JumpIfFalse, typeir.JumpIfTrue:
			// Args format: first arg is the target label, rest are metadata
			if len(inst.Args) > 0 {
				if t, ok := resolveTarget(inst.Args[0]); ok {
					if !targets[t] {
						targets[t] = true
						blockStarts = append(blockStarts, t)
					}
				}
			}
			// Fall-through also creates a block
			if i+1 < len(fn.Body) && !targets[i+1] {
				targets[i+1] = true
				blockStarts = append(blockStarts, i+1)
			}
		}
	}

	// Sort block starts
	sortInts(blockStarts)

	// Build block ranges
	type blockRange struct {
		start, end int
		name       string
	}
	var blocks []blockRange
	for idx, start := range blockStarts {
		end := len(fn.Body)
		if idx+1 < len(blockStarts) {
			end = blockStarts[idx+1]
		}
		name := fmt.Sprintf("bb%d", start)
		blocks = append(blocks, blockRange{start, end, name})
	}

	blockMap := make(map[int]string)
	for _, blk := range blocks {
		blockMap[blk.start] = blk.name
	}

	// Emit the stub entry block (allocas + param pushes must be in entry)
	// The entry block must not have any predecessors in LLVM IR, so we
	// always emit a dedicated entry that branches to the first real block (bb0).
	fmt.Fprintf(b, "entry:\n")
	generateAlloca(b, fn)
	// Push params in REVERSE order so the FIRST pop yields the FIRST param.
	// (TypeIRgen emits StoreVar for params in declaration order, and StoreVar
	// pops from stack so the last-pushed value is consumed first.)
	for i := len(fn.Params) - 1; i >= 0; i-- {
		p := fn.Params[i]
		pn := newStackCounter()
		ptn := typeir.TypeName(p.Type)
		if ptn == "string" {
			b.WriteString(fmt.Sprintf("  %%pp%d = ptrtoint i8* %%_%s to i64\n", pn, p.Name))
			b.WriteString(fmt.Sprintf("  call void @__vx_stack_push(i64 %%pp%d)\n", pn))
		} else if _, isStruct := p.Type.(typeir.StructTyp); isStruct {
			// Struct pointer is already i64.
			b.WriteString(fmt.Sprintf("  call void @__vx_stack_push(i64 %%_%s)\n", p.Name))
		} else if pt, ok := p.Type.(typeir.PointerTyp); ok {
			// Pointer to struct: push as i64.
			if _, isStruct := pt.To.(typeir.StructTyp); isStruct {
				b.WriteString(fmt.Sprintf("  call void @__vx_stack_push(i64 %%_%s)\n", p.Name))
			} else {
				b.WriteString(fmt.Sprintf("  %%pp%d = sext i32 %%_%s to i64\n", pn, p.Name))
				b.WriteString(fmt.Sprintf("  call void @__vx_stack_push(i64 %%pp%d)\n", pn))
			}
		} else {
			// int/bool params are declared i64 in user-defined functions;
			// push the value directly without sign-extension.
			b.WriteString(fmt.Sprintf("  call void @__vx_stack_push(i64 %%_%s)\n", p.Name))
		}
	}
	if len(blocks) > 0 {
		b.WriteString(fmt.Sprintf("  br label %%%s\n", blocks[0].name))
	} else {
		b.WriteString("  ret void\n")
	}

	// Emit each block
	for bi, blk := range blocks {
		b.WriteString(fmt.Sprintf("%s:\n", blk.name))

		terminated := false
		for i := blk.start; i < blk.end; i++ {
			inst := fn.Body[i]
			nextLabel := ""
			if i+1 < len(fn.Body) {
				if name, ok := blockMap[i+1]; ok {
					nextLabel = name
				} else {
					nextLabel = fmt.Sprintf("bb%d", i+1)
				}
			}
			switch inst.Op {
			case typeir.Jump, typeir.JumpIfFalse, typeir.JumpIfTrue,
				typeir.Return, typeir.ReturnVoid, typeir.Halt:
				terminated = true
			}
			if err := emitStackInst(b, &inst, targetBlockName(inst, blockMap, labelToIndex), nextLabel, fn, strIndex, funcMap, structTypes, linkage, globals); err != nil {
				return fmt.Errorf("inst %d: %w", i, err)
			}
		}

		if !terminated {
			if bi+1 < len(blocks) {
				nextBlockName := blocks[bi+1].name
				b.WriteString(fmt.Sprintf("  br label %%%s\n", nextBlockName))
			} else {
				b.WriteString("  ret void\n")
			}
		}
	}

	return nil
}

func targetBlockName(inst typeir.TypedInstruction, blockMap map[int]string, labelToIndex map[string]int) string {
	switch inst.Op {
	case typeir.Jump:
		if len(inst.Args) > 0 {
			return resolveJumpTarget(inst.Args[0], blockMap, labelToIndex)
		}
	case typeir.JumpIfFalse, typeir.JumpIfTrue:
		if len(inst.Args) > 0 {
			return resolveJumpTarget(inst.Args[0], blockMap, labelToIndex)
		}
	}
	return ""
}

// resolveJumpTarget resolves a jump target arg (numeric index or label name) to a block name.
func resolveJumpTarget(arg string, blockMap map[int]string, labelToIndex map[string]int) string {
	// Try numeric index first
	if t, err := strconv.Atoi(arg); err == nil {
		if name, ok := blockMap[t]; ok {
			return name
		}
	}
	// Try label name → instruction index → block name
	if idx, ok := labelToIndex[arg]; ok {
		if name, ok := blockMap[idx]; ok {
			return name
		}
		// Label index might not be a block start; find the block containing it
		for _, name := range blockMap {
			_ = name
		}
		// Linear scan for the block that contains this instruction
		for blockIdx, blockName := range blockMap {
			// Find the block's range by checking blockMap keys
			_ = blockIdx
			_ = blockName
		}
		// Simpler: find which block range contains idx
		sortedStarts := make([]int, 0, len(blockMap))
		for s := range blockMap {
			sortedStarts = append(sortedStarts, s)
		}
		sortInts(sortedStarts)
		for si, s := range sortedStarts {
			var e int
			if si+1 < len(sortedStarts) {
				e = sortedStarts[si+1]
			} else {
				e = 1 << 30
			}
			if idx >= s && idx < e {
				return blockMap[s]
			}
		}
	}
	return ""
}

// localNameForID resolves a numeric local ID to its global variable name.
// typeirgen allocates global IDs 0..len(globals)-1 in name-sorted order and
// appends params/locals after, so any ID below len(globals) is a global.
func localNameForID(id uint32, globals []string) (string, bool) {
	if int(id) < len(globals) {
		return globals[id], true
	}
	return "", false
}

func emitStackInst(b *strings.Builder, inst *typeir.TypedInstruction, target string, nextBlock string, fn *typeir.FunctionIR, strIndex map[string]int, funcMap map[string]*typeir.FunctionIR, structTypes []typeir.StructTypeDef, linkage map[uint32]typeir.LinkageEntry, globals []string) error {
	op := inst.Op
	args := inst.Args

	switch op {
	case typeir.ConstInt:
		val := "0"
		if len(args) > 0 {
			val = args[0]
		}
		n := newStackCounter()
		b.WriteString(fmt.Sprintf("  %%c%d = add i64 %s, 0\n", n, val))
		b.WriteString(fmt.Sprintf("  call void @__vx_stack_push(i64 %%c%d)\n", n))

	case typeir.ConstString:
		if len(args) > 0 {
			n := newStackCounter()
			idx, ok := strIndex[args[0]]
			if !ok {
				idx = 0
			}
			b.WriteString(fmt.Sprintf("  %%cs%d = getelementptr [%d x i8], [%d x i8]* @\"str.%d\", i64 0, i64 0\n", n, len(args[0])+1, len(args[0])+1, idx))
			b.WriteString(fmt.Sprintf("  %%cs%d_i = ptrtoint i8* %%cs%d to i64\n", n, n))
			b.WriteString(fmt.Sprintf("  call void @__vx_stack_push(i64 %%cs%d_i)\n", n))
		}

	case typeir.ConstBool:
		val := "0"
		if len(args) > 0 && (args[0] == "1" || args[0] == "true") {
			val = "1"
		}
		n := newStackCounter()
		b.WriteString(fmt.Sprintf("  %%cb%d = add i64 %s, 0\n", n, val))
		b.WriteString(fmt.Sprintf("  call void @__vx_stack_push(i64 %%cb%d)\n", n))

	case typeir.LoadVar:
		if len(args) > 0 {
			n := newStackCounter()
			vid, parseErr := strconv.Atoi(args[0])
			if parseErr != nil {
				break
			}
			typ, hasTyp := fn.LocalTypes[uint32(vid)]
			if !hasTyp {
				break
			}
			tn := typeir.TypeName(typ)
			gname, isGlobal := localNameForID(uint32(vid), globals)
			if isGlobal {
				// Shared global variable: load from the module-level slot.
				if tn == "string" {
					b.WriteString(fmt.Sprintf("  %%lv%d = load i8*, i8** @g_%s, align 8\n", n, gname))
					b.WriteString(fmt.Sprintf("  %%lv%d_i = ptrtoint i8* %%lv%d to i64\n", n, n))
					b.WriteString(fmt.Sprintf("  call void @__vx_stack_push(i64 %%lv%d_i)\n", n))
				} else {
					b.WriteString(fmt.Sprintf("  %%lv%d = load i64, i64* @g_%s, align 8\n", n, gname))
					b.WriteString(fmt.Sprintf("  call void @__vx_stack_push(i64 %%lv%d)\n", n))
				}
				break
			}
			if tn == "string" {
				b.WriteString(fmt.Sprintf("  %%lv%d = load i8*, i8** %%v%s, align 4\n", n, args[0]))
				b.WriteString(fmt.Sprintf("  %%lv%d_i = ptrtoint i8* %%lv%d to i64\n", n, n))
				b.WriteString(fmt.Sprintf("  call void @__vx_stack_push(i64 %%lv%d_i)\n", n))
			} else {
				// int/bool/unknown/struct: load the full i64 value.
				b.WriteString(fmt.Sprintf("  %%lv%d = load i64, i64* %%v%s, align 4\n", n, args[0]))
				b.WriteString(fmt.Sprintf("  call void @__vx_stack_push(i64 %%lv%d)\n", n))
			}
		}

	case typeir.StoreVar:
		if len(args) > 0 {
			n := newStackCounter()
			vid, parseErr := strconv.Atoi(args[0])
			if parseErr != nil {
				break
			}
			typ, hasTyp := fn.LocalTypes[uint32(vid)]
			if !hasTyp {
				break
			}
			b.WriteString(fmt.Sprintf("  %%sv%d = call i64 @__vx_stack_pop()\n", n))
			tn := typeir.TypeName(typ)
			gname, isGlobal := localNameForID(uint32(vid), globals)
			if isGlobal {
				if tn == "string" {
					b.WriteString(fmt.Sprintf("  %%sv%d_p = inttoptr i64 %%sv%d to i8*\n", n, n))
					b.WriteString(fmt.Sprintf("  store i8* %%sv%d_p, i8** @g_%s, align 8\n", n, gname))
				} else {
					b.WriteString(fmt.Sprintf("  store i64 %%sv%d, i64* @g_%s, align 8\n", n, gname))
				}
				break
			}
			if tn == "string" {
				b.WriteString(fmt.Sprintf("  %%sv%d_p = inttoptr i64 %%sv%d to i8*\n", n, n))
				b.WriteString(fmt.Sprintf("  store i8* %%sv%d_p, i8** %%v%s, align 4\n", n, args[0]))
			} else {
				// int/bool/float/unknown/struct: store the full i64 value
				// so opaque handles survive without truncation.
				b.WriteString(fmt.Sprintf("  store i64 %%sv%d, i64* %%v%s, align 4\n", n, args[0]))
			}
		}

	case typeir.I32Add:
		n := newStackCounter()
		b.WriteString(fmt.Sprintf("  %%a%d = call i64 @__vx_stack_pop()\n", n))
		b.WriteString(fmt.Sprintf("  %%b%d = call i64 @__vx_stack_pop()\n", n))
		b.WriteString(fmt.Sprintf("  %%c%d = add i64 %%a%d, %%b%d\n", n, n, n))
		b.WriteString(fmt.Sprintf("  call void @__vx_stack_push(i64 %%c%d)\n", n))

	case typeir.I32Sub:
		n := newStackCounter()
		b.WriteString(fmt.Sprintf("  %%a%d = call i64 @__vx_stack_pop()\n", n))
		b.WriteString(fmt.Sprintf("  %%b%d = call i64 @__vx_stack_pop()\n", n))
		b.WriteString(fmt.Sprintf("  %%c%d = sub i64 %%b%d, %%a%d\n", n, n, n))
		b.WriteString(fmt.Sprintf("  call void @__vx_stack_push(i64 %%c%d)\n", n))

	case typeir.I32Mul:
		n := newStackCounter()
		b.WriteString(fmt.Sprintf("  %%a%d = call i64 @__vx_stack_pop()\n", n))
		b.WriteString(fmt.Sprintf("  %%b%d = call i64 @__vx_stack_pop()\n", n))
		b.WriteString(fmt.Sprintf("  %%c%d = mul i64 %%b%d, %%a%d\n", n, n, n))
		b.WriteString(fmt.Sprintf("  call void @__vx_stack_push(i64 %%c%d)\n", n))

	case typeir.I32Div:
		n := newStackCounter()
		b.WriteString(fmt.Sprintf("  %%a%d = call i64 @__vx_stack_pop()\n", n))
		b.WriteString(fmt.Sprintf("  %%b%d = call i64 @__vx_stack_pop()\n", n))
		b.WriteString(fmt.Sprintf("  %%c%d = sdiv i64 %%b%d, %%a%d\n", n, n, n))
		b.WriteString(fmt.Sprintf("  call void @__vx_stack_push(i64 %%c%d)\n", n))

	case typeir.I32Mod:
		n := newStackCounter()
		b.WriteString(fmt.Sprintf("  %%a%d = call i64 @__vx_stack_pop()\n", n))
		b.WriteString(fmt.Sprintf("  %%b%d = call i64 @__vx_stack_pop()\n", n))
		b.WriteString(fmt.Sprintf("  %%c%d = srem i64 %%b%d, %%a%d\n", n, n, n))
		b.WriteString(fmt.Sprintf("  call void @__vx_stack_push(i64 %%c%d)\n", n))

	case typeir.I32Neg:
		n := newStackCounter()
		b.WriteString(fmt.Sprintf("  %%a%d = call i64 @__vx_stack_pop()\n", n))
		b.WriteString(fmt.Sprintf("  %%c%d = sub i64 0, %%a%d\n", n, n))
		b.WriteString(fmt.Sprintf("  call void @__vx_stack_push(i64 %%c%d)\n", n))

	case typeir.I32Eq:
		n := newStackCounter()
		b.WriteString(fmt.Sprintf("  %%a%d = call i64 @__vx_stack_pop()\n", n))
		b.WriteString(fmt.Sprintf("  %%b%d = call i64 @__vx_stack_pop()\n", n))
		b.WriteString(fmt.Sprintf("  %%c%d = icmp eq i64 %%a%d, %%b%d\n", n, n, n))
		b.WriteString(fmt.Sprintf("  %%d%d = zext i1 %%c%d to i64\n", n, n))
		b.WriteString(fmt.Sprintf("  call void @__vx_stack_push(i64 %%d%d)\n", n))

	case typeir.I32Neq:
		n := newStackCounter()
		b.WriteString(fmt.Sprintf("  %%a%d = call i64 @__vx_stack_pop()\n", n))
		b.WriteString(fmt.Sprintf("  %%b%d = call i64 @__vx_stack_pop()\n", n))
		b.WriteString(fmt.Sprintf("  %%c%d = icmp ne i64 %%a%d, %%b%d\n", n, n, n))
		b.WriteString(fmt.Sprintf("  %%d%d = zext i1 %%c%d to i64\n", n, n))
		b.WriteString(fmt.Sprintf("  call void @__vx_stack_push(i64 %%d%d)\n", n))

	case typeir.I32Lt:
		n := newStackCounter()
		b.WriteString(fmt.Sprintf("  %%a%d = call i64 @__vx_stack_pop()\n", n))
		b.WriteString(fmt.Sprintf("  %%b%d = call i64 @__vx_stack_pop()\n", n))
		b.WriteString(fmt.Sprintf("  %%c%d = icmp slt i64 %%b%d, %%a%d\n", n, n, n))
		b.WriteString(fmt.Sprintf("  %%d%d = zext i1 %%c%d to i64\n", n, n))
		b.WriteString(fmt.Sprintf("  call void @__vx_stack_push(i64 %%d%d)\n", n))

	case typeir.I32Gt:
		n := newStackCounter()
		b.WriteString(fmt.Sprintf("  %%a%d = call i64 @__vx_stack_pop()\n", n))
		b.WriteString(fmt.Sprintf("  %%b%d = call i64 @__vx_stack_pop()\n", n))
		b.WriteString(fmt.Sprintf("  %%c%d = icmp sgt i64 %%b%d, %%a%d\n", n, n, n))
		b.WriteString(fmt.Sprintf("  %%d%d = zext i1 %%c%d to i64\n", n, n))
		b.WriteString(fmt.Sprintf("  call void @__vx_stack_push(i64 %%d%d)\n", n))

	case typeir.I32Le:
		n := newStackCounter()
		b.WriteString(fmt.Sprintf("  %%a%d = call i64 @__vx_stack_pop()\n", n))
		b.WriteString(fmt.Sprintf("  %%b%d = call i64 @__vx_stack_pop()\n", n))
		b.WriteString(fmt.Sprintf("  %%c%d = icmp sle i64 %%b%d, %%a%d\n", n, n, n))
		b.WriteString(fmt.Sprintf("  %%d%d = zext i1 %%c%d to i64\n", n, n))
		b.WriteString(fmt.Sprintf("  call void @__vx_stack_push(i64 %%d%d)\n", n))

	case typeir.I32Ge:
		n := newStackCounter()
		b.WriteString(fmt.Sprintf("  %%a%d = call i64 @__vx_stack_pop()\n", n))
		b.WriteString(fmt.Sprintf("  %%b%d = call i64 @__vx_stack_pop()\n", n))
		b.WriteString(fmt.Sprintf("  %%c%d = icmp sge i64 %%b%d, %%a%d\n", n, n, n))
		b.WriteString(fmt.Sprintf("  %%d%d = zext i1 %%c%d to i64\n", n, n))
		b.WriteString(fmt.Sprintf("  call void @__vx_stack_push(i64 %%d%d)\n", n))

	case typeir.NeqInt:
		n := newStackCounter()
		b.WriteString(fmt.Sprintf("  %%a%d = call i64 @__vx_stack_pop()\n", n))
		b.WriteString(fmt.Sprintf("  %%b%d = call i64 @__vx_stack_pop()\n", n))
		b.WriteString(fmt.Sprintf("  %%c%d = icmp ne i64 %%a%d, %%b%d\n", n, n, n))
		b.WriteString(fmt.Sprintf("  %%d%d = zext i1 %%c%d to i64\n", n, n))
		b.WriteString(fmt.Sprintf("  call void @__vx_stack_push(i64 %%d%d)\n", n))

	case typeir.BoolNot:
		n := newStackCounter()
		b.WriteString(fmt.Sprintf("  %%bn%d = call i64 @__vx_stack_pop()\n", n))
		b.WriteString(fmt.Sprintf("  %%bn%d_c = icmp eq i64 %%bn%d, 0\n", n, n))
		b.WriteString(fmt.Sprintf("  %%bn%d_z = zext i1 %%bn%d_c to i64\n", n, n))
		b.WriteString(fmt.Sprintf("  call void @__vx_stack_push(i64 %%bn%d_z)\n", n))

	case typeir.BoolAnd:
		n := newStackCounter()
		b.WriteString(fmt.Sprintf("  %%ba1%d = call i64 @__vx_stack_pop()\n", n))
		b.WriteString(fmt.Sprintf("  %%ba2%d = call i64 @__vx_stack_pop()\n", n))
		b.WriteString(fmt.Sprintf("  %%ba%d = and i64 %%ba2%d, %%ba1%d\n", n, n, n))
		b.WriteString(fmt.Sprintf("  call void @__vx_stack_push(i64 %%ba%d)\n", n))

	case typeir.BoolOr:
		n := newStackCounter()
		b.WriteString(fmt.Sprintf("  %%bo1%d = call i64 @__vx_stack_pop()\n", n))
		b.WriteString(fmt.Sprintf("  %%bo2%d = call i64 @__vx_stack_pop()\n", n))
		b.WriteString(fmt.Sprintf("  %%bo%d = or i64 %%bo2%d, %%bo1%d\n", n, n, n))
		b.WriteString(fmt.Sprintf("  call void @__vx_stack_push(i64 %%bo%d)\n", n))

	case typeir.StrEq:
		n := newStackCounter()
		b.WriteString(fmt.Sprintf("  %%a%d = call i64 @__vx_stack_pop()\n", n))
		b.WriteString(fmt.Sprintf("  %%b%d = call i64 @__vx_stack_pop()\n", n))
		b.WriteString(fmt.Sprintf("  %%c%d = inttoptr i64 %%b%d to i8*\n", n, n))
		b.WriteString(fmt.Sprintf("  %%d%d = inttoptr i64 %%a%d to i8*\n", n, n))
		b.WriteString(fmt.Sprintf("  %%e%d = call i32 @strcmp(i8* %%d%d, i8* %%c%d)\n", n, n, n))
		b.WriteString(fmt.Sprintf("  %%f%d = icmp eq i32 %%e%d, 0\n", n, n))
		b.WriteString(fmt.Sprintf("  %%g%d = zext i1 %%f%d to i64\n", n, n))
		b.WriteString(fmt.Sprintf("  call void @__vx_stack_push(i64 %%g%d)\n", n))

	case typeir.StrConcat:
		n := newStackCounter()
		b.WriteString(fmt.Sprintf("  %%sa%d = call i64 @__vx_stack_pop()\n", n))
		b.WriteString(fmt.Sprintf("  %%sb%d = call i64 @__vx_stack_pop()\n", n))
		b.WriteString(fmt.Sprintf("  %%sa_p%d = inttoptr i64 %%sa%d to i8*\n", n, n))
		b.WriteString(fmt.Sprintf("  %%sb_p%d = inttoptr i64 %%sb%d to i8*\n", n, n))
		b.WriteString(fmt.Sprintf("  %%sc%d = call i8* @__vx_strcat(i8* %%sb_p%d, i8* %%sa_p%d)\n", n, n, n))
		b.WriteString(fmt.Sprintf("  %%sc_i%d = ptrtoint i8* %%sc%d to i64\n", n, n))
		b.WriteString(fmt.Sprintf("  call void @__vx_stack_push(i64 %%sc_i%d)\n", n))

	case typeir.ArrayConcat:
		n := newStackCounter()
		b.WriteString(fmt.Sprintf("  %%ac_a%d = call i64 @__vx_stack_pop()\n", n))
		b.WriteString(fmt.Sprintf("  %%ac_b%d = call i64 @__vx_stack_pop()\n", n))
		b.WriteString(fmt.Sprintf("  %%ac_r%d = call i64 @__vx_array_concat(i64 %%ac_b%d, i64 %%ac_a%d)\n", n, n, n))
		b.WriteString(fmt.Sprintf("  call void @__vx_stack_push(i64 %%ac_r%d)\n", n))

	case typeir.Return:
		if len(args) > 0 && args[0] != "" {
			n := newStackCounter()
			b.WriteString(fmt.Sprintf("  %%r%d = call i64 @__vx_stack_pop()\n", n))
			// Check return type to decide how to handle the return value.
			var retType typeir.Typ = typeir.Void
			if fn != nil && fn.ReturnType != nil {
				retType = fn.ReturnType
			}
			retTypeName := typeir.TypeName(retType)
			if retTypeName == "string" {
				b.WriteString(fmt.Sprintf("  %%r%d_p = inttoptr i64 %%r%d to i8*\n", n, n))
				b.WriteString(fmt.Sprintf("  ret i8* %%r%d_p\n", n))
			} else if _, isStruct := retType.(typeir.StructTyp); isStruct {
				// Struct type: the stack holds an i64 pointer. Return the pointer
				// as the struct type (LLVM will handle the cast).
				b.WriteString(fmt.Sprintf("  ret i64 %%r%d\n", n))
			} else {
				// Use i64 for int/bool returns to match the function signature.
				retLLVMType := "i64"
				if retTypeName == "float" {
					retLLVMType = "double"
				}
				b.WriteString(fmt.Sprintf("  ret %s %%r%d\n", retLLVMType, n))
			}
		} else {
			b.WriteString("  ret void\n")
		}

	case typeir.ReturnVoid:
		b.WriteString("  ret void\n")

	case typeir.Jump:
		if target != "" {
			b.WriteString(fmt.Sprintf("  br label %%%s\n", target))
		}

	case typeir.JumpIfFalse:
		n := newStackCounter()
		b.WriteString(fmt.Sprintf("  %%jf%d = call i64 @__vx_stack_pop()\n", n))
		b.WriteString(fmt.Sprintf("  %%jf%d_c = icmp eq i64 %%jf%d, 0\n", n, n))
		b.WriteString(fmt.Sprintf("  ; JumpIfFalse args=%v target=%q next=%q\n", args, target, nextBlock))
		if target != "" && nextBlock != "" {
			b.WriteString(fmt.Sprintf("  br i1 %%jf%d_c, label %%%s, label %%%s\n", n, target, nextBlock))
		} else if target != "" {
			b.WriteString(fmt.Sprintf("  br label %%%s\n", target))
		} else if nextBlock != "" {
			b.WriteString(fmt.Sprintf("  br label %%%s\n", nextBlock))
		}

	case typeir.JumpIfTrue:
		n := newStackCounter()
		b.WriteString(fmt.Sprintf("  %%jt%d = call i64 @__vx_stack_pop()\n", n))
		b.WriteString(fmt.Sprintf("  %%jt%d_c = icmp ne i64 %%jt%d, 0\n", n, n))
		if target != "" && nextBlock != "" {
			b.WriteString(fmt.Sprintf("  br i1 %%jt%d_c, label %%%s, label %%%s\n", n, target, nextBlock))
		} else if target != "" {
			b.WriteString(fmt.Sprintf("  br label %%%s\n", target))
		}

	case typeir.Call:
		if len(args) == 0 {
			break
		}
		callee := ""
		paramTypes := args[1:]
		if strings.HasPrefix(args[0], ";") {
			callee = args[0][1:]
		} else {
			callee = args[0]
		}
		if callee == "" {
			break
		}
		n := newStackCounter()
		numParams := len(paramTypes)
		// Pop params in reverse (stack order)
		params := make([]string, numParams)
		ptrParams := ptrParamFns[callee]
		// Look up callee's function info for param types
		calleeFN := funcMap[callee]
		// Module-defined (non-extern) functions receive int/bool params as
		// i64 so opaque handles (FILE*) pass through without truncation.
		isModuleFn := calleeFN != nil
		if calleeFN != nil && linkage != nil {
			if le, ok := linkage[calleeFN.ID]; ok {
				isModuleFn = !le.IsExternal
			}
		}
		for i := numParams - 1; i >= 0; i-- {
			pi := newStackCounter()
			b.WriteString(fmt.Sprintf("  %%cp%d = call i64 @__vx_stack_pop()\n", pi))
			isPtr := paramTypes[i] == "string" || paramTypes[i] == "pointer" ||
				(ptrParams != nil && ptrParams[i])
			// Check if this parameter is a struct type (passed as i64 pointer)
			isStructParam := false
			if calleeFN != nil && i < len(calleeFN.Params) {
				pt := calleeFN.Params[i].Type
				if _, ok := pt.(typeir.StructTyp); ok {
					isStructParam = true
				}
				if pt2, ok := pt.(typeir.PointerTyp); ok {
					if _, ok := pt2.To.(typeir.StructTyp); ok {
						isStructParam = true
					}
				}
			}
			if isPtr {
				b.WriteString(fmt.Sprintf("  %%cp%d_p = inttoptr i64 %%cp%d to i8*\n", pi, pi))
				params[i] = fmt.Sprintf("i8* %%cp%d_p", pi)
			} else if isStructParam {
				params[i] = fmt.Sprintf("i64 %%cp%d", pi)
			} else if callee == "malloc" && i == 0 {
				params[i] = fmt.Sprintf("i64 %%cp%d", pi)
			} else if callee == "calloc" && i < 2 {
				params[i] = fmt.Sprintf("i64 %%cp%d", pi)
			} else if callee == "realloc" && i == 1 {
				params[i] = fmt.Sprintf("i64 %%cp%d", pi)
			} else if callee == "strcmp" || callee == "strncmp" || callee == "strlen" || callee == "memcpy" || callee == "memset" || callee == "memmove" {
				// Known C functions that take i8* pointer params regardless of VX type
				b.WriteString(fmt.Sprintf("  %%cp%d_p = inttoptr i64 %%cp%d to i8*\n", pi, pi))
				params[i] = fmt.Sprintf("i8* %%cp%d_p", pi)
			} else if isModuleFn && (paramTypes[i] == "int" || paramTypes[i] == "bool") {
				// Module-defined function: pass the full i64 value.
				params[i] = fmt.Sprintf("i64 %%cp%d", pi)
			} else {
				b.WriteString(fmt.Sprintf("  %%cp%d_t = trunc i64 %%cp%d to i32\n", pi, pi))
				params[i] = fmt.Sprintf("i32 %%cp%d_t", pi)
			}
		}
		paramList := strings.Join(params, ", ")
		// Builtins that return their result via the virtual stack instead of
		// a return value (the translator's builtinRuntime implementations).
		voidFns := map[string]bool{"len": true, "ord": true, "chr": true, "itoa": true,
			"__vx_stack_push": true, "__vx_stack_pop": true}
		// out/println map to the translator's @out wrapper: void return,
		// i8* argument. The wrapper does not push a result onto the virtual
		// stack, so push a dummy 0 afterwards to keep the operand stack
		// balanced for the caller's trailing Pop.
		if callee == "out" || callee == "println" {
			b.WriteString(fmt.Sprintf("  call void @out(%s)\n", paramList))
			dn := newStackCounter()
			b.WriteString(fmt.Sprintf("  %%od%d = add i64 0, 0\n", dn))
			b.WriteString(fmt.Sprintf("  call void @__vx_stack_push(i64 %%od%d)\n", dn))
		} else if voidFns[callee] {
			b.WriteString(fmt.Sprintf("  call void @%s(%s)\n", callee, paramList))
		} else if callee == "strcmp" {
			b.WriteString(fmt.Sprintf("  %%cr%d = call i32 @strcmp(%s)\n", n, paramList))
			b.WriteString(fmt.Sprintf("  %%cr%d_e = sext i32 %%cr%d to i64\n", n, n))
			b.WriteString(fmt.Sprintf("  call void @__vx_stack_push(i64 %%cr%d_e)\n", n))
		} else if ptrReturnFns[callee] {
			// Known pointer-returning extern (fopen, malloc, …).
			b.WriteString(fmt.Sprintf("  %%cr%d = call i8* @%s(%s)\n", n, callee, paramList))
			b.WriteString(fmt.Sprintf("  %%cr%d_e = ptrtoint i8* %%cr%d to i64\n", n, n))
			b.WriteString(fmt.Sprintf("  call void @__vx_stack_push(i64 %%cr%d_e)\n", n))
		} else if calleeFN != nil && calleeFN.ReturnType != nil {
			rt := calleeFN.ReturnType
			rtn := typeir.TypeName(rt)
			if rtn == "void" {
				// Void user function (e.g. sknl(), nxt()): call without a
				// return value; nothing is pushed onto the operand stack.
				b.WriteString(fmt.Sprintf("  call void @%s(%s)\n", callee, paramList))
			} else if rtn == "string" {
				b.WriteString(fmt.Sprintf("  %%cr%d = call i8* @%s(%s)\n", n, callee, paramList))
				b.WriteString(fmt.Sprintf("  %%cr%d_e = ptrtoint i8* %%cr%d to i64\n", n, n))
				b.WriteString(fmt.Sprintf("  call void @__vx_stack_push(i64 %%cr%d_e)\n", n))
			} else if _, isStruct := rt.(typeir.StructTyp); isStruct {
				// Struct-returning function: returns i64 pointer
				b.WriteString(fmt.Sprintf("  %%cr%d = call i64 @%s(%s)\n", n, callee, paramList))
				b.WriteString(fmt.Sprintf("  call void @__vx_stack_push(i64 %%cr%d)\n", n))
			} else if isModuleFn {
				// Module-defined function returns i64 (int/bool).
				b.WriteString(fmt.Sprintf("  %%cr%d = call i64 @%s(%s)\n", n, callee, paramList))
				b.WriteString(fmt.Sprintf("  call void @__vx_stack_push(i64 %%cr%d)\n", n))
			} else {
				b.WriteString(fmt.Sprintf("  %%cr%d = call i32 @%s(%s)\n", n, callee, paramList))
				b.WriteString(fmt.Sprintf("  %%cr%d_e = sext i32 %%cr%d to i64\n", n, n))
				b.WriteString(fmt.Sprintf("  call void @__vx_stack_push(i64 %%cr%d_e)\n", n))
			}
		} else {
			b.WriteString(fmt.Sprintf("  %%cr%d = call i32 @%s(%s)\n", n, callee, paramList))
			b.WriteString(fmt.Sprintf("  %%cr%d_e = sext i32 %%cr%d to i64\n", n, n))
			b.WriteString(fmt.Sprintf("  call void @__vx_stack_push(i64 %%cr%d_e)\n", n))
		}

	// ==================== Platform operations (stack mode) ====================
	case typeir.SysArgv:
		n := newStackCounter()
		b.WriteString(fmt.Sprintf("  %%sa%d = call i32 @__vx_platform_sysargv()\n", n))
		b.WriteString(fmt.Sprintf("  %%sa_ext%d = sext i32 %%sa%d to i64\n", n, n))
		b.WriteString(fmt.Sprintf("  call void @__vx_stack_push(i64 %%sa_ext%d)\n", n))

	case typeir.System:
		n := newStackCounter()
		b.WriteString(fmt.Sprintf("  %%cmd%d = call i64 @__vx_stack_pop()\n", n))
		b.WriteString(fmt.Sprintf("  %%cmd_ptr%d = inttoptr i64 %%cmd%d to i8*\n", n, n))
		b.WriteString(fmt.Sprintf("  %%sys_ret%d = call i32 @__vx_platform_system(i8* %%cmd_ptr%d)\n", n, n))
		b.WriteString(fmt.Sprintf("  %%sys_ext%d = sext i32 %%sys_ret%d to i64\n", n, n))
		b.WriteString(fmt.Sprintf("  call void @__vx_stack_push(i64 %%sys_ext%d)\n", n))

	case typeir.FileRead:
		n := newStackCounter()
		b.WriteString(fmt.Sprintf("  %%fr_path%d = call i64 @__vx_stack_pop()\n", n))
		b.WriteString(fmt.Sprintf("  %%fr_ptr%d = inttoptr i64 %%fr_path%d to i8*\n", n, n))
		b.WriteString(fmt.Sprintf("  %%fr_ret%d = call i8* @__vx_platform_fileread(i8* %%fr_ptr%d)\n", n, n))
		b.WriteString(fmt.Sprintf("  %%fr_int%d = ptrtoint i8* %%fr_ret%d to i64\n", n, n))
		b.WriteString(fmt.Sprintf("  call void @__vx_stack_push(i64 %%fr_int%d)\n", n))

	case typeir.FileWrite:
		n := newStackCounter()
		b.WriteString(fmt.Sprintf("  %%fw_data%d = call i64 @__vx_stack_pop()\n", n))
		b.WriteString(fmt.Sprintf("  %%fw_path%d = call i64 @__vx_stack_pop()\n", n))
		b.WriteString(fmt.Sprintf("  %%fw_dptr%d = inttoptr i64 %%fw_data%d to i8*\n", n, n))
		b.WriteString(fmt.Sprintf("  %%fw_pptr%d = inttoptr i64 %%fw_path%d to i8*\n", n, n))
		b.WriteString(fmt.Sprintf("  %%fw_ret%d = call i32 @__vx_platform_filewrite(i8* %%fw_pptr%d, i8* %%fw_dptr%d)\n", n, n, n))
		b.WriteString(fmt.Sprintf("  %%fw_ext%d = sext i32 %%fw_ret%d to i64\n", n, n))
		b.WriteString(fmt.Sprintf("  call void @__vx_stack_push(i64 %%fw_ext%d)\n", n))

	case typeir.FileExists:
		n := newStackCounter()
		b.WriteString(fmt.Sprintf("  %%fe_path%d = call i64 @__vx_stack_pop()\n", n))
		b.WriteString(fmt.Sprintf("  %%fe_ptr%d = inttoptr i64 %%fe_path%d to i8*\n", n, n))
		b.WriteString(fmt.Sprintf("  %%fe_ret%d = call i32 @__vx_platform_fileexists(i8* %%fe_ptr%d)\n", n, n))
		b.WriteString(fmt.Sprintf("  %%fe_ext%d = sext i32 %%fe_ret%d to i64\n", n, n))
		b.WriteString(fmt.Sprintf("  call void @__vx_stack_push(i64 %%fe_ext%d)\n", n))

	case typeir.Halt:
		b.WriteString("  call void @__vx_platform_halt()\n")
		b.WriteString("  unreachable\n")

	case typeir.Noop:
		// No operation

	case typeir.Pop:
		fmt.Fprintf(b, "  call i64 @__vx_stack_pop()\n")

	case typeir.Dup:
		n := newStackCounter()
		b.WriteString(fmt.Sprintf("  %%dup%d = call i64 @__vx_stack_pop()\n", n))
		b.WriteString(fmt.Sprintf("  call void @__vx_stack_push(i64 %%dup%d)\n", n))
		b.WriteString(fmt.Sprintf("  call void @__vx_stack_push(i64 %%dup%d)\n", n))

	// ==================== Struct / Pointer Operations ====================

	// Newz: heap-allocate a struct. The struct name is in inst.Types[0].
	case typeir.Newz:
		n := newStackCounter()
		structName := ""
		if len(inst.Types) > 0 {
			if st, ok := inst.Types[0].(typeir.StructTyp); ok {
				structName = st.Name
			}
		}
		if structName == "" && len(inst.Args) > 0 {
			structName = inst.Args[0]
		}
		st := findStructType(structTypes, structName)
		if st == nil {
			// Unknown struct: push null
			b.WriteString(fmt.Sprintf("  %%nz%d = inttoptr i64 0 to i8*\n", n))
			b.WriteString(fmt.Sprintf("  %%nz%di = ptrtoint i8* %%nz%d to i64\n", n, n))
			b.WriteString(fmt.Sprintf("  call void @__vx_stack_push(i64 %%nz%di)\n", n))
			break
		}
		// malloc(sizeof(%struct.Name))
		b.WriteString(fmt.Sprintf("  %%nz_malloc%d = call i8* @malloc(i64 ptrtoint (%%struct.%s* getelementptr (%%struct.%s, %%struct.%s* null, i32 1) to i64))\n",
			n, structName, structName, structName))
		b.WriteString(fmt.Sprintf("  %%nz%di = ptrtoint i8* %%nz_malloc%d to i64\n", n, n))
		b.WriteString(fmt.Sprintf("  call void @__vx_stack_push(i64 %%nz%di)\n", n))

	// AddressOf: push the address of a local variable.
	case typeir.AddressOf:
		// In the stack model the value on the stack already IS the data
		// pointer (strings, heap structs), so &x is the identity here.
		// Used by len(&s) to compute the string length.
		// Not currently emitted for structs; keep as no-op.

	// Deref: load through a pointer.
	case typeir.Deref:
		// Pop pointer (i64), cast to i8*, load i32, push back.
		n := newStackCounter()
		b.WriteString(fmt.Sprintf("  %%dr_ptr%d = call i64 @__vx_stack_pop()\n", n))
		b.WriteString(fmt.Sprintf("  %%dr_p%d = inttoptr i64 %%dr_ptr%d to i32*\n", n, n))
		b.WriteString(fmt.Sprintf("  %%dr_v%d = load i32, i32* %%dr_p%d, align 4\n", n, n))
		b.WriteString(fmt.Sprintf("  %%dr_i%d = sext i32 %%dr_v%d to i64\n", n, n))
		b.WriteString(fmt.Sprintf("  call void @__vx_stack_push(i64 %%dr_i%d)\n", n))

	// PropertyGet: pop struct pointer (i64), GEP to field, load, push.
	case typeir.PropertyGet:
		n := newStackCounter()
		fieldName := ""
		structNameHint := ""
		if len(args) > 0 {
			fieldName = args[0]
		}
		if len(args) > 1 {
			structNameHint = args[1]
		}
		// Pop struct pointer from stack
		b.WriteString(fmt.Sprintf("  %%pg_sptr%d = call i64 @__vx_stack_pop()\n", n))
		// Find the struct type. Use the hint if provided, otherwise scan all types.
		var foundST *typeir.StructTypeDef
		var fieldIdx int
		if structNameHint != "" {
			if st := findStructType(structTypes, structNameHint); st != nil {
				if idx, _, ok := findStructFieldIndex(st, fieldName); ok {
					foundST = st
					fieldIdx = idx
				}
			}
		}
		if foundST == nil {
			for si := range structTypes {
				st := &structTypes[si]
				if idx, _, ok := findStructFieldIndex(st, fieldName); ok {
					foundST = st
					fieldIdx = idx
					break
				}
			}
		}
		if foundST != nil {
			b.WriteString(fmt.Sprintf("  %%pg_p%d = inttoptr i64 %%pg_sptr%d to %%struct.%s*\n", n, n, foundST.Name))
			fldPtr := llvmFieldGEP(b, n, fmt.Sprintf("%%pg_p%d", n), foundST.Name, fieldIdx, "i64")
			// All struct fields are i64 (see generateStructTypeDecl).
			b.WriteString(fmt.Sprintf("  %%pg_v%d = load i64, i64* %s, align 4\n", n, fldPtr))
			b.WriteString(fmt.Sprintf("  call void @__vx_stack_push(i64 %%pg_v%d)\n", n))
		} else {
			// Fallback: push 0
			b.WriteString(fmt.Sprintf("  call void @__vx_stack_push(i64 0)\n"))
		}

	// PointerMember: pop pointer (i64), GEP to member, load, push.
	case typeir.PointerMember:
		n := newStackCounter()
		memberName := ""
		structNameHint := ""
		if len(args) > 0 {
			memberName = args[0]
		}
		if len(args) > 1 {
			structNameHint = args[1]
		}
		b.WriteString(fmt.Sprintf("  %%pm_sptr%d = call i64 @__vx_stack_pop()\n", n))
		var foundST *typeir.StructTypeDef
		var fieldIdx int
		if structNameHint != "" {
			if st := findStructType(structTypes, structNameHint); st != nil {
				if idx, _, ok := findStructFieldIndex(st, memberName); ok {
					foundST = st
					fieldIdx = idx
				}
			}
		}
		if foundST == nil {
			for si := range structTypes {
				st := &structTypes[si]
				if idx, _, ok := findStructFieldIndex(st, memberName); ok {
					foundST = st
					fieldIdx = idx
					break
				}
			}
		}
		if foundST != nil {
			b.WriteString(fmt.Sprintf("  %%pm_p%d = inttoptr i64 %%pm_sptr%d to %%struct.%s*\n", n, n, foundST.Name))
			fldPtr := llvmFieldGEP(b, n, fmt.Sprintf("%%pm_p%d", n), foundST.Name, fieldIdx, "i64")
			// All struct fields are i64 (see generateStructTypeDecl).
			b.WriteString(fmt.Sprintf("  %%pm_v%d = load i64, i64* %s, align 4\n", n, fldPtr))
			b.WriteString(fmt.Sprintf("  call void @__vx_stack_push(i64 %%pm_v%d)\n", n))
		} else {
			b.WriteString(fmt.Sprintf("  call void @__vx_stack_push(i64 0)\n"))
		}

	// FieldLoad: same as PropertyGet — pop struct ptr, GEP, load field.
	case typeir.FieldLoad:
		fieldName := ""
		if len(args) > 0 {
			fieldName = args[0]
		}
		// Reuse PropertyGet logic
		pgInst := typeir.TypedInstruction{Op: typeir.PropertyGet, Args: []string{fieldName}}
		return emitStackInst(b, &pgInst, target, nextBlock, fn, strIndex, funcMap, structTypes, linkage, globals)

	// StructNew: struct type definition — handled at module level.
	// No runtime code needed.
	case typeir.StructNew:
		// Type declaration already emitted by Generate(). Skip.

	// FieldStore: pop value, pop struct ptr, GEP, store.
	case typeir.FieldStore:
		n := newStackCounter()
		fieldName := ""
		structNameHint := ""
		if len(args) > 0 {
			fieldName = args[0]
		}
		if len(args) > 1 {
			structNameHint = args[1]
		}
		// Pop struct pointer (i64) from stack (pushed after value)
		b.WriteString(fmt.Sprintf("  %%fs_sptr%d = call i64 @__vx_stack_pop()\n", n))
		// Pop value (i64) from stack (pushed first)
		b.WriteString(fmt.Sprintf("  %%fs_val%d = call i64 @__vx_stack_pop()\n", n))
		// Find the struct type
		var foundST *typeir.StructTypeDef
		var fieldIdx int
		if structNameHint != "" {
			if st := findStructType(structTypes, structNameHint); st != nil {
				if idx, _, ok := findStructFieldIndex(st, fieldName); ok {
					foundST = st
					fieldIdx = idx
				}
			}
		}
		if foundST == nil {
			for si := range structTypes {
				st := &structTypes[si]
				if idx, _, ok := findStructFieldIndex(st, fieldName); ok {
					foundST = st
					fieldIdx = idx
					break
				}
			}
		}
		if foundST != nil {
			b.WriteString(fmt.Sprintf("  %%fs_p%d = inttoptr i64 %%fs_sptr%d to %%struct.%s*\n", n, n, foundST.Name))
			fldPtr := llvmFieldGEP(b, n, fmt.Sprintf("%%fs_p%d", n), foundST.Name, fieldIdx, "i64")
			// All struct fields are i64 (see generateStructTypeDecl).
			b.WriteString(fmt.Sprintf("  store i64 %%fs_val%d, i64* %s, align 4\n", n, fldPtr))
		} else {
			// Fallback: silently pop value
			nop := newStackCounter()
			b.WriteString(fmt.Sprintf("  %%fs_nop%d = add i64 0, 0\n", nop))
		}

	// ==================== Array Index Operations ====================

	// IndexGet: pop index, pop pointer, load element at pointer[index], push.
	// For string targets (s[i]) the byte is copied into a fresh 1-char heap
	// string via chr(), so comparisons like s[i] == 'c' use strcmp on an
	// independent null-terminated string (a raw pointer into s would keep
	// trailing characters and fail strcmp).
	case typeir.IndexGet:
		// Non-string index access: use i8* for byte-level access.
		// This makes buf[i] work for character buffers (1 byte per element).
		n := newStackCounter()
		b.WriteString(fmt.Sprintf("  %%ig_idx%d = call i64 @__vx_stack_pop()\n", n))
		b.WriteString(fmt.Sprintf("  %%ig_ptr%d = call i64 @__vx_stack_pop()\n", n))
		isStringIdx := len(inst.Args) > 0 && inst.Args[0] == "s"
		if isStringIdx {
			b.WriteString(fmt.Sprintf("  %%ig_p%d = inttoptr i64 %%ig_ptr%d to i8*\n", n, n))
			b.WriteString(fmt.Sprintf("  %%ig_gep%d = getelementptr i8, i8* %%ig_p%d, i64 %%ig_idx%d\n", n, n, n))
			b.WriteString(fmt.Sprintf("  %%ig_c%d = load i8, i8* %%ig_gep%d, align 1\n", n, n))
			b.WriteString(fmt.Sprintf("  %%ig_c%d_z = zext i8 %%ig_c%d to i64\n", n, n))
			b.WriteString(fmt.Sprintf("  call void @chr(i32 %%ig_c%d_z)\n", n))
		} else {
			// Use i8* GEP so buf[i] accesses individual bytes.
			b.WriteString(fmt.Sprintf("  %%ig_p%d = inttoptr i64 %%ig_ptr%d to i8*\n", n, n))
			b.WriteString(fmt.Sprintf("  %%ig_gep%d = getelementptr i8, i8* %%ig_p%d, i64 %%ig_idx%d\n", n, n, n))
			b.WriteString(fmt.Sprintf("  %%ig_v%d = load i8, i8* %%ig_gep%d, align 1\n", n, n))
			b.WriteString(fmt.Sprintf("  %%ig_i%d = zext i8 %%ig_v%d to i64\n", n, n))
			b.WriteString(fmt.Sprintf("  call void @__vx_stack_push(i64 %%ig_i%d)\n", n))
		}

	// IndexSet: pop value, pop index, pop pointer, store value at pointer[index].
	case typeir.IndexSet:
		n := newStackCounter()
		b.WriteString(fmt.Sprintf("  %%is_idx%d = call i64 @__vx_stack_pop()\n", n))
		b.WriteString(fmt.Sprintf("  %%is_ptr%d = call i64 @__vx_stack_pop()\n", n))
		b.WriteString(fmt.Sprintf("  %%is_val%d = call i64 @__vx_stack_pop()\n", n))
		// Use i8* GEP so buf[i] stores individual bytes (for character buffers).
		b.WriteString(fmt.Sprintf("  %%is_p%d = inttoptr i64 %%is_ptr%d to i8*\n", n, n))
		b.WriteString(fmt.Sprintf("  %%is_gep%d = getelementptr i8, i8* %%is_p%d, i64 %%is_idx%d\n", n, n, n))
		b.WriteString(fmt.Sprintf("  %%is_v%d = trunc i64 %%is_val%d to i8\n", n, n))
		b.WriteString(fmt.Sprintf("  store i8 %%is_v%d, i8* %%is_gep%d, align 1\n", n, n))

	default:
		// Unknown ops: silently skip to avoid corrupting the virtual stack
	}

	return nil
}

var stackCounter int

func newStackCounter() int {
	stackCounter++
	return stackCounter
}

func sortInts(s []int) {
	for i := 0; i < len(s); i++ {
		for j := i + 1; j < len(s); j++ {
			if s[j] < s[i] {
				s[i], s[j] = s[j], s[i]
			}
		}
	}
}

// escapeLLVMString escapes a Go string for use inside an LLVM IR `c"..."`
// string constant. Backslashes, double quotes, and non-printable characters
// are escaped using LLVM's \xx hex escapes.
func escapeLLVMString(s string) string {
	var b strings.Builder
	for _, r := range s {
		switch r {
		case '\\':
			b.WriteString("\\\\")
		case '"':
			b.WriteString("\\22")
		case '\n':
			b.WriteString("\\0A")
		case '\t':
			b.WriteString("\\09")
		case '\r':
			b.WriteString("\\0D")
		case 0:
			b.WriteString("\\00")
		default:
			if r < 0x20 || r == 0x7f {
				b.WriteString(fmt.Sprintf("\\%02X", r))
			} else {
				b.WriteRune(r)
			}
		}
	}
	return b.String()
}

// parseParamTypeName maps a TypeIR type name string to a typeir.Typ.
// Used by Call emission to infer the LLVM type of arguments whose
// type was lost (e.g. external function calls where the stack slot
// was pushed with typeir.Unknown).
func parseParamTypeName(name string) (typeir.Typ, error) {
	switch strings.ToLower(name) {
	case "void":
		return typeir.Void, nil
	case "int", "i32":
		return typeir.Int, nil
	case "float", "f64", "double":
		return typeir.Float, nil
	case "bool":
		return typeir.Bool, nil
	case "string", "str":
		return typeir.String, nil
	default:
		return typeir.Int, fmt.Errorf("unknown param type name: %q", name)
	}
}
