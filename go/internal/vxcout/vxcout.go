package vxcout

import (
	"fmt"
	"io"
	"os"
	"runtime"
	"time"
)

const (
	reset  = "\x1b[0m"
	red    = "\x1b[31m"
	green  = "\x1b[32m"
	yellow = "\x1b[33m"
	cyan   = "\x1b[36m"
	blue   = "\x1b[34m"
	white  = "\x1b[37m"
	dim    = "\x1b[90m"
	bold   = "\x1b[1m"
)

var (
	colorSupported = isTerminal(os.Stdout)
)

func isTerminal(w io.Writer) bool {
	if runtime.GOOS == "windows" {
		return false
	}
	fd, ok := w.(*os.File)
	if !ok {
		return false
	}
	return isTerminalFd(int(fd.Fd()))
}

func isTerminalFd(fd int) bool {
	return true
}

type VXCOut struct {
	verbose  bool
	useColor bool
	writer   io.Writer
}

func NewVXCOut(writer io.Writer, verbose bool) *VXCOut {
	if writer == nil {
		writer = os.Stdout
	}
	return &VXCOut{
		verbose:  verbose,
		useColor: colorSupported,
		writer:   writer,
	}
}

func (v *VXCOut) SetVerbose(verbose bool) {
	v.verbose = verbose
}

func (v *VXCOut) DisableColor() {
	v.useColor = false
}

func (v *VXCOut) EnableColor() {
	v.useColor = true
}

func (v *VXCOut) c(color, msg string) string {
	if v.useColor {
		return color + msg + reset
	}
	return msg
}

func (v *VXCOut) PrintVersion() {
	fmt.Fprintln(v.writer, v.c(cyan, "VX Self-Bootstrapping Compiler v1.6.0"))
}

func (v *VXCOut) PrintHelp() {
	fmt.Fprintln(v.writer, v.c(bold, "VX Self-Bootstrapping Compiler v1.6.0"))
	fmt.Fprintln(v.writer)
	fmt.Fprintln(v.writer, v.c(white, "Usage:"), " vxc [options] <source.vx>")
	fmt.Fprintln(v.writer)
	fmt.Fprintln(v.writer, v.c(white, "Options:"))
	fmt.Fprintln(v.writer, "  -o <file>          Output VXOBJ file (default: out.vxobj)")
	fmt.Fprintln(v.writer, "  --target <triple>  Target triple (default: x86_64-linux-gnu)")
	fmt.Fprintln(v.writer, "  -v                 Verbose output")
	fmt.Fprintln(v.writer, "  --version          Print version")
	fmt.Fprintln(v.writer, "  -no-ownership      Skip ownership check")
	fmt.Fprintln(v.writer, "  -h, --help         Show this help")
	fmt.Fprintln(v.writer)
	fmt.Fprintln(v.writer, v.c(dim, "Examples:"))
	fmt.Fprintln(v.writer, v.c(dim, "  vxc -o output.vxobj main.vx"))
	fmt.Fprintln(v.writer, v.c(dim, "  vxc --target x86_64-macos-macho main.vx"))
	fmt.Fprintln(v.writer, v.c(dim, "  vxc -v main.vx"))
}

func (v *VXCOut) PrintStage(stage, msg string) {
	fmt.Fprintln(v.writer, v.c(cyan, "["+stage+"]"), " ", msg)
}

func (v *VXCOut) PrintCompiling(source string) {
	fmt.Fprintln(v.writer, v.c(bold, "[COMPILING]"), " ", v.c(cyan, source))
}

func (v *VXCOut) PrintStep(step, total int, msg string) {
	if v.verbose {
		fmt.Fprintln(v.writer, v.c(dim, "   Compiling "+msg+"..."))
	}
}

func (v *VXCOut) PrintSuccess(msg string) {
	fmt.Fprintln(v.writer, v.c(green, "[FINISHED]"), " ", msg)
}

func (v *VXCOut) PrintError(msg string) {
	fmt.Fprintln(v.writer, v.c(red, "[ERROR]"), " ", msg)
}

func (v *VXCOut) PrintWarning(msg string) {
	fmt.Fprintln(v.writer, v.c(yellow, "[WARNING]"), " ", msg)
}

func (v *VXCOut) PrintInfo(msg string) {
	fmt.Fprintln(v.writer, v.c(cyan, "[INFO]"), " ", msg)
}

func (v *VXCOut) PrintErrorLocation(errMsg, file string, line, col int) {
	fmt.Fprintln(v.writer, v.c(red, "[ERROR]"), " ", errMsg)
	fmt.Fprintln(v.writer, v.c(dim, "  --> ")+file+fmt.Sprintf(":%d:%d", line, col))
}

func (v *VXCOut) PrintCompiled(source, output string, size int) {
	fmt.Fprintln(v.writer, v.c(green, "[FINISHED]"), " ", v.c(cyan, source), " -> ", v.c(cyan, output), " (", fmt.Sprintf("%d", size), " bytes)")
}

func (v *VXCOut) PrintImport(path string) {
	if v.verbose {
		fmt.Fprintln(v.writer, v.c(dim, "  Processing import: ")+path)
	}
}

func (v *VXCOut) PrintStageDetail(stage, detail string) {
	if v.verbose {
		fmt.Fprintln(v.writer, v.c(dim, "  ")+stage+": "+detail)
	}
}

func (v *VXCOut) PrintElapsed(start time.Time) {
	if v.verbose {
		elapsed := time.Since(start)
		fmt.Fprintln(v.writer, v.c(dim, "  Done in ")+elapsed.String())
	}
}

var globalOut *VXCOut

func Get() *VXCOut {
	if globalOut == nil {
		globalOut = NewVXCOut(os.Stdout, false)
	}
	return globalOut
}

func SetGlobal(out *VXCOut) {
	globalOut = out
}
