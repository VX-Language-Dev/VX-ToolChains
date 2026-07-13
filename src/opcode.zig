//! OpCode 使用 explicit tag values 保证与字节码文件中的编码一致。
//! `fromByte` 通过 comptime 查找表实现，新增 OpCode 时只需在 enum 中添加变体并指定显式判别值即可。
//! 无需手动维护冗长的 switch 表。

pub const OpCode = enum(u8) {
    LoadConst = 0x01,
    LoadNil = 0x02,
    LoadTrue = 0x03,
    LoadFalse = 0x04,
    LoadVar = 0x05,
    StoreVar = 0x06,
    DefineVar = 0x07,
    Call = 0x08,
    Return = 0x09,
    MakeFunction = 0x0A,
    Jump = 0x0B,
    JumpIfFalse = 0x0C,
    JumpIfTrue = 0x0D,
    Break = 0x0E,
    Continue = 0x0F,
    BinaryAdd = 0x10,
    BinarySub = 0x11,
    BinaryMul = 0x12,
    BinaryDiv = 0x13,
    BinaryMod = 0x14,
    BinaryPow = 0x15,
    BinaryEq = 0x16,
    BinaryNe = 0x17,
    BinaryLt = 0x18,
    BinaryGt = 0x19,
    BinaryLe = 0x1A,
    BinaryGe = 0x1B,
    BinaryAnd = 0x1C,
    BinaryOr = 0x1D,
    UnaryNeg = 0x1E,
    UnaryNot = 0x1F,
    MakeStruct = 0x20,
    MakeClass = 0x21,
    MakeEnum = 0x22,
    MakeUnion = 0x23,
    LoadField = 0x24,
    StoreField = 0x25,
    MakeArray = 0x26,
    MakeMap = 0x27,
    IndexGet = 0x28,
    IndexSet = 0x29,
    PropertyGet = 0x2A,
    PropertySet = 0x2B,
    AddressOf = 0x2C,
    Deref = 0x2D,
    PointerMember = 0x2E,
    Import = 0x2F,
    New = 0x30,
    Halt = 0x31,
    SysArgv = 0x32,
    System = 0x33,
    FileRead = 0x34,
    FileWrite = 0x35,
    FileExists = 0x36,
    Dup = 0x37,
    Pop = 0x38,
    // Memory Safety / Ownership
    Newz = 0x39,
    Free = 0x3A,
    OwnershipMove = 0x3B,
    ScopeDrop = 0x3C,
    BorrowCheck = 0x3D,
    AliveCheck = 0x3E,
    // Specialized type ops (0x40-0x57)
    AddInt = 0x40,
    AddFloat = 0x41,
    SubInt = 0x42,
    SubFloat = 0x43,
    MulInt = 0x44,
    MulFloat = 0x45,
    DivInt = 0x46,
    DivFloat = 0x47,
    ModInt = 0x48,
    EqInt = 0x49,
    EqFloat = 0x4A,
    LtInt = 0x4B,
    LtFloat = 0x4C,
    GtInt = 0x4D,
    GtFloat = 0x4E,
    LeInt = 0x4F,
    LeFloat = 0x50,
    GeInt = 0x51,
    GeFloat = 0x52,
    NegInt = 0x53,
    NegFloat = 0x54,
    Not = 0x55,
    And = 0x56,
    Or = 0x57,
    // Iterator ops (for-in support)
    Iterate = 0x60,
    Next = 0x61,

    /// 最大 opcode 值 + 1，用于确定查找表大小。
    const OP_CODE_MAX: u8 = 0x62; // 98

    /// 查找表：将 u8 字节码映射到 ?OpCode。
    /// 通过 comptime 自动遍历所有 enum 变体填充。
    /// 未使用的字节码位置为 null（返回 error.UnknownOpcode）。
    const lookup_table = blk: {
        var table: [OP_CODE_MAX]?OpCode = [_]?OpCode{null} ** OP_CODE_MAX;
        for (@typeInfo(OpCode).Enum.fields) |field| {
            table[field.value] = @enumFromInt(field.value);
        }
        break :blk table;
    };

    /// 将 u8 字节码转换为 OpCode。
    /// 如果字节码无效或未定义，返回 error.UnknownOpcode。
    pub fn fromByte(value: u8) !OpCode {
        if (value >= OP_CODE_MAX) return error.UnknownOpcode;
        return lookup_table[value] orelse error.UnknownOpcode;
    }
};
