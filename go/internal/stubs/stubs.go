// 空桩模块 - 用于尚未翻译的模块
// aot_backend, decompiler, delinker, builder, cache, lld_linker
package stubs

import "errors"

// NotImplemented 返回一个占位错误，表示对应功能尚未实现
func NotImplemented(name string) error {
	return errors.New("not implemented: " + name)
}

// Placeholder 是一个无参数的占位函数，返回零值 nil 错误
func Placeholder() error {
	return nil
}

// PlaceholderValue 返回一个占位字符串值
func PlaceholderValue() string {
	return ""
}
