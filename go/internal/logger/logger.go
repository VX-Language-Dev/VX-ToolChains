package logger

import (
	"fmt"
	"io"
	"os"
	"strings"
)

// LogLevel 日志级别
type LogLevel int

const (
	LogLevelTrace LogLevel = iota
	LogLevelDebug
	LogLevelInfo
	LogLevelWarn
	LogLevelErr
)

// String 返回日志级别的字符串表示
func (l LogLevel) String() string {
	switch l {
	case LogLevelTrace:
		return "TRACE"
	case LogLevelDebug:
		return "DEBUG"
	case LogLevelInfo:
		return " INFO"
	case LogLevelWarn:
		return " WARN"
	case LogLevelErr:
		return "ERROR"
	default:
		return "UNKNOWN"
	}
}

// ColorCode 返回日志级别的 ANSI 颜色码
func (l LogLevel) ColorCode() string {
	switch l {
	case LogLevelTrace:
		return "\x1b[90m"
	case LogLevelDebug:
		return "\x1b[36m"
	case LogLevelInfo:
		return "\x1b[32m"
	case LogLevelWarn:
		return "\x1b[33m"
	case LogLevelErr:
		return "\x1b[31m"
	default:
		return ""
	}
}

// ResetCode 返回 ANSI 重置码
func ResetCode() string {
	return "\x1b[0m"
}

// ErrorContext 错误上下文信息
type ErrorContext struct {
	File    *string
	Line    *uint32
	Column  *uint32
	Hint    *string
	Related *string
}

// Logger 日志记录器
type Logger struct {
	Level    LogLevel
	UseColor bool
	Writer   io.Writer
}

// NewLogger 创建新的 Logger 实例
func NewLogger(writer io.Writer, level LogLevel) *Logger {
	if writer == nil {
		writer = os.Stdout
	}
	return &Logger{
		Level:    level,
		UseColor: true,
		Writer:   writer,
	}
}

// NoColor 禁用颜色输出
func (l *Logger) NoColor() *Logger {
	l.UseColor = false
	return l
}

// SetLevel 设置日志级别
func (l *Logger) SetLevel(level LogLevel) {
	l.Level = level
}

// Log 输出指定级别的日志
func (l *Logger) Log(level LogLevel, format string, args ...any) {
	if level < l.Level {
		return
	}

	prefix := ""
	suffix := ""
	if l.UseColor {
		prefix = level.ColorCode()
		suffix = ResetCode()
	}

	fmt.Fprintf(l.Writer, "%s[%s]%s "+format+"\n", append([]any{prefix, level.String(), suffix}, args...)...)
}

// LogError 输出带上下文的错误日志
func (l *Logger) LogError(ctx ErrorContext, format string, args ...any) {
	if LogLevelErr < l.Level {
		return
	}

	prefix := ""
	suffix := ""
	hintPrefix := ""
	relatedPrefix := ""
	if l.UseColor {
		prefix = "\x1b[31m"
		suffix = "\x1b[0m"
		hintPrefix = "\x1b[36m"
		relatedPrefix = "\x1b[90m"
	}

	fmt.Fprintf(l.Writer, "%s[ERROR]%s "+format+"\n", append([]any{prefix, suffix}, args...)...)

	if ctx.File != nil {
		fmt.Fprintf(l.Writer, "  %sat %s", relatedPrefix, *ctx.File)
		if ctx.Line != nil {
			fmt.Fprintf(l.Writer, ":%d", *ctx.Line)
			if ctx.Column != nil {
				fmt.Fprintf(l.Writer, ":%d", *ctx.Column)
			}
		}
		fmt.Fprintln(l.Writer, suffix)
	}

	if ctx.Hint != nil {
		fmt.Fprintf(l.Writer, "  %shint: %s%s\n", hintPrefix, *ctx.Hint, suffix)
	}

	if ctx.Related != nil {
		fmt.Fprintf(l.Writer, "  %snote: %s%s\n", relatedPrefix, *ctx.Related, suffix)
	}
}

// Trace 输出 TRACE 级别日志
func (l *Logger) Trace(format string, args ...any) {
	l.Log(LogLevelTrace, format, args...)
}

// Debug 输出 DEBUG 级别日志
func (l *Logger) Debug(format string, args ...any) {
	l.Log(LogLevelDebug, format, args...)
}

// Info 输出 INFO 级别日志
func (l *Logger) Info(format string, args ...any) {
	l.Log(LogLevelInfo, format, args...)
}

// Warn 输出 WARN 级别日志
func (l *Logger) Warn(format string, args ...any) {
	l.Log(LogLevelWarn, format, args...)
}

// Err 输出 ERROR 级别日志
func (l *Logger) Err(format string, args ...any) {
	l.Log(LogLevelErr, format, args...)
}

// Report 输出带上下文的错误日志（LogError 的别名）
func (l *Logger) Report(ctx ErrorContext, format string, args ...any) {
	l.LogError(ctx, format, args...)
}

// Scope 创建带作用域的日志记录器
func (l *Logger) Scope(scopeName string) *ScopedLogger {
	return &ScopedLogger{
		Logger:    l,
		ScopeName: scopeName,
	}
}

// ScopedLogger 带作用域的日志记录器
type ScopedLogger struct {
	Logger    *Logger
	ScopeName string
}

// Log 输出指定作用域和级别的日志
func (s *ScopedLogger) Log(level LogLevel, format string, args ...any) {
	if level < s.Logger.Level {
		return
	}

	prefix := ""
	suffix := ""
	if s.Logger.UseColor {
		prefix = level.ColorCode()
		suffix = ResetCode()
	}

	fmt.Fprintf(s.Logger.Writer, "%s[%s][%s]%s "+format+"\n", append([]any{prefix, level.String(), s.ScopeName, suffix}, args...)...)
}

// Trace 输出 TRACE 级别日志
func (s *ScopedLogger) Trace(format string, args ...any) {
	s.Log(LogLevelTrace, format, args...)
}

// Debug 输出 DEBUG 级别日志
func (s *ScopedLogger) Debug(format string, args ...any) {
	s.Log(LogLevelDebug, format, args...)
}

// Info 输出 INFO 级别日志
func (s *ScopedLogger) Info(format string, args ...any) {
	s.Log(LogLevelInfo, format, args...)
}

// Warn 输出 WARN 级别日志
func (s *ScopedLogger) Warn(format string, args ...any) {
	s.Log(LogLevelWarn, format, args...)
}

// Err 输出 ERROR 级别日志
func (s *ScopedLogger) Err(format string, args ...any) {
	s.Log(LogLevelErr, format, args...)
}

// ErrContext 输出带上下文的错误日志
func (s *ScopedLogger) ErrContext(ctx ErrorContext, format string, args ...any) {
	if LogLevelErr < s.Logger.Level {
		return
	}

	prefix := ""
	suffix := ""
	hintPrefix := ""
	relatedPrefix := ""
	if s.Logger.UseColor {
		prefix = "\x1b[31m"
		suffix = "\x1b[0m"
		hintPrefix = "\x1b[36m"
		relatedPrefix = "\x1b[90m"
	}

	fmt.Fprintf(s.Logger.Writer, "%s[%s][ERROR]%s "+format+"\n", append([]any{prefix, s.ScopeName, suffix}, args...)...)

	if ctx.File != nil {
		fmt.Fprintf(s.Logger.Writer, "  %sat %s", relatedPrefix, *ctx.File)
		if ctx.Line != nil {
			fmt.Fprintf(s.Logger.Writer, ":%d", *ctx.Line)
			if ctx.Column != nil {
				fmt.Fprintf(s.Logger.Writer, ":%d", *ctx.Column)
			}
		}
		fmt.Fprintln(s.Logger.Writer, suffix)
	}

	if ctx.Hint != nil {
		fmt.Fprintf(s.Logger.Writer, "  %shint: %s%s\n", hintPrefix, *ctx.Hint, suffix)
	}

	if ctx.Related != nil {
		fmt.Fprintf(s.Logger.Writer, "  %snote: %s%s\n", relatedPrefix, *ctx.Related, suffix)
	}
}

// LogLevelFromEnv 从环境变量 VX_LOG_LEVEL 读取日志级别
func LogLevelFromEnv() LogLevel {
	levelStr := strings.ToLower(os.Getenv("VX_LOG_LEVEL"))
	switch levelStr {
	case "trace":
		return LogLevelTrace
	case "debug":
		return LogLevelDebug
	case "info":
		return LogLevelInfo
	case "warn":
		return LogLevelWarn
	case "error":
		return LogLevelErr
	default:
		return LogLevelInfo
	}
}
