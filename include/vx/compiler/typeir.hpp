// typeir.hpp — TypeIR 虚拟指令中间表示。
// 纯虚拟栈机指令：不含任何可在物理 CPU 上直接执行的指令；
// 下游由 vlk 模块按目标平台 ABI 翻译为 LLVM IR 并经 LLD 链接。
// 操作码数值对齐自举 compiler/typeir.vx，保证与既有 .vxobj 线格式可互操作。
#pragma once

#include "vx/compiler/ast.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace vx::compiler::ir {

// ---- 类型标签（对齐 typeir.vx）----
enum TypeTag : std::uint8_t {
  TYPE_VOID = 0,
  TYPE_INT = 1,
  TYPE_FLOAT = 2,
  TYPE_BOOL = 3,
  TYPE_STRING = 4,
  TYPE_STRUCT = 5,
  TYPE_ARRAY = 6,
  TYPE_MAP = 7,
  TYPE_FUNC = 8,
  TYPE_POINTER = 9,
  TYPE_GENERIC = 10,
  TYPE_UNKNOWN = 11,
};

// ---- 指令操作码（数值对齐 typeir.vx）----
enum Op : std::uint8_t {
  OP_CONST_INT = 0,
  OP_CONST_FLOAT = 1,
  OP_CONST_BOOL = 2,
  OP_CONST_STRING = 3,
  OP_CONST_NIL = 4,
  OP_LOAD_VAR = 5,
  OP_STORE_VAR = 6,
  OP_I32_ADD = 10,
  OP_I32_SUB = 11,
  OP_I32_MUL = 12,
  OP_I32_DIV = 13,
  OP_I32_MOD = 14,
  OP_F64_ADD = 15,
  OP_F64_SUB = 16,
  OP_F64_MUL = 17,
  OP_F64_DIV = 18,
  OP_I64_ADD = 19,
  OP_I32_EQ = 20,
  OP_I32_NEQ = 21,
  OP_I32_LT = 22,
  OP_I32_GT = 23,
  OP_I32_LE = 24,
  OP_I32_GE = 25,
  OP_F64_EQ = 26,
  OP_F64_NE = 27,
  OP_F64_LT = 28,
  OP_F64_GT = 29,
  OP_F64_LE = 30,
  OP_F64_GE = 31,
  OP_I32_NEG = 32,
  OP_F64_NEG = 33,
  OP_BOOL_NOT = 34,
  OP_I32_AND = 35,
  OP_I32_OR = 36,
  OP_BOOL_AND = 37,
  OP_BOOL_OR = 38,
  OP_JUMP = 40,
  OP_JUMP_IF_FALSE = 41,
  OP_JUMP_IF_TRUE = 42,
  OP_STR_EQ = 43,
  OP_NEQ_INT = 44,
  OP_I64_SUB = 45,
  OP_I64_MUL = 46,
  OP_I64_DIV = 47,
  OP_CALL = 50,
  OP_RETURN = 52,
  OP_RETURN_VOID = 53,
  OP_STRUCT_NEW = 60,
  OP_FIELD_LOAD = 61,
  OP_FIELD_STORE = 62,
  OP_ARRAY_NEW = 63,
  OP_INDEX_GET = 64,
  OP_INDEX_SET = 65,
  OP_MAP_NEW = 66,
  OP_NEWZ = 70,
  OP_FREE = 71,
  OP_OWNERSHIP_MOVE = 72,
  OP_BORROW_CHECK = 73,
  OP_DEREF = 74,
  OP_ALIVE_CHECK = 75,
  OP_DUP = 80,
  OP_POP = 81,
  OP_ADDRESS_OF = 82,
  OP_PROPERTY_GET = 83,
  OP_POINTER_MEMBER = 84,
  OP_SCOPE_DROP = 85,
  OP_SYS_ARGV = 86,
  OP_SYSTEM = 87,
  OP_FILE_READ = 88,
  OP_FILE_WRITE = 89,
  OP_FILE_EXISTS = 90,
  OP_NOOP = 91, // 标签标记：args = 标签名
  OP_IMPORT = 92,
  OP_HALT = 93,
  OP_STR_CONCAT = 94,
};

// 单条指令：op + 文本形式 args。
// args 编码约定（对齐自举 codegen.vx 发射格式）：
//   CONST_INT   "i<digits>"        CONST_FLOAT "f<text>"
//   CONST_STRING "s<bytes>"        CONST_BOOL  "btrue"/"bfalse"
//   LOAD/STORE_VAR  "<slot>"（十进制）
//   CALL  "f;<name>"（普通调用）/ "m;<method>"（方法调用，接收者已在栈上）
//   JUMP*  "<label>"               NOOP（标签） "<label>"
//   FIELD_LOAD/STORE "<field>"    NEWZ "<class>"
//   ARRAY_NEW/MAP_NEW "<count>"
struct Inst {
  std::uint8_t op;
  std::string args;
};

// 函数级 IR：局部变量为按名分配的连续槽位（0 起），参数占最前槽位。
struct FuncIR {
  std::string name;
  std::uint32_t id = 0;
  std::string ret_type; // "" 或 "void" 表示无返回
  std::vector<Param> params;
  std::vector<Inst> instructions;
  std::uint32_t local_count = 0;
};

// extern 声明 → 链接表条目（跨模块符号，由链接器解析）。
// 自有字符串（std::string）而非 string_view：导入符号的签名来自外部 .vxh
// 解析结果，不能依赖某个 arena 的生命周期。
struct AbiParam {
  std::string name;
  std::string type;
};

struct LinkEntry {
  std::string name;        // VX 侧可见名（库内原名，如 "add"）
  std::vector<AbiParam> params;
  std::string ret_type;
  // ---- VX ABI 元数据（普通 extern 时为空/默认，链接时回退用 name）----
  std::string abi_symbol;  // VX ABI 符号名（"_vx_math_add"）；空 = 直接用 name
  std::string lib;         // 来源库名（末段，如 "math"）；空 = 非导入的普通 extern
  bool from_cpp = false;   // 由 @cppimp 导入（链接期需生成 C++ shim）
};

// 一个被导入的库：记录链接期所需的实现文件 / C++ 头文件。
struct LibDep {
  std::string lib;        // 库名（末段）
  std::string ref;        // 原始库引用（"collections/vec" / "./mylib"）
  std::string impl_file;  // 实现文件绝对路径（.c/.a/.dylib/.cpp），可空
  bool from_cpp = false;  // true = C++ 头文件导入（需生成 shim）
  std::string cpp_header; // from_cpp 时的 C++ 头文件绝对路径
};

// 类/结构布局：字段名与类型（用于后端按偏移访问字段）。
struct StructField {
  std::string name;
  std::string type; // VX 类型名（int/float/bool/string/var/...）
};

struct StructLayout {
  std::string name;
  std::vector<StructField> fields;
};

struct ModuleIR {
  std::vector<FuncIR> funcs;
  std::vector<LinkEntry> linkage;
  // 顶层 var 声明暂不生成代码（对齐自举 codegen.vx：top_vars 不进指令流），
  // 名字记录在此，待引入全局槽位寻址后再落地。
  std::vector<std::string> global_vars;
  // 导入的库依赖（@imp / @cppimp），供后端链接期并入实现文件并生成 shim。
  std::vector<LibDep> libs;
  // 类/结构布局表（P4：后端按字段索引访问）。
  std::vector<StructLayout> structs;
};

} // namespace vx::compiler::ir
