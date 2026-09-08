// vxbootstrap — VXOBJ bootstrap generator.
//
// vxbootstrap generates VXOBJ v4 files from a JSON description, bypassing
// the VX language frontend (lexer, parser, semantic analysis, typeirgen).
// This is the bootstrap path: the tool is written in Go but produces valid
// VXOBJ files that can be translated by vlnk into native executables.
//
// Usage:
//
//	vxbootstrap -o output.vxobj input.json
//	vxbootstrap -o output.vxobj - <<'EOF'
//	{ "functions": [{ "name": "main", ... }] }
//	EOF
package main

import (
	"bytes"
	"encoding/json"
	"fmt"
	"io"
	"os"
	"strconv"
	"strings"

	"github.com/vx-lang/vxc/internal/bytecode"
	"github.com/vx-lang/vxc/internal/typeir"
)

// ==================== JSON Input Format ====================

// ProgramJSON is the top-level JSON structure describing a VX program.
type ProgramJSON struct {
	Name    string                 `json:"name"`
	Target  string                 `json:"target"`
	Structs []StructJSON           `json:"structs,omitempty"`
	Funcs   []FunctionJSON         `json:"functions"`
	Linkage map[string]LinkageJSON `json:"linkage,omitempty"`
}

// StructJSON describes a struct type definition.
type StructJSON struct {
	Name   string      `json:"name"`
	Fields []FieldJSON `json:"fields,omitempty"`
}

// FieldJSON describes a struct field.
type FieldJSON struct {
	Name string `json:"name"`
	Type string `json:"type"`
}

// FunctionJSON describes a function.
type FunctionJSON struct {
	ID         int               `json:"id"`
	Name       string            `json:"name"`
	ReturnType string            `json:"return_type"`
	Params     []ParamJSON       `json:"params,omitempty"`
	Body       []InstructionJSON `json:"body"`
}

// ParamJSON describes a function parameter.
type ParamJSON struct {
	Name string `json:"name"`
	Type string `json:"type"`
}

// InstructionJSON describes a single TypeIR instruction.
type InstructionJSON struct {
	Op   string   `json:"op"`
	Args []string `json:"args,omitempty"`
}

// LinkageJSON describes a linkage table entry.
type LinkageJSON struct {
	External bool   `json:"external"`
	ExtName  string `json:"ext_name,omitempty"`
}

// ==================== TypeIR Construction ====================

// buildModule converts a ProgramJSON into a typeir.TypeModule.
func buildModule(prog *ProgramJSON) (*typeir.TypeModule, error) {
	module := &typeir.TypeModule{
		Name:        prog.Name,
		Functions:   make([]typeir.FunctionIR, 0, len(prog.Funcs)),
		StructTypes: make([]typeir.StructTypeDef, 0, len(prog.Structs)),
		Linkage:     make(map[uint32]typeir.LinkageEntry),
	}

	// Build struct types
	for _, s := range prog.Structs {
		st, err := buildStruct(s)
		if err != nil {
			return nil, fmt.Errorf("struct %q: %w", s.Name, err)
		}
		module.StructTypes = append(module.StructTypes, *st)
	}

	// Build functions
	for _, f := range prog.Funcs {
		fn, err := buildFunction(&f, module.StructTypes)
		if err != nil {
			return nil, fmt.Errorf("function %q: %w", f.Name, err)
		}
		module.Functions = append(module.Functions, *fn)
	}

	// Build linkage table
	for keyStr, entry := range prog.Linkage {
		id, err := strconv.ParseUint(keyStr, 10, 32)
		if err != nil {
			return nil, fmt.Errorf("invalid linkage key %q: %w", keyStr, err)
		}
		module.Linkage[uint32(id)] = typeir.LinkageEntry{
			IsExternal: entry.External,
			Name:       entry.ExtName,
		}
	}

	return module, nil
}

func buildStruct(s StructJSON) (*typeir.StructTypeDef, error) {
	fields := make([]typeir.StructField, len(s.Fields))
	for i, f := range s.Fields {
		typ, err := parseType(f.Type)
		if err != nil {
			return nil, fmt.Errorf("field %q: %w", f.Name, err)
		}
		fields[i] = typeir.StructField{
			Name:      f.Name,
			FieldType: typ,
		}
	}
	return &typeir.StructTypeDef{
		Name:   s.Name,
		Fields: fields,
	}, nil
}

func buildFunction(f *FunctionJSON, structTypes []typeir.StructTypeDef) (*typeir.FunctionIR, error) {
	retType, err := parseType(f.ReturnType)
	if err != nil {
		return nil, fmt.Errorf("return type: %w", err)
	}

	hasReturn := f.ReturnType != "" && f.ReturnType != "void"

	params := make([]typeir.ParamIR, len(f.Params))
	for i, p := range f.Params {
		pt, err := parseType(p.Type)
		if err != nil {
			return nil, fmt.Errorf("param %q: %w", p.Name, err)
		}
		params[i] = typeir.ParamIR{Name: p.Name, Type: pt}
	}

	body := make([]typeir.TypedInstruction, len(f.Body))
	for i, inst := range f.Body {
		ti, err := buildInstruction(&inst)
		if err != nil {
			return nil, fmt.Errorf("instruction %d (%s): %w", i, inst.Op, err)
		}
		body[i] = ti
	}

	return &typeir.FunctionIR{
		ID:         uint32(f.ID),
		Name:       f.Name,
		ParamCount: uint32(len(params)),
		HasReturn:  hasReturn,
		ReturnType: retType,
		Params:     params,
		Body:       body,
	}, nil
}

func buildInstruction(inst *InstructionJSON) (typeir.TypedInstruction, error) {
	op, err := parseOpcode(inst.Op)
	if err != nil {
		return typeir.TypedInstruction{}, err
	}

	// Handle special instructions that don't use args as-is
	switch op {
	case typeir.Call:
		// Call convention (matches vxc/typeirgen): args[0] is the callee
		// marker ";name" (when the callee is external) or a numeric funcID
		// (when the callee is defined in this module). The remaining args
		// are the parameter type names (e.g. "string", "int").
		return typeir.TypedInstruction{
			Op:   op,
			Args: inst.Args,
		}, nil
	case typeir.ConstInt:
		if len(inst.Args) > 0 {
			return typeir.TypedInstruction{
				Op:   op,
				Args: []string{inst.Args[0]},
			}, nil
		}
	case typeir.ConstFloat:
		if len(inst.Args) > 0 {
			return typeir.TypedInstruction{
				Op:   op,
				Args: []string{inst.Args[0]},
			}, nil
		}
	case typeir.ConstBool:
		if len(inst.Args) > 0 {
			return typeir.TypedInstruction{
				Op:   op,
				Args: []string{inst.Args[0]},
			}, nil
		}
	case typeir.ConstString:
		if len(inst.Args) > 0 {
			return typeir.TypedInstruction{
				Op:   op,
				Args: []string{inst.Args[0]},
			}, nil
		}
	}

	return typeir.TypedInstruction{
		Op:   op,
		Args: inst.Args,
	}, nil
}

// ==================== Opcode Parsing ====================

func parseOpcode(name string) (typeir.Opcode, error) {
	switch strings.ToLower(name) {
	case "const_int", "ci":
		return typeir.ConstInt, nil
	case "const_float", "cf":
		return typeir.ConstFloat, nil
	case "const_bool", "cb":
		return typeir.ConstBool, nil
	case "const_string", "cs":
		return typeir.ConstString, nil
	case "const_nil", "cn":
		return typeir.ConstNil, nil
	case "load_var", "lv":
		return typeir.LoadVar, nil
	case "store_var", "sv":
		return typeir.StoreVar, nil
	case "i32_add", "i32a":
		return typeir.I32Add, nil
	case "i32_sub", "i32s":
		return typeir.I32Sub, nil
	case "i32_mul", "i32m":
		return typeir.I32Mul, nil
	case "i32_div", "i32d":
		return typeir.I32Div, nil
	case "i32_mod", "i32mo":
		return typeir.I32Mod, nil
	case "i64_add", "i64a":
		return typeir.I64Add, nil
	case "i64_sub", "i64s":
		return typeir.I64Sub, nil
	case "i64_mul", "i64m":
		return typeir.I64Mul, nil
	case "i64_div", "i64d":
		return typeir.I64Div, nil
	case "f64_add", "f64a":
		return typeir.F64Add, nil
	case "f64_sub", "f64s":
		return typeir.F64Sub, nil
	case "f64_mul", "f64m":
		return typeir.F64Mul, nil
	case "f64_div", "f64d":
		return typeir.F64Div, nil
	case "bool_and", "ba":
		return typeir.BoolAnd, nil
	case "bool_or", "bo":
		return typeir.BoolOr, nil
	case "bool_not", "bn":
		return typeir.BoolNot, nil
	case "i32_eq", "i32e":
		return typeir.I32Eq, nil
	case "i32_neq", "i32n":
		return typeir.I32Neq, nil
	case "i32_lt", "i32l":
		return typeir.I32Lt, nil
	case "i32_gt", "i32g":
		return typeir.I32Gt, nil
	case "i32_le", "i32le":
		return typeir.I32Le, nil
	case "i32_ge", "i32ge":
		return typeir.I32Ge, nil
	case "f64_eq", "f64e":
		return typeir.F64Eq, nil
	case "f64_lt", "f64l":
		return typeir.F64Lt, nil
	case "f64_gt", "f64g":
		return typeir.F64Gt, nil
	case "f64_le", "f64le":
		return typeir.F64Le, nil
	case "f64_ge", "f64ge":
		return typeir.F64Ge, nil
	case "f64_ne", "f64ne":
		return typeir.F64Ne, nil
	case "str_eq", "ste":
		return typeir.StrEq, nil
	case "neq_int", "nei":
		return typeir.NeqInt, nil
	case "jump", "jp":
		return typeir.Jump, nil
	case "jump_if_false", "jf":
		return typeir.JumpIfFalse, nil
	case "jump_if_true", "jt":
		return typeir.JumpIfTrue, nil
	case "dup", "dp":
		return typeir.Dup, nil
	case "pop", "pp":
		return typeir.Pop, nil
	case "call", "cl":
		return typeir.Call, nil
	case "return", "rt":
		return typeir.Return, nil
	case "return_void", "rtv":
		return typeir.ReturnVoid, nil
	case "struct_new", "sn":
		return typeir.StructNew, nil
	case "field_load", "fl":
		return typeir.FieldLoad, nil
	case "field_store", "fs":
		return typeir.FieldStore, nil
	case "array_new", "an":
		return typeir.ArrayNew, nil
	case "map_new", "mn":
		return typeir.MapNew, nil
	case "index_get", "ig":
		return typeir.IndexGet, nil
	case "index_set", "is":
		return typeir.IndexSet, nil
	case "address_of", "ao":
		return typeir.AddressOf, nil
	case "deref", "dr":
		return typeir.Deref, nil
	case "pointer_member", "pm":
		return typeir.PointerMember, nil
	case "sys_argv", "sa":
		return typeir.SysArgv, nil
	case "system", "sy":
		return typeir.System, nil
	case "file_read", "frd":
		return typeir.FileRead, nil
	case "file_write", "fw":
		return typeir.FileWrite, nil
	case "file_exists", "fe":
		return typeir.FileExists, nil
	case "halt", "ht":
		return typeir.Halt, nil
	case "newz", "nw":
		return typeir.Newz, nil
	case "free", "fr":
		return typeir.Free, nil
	case "ownership_move", "om":
		return typeir.OwnershipMove, nil
	case "scope_drop", "sd":
		return typeir.ScopeDrop, nil
	case "borrow_check", "bc":
		return typeir.BorrowCheck, nil
	case "alive_check", "ac":
		return typeir.AliveCheck, nil
	case "noop", "np":
		return typeir.Noop, nil
	case "import", "im":
		return typeir.Import, nil
	default:
		return 0, fmt.Errorf("unknown opcode: %q", name)
	}
}

// ==================== Type Parsing ====================

func parseType(name string) (typeir.Typ, error) {
	switch strings.ToLower(name) {
	case "void":
		return typeir.Void, nil
	case "int", "i32", "i64":
		return typeir.Int, nil
	case "float", "f32", "f64":
		return typeir.Float, nil
	case "bool":
		return typeir.Bool, nil
	case "string", "str":
		return typeir.String, nil
	case "":
		return typeir.Void, nil
	default:
		// Treat unknown type names as struct types rather than silently
		// defaulting to Int, which would produce incorrect codegen.
		return typeir.StructTyp{Name: name}, nil
	}
}

// ==================== CLI Entry Point ====================

func main() {
	outputPath := ""
	args := os.Args[1:]

	for i := 0; i < len(args); i++ {
		switch args[i] {
		case "-o":
			if i+1 < len(args) {
				outputPath = args[i+1]
				i++
			}
		case "-h", "--help":
			printUsage()
			return
		default:
			// Assume it's the input file
			if inputPath == "" {
				inputPath = args[i]
			}
		}
	}

	if inputPath == "" || outputPath == "" {
		printUsage()
		os.Exit(1)
	}

	// Read input
	var jsonData []byte
	var err error
	if inputPath == "-" {
		jsonData, err = readStdin()
	} else {
		jsonData, err = os.ReadFile(inputPath)
	}
	if err != nil {
		fmt.Fprintf(os.Stderr, "error: cannot read input: %v\n", err)
		os.Exit(1)
	}

	// Parse JSON
	var prog ProgramJSON
	if err := json.Unmarshal(jsonData, &prog); err != nil {
		fmt.Fprintf(os.Stderr, "error: invalid JSON: %v\n", err)
		os.Exit(1)
	}

	// Default target
	if prog.Target == "" {
		prog.Target = "x86_64-unknown-unknown"
	}
	if prog.Name == "" {
		prog.Name = "bootstrap"
	}

	// Build TypeIR module
	module, err := buildModule(&prog)
	if err != nil {
		fmt.Fprintf(os.Stderr, "error: %v\n", err)
		os.Exit(1)
	}

	// Serialize TypeIR to binary format
	typeIRData, err := typeir.SerializeTypeModule(module)
	if err != nil {
		fmt.Fprintf(os.Stderr, "error: TypeIR serialization failed: %v\n", err)
		os.Exit(1)
	}

	// Pack into VXOBJ container
	var buf bytes.Buffer
	if err := bytecode.WriteVxobjV4(&buf, prog.Target, typeIRData, nil, nil, nil); err != nil {
		fmt.Fprintf(os.Stderr, "error: VXOBJ write failed: %v\n", err)
		os.Exit(1)
	}
	vxobjData := buf.Bytes()

	// Write output
	if err := os.WriteFile(outputPath, vxobjData, 0644); err != nil {
		fmt.Fprintf(os.Stderr, "error: cannot write output: %v\n", err)
		os.Exit(1)
	}

	fmt.Fprintf(os.Stderr, "vxbootstrap: wrote %s (%d bytes, %d functions, target=%s)\n",
		outputPath, len(vxobjData), len(module.Functions), prog.Target)
}

var inputPath string

func printUsage() {
	fmt.Fprintf(os.Stderr, `Usage: vxbootstrap -o output.vxobj input.json

Generate a VXOBJ v4 file from a JSON description of a TypeIR program.

Options:
  -o <file>    Output VXOBJ file path (required)
  input.json   Input JSON file, or "-" for stdin
  -h           Show this help

Example input.json:
  {
    "name": "hello",
    "target": "x86_64-unknown-unknown",
    "functions": [
      {
        "id": 0,
        "name": "main",
        "return_type": "int",
        "body": [
          {"op": "const_int", "args": ["42"]},
          {"op": "return", "args": ["int"]}
        ]
      }
    ]
  }
`)
}

func readStdin() ([]byte, error) {
	stat, err := os.Stdin.Stat()
	if err != nil {
		return nil, err
	}
	if (stat.Mode() & os.ModeCharDevice) != 0 {
		return nil, fmt.Errorf("stdin is a terminal; pipe JSON or use a file")
	}
	return io.ReadAll(os.Stdin)
}

// io.ReadAll is used above
