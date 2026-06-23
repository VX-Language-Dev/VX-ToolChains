// ==================== OpCode ====================
//
// OpCode 使用 #[repr(u8)] 保证与字节码文件中的编码一致。
// TryFrom<u8> 通过查找表实现，新增 OpCode 时只需：
//   1. 在 enum 中添加变体并指定显式判别值
//   2. 在 OP_LOOKUP_TABLE 的对应位置添加条目
// 无需手动维护冗长的 match 表。

#[derive(Clone, Copy, Debug, PartialEq)]
#[repr(u8)]
pub enum OpCode {
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
}

/// 最大 opcode 值 + 1，用于确定查找表大小。
const OP_CODE_MAX: u8 = 0x58; // 88

/// 查找表：将 u8 字节码映射到 Option<OpCode>。
/// 新增 OpCode 时，在对应索引位置添加 Some(OpCode::Xxx) 即可。
/// 未使用的字节码位置为 None（返回错误）。
const OP_LOOKUP_TABLE: [Option<OpCode>; OP_CODE_MAX as usize] = {
    // 使用 const 表达式初始化数组（Rust 1.59+ 支持）
    let mut table: [Option<OpCode>; OP_CODE_MAX as usize] = [None; OP_CODE_MAX as usize];
    // 辅助宏：在编译期填充表项
    macro_rules! fill {
        ($t:ident, $val:expr) => {
            $t[$val as usize] = Some($val)
        };
    }
    fill!(table, OpCode::LoadConst);
    fill!(table, OpCode::LoadNil);
    fill!(table, OpCode::LoadTrue);
    fill!(table, OpCode::LoadFalse);
    fill!(table, OpCode::LoadVar);
    fill!(table, OpCode::StoreVar);
    fill!(table, OpCode::DefineVar);
    fill!(table, OpCode::Call);
    fill!(table, OpCode::Return);
    fill!(table, OpCode::MakeFunction);
    fill!(table, OpCode::Jump);
    fill!(table, OpCode::JumpIfFalse);
    fill!(table, OpCode::JumpIfTrue);
    fill!(table, OpCode::Break);
    fill!(table, OpCode::Continue);
    fill!(table, OpCode::BinaryAdd);
    fill!(table, OpCode::BinarySub);
    fill!(table, OpCode::BinaryMul);
    fill!(table, OpCode::BinaryDiv);
    fill!(table, OpCode::BinaryMod);
    fill!(table, OpCode::BinaryPow);
    fill!(table, OpCode::BinaryEq);
    fill!(table, OpCode::BinaryNe);
    fill!(table, OpCode::BinaryLt);
    fill!(table, OpCode::BinaryGt);
    fill!(table, OpCode::BinaryLe);
    fill!(table, OpCode::BinaryGe);
    fill!(table, OpCode::BinaryAnd);
    fill!(table, OpCode::BinaryOr);
    fill!(table, OpCode::UnaryNeg);
    fill!(table, OpCode::UnaryNot);
    fill!(table, OpCode::MakeStruct);
    fill!(table, OpCode::MakeClass);
    fill!(table, OpCode::MakeEnum);
    fill!(table, OpCode::MakeUnion);
    fill!(table, OpCode::LoadField);
    fill!(table, OpCode::StoreField);
    fill!(table, OpCode::MakeArray);
    fill!(table, OpCode::MakeMap);
    fill!(table, OpCode::IndexGet);
    fill!(table, OpCode::IndexSet);
    fill!(table, OpCode::PropertyGet);
    fill!(table, OpCode::PropertySet);
    fill!(table, OpCode::AddressOf);
    fill!(table, OpCode::Deref);
    fill!(table, OpCode::PointerMember);
    fill!(table, OpCode::Import);
    fill!(table, OpCode::New);
    fill!(table, OpCode::Halt);
    fill!(table, OpCode::SysArgv);
    fill!(table, OpCode::System);
    fill!(table, OpCode::FileRead);
    fill!(table, OpCode::FileWrite);
    fill!(table, OpCode::FileExists);
    fill!(table, OpCode::Dup);
    fill!(table, OpCode::Pop);
    fill!(table, OpCode::Newz);
    fill!(table, OpCode::Free);
    fill!(table, OpCode::OwnershipMove);
    fill!(table, OpCode::ScopeDrop);
    fill!(table, OpCode::BorrowCheck);
    fill!(table, OpCode::AliveCheck);
    // Specialized ops
    fill!(table, OpCode::AddInt);
    fill!(table, OpCode::AddFloat);
    fill!(table, OpCode::SubInt);
    fill!(table, OpCode::SubFloat);
    fill!(table, OpCode::MulInt);
    fill!(table, OpCode::MulFloat);
    fill!(table, OpCode::DivInt);
    fill!(table, OpCode::DivFloat);
    fill!(table, OpCode::ModInt);
    fill!(table, OpCode::EqInt);
    fill!(table, OpCode::EqFloat);
    fill!(table, OpCode::LtInt);
    fill!(table, OpCode::LtFloat);
    fill!(table, OpCode::GtInt);
    fill!(table, OpCode::GtFloat);
    fill!(table, OpCode::LeInt);
    fill!(table, OpCode::LeFloat);
    fill!(table, OpCode::GeInt);
    fill!(table, OpCode::GeFloat);
    fill!(table, OpCode::NegInt);
    fill!(table, OpCode::NegFloat);
    fill!(table, OpCode::Not);
    fill!(table, OpCode::And);
    fill!(table, OpCode::Or);
    table
};

impl TryFrom<u8> for OpCode {
    type Error = String;
    fn try_from(v: u8) -> Result<Self, Self::Error> {
        OP_LOOKUP_TABLE
            .get(v as usize)
            .copied()
            .flatten()
            .ok_or_else(|| format!("Unknown opcode: 0x{:02X}", v))
    }
}
