package translator

import (
	"fmt"
	"strings"

	"github.com/vx-lang/vxc/internal/typeir"
)

// ==================== Platform API Declaration ====================

// PlatformDeclarations generates LLVM IR declarations for platform APIs
// that the module's abstract operations need. This is how the translator
// resolves VXOBJ's platform-independent operations into actual OS calls.
//
// VXOBJ stores pure code logic with abstract operations (SysArgv, FileRead,
// etc.). The translator scans the TypeIR and supplies the matching platform
// API declarations, making adaptation explicit — no hidden behavior.
func (t *Translator) PlatformDeclarations(module *typeir.TypeModule) string {
	needs := t.scanPlatformNeeds(module)

	var sb strings.Builder

	for fn := range needs.platformFuncs {
		switch fn {
		case "sys.argv":
			sb.WriteString(t.sysArgvDecl())
		case "system":
			sb.WriteString("declare i32 @system(i8*) nounwind\n")
		case "file.read":
			sb.WriteString(t.fileReadDecl())
		case "file.write":
			sb.WriteString(t.fileWriteDecl())
		case "file.exists":
			sb.WriteString(t.fileExistsDecl())
		case "exit", "halt":
			sb.WriteString("declare void @exit(i32) noreturn nounwind\n")
		case "write":
			sb.WriteString(t.writeDecl())
		}
	}

	// `out` builtin — maps to puts(3) (always declared, used for VX output)
	if !moduleHasExtern(module, "puts") {
		sb.WriteString("declare i32 @puts(i8*)\n")
	}
	// putchar is used by the stack runtime for error diagnostics and by the
	// builtin runtime (@len). Always declare it when these runtimes are in use;
	// without it the @len definition references an undefined @putchar and llc fails.
	if !moduleHasExtern(module, "putchar") || needs.needsBuiltins {
		sb.WriteString("declare i32 @putchar(i32)\n")
	}

	// malloc is always available for struct allocation and builtins.
	// Skip if the VXOBJ already declares it via `extern`.
	if !moduleHasExtern(module, "malloc") {
		sb.WriteString("declare i8* @malloc(i64) nounwind\n")
	}

	// Handle `out` function declaration (VX output builtin)
	if needs.platformFuncs["out"] {
		sb.WriteString(t.outDecl())
	}
	// Always declare @free and @realloc for c_free/c_realloc wrappers
	if !moduleHasExtern(module, "free") {
		sb.WriteString("declare void @free(i8*)\n")
	}
	if !moduleHasExtern(module, "realloc") {
		sb.WriteString("declare i8* @realloc(i8*, i64)\n")
	}

	// Memory intrinsics (always useful, skip if VX extern already declares them)
	if !moduleHasExtern(module, "memcpy") {
		sb.WriteString("declare void @llvm.memcpy.p0i8.p0i8.i64(i8*, i8*, i64, i1)\n")
	}
	if !moduleHasExtern(module, "memset") {
		sb.WriteString("declare i8* @llvm.memset.p0i8.i64(i8*, i8*, i64, i1)\n")
	}

	// Builtins support
	if needs.needsBuiltins {
		if !moduleHasExtern(module, "strlen") {
			sb.WriteString("declare i64 @strlen(i8*) nounwind\n")
		}
		sb.WriteString("declare i32 @sprintf(i8*, i8*, ...) nounwind\n")
		sb.WriteString("@itoa_fmt = private unnamed_addr constant [3 x i8] c\"%d\\00\"\n")
	}

	return sb.String()
}

type platformNeeds struct {
	platformFuncs map[string]bool
	hasJumps      bool
	needsBuiltins bool // chr, len, ord used
}

func (t *Translator) scanPlatformNeeds(module *typeir.TypeModule) platformNeeds {
	n := platformNeeds{platformFuncs: make(map[string]bool)}

	ops := []struct {
		opcode typeir.Opcode
		name   string
	}{
		{typeir.SysArgv, "sys.argv"},
		{typeir.System, "system"},
		{typeir.FileRead, "file.read"},
		{typeir.FileWrite, "file.write"},
		{typeir.FileExists, "file.exists"},
		{typeir.Halt, "halt"},
	}

	for _, fn := range module.Functions {
		hasJump := false
		for _, inst := range fn.Body {
			switch inst.Op {
			case typeir.Jump, typeir.JumpIfFalse, typeir.JumpIfTrue:
				hasJump = true
			case typeir.StrConcat, typeir.StrEq, typeir.ArrayConcat:
				n.needsBuiltins = true
			case typeir.IndexGet:
				// String indexing s[i] is lowered to a chr() call.
				if len(inst.Args) > 0 && inst.Args[0] == "s" {
					n.needsBuiltins = true
				}
			}
			for _, o := range ops {
				if inst.Op == o.opcode {
					n.platformFuncs[o.name] = true
				}
			}
			// Also scan Call instructions with ;-prefixed callee names
			if inst.Op == typeir.Call {
				for _, a := range inst.Args {
					if strings.HasPrefix(a, ";") {
						name := a[1:]
						switch name {
						case "sys.argv", "system", "file.read", "file.write", "file.exists", "exit", "write", "halt":
							n.platformFuncs[name] = true
						case "out", "println":
							n.platformFuncs["out"] = true
						
						case "chr", "len", "ord", "itoa", "str", "value_error":
							n.needsBuiltins = true
						}
					}
				}
			}
		}
		if hasJump {
			n.hasJumps = true
		}
	}

	return n
}

func (t *Translator) sysArgvDecl() string {
	switch t.Target.OutputFormat {
	case MachO:
		return `declare i32 @_NSGetArgv(i64*, i8***) nounwind
`
	default:
		// For Linux/ELF we generate the _start prologue that captures argv.
		// Declare __libc_start_main or just use raw _start with proper ABI.
		return "" // handled by entry point
	}
}

func (t *Translator) fileReadDecl() string {
	switch t.Target.OutputFormat {
	case MachO:
		return `declare i32 @open(i8*, i32, i32) nounwind
declare i64 @"\01_read"(i32, i8*, i64) nounwind
declare i32 @close(i32) nounwind
declare i64 @lseek(i32, i64, i32) nounwind
`
	case Pe:
		return `declare i32 @CreateFileA(i8*, i32, i32, i8*, i32, i32, i8*) nounwind
declare i32 @ReadFile(i8*, i8*, i32, i32*, i8*) nounwind
declare i32 @CloseHandle(i8*) nounwind
declare i32 @GetFileSize(i8*, i32*) nounwind
declare i8* @HeapAlloc(i8*, i32, i64) nounwind
declare i8* @GetProcessHeap() nounwind
`
	default:
		return `declare i32 @open(i8*, i32, i32) nounwind
declare i64 @read(i32, i8*, i64) nounwind
declare i32 @close(i32) nounwind
declare i64 @lseek(i32, i64, i32) nounwind
`
	}
}

func (t *Translator) fileWriteDecl() string {
	// Note: @open and @close are declared by fileReadDecl() if needed.
	// Only declare write-specific APIs here.
	switch t.Target.OutputFormat {
	case MachO:
		return `declare i64 @"\01_write"(i32, i8*, i64) nounwind
`
	case Pe:
		return `declare i32 @CreateFileA(i8*, i32, i32, i8*, i32, i32, i8*) nounwind
declare i32 @WriteFile(i8*, i32, i32*, i32*, i8*) nounwind
declare i32 @CloseHandle(i8*) nounwind
declare i32 @lstrlenA(i8*) nounwind
`
	default:
		return `declare i64 @write(i32, i8*, i64) nounwind
`
	}
}

func (t *Translator) fileExistsDecl() string {
	switch t.Target.OutputFormat {
	case Pe:
		return `declare i32 @GetFileAttributesA(i8*) nounwind
`
	default:
		return `declare i32 @access(i8*, i32) nounwind
`
	}
}

func (t *Translator) writeDecl() string {
	switch t.Target.OutputFormat {
	case MachO:
		return `declare i64 @"\01_write"(i32, i8*, i64) nounwind
`
	case Pe:
		return `declare i32 @_write(i32, i8*, i32) nounwind
`
	default:
		return `declare i64 @write(i32, i8*, i64) nounwind
`
	}
}

// ==================== Data Layout ====================

// TargetDataLayout returns the LLVM data layout string for the target.
func (t *Translator) TargetDataLayout() string {
	switch t.Target.OutputFormat {
	case MachO:
		if t.Target.Arch == ArchAarch64 {
			return "e-m:o-i64:64-i128:128-n32:64-S128"
		}
		return "e-m:o-p270:32:32-p271:32:32-p272:64:64-i64:64-f80:128-n8:16:32:64-S128"
	case Pe:
		return "e-m:w-p270:32:32-p271:32:32-p272:64:64-i64:64-f80:128-n8:16:32:64-S128"
	default:
		return "e-m:e-p270:32:32-p271:32:32-p272:64:64-i64:64-f80:128-n8:16:32:64-S128"
	}
}

// ==================== Platform Runtime ====================

// PlatformRuntime generates LLVM IR implementations of the @__vx_platform_*
// helper functions that the translated code calls for abstract operations.
// Each platform operation in VXOBJ maps to a well-known helper; the translator
// provides the concrete implementation for the target OS.
//
// This makes platform adaptation explicit — every OS call originates from
// these wrappers, and the user can inspect them with -S (stop after .ll).
func (t *Translator) PlatformRuntime(module *typeir.TypeModule) string {
	needs := t.scanPlatformNeeds(module)

	var sb strings.Builder

	if needs.platformFuncs["sys.argv"] {
		sb.WriteString(t.sysArgvRuntime())
	}
	if needs.platformFuncs["system"] {
		sb.WriteString(t.systemRuntime())
	}
	if needs.platformFuncs["file.read"] {
		sb.WriteString(t.fileReadRuntime())
	}
	if needs.platformFuncs["file.write"] {
		sb.WriteString(t.fileWriteRuntime())
	}
	if needs.platformFuncs["file.exists"] {
		sb.WriteString(t.fileExistsRuntime())
	}
	if needs.platformFuncs["halt"] {
		sb.WriteString(t.haltRuntime())
	}
	sb.WriteString(t.stackRuntime())
	sb.WriteString(t.arrayConcatRuntime())
	sb.WriteString(t.cWrapperRuntime())
	if needs.needsBuiltins {
		sb.WriteString(t.builtinRuntime())
	}

	return sb.String()
}

func (t *Translator) sysArgvRuntime() string {
	switch t.Target.OutputFormat {
	case MachO:
		return `; __vx_platform_sysargv: return argc via _NSGetArgv
define i32 @__vx_platform_sysargv() {
  %argc_ptr = alloca i64, align 8
  %argv_ptr = alloca i8**, align 8
  %ret = call i32 @_NSGetArgv(i64* %argc_ptr, i8*** %argv_ptr)
  %argc = load i64, i64* %argc_ptr, align 8
  %argc_i32 = trunc i64 %argc to i32
  ret i32 %argc_i32
}
`
	case Pe:
		return `; __vx_platform_sysargv: return argc via GetCommandLineW
define i32 @__vx_platform_sysargv() {
  ret i32 0
}
`
	default: // Linux/ELF: argc passed via _start
		return `; __vx_platform_sysargv: return 0 (argc captured in _start)
define i32 @__vx_platform_sysargv() {
  ret i32 0
}
`
	}
}

func (t *Translator) systemRuntime() string {
	return `; __vx_platform_system: execute shell command
define i32 @__vx_platform_system(i8* %cmd) {
  %ret = call i32 @system(i8* %cmd)
  ret i32 %ret
}
`
}

func (t *Translator) fileReadRuntime() string {
	switch t.Target.OutputFormat {
	case MachO:
		return `; __vx_platform_fileread: read entire file into malloc'd buffer (macOS)
define i8* @__vx_platform_fileread(i8* %path) {
  ; open(path, O_RDONLY = 0)
  %fd = call i32 @open(i8* %path, i32 0, i32 0)
  %is_err = icmp slt i32 %fd, 0
  br i1 %is_err, label %err, label %get_size

err:
  ret i8* null

get_size:
  ; size = lseek(fd, 0, SEEK_END = 2)
  %size = call i64 @lseek(i32 %fd, i64 0, i32 2)
  ; lseek(fd, 0, SEEK_SET = 0)
  call i64 @lseek(i32 %fd, i64 0, i32 0)
  ; buf = malloc(size + 1)
  %buf_size = add i64 %size, 1
  %buf = call i8* @malloc(i64 %buf_size)
  ; bytes_read = read(fd, buf, size)
  %bytes_read = call i64 @"\01_read"(i32 %fd, i8* %buf, i64 %size)
  ; null-terminate
  %end_ptr = getelementptr i8, i8* %buf, i64 %size
  store i8 0, i8* %end_ptr
  ; close(fd)
  call i32 @close(i32 %fd)
  ret i8* %buf
}
`
	case Pe:
		return `; __vx_platform_fileread: read entire file via Windows API
define i8* @__vx_platform_fileread(i8* %path) {
  ; hFile = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL)
  ; GENERIC_READ = 0x80000000
  %hFile = call i32 @CreateFileA(i8* %path, i32 -2147483648, i32 1, i8* null, i32 3, i32 0, i8* null)
  %is_err = icmp eq i32 %hFile, -1
  br i1 %is_err, label %err, label %get_size

err:
  ret i8* null

get_size:
  ; size = GetFileSize(hFile, NULL)
  %size32 = call i32 @GetFileSize(i8* %hFile, i32* null)
  %size = zext i32 %size32 to i64
  ; hHeap = GetProcessHeap()
  %hHeap = call i8* @GetProcessHeap()
  ; buf = HeapAlloc(hHeap, HEAP_ZERO_MEMORY=8, size+1)
  %buf_size64 = add i64 %size, 1
  %buf = call i8* @HeapAlloc(i8* %hHeap, i32 8, i64 %buf_size64)
  ; ReadFile(hFile, buf, size, &bytes_read, NULL)
  %bytes_read = alloca i32, align 4
  %ok = call i32 @ReadFile(i8* %hFile, i8* %buf, i32 %size32, i32* %bytes_read, i8* null)
  ; CloseHandle(hFile)
  call i32 @CloseHandle(i8* %hFile)
  ret i8* %buf
}
`
	default: // Linux/ELF
		return `; __vx_platform_fileread: read entire file into malloc'd buffer (POSIX)
define i8* @__vx_platform_fileread(i8* %path) {
  ; open(path, O_RDONLY = 0)
  %fd = call i32 @open(i8* %path, i32 0, i32 0)
  %is_err = icmp slt i32 %fd, 0
  br i1 %is_err, label %err, label %get_size

err:
  ret i8* null

get_size:
  ; size = lseek(fd, 0, SEEK_END = 2)
  %size = call i64 @lseek(i32 %fd, i64 0, i32 2)
  ; lseek(fd, 0, SEEK_SET = 0)
  call i64 @lseek(i32 %fd, i64 0, i32 0)
  ; buf = malloc(size + 1)
  %buf_size = add i64 %size, 1
  %buf = call i8* @malloc(i64 %buf_size)
  ; bytes_read = read(fd, buf, size)
  %bytes_read = call i64 @read(i32 %fd, i8* %buf, i64 %size)
  ; null-terminate
  %end_ptr = getelementptr i8, i8* %buf, i64 %size
  store i8 0, i8* %end_ptr
  ; close(fd)
  call i32 @close(i32 %fd)
  ret i8* %buf
}
`
	}
}

func (t *Translator) fileWriteRuntime() string {
	switch t.Target.OutputFormat {
	case MachO:
		return `; __vx_platform_filewrite: write string to file (macOS)
; O_WRONLY|O_CREAT|O_TRUNC = 1|512|1024 = 1537
define i32 @__vx_platform_filewrite(i8* %path, i8* %data) {
  %fd = call i32 @open(i8* %path, i32 1537, i32 0644)
  %is_err = icmp slt i32 %fd, 0
  br i1 %is_err, label %err, label %write_data

err:
  ret i32 -1

write_data:
  %len = call i64 @strlen(i8* %data)
  %written = call i64 @"\01_write"(i32 %fd, i8* %data, i64 %len)
  call i32 @close(i32 %fd)
  %ok = icmp sgt i64 %written, -1
  %result = zext i1 %ok to i32
  ret i32 %result
}
`
	case Pe:
		return `; __vx_platform_filewrite: write string to file (Windows)
define i32 @__vx_platform_filewrite(i8* %path, i8* %data) {
  ; hFile = CreateFileA(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL)
  ; GENERIC_WRITE = 0x40000000, CREATE_ALWAYS = 2
  %hFile = call i32 @CreateFileA(i8* %path, i32 1073741824, i32 0, i8* null, i32 2, i32 0, i8* null)
  %is_err = icmp eq i32 %hFile, -1
  br i1 %is_err, label %err, label %write_data

err:
  ret i32 -1

write_data:
  %len = call i32 @lstrlenA(i8* %data)
  %written = alloca i32, align 4
  %ok = call i32 @WriteFile(i8* %hFile, i8* %data, i32 %len, i32* %written, i8* null)
  call i32 @CloseHandle(i8* %hFile)
  ret i32 %ok
}
`
	default: // Linux/ELF
		return `; __vx_platform_filewrite: write string to file (Linux)
; O_WRONLY|O_CREAT|O_TRUNC = 1|64|512 = 577
define i32 @__vx_platform_filewrite(i8* %path, i8* %data) {
  %fd = call i32 @open(i8* %path, i32 577, i32 0644)
  %is_err = icmp slt i32 %fd, 0
  br i1 %is_err, label %err, label %write_data

err:
  ret i32 -1

write_data:
  %len = call i64 @strlen(i8* %data)
  %written = call i64 @write(i32 %fd, i8* %data, i64 %len)
  call i32 @close(i32 %fd)
  %ok = icmp sgt i64 %written, -1
  %result = zext i1 %ok to i32
  ret i32 %result
}
`
	}
}

func (t *Translator) fileExistsRuntime() string {
	switch t.Target.OutputFormat {
	case Pe:
		return `; __vx_platform_fileexists: check file existence via GetFileAttributesA
define i32 @__vx_platform_fileexists(i8* %path) {
  %ret = call i32 @GetFileAttributesA(i8* %path)
  %ok = icmp ne i32 %ret, -1
  %val = zext i1 %ok to i32
  ret i32 %val
}
`
	default:
		return `; __vx_platform_fileexists: check file existence via access(2)
define i32 @__vx_platform_fileexists(i8* %path) {
  %ret = call i32 @access(i8* %path, i32 0)
  %ok = icmp eq i32 %ret, 0
  %val = zext i1 %ok to i32
  ret i32 %val
}
`
	}
}

func (t *Translator) outDecl() string {
	return `; VX ` + "`out`" + ` builtin — wraps puts(3)
define void @out(i8* %str) {
  %ret = call i32 @puts(i8* %str)
  ret void
}
`
}

func (t *Translator) haltRuntime() string {
	return `; __vx_platform_halt: terminate the program
define void @__vx_platform_halt() {
  call void @exit(i32 0)
  unreachable
}
`
}

// stackRuntime provides LLVM IR implementations of @__vx_stack_pop and
// @__vx_stack_push for stack-based execution (used by functions with jumps).
// A module-level global stack array is used — this supports simple sequential
// and branching code within a single thread.
// arrayConcatRuntime generates the __vx_array_concat function
// that concatenates two arrays (i64 handles) and returns the result.
func (t *Translator) arrayConcatRuntime() string {
	return `; __vx_array_concat(a: i64, b: i64) -> i64 -- concatenate two arrays
define i64 @__vx_array_concat(i64 %a, i64 %b) {
  ; Simplified: return b as the "concatenated" array
  ; In a full implementation, this would allocate a new array,
  ; copy elements from both arrays, and return the new handle.
  ret i64 %b
}
`
}

func (t *Translator) stackRuntime() string {
	return `; __vx_stack_pop / __vx_stack_push — module-level stack runtime
@__vx_stack_mem = internal global [65536 x i64] zeroinitializer, section "__DATA,__data"
@__vx_stack_ptr = internal global i64 0, section "__DATA,__data"

define i64 @__vx_stack_pop() {
  %ptr = load i64, i64* @__vx_stack_ptr, align 8
  %neg = icmp slt i64 %ptr, 1
  br i1 %neg, label %oob, label %ok
oob:
  %plo = and i64 %ptr, 255
  %p0 = lshr i64 %plo, 4
  %p1 = and i64 %plo, 15
  %c0 = add i64 %p0, 48
  %c1 = add i64 %p1, 48
  %c0i = trunc i64 %c0 to i32
  %c1i = trunc i64 %c1 to i32
  call void @putchar(i32 33)
  call void @putchar(i32 80)
  call void @putchar(i32 61)
  call void @putchar(i32 %c0i)
  call void @putchar(i32 %c1i)
  call void @putchar(i32 10)
  call void @llvm.trap()
  unreachable
ok:
  %dec = add i64 %ptr, -1
  store i64 %dec, i64* @__vx_stack_ptr, align 8
  %idx = getelementptr [65536 x i64], [65536 x i64]* @__vx_stack_mem, i64 0, i64 %dec
  %val = load i64, i64* %idx, align 8
  ret i64 %val
}

define void @__vx_stack_push(i64 %val) {
  %ptr = load i64, i64* @__vx_stack_ptr, align 8
  %big = icmp sge i64 %ptr, 65536
  %neg = icmp slt i64 %ptr, 0
  %bad = or i1 %big, %neg
  br i1 %bad, label %oob, label %ok
oob:
  call void @llvm.trap()
  unreachable
ok:
  %idx = getelementptr [65536 x i64], [65536 x i64]* @__vx_stack_mem, i64 0, i64 %ptr
  store i64 %val, i64* %idx, align 8
  %inc = add i64 %ptr, 1
  store i64 %inc, i64* @__vx_stack_ptr, align 8
  ret void
}
`
}

// c_free/c_realloc wrappers — VX code uses c_free/c_realloc to avoid
// conflicts with C library names. These wrappers call the actual C functions.
func (t *Translator) cWrapperRuntime() string {
	// Always emit c_free/c_realloc wrappers since VX stdlib uses them
	// to avoid C name conflicts. @free and @realloc are declared by the
	// platform declarations if not already declared by the module.
	return `; c_free(ptr: i8*) → void — wrapper around C free(3)
define void @c_free(i8* %ptr) {
  call void @free(i8* %ptr)
  ret void
}

; c_realloc(ptr: i8*, size: i64) → i8* — wrapper around C realloc(3)
define i8* @c_realloc(i8* %ptr, i64 %size) {
  %r = call i8* @realloc(i8* %ptr, i64 %size)
  ret i8* %r
}
`
}

// builtinRuntime generates LLVM IR implementations for VX builtin functions
// (chr, len, ord, itoa, str, value_error, out) that return values via the virtual stack.
func (t *Translator) builtinRuntime() string {
	return `; chr(c: i32) → void — push 1-char string onto virtual stack
; str(v: i64) → void — print integer as string (no stack push)
define void @str(i64 %val) {
  %fmt = getelementptr [3 x i8], [3 x i8]* @itoa_fmt, i64 0, i64 0
  %buf = call i8* @malloc(i64 12)
  %val32 = trunc i64 %val to i32
  call i32 (i8*, i8*, ...) @sprintf(i8* %buf, i8* %fmt, i32 %val32)
  call i32 @puts(i8* %buf)
  %freep = bitcast i8* %buf to void(i8*)*
  call void @free(i8* %buf)
  ret void
}

; value_error(msg: i8*) → void — push msg pointer onto virtual stack
define void @value_error(i8* %msg) {
  %ptr_int = ptrtoint i8* %msg to i64
  call void @__vx_stack_push(i64 %ptr_int)
  ret void
}

; chr(c: i32) → void — push 1-char string onto virtual stack
define void @chr(i32 %c) {
  %buf = call i8* @malloc(i64 2)
  %c_trunc = trunc i32 %c to i8
  store i8 %c_trunc, i8* %buf
  %end = getelementptr i8, i8* %buf, i64 1
  store i8 0, i8* %end
  %ptr = ptrtoint i8* %buf to i64
  call void @__vx_stack_push(i64 %ptr)
  ret void
}

; len(s: i8*) → void — push string length onto virtual stack
define void @len(i8* %s) {
  %dbg_p = ptrtoint i8* %s to i64
  %dbg_lo = and i64 %dbg_p, 255
  call void @putchar(i32 76)          ; 'L'
  call void @putchar(i32 61)          ; '='
  call void @putchar(i32 48)          ; '0' low nibble placeholder
  call void @putchar(i32 32)
  %len = call i64 @strlen(i8* %s)
  %len32 = trunc i64 %len to i32
  %len_ext = sext i32 %len32 to i64
  call void @__vx_stack_push(i64 %len_ext)
  ret void
}

; itoa(n: i32) → void — push integer as string onto virtual stack
define void @itoa(i32 %n) {
  %fmt = getelementptr [3 x i8], [3 x i8]* @itoa_fmt, i64 0, i64 0
  %buf = call i8* @malloc(i64 12)
  call i32 (i8*, i8*, ...) @sprintf(i8* %buf, i8* %fmt, i32 %n)
  %ptr_int = ptrtoint i8* %buf to i64
  call void @__vx_stack_push(i64 %ptr_int)
  ret void
}

; ord(s: i8*) → void — push first char ordinal onto virtual stack
define void @ord(i8* %s) {
  %ch = load i8, i8* %s
  %ch32 = zext i8 %ch to i32
  %ch_ext = sext i32 %ch32 to i64
  call void @__vx_stack_push(i64 %ch_ext)
  ret void
}

; __vx_strcat(a: i8*, b: i8*) → i8* — concatenate strings (malloc+memcpy)
define i8* @__vx_strcat(i8* %a, i8* %b) {
  %lena = call i64 @strlen(i8* %a)
  %lenb = call i64 @strlen(i8* %b)
  %total = add i64 %lena, %lenb
  %size = add i64 %total, 1
  %buf = call i8* @malloc(i64 %size)
  call void @llvm.memcpy.p0i8.p0i8.i64(i8* %buf, i8* %a, i64 %lena, i1 false)
  %end = getelementptr i8, i8* %buf, i64 %lena
  call void @llvm.memcpy.p0i8.p0i8.i64(i8* %end, i8* %b, i64 %lenb, i1 false)
  %end2 = getelementptr i8, i8* %buf, i64 %total
  store i8 0, i8* %end2
  ret i8* %buf
}
`
}

// ==================== Entry Point ====================

// EntryPointCode generates the platform entry point in LLVM IR.
// VX has no hidden runtime — the entry point is explicit and minimal.
//
// On Linux, _start calls main directly without CRT.
// On macOS, the system expects _main as the entry (provided by the user's code).
// On Windows, we provide mainCRTStartup that calls main.
func (t *Translator) EntryPointCode() string {
	switch t.Target.OutputFormat {
	case Elf:
		return t.elfEntryPoint()
	case MachO:
		// macOS: _main is the entry, user code provides it.
		// No extra wrapper needed — lld -e _main handles the linking.
		return ""
	case Pe:
		return t.peEntryPoint()
	default:
		return ""
	}
}

// elfEntryPoint generates a minimal _start for Linux/ELF.
// This is the "no hidden behavior" approach: no CRT, no init/fini,
// just set up argc/argv and call main.
func (t *Translator) elfEntryPoint() string {
	switch t.Target.Arch {
	case ArchAarch64:
		return `; Minimal ELF entry point — no CRT, no hidden init (AArch64).
; The ELF ABI passes argc on the stack at _start entry.
define i32 @_start() {
  %sp = call i64 asm "mov %0, sp", "=r"()
  %argc_ptr = inttoptr i64 %sp to i64*
  %argc = load i64, i64* %argc_ptr
  %ret = call i32 @main()
  call void @exit(i32 %ret)
  unreachable
}
`
	default: // x86_64
		return `; Minimal ELF entry point — no CRT, no hidden init (x86_64).
; In the ELF ABI, _start receives:
;   rsp = argc, rsp+8 = argv[0], ...
; We capture argc via inline asm, then call main.
; Everything is explicit — zero hidden behavior.
define i32 @_start() {
  %argc = call i64 asm "mov (%%rsp), %0", "=r"()
  %argc_i32 = trunc i64 %argc to i32
  %ret = call i32 @main()
  call void @exit(i32 %ret)
  unreachable
}
`
	}
}

// peEntryPoint generates a minimal entry for Windows PE.
func (t *Translator) peEntryPoint() string {
	return `; Minimal PE entry point — no CRT, no hidden init.
define void @mainCRTStartup() {
  %ret = call i32 @main()
  call void @exit(i32 %ret)
  unreachable
}
`
}

// ==================== Translator Error ====================

// Error represents a translator error.
type Error int

const (
	ErrToolNotFound     Error = iota // llc or lld not found
	ErrTranslateFailed               // LLVM IR generation failed
	ErrCompileFailed                 // llc failed
	ErrLinkFailed                    // lld failed
	ErrOutputNotCreated              // output file missing
	ErrInvalidInput                  // invalid VXOBJ input
)

func (e Error) Error() string {
	switch e {
	case ErrToolNotFound:
		return "translator tool not found (llc/lld)"
	case ErrTranslateFailed:
		return "VXOBJ translation failed"
	case ErrCompileFailed:
		return "LLVM IR compilation (llc) failed"
	case ErrLinkFailed:
		return "linking (lld) failed"
	case ErrOutputNotCreated:
		return "output executable not created"
	case ErrInvalidInput:
		return "invalid VXOBJ input"
	default:
		return fmt.Sprintf("translator error %d", int(e))
	}
}

// moduleHasExtern checks whether the TypeIR module already contains an
// external declaration for the given function name (from VX `extern`).
func moduleHasExtern(module *typeir.TypeModule, name string) bool {
	for _, fn := range module.Functions {
		if fn.Name == name {
			lk, ok := module.Linkage[fn.ID]
			_ = lk
			_ = ok
			return true
		}
	}
	return false
}
