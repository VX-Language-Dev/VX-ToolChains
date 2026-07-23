package main

import (
	"bytes"
	"flag"
	"fmt"
	"os"
	"path/filepath"
	"strconv"
	"strings"

	"github.com/vx-lang/vxc/internal/bytecode"
	"github.com/vx-lang/vxc/internal/typeir"
)

func main() {
	var (
		outputFile = flag.String("o", "", "Output VX source file path")
		infoOnly   = flag.Bool("info", false, "Show VXOBJ container info only")
		dumpIR     = flag.Bool("ir", false, "Dump TypeIR structure (for debugging)")
	)
	flag.Usage = func() {
		fmt.Fprintf(os.Stderr, "VX Decompiler v1.0 (Go) - VXOBJ v4 TypeIR -> VX Source\n")
		fmt.Fprintf(os.Stderr, "Usage: %s <input.vxobj> [options]\n", os.Args[0])
		fmt.Fprintf(os.Stderr, "Options:\n")
		fmt.Fprintf(os.Stderr, "  -o <path>       Output VX source path (default: input.decompiled.vx)\n")
		fmt.Fprintf(os.Stderr, "  --info          Show VXOBJ container info only\n")
		fmt.Fprintf(os.Stderr, "  --ir            Dump TypeIR structure (for debugging)\n")
		fmt.Fprintf(os.Stderr, "  -h, --help      Show this help message\n")
	}
	flag.Parse()

	if flag.NArg() == 0 {
		flag.Usage()
		os.Exit(1)
	}

	input := flag.Arg(0)

	data, err := os.ReadFile(input)
	if err != nil {
		fmt.Fprintf(os.Stderr, "[Error] Failed to read file '%s': %v\n", input, err)
		os.Exit(1)
	}

	container, err := bytecode.ParseVxobjV4(data)
	if err != nil {
		fmt.Fprintf(os.Stderr, "[Error] Failed to parse VXOBJ: %v\n", err)
		os.Exit(1)
	}

	if *infoOnly {
		printContainerInfo(container)
		return
	}

	typeIRData := container.GetSection(bytecode.SECTION_TYPE_IR)
	if typeIRData == nil {
		fmt.Fprintln(os.Stderr, "[Error] No TypeIR section found in VXOBJ")
		os.Exit(1)
	}

	module, err := typeir.DeserializeTypeModule(typeIRData)
	if err != nil {
		fmt.Fprintf(os.Stderr, "[Error] Failed to deserialize TypeIR: %v\n", err)
		os.Exit(1)
	}

	if *dumpIR {
		dumpTypeIR(module)
		return
	}

	outPath := *outputFile
	if outPath == "" {
		ext := filepath.Ext(input)
		if ext != "" {
			outPath = strings.TrimSuffix(input, ext) + ".decompiled.vx"
		} else {
			outPath = input + ".decompiled.vx"
		}
	}

	if err := decompileAndWrite(container, module, outPath); err != nil {
		fmt.Fprintf(os.Stderr, "[Error] Decompilation failed: %v\n", err)
		os.Exit(1)
	}

	fmt.Printf("Decompiled to %s\n", outPath)
}

func printContainerInfo(container *bytecode.VxObjV4Container) {
	fmt.Fprintf(os.Stderr, "VXOBJ v%d Container\n", container.Header.Version)
	fmt.Fprintf(os.Stderr, "  Target: %s\n", container.Header.TargetTriple)
	fmt.Fprintf(os.Stderr, "  Sections: %d\n", len(container.Header.Sections))
	for _, sec := range container.Header.Sections {
		data := container.GetSection(sec.Name)
		if data == nil {
			data = []byte{}
		}
		fmt.Fprintf(os.Stderr, "    - %s: %d bytes\n", sec.Name, len(data))
	}
	if container.HasExternalDeps() {
		fmt.Fprintf(os.Stderr, "  External dependencies:\n")
		depsData := container.GetSection(bytecode.SECTION_EXTERNAL_DEPS)
		if depsData != nil {
			deps := bytecode.DeserializeExternalDeps(depsData)
			for _, dep := range deps {
				if dep.Path != nil {
					fmt.Fprintf(os.Stderr, "    - %s (%s)\n", dep.Name, *dep.Path)
				} else {
					fmt.Fprintf(os.Stderr, "    - %s\n", dep.Name)
				}
			}
		}
	}
}

func dumpTypeIR(module *typeir.TypeModule) {
	fmt.Fprintln(os.Stderr, "=== TypeIR Module ===")
	fmt.Fprintf(os.Stderr, "Functions: %d\n", len(module.Functions))
	fmt.Fprintf(os.Stderr, "Struct layouts: %d\n", len(module.StructTypes))
	fmt.Fprintf(os.Stderr, "Linkage entries: %d\n", len(module.Linkage))

	for _, fn := range module.Functions {
		linkage, hasLinkage := module.Linkage[fn.ID]
		isExt := hasLinkage && linkage.IsExternal
		suffix := ""
		if isExt {
			suffix = " [external]"
		}
		fmt.Fprintf(os.Stderr, "\n--- func %s (id=%d, params=%d, vars=%d)%s ---\n",
			fn.Name, fn.ID, fn.ParamCount, fn.VarCount, suffix)

		for pi, param := range fn.Params {
			fmt.Fprintf(os.Stderr, "  param[%d]: %s : %s\n", pi, param.Name, typeir.TypeName(param.Type))
		}
		if fn.HasReturn {
			fmt.Fprintf(os.Stderr, "  return: %s\n", typeir.TypeName(fn.ReturnType))
		}
		for vid, ty := range fn.LocalTypes {
			fmt.Fprintf(os.Stderr, "  var[%d]: %s\n", vid, typeir.TypeName(ty))
		}
		for ii, inst := range fn.Body {
			fmt.Fprintf(os.Stderr, "  [%04d] ", ii)
			printInstruction(inst)
			fmt.Fprintln(os.Stderr)
		}
	}

	for si, sl := range module.StructTypes {
		fmt.Fprintf(os.Stderr, "\n--- struct %s (layout %d) ---\n", sl.Name, si)
		for fi, field := range sl.Fields {
			fmt.Fprintf(os.Stderr, "  field[%d]: %s : %s\n", fi, field.Name, typeir.TypeName(field.FieldType))
		}
	}
}

func printInstruction(inst typeir.TypedInstruction) {
	args := inst.Args
	switch inst.Op {
	case typeir.ConstInt:
		fmt.Fprintf(os.Stderr, "ConstInt(%s)", args[0])
	case typeir.ConstFloat:
		fmt.Fprintf(os.Stderr, "ConstFloat(%s)", args[0])
	case typeir.ConstBool:
		fmt.Fprintf(os.Stderr, "ConstBool(%s)", args[0])
	case typeir.ConstString:
		fmt.Fprintf(os.Stderr, "ConstString(\"%s\")", args[0])
	case typeir.ConstNil:
		fmt.Fprintf(os.Stderr, "ConstNil")
	case typeir.LoadVar:
		fmt.Fprintf(os.Stderr, "LoadVar(%s)", args[0])
	case typeir.StoreVar:
		fmt.Fprintf(os.Stderr, "StoreVar(%s)", args[0])
	default:
		fmt.Fprintf(os.Stderr, "%s(%s)", inst.Op, strings.Join(args, ", "))
	}
}

func decompileAndWrite(container *bytecode.VxObjV4Container, module *typeir.TypeModule, outPath string) error {
	var buf bytes.Buffer

	fmt.Fprintf(&buf, "// Decompiled from VXOBJ v4 TypeIR\n")
	fmt.Fprintf(&buf, "// Target: %s\n\n", container.Header.TargetTriple)

	d := &decompiler{
		module: module,
		output: &buf,
	}

	d.decompile()

	return os.WriteFile(outPath, buf.Bytes(), 0644)
}

type decompiler struct {
	module *typeir.TypeModule
	output *bytes.Buffer
}

func (d *decompiler) decompile() {
	hasExterns := false
	for _, fn := range d.module.Functions {
		linkage, has := d.module.Linkage[fn.ID]
		if has && linkage.IsExternal {
			d.writeFuncSignature(&fn, true)
			fmt.Fprintf(d.output, " // extern: %s\n", linkage.Name)
			hasExterns = true
		}
	}
	if hasExterns {
		fmt.Fprintln(d.output)
	}

	for _, sl := range d.module.StructTypes {
		fmt.Fprintf(d.output, "struct %s {\n", sl.Name)
		for _, field := range sl.Fields {
			fmt.Fprintf(d.output, "    %s: %s,\n", field.Name, typeir.TypeName(field.FieldType))
		}
		fmt.Fprintf(d.output, "}\n\n")
	}

	for _, fn := range d.module.Functions {
		linkage, has := d.module.Linkage[fn.ID]
		isExt := has && linkage.IsExternal
		if isExt {
			continue
		}
		d.decompileFunc(&fn)
		fmt.Fprintln(d.output)
	}
}

func (d *decompiler) writeFuncSignature(fn *typeir.FunctionIR, isExtern bool) {
	if isExtern {
		fmt.Fprint(d.output, "extern ")
	}
	fmt.Fprintf(d.output, "func %s(", fn.Name)
	for pi, param := range fn.Params {
		if pi > 0 {
			fmt.Fprint(d.output, ", ")
		}
		fmt.Fprintf(d.output, "%s: %s", param.Name, typeir.TypeName(param.Type))
	}
	fmt.Fprint(d.output, ")")
	if fn.HasReturn {
		fmt.Fprintf(d.output, ": %s", typeir.TypeName(fn.ReturnType))
	}
}

func (d *decompiler) decompileFunc(fn *typeir.FunctionIR) {
	d.writeFuncSignature(fn, false)
	fmt.Fprintln(d.output, " {")

	printedVarHeader := false
	for vid := uint32(0); vid < fn.VarCount; vid++ {
		if vid < fn.ParamCount {
			continue
		}
		if ty, ok := fn.LocalTypes[vid]; ok {
			if !printedVarHeader {
				fmt.Fprintln(d.output, "    // local variables")
				printedVarHeader = true
			}
			fmt.Fprintf(d.output, "    var v%d: %s;\n", vid, typeir.TypeName(ty))
		}
	}
	if printedVarHeader {
		fmt.Fprintln(d.output)
	}

	for idx, inst := range fn.Body {
		fmt.Fprint(d.output, "    ")
		d.decompileInstruction(inst, idx)
		fmt.Fprintln(d.output)
	}

	fmt.Fprintln(d.output, "}")
}

func (d *decompiler) decompileInstruction(inst typeir.TypedInstruction, idx int) {
	args := inst.Args

	switch inst.Op {
	case typeir.ConstInt:
		fmt.Fprintf(d.output, "v%d = %s", idx, args[0])
	case typeir.ConstFloat:
		fmt.Fprintf(d.output, "v%d = %s", idx, args[0])
	case typeir.ConstBool:
		v := "false"
		if args[0] == "1" || args[0] == "true" {
			v = "true"
		}
		fmt.Fprintf(d.output, "v%d = %s", idx, v)
	case typeir.ConstString:
		fmt.Fprintf(d.output, "v%d = \"%s\"", idx, args[0])
	case typeir.ConstNil:
		fmt.Fprintf(d.output, "v%d = nil", idx)
	case typeir.LoadVar:
		if len(args) > 0 {
			fmt.Fprintf(d.output, "v%d = v%s", idx, args[0])
		}
	case typeir.StoreVar:
		if len(args) > 0 {
			fmt.Fprintf(d.output, "v%s = v%d", args[0], idx)
		}
	case typeir.I32Add:
		d.writeBinOp(idx, "+", args)
	case typeir.I32Sub:
		d.writeBinOp(idx, "-", args)
	case typeir.I32Mul:
		d.writeBinOp(idx, "*", args)
	case typeir.I32Div:
		d.writeBinOp(idx, "/", args)
	case typeir.I32Mod:
		d.writeBinOp(idx, "%", args)
	case typeir.F64Add:
		d.writeBinOp(idx, "+", args)
	case typeir.F64Sub:
		d.writeBinOp(idx, "-", args)
	case typeir.F64Mul:
		d.writeBinOp(idx, "*", args)
	case typeir.F64Div:
		d.writeBinOp(idx, "/", args)
	case typeir.I32Eq:
		d.writeBinOp(idx, "==", args)
	case typeir.I32Neq:
		d.writeBinOp(idx, "!=", args)
	case typeir.I32Lt:
		d.writeBinOp(idx, "<", args)
	case typeir.I32Gt:
		d.writeBinOp(idx, ">", args)
	case typeir.I32Le:
		d.writeBinOp(idx, "<=", args)
	case typeir.I32Ge:
		d.writeBinOp(idx, ">=", args)
	case typeir.F64Eq:
		d.writeBinOp(idx, "==", args)
	case typeir.F64Lt:
		d.writeBinOp(idx, "<", args)
	case typeir.F64Gt:
		d.writeBinOp(idx, ">", args)
	case typeir.F64Le:
		d.writeBinOp(idx, "<=", args)
	case typeir.F64Ge:
		d.writeBinOp(idx, ">=", args)
	case typeir.I32Neg:
		if len(args) > 0 {
			fmt.Fprintf(d.output, "v%d = -v%s", idx, args[0])
		}
	case typeir.F64Neg:
		if len(args) > 0 {
			fmt.Fprintf(d.output, "v%d = -v%s", idx, args[0])
		}
	case typeir.BoolNot:
		if len(args) > 0 {
			fmt.Fprintf(d.output, "v%d = not v%s", idx, args[0])
		}
	case typeir.I32And:
		d.writeBinOp(idx, "and", args)
	case typeir.I32Or:
		d.writeBinOp(idx, "or", args)
	case typeir.Jump:
		if len(args) > 0 {
			fmt.Fprintf(d.output, "goto [%s]", args[0])
		}
	case typeir.JumpIfFalse:
		if len(args) >= 2 {
			fmt.Fprintf(d.output, "if not v%s: goto [%s]", args[0], args[1])
		}
	case typeir.JumpIfTrue:
		if len(args) >= 2 {
			fmt.Fprintf(d.output, "if v%s: goto [%s]", args[0], args[1])
		}
	case typeir.Call:
		d.decompileCall(args, idx)
	case typeir.Return:
		if len(args) > 0 && args[0] != "" {
			fmt.Fprintf(d.output, "return v%s", args[0])
		} else {
			fmt.Fprintf(d.output, "return")
		}
	case typeir.StructNew:
		d.decompileMakeStruct(args, idx)
	case typeir.FieldLoad:
		if len(args) >= 2 {
			fmt.Fprintf(d.output, "v%d = v%s.field%s", idx, args[0], args[1])
		}
	case typeir.FieldStore:
		if len(args) >= 3 {
			fmt.Fprintf(d.output, "v%s.field%s = v%s", args[0], args[1], args[2])
		}
	case typeir.ArrayNew:
		d.decompileMakeArray(args, idx)
	case typeir.IndexGet:
		if len(args) >= 2 {
			fmt.Fprintf(d.output, "v%d = v%s[v%s]", idx, args[0], args[1])
		}
	case typeir.IndexSet:
		if len(args) >= 3 {
			fmt.Fprintf(d.output, "v%s[v%s] = v%s", args[0], args[1], args[2])
		}
	case typeir.MapNew:
		d.decompileMakeMap(args, idx)
	case typeir.Newz:
		fmt.Fprintf(d.output, "v%d = alloc()", idx)
	case typeir.Free:
		if len(args) > 0 {
			fmt.Fprintf(d.output, "free v%s", args[0])
		}
	case typeir.OwnershipMove:
		if len(args) > 0 {
			fmt.Fprintf(d.output, "move v%s", args[0])
		}
	case typeir.BorrowCheck:
		if len(args) > 0 {
			fmt.Fprintf(d.output, "borrow v%s", args[0])
		}
	case typeir.Deref:
		if len(args) > 0 {
			fmt.Fprintf(d.output, "v%d = *v%s", idx, args[0])
		}
	case typeir.AliveCheck:
		if len(args) > 0 {
			fmt.Fprintf(d.output, "alive_check v%s", args[0])
		}
	case typeir.Dup:
		fmt.Fprintf(d.output, "dup")
	case typeir.Pop:
		fmt.Fprintf(d.output, "pop")
	default:
		fmt.Fprintf(d.output, "// unknown op %s", inst.Op)
	}
}

func (d *decompiler) writeBinOp(idx int, op string, args []string) {
	if len(args) >= 2 {
		fmt.Fprintf(d.output, "v%d = v%s %s v%s", idx, args[0], op, args[1])
	}
}

func (d *decompiler) decompileCall(args []string, idx int) {
	if len(args) == 0 {
		return
	}
	funcID := args[0]
	extName := ""
	var callArgs []string

	for _, arg := range args[1:] {
		if strings.HasPrefix(arg, ";") {
			extName = arg[1:]
		} else {
			callArgs = append(callArgs, arg)
		}
	}

	callee := extName
	if callee == "" {
		id, err := strconv.ParseUint(funcID, 10, 32)
		if err == nil {
			callee = fmt.Sprintf("f%d", id)
		} else {
			callee = funcID
		}
	}

	fmt.Fprintf(d.output, "v%d = %s(", idx, callee)
	for ai, arg := range callArgs {
		if ai > 0 {
			fmt.Fprint(d.output, ", ")
		}
		fmt.Fprintf(d.output, "v%s", arg)
	}
	fmt.Fprint(d.output, ")")
}

func (d *decompiler) decompileMakeStruct(args []string, idx int) {
	if len(args) == 0 {
		return
	}
	layoutID := args[0]
	fmt.Fprintf(d.output, "v%d = struct_s%s(", idx, layoutID)
	for ai, arg := range args[1:] {
		if ai > 0 {
			fmt.Fprint(d.output, ", ")
		}
		fmt.Fprintf(d.output, "v%s", arg)
	}
	fmt.Fprint(d.output, ")")
}

func (d *decompiler) decompileMakeArray(args []string, idx int) {
	fmt.Fprintf(d.output, "v%d = [", idx)
	for ai, arg := range args {
		if ai > 0 {
			fmt.Fprint(d.output, ", ")
		}
		fmt.Fprintf(d.output, "v%s", arg)
	}
	fmt.Fprintf(d.output, "]")
}

func (d *decompiler) decompileMakeMap(args []string, idx int) {
	fmt.Fprintf(d.output, "v%d = {", idx)
	for i := 0; i+1 < len(args); i += 2 {
		if i > 0 {
			fmt.Fprint(d.output, ", ")
		}
		fmt.Fprintf(d.output, "v%s: v%s", args[i], args[i+1])
	}
	fmt.Fprintf(d.output, "}")
}
