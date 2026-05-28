// ==================== VXOBJ 字节码序列化/反序列化模块 ====================
// 提供 VXOBJ 格式的写入（序列化）和读取（反序列化）功能
//
// VXOBJ 格式 (v2):
//   - 5 字节魔数: "VXOBJ"
//   - 4 字节版本号 (大端序 u32): 2
//   - 4 字节常量数量 (大端序 u32)
//   - 常量池: 每个常量 [1 字节类型][值...]
//   - 4 字节函数数量 (大端序 u32)
//   - 函数区: 每个函数 [名称][参数][返回值][指令...]
//   - 可选: 结构体/类定义

use std::collections::HashMap;
use std::io::{self, Read, Write};

// ==================== 常量类型 ====================

/// 编译时常量值（与编译器中的 ConstantValue 对应）
#[derive(Debug, Clone)]
pub enum SerializedConstant {
    Nil,
    Bool(bool),
    Int(i64),
    Float(f64),
    String(String),
}

// ==================== 序列化函数 ====================

/// 将 u32 以大端序写入 writer
fn write_u32_be(w: &mut dyn Write, v: u32) -> io::Result<()> {
    w.write_all(&v.to_be_bytes())
}

/// 将 i32 以大端序写入 writer
#[allow(dead_code)]
fn write_i32_be(w: &mut dyn Write, v: i32) -> io::Result<()> {
    w.write_all(&v.to_be_bytes())
}

/// 写入带长度前缀的字符串（大端序 u32 长度 + UTF-8 字节）
pub fn write_string(w: &mut dyn Write, s: &str) -> io::Result<()> {
    let bytes = s.as_bytes();
    write_u32_be(w, bytes.len() as u32)?;
    w.write_all(bytes)
}

/// 写入一个常量值
pub fn write_constant(w: &mut dyn Write, c: &SerializedConstant) -> io::Result<()> {
    match c {
        SerializedConstant::Nil => w.write_all(&[0]),
        SerializedConstant::Bool(b) => {
            w.write_all(&[4])?;
            w.write_all(&[if *b { 1 } else { 0 }])
        }
        SerializedConstant::Int(v) => {
            w.write_all(&[1])?;
            w.write_all(&(*v as i64).to_be_bytes())
        }
        SerializedConstant::Float(v) => {
            w.write_all(&[2])?;
            w.write_all(&v.to_be_bytes())
        }
        SerializedConstant::String(s) => {
            w.write_all(&[3])?;
            write_string(w, s)
        }
    }
}

// ==================== 反序列化函数 ====================

/// 从 reader 读取 u8
fn read_u8(r: &mut dyn Read) -> io::Result<u8> {
    let mut buf = [0u8; 1];
    r.read_exact(&mut buf)?;
    Ok(buf[0])
}

/// 从 reader 以大端序读取 u32
fn read_u32_be(r: &mut dyn Read) -> io::Result<u32> {
    let mut buf = [0u8; 4];
    r.read_exact(&mut buf)?;
    Ok(((buf[0] as u32) << 24)
        | ((buf[1] as u32) << 16)
        | ((buf[2] as u32) << 8)
        | (buf[3] as u32))
}

/// 从 reader 以大端序读取 i32
fn read_i32_be(r: &mut dyn Read) -> io::Result<i32> {
    read_u32_be(r).map(|v| v as i32)
}

/// 从 reader 以大端序读取 u64
fn read_u64_be(r: &mut dyn Read) -> io::Result<u64> {
    let mut buf = [0u8; 8];
    r.read_exact(&mut buf)?;
    let mut v: u64 = 0;
    for b in &buf {
        v = (v << 8) | (*b as u64);
    }
    Ok(v)
}

/// 从 reader 以大端序读取 f64
fn read_f64_be(r: &mut dyn Read) -> io::Result<f64> {
    let bits = read_u64_be(r)?;
    Ok(f64::from_bits(bits))
}

/// 从 reader 读取带长度前缀的字符串
pub fn read_string(r: &mut dyn Read) -> io::Result<String> {
    let len = read_u32_be(r)? as usize;
    let mut buf = vec![0u8; len];
    r.read_exact(&mut buf)?;
    Ok(String::from_utf8_lossy(&buf).to_string())
}

/// 从 reader 读取一个常量值
pub fn read_constant(r: &mut dyn Read) -> io::Result<SerializedConstant> {
    let type_id = read_u8(r)?;
    match type_id {
        0 => Ok(SerializedConstant::Nil),
        1 => Ok(SerializedConstant::Int(read_u64_be(r)? as i64)),
        2 => Ok(SerializedConstant::Float(read_f64_be(r)?)),
        3 => Ok(SerializedConstant::String(read_string(r)?)),
        4 => Ok(SerializedConstant::Bool(read_u8(r)? != 0)),
        _ => Err(io::Error::new(
            io::ErrorKind::InvalidData,
            format!("Unknown constant type: {}", type_id),
        )),
    }
}

// ==================== VXOBJ 文件格式解析器 ====================

/// VXOBJ 解析结果：包含完整解析的结构化数据
#[derive(Debug, Clone)]
pub struct VxObjModule {
    pub version: u32,
    pub constants: Vec<SerializedConstant>,
    pub functions: Vec<VxObjFunction>,
    pub struct_defs: HashMap<String, Vec<String>>,
}

#[derive(Debug, Clone)]
pub struct VxObjFunction {
    pub name: String,
    pub num_params: u32,
    pub has_return: bool,
    pub param_names: Vec<String>,
    pub instructions: Vec<VxObjInstruction>,
}

#[derive(Debug, Clone)]
pub struct VxObjInstruction {
    pub op: u8,
    pub arg_type: u8,
    pub iarg: Option<i32>,
    pub sarg: Option<String>,
}

/// 解析 VXOBJ 字节数组，返回结构化模块
pub fn parse_vxobj(data: &[u8]) -> io::Result<VxObjModule> {
    use std::io::Cursor;
    let mut r = Cursor::new(data);

    // 验证魔数
    let mut magic = [0u8; 5];
    r.read_exact(&mut magic)?;
    if &magic != b"VXOBJ" {
        return Err(io::Error::new(
            io::ErrorKind::InvalidData,
            "Invalid magic number: expected VXOBJ",
        ));
    }

    let version = read_u32_be(&mut r)?;
    if version != 2 {
        return Err(io::Error::new(
            io::ErrorKind::InvalidData,
            format!("Unsupported version: {}", version),
        ));
    }

    // 读取常量池
    let num_constants = read_u32_be(&mut r)?;
    let mut constants = Vec::with_capacity(num_constants as usize);
    for _ in 0..num_constants {
        constants.push(read_constant(&mut r)?);
    }

    // 读取函数
    let num_functions = read_u32_be(&mut r)?;
    let mut functions = Vec::with_capacity(num_functions as usize);
    for _ in 0..num_functions {
        let name = read_string(&mut r)?;
        let num_params = read_u32_be(&mut r)?;
        let has_return = read_u8(&mut r)? != 0;

        let num_param_names = read_u32_be(&mut r)?;
        let mut param_names = Vec::with_capacity(num_param_names as usize);
        for _ in 0..num_param_names {
            param_names.push(read_string(&mut r)?);
        }

        // 跳过局部常量池（当前未使用）
        let num_local_consts = read_u32_be(&mut r)?;
        for _ in 0..num_local_consts {
            let _ = read_constant(&mut r)?;
        }

        // 读取指令
        let num_insts = read_u32_be(&mut r)?;
        let mut instructions = Vec::with_capacity(num_insts as usize);
        for _ in 0..num_insts {
            let op = read_u8(&mut r)?;
            let arg_type = read_u8(&mut r)?;

            let (iarg, sarg) = match arg_type {
                0 => (None, None),
                1 => (Some(read_i32_be(&mut r)?), None),
                2 => (None, Some(read_string(&mut r)?)),
                _ => {
                    return Err(io::Error::new(
                        io::ErrorKind::InvalidData,
                        format!("Unknown argument type: {}", arg_type),
                    ))
                }
            };

            instructions.push(VxObjInstruction {
                op,
                arg_type,
                iarg,
                sarg,
            });
        }

        functions.push(VxObjFunction {
            name,
            num_params,
            has_return,
            param_names,
            instructions,
        });
    }

    // 读取结构体/类定义（如果还有剩余数据）
    let mut struct_defs = HashMap::new();
    let remaining = data.len() - (r.position() as usize);
    if remaining >= 4 {
        let num_structs = read_u32_be(&mut r)?;
        for _ in 0..num_structs {
            let sname = read_string(&mut r)?;
            let num_fields = read_u32_be(&mut r)?;
            let mut fields = Vec::with_capacity(num_fields as usize);
            for _ in 0..num_fields {
                fields.push(read_string(&mut r)?);
            }
            struct_defs.insert(sname, fields);
        }
    }

    Ok(VxObjModule {
        version,
        constants,
        functions,
        struct_defs,
    })
}

// ==================== 高层序列化 API ====================

/// 将常量池和函数列表序列化为 VXOBJ 格式写入 writer
///
/// 参数:
/// - `w`: 写入目标
/// - `constants`: 常量池
/// - `functions`: 函数列表，每个元素为 (名称, 参数数量, 是否有返回值, 参数名列表, 指令列表)
/// - `struct_defs`: 结构体/类定义 (名称 -> 字段列表)
/// - `instructions`: 指令列表，每个元素为 (opcode, arg_type, iarg, sarg)
///
/// arg_type: 0=无参数, 1=整数参数, 2=字符串参数
pub fn write_vxobj(
    w: &mut dyn Write,
    constants: &[SerializedConstant],
    functions: &[(
        &str,                     // name
        u32,                      // num_params
        bool,                     // has_return
        &[String],                // param_names
        &[(u8, u8, Option<i32>, Option<String>)], // instructions: (op, arg_type, iarg, sarg)
    )],
    struct_defs: &HashMap<String, Vec<String>>,
) -> io::Result<()> {
    // 写入魔数和版本
    w.write_all(b"VXOBJ")?;
    write_u32_be(w, 2)?;

    // 写入常量池
    write_u32_be(w, constants.len() as u32)?;
    for c in constants {
        write_constant(w, c)?;
    }

    // 写入函数
    write_u32_be(w, functions.len() as u32)?;
    for (name, num_params, has_return, param_names, insts) in functions {
        write_string(w, name)?;
        write_u32_be(w, *num_params)?;
        w.write_all(&[if *has_return { 1 } else { 0 }])?;

        write_u32_be(w, param_names.len() as u32)?;
        for pn in *param_names {
            write_string(w, pn)?;
        }

        // 局部常量池（当前未使用，写入 0）
        write_u32_be(w, 0)?;

        // 写入指令
        write_u32_be(w, insts.len() as u32)?;
        for (op, arg_type, iarg, sarg) in *insts {
            w.write_all(&[*op])?;
            w.write_all(&[*arg_type])?;
            match arg_type {
                1 => {
                    if let Some(v) = iarg {
                        write_i32_be(w, *v)?;
                    }
                }
                2 => {
                    if let Some(ref s) = sarg {
                        write_string(w, s)?;
                    }
                }
                _ => {}
            }
        }
    }

    // 写入结构体/类定义
    write_u32_be(w, struct_defs.len() as u32)?;
    for (sname, fields) in struct_defs {
        write_string(w, sname)?;
        write_u32_be(w, fields.len() as u32)?;
        for fname in fields {
            write_string(w, fname)?;
        }
    }

    w.flush()?;
    Ok(())
}

// ==================== 测试 ====================

#[cfg(test)]
mod tests {
    use super::*;
    use std::collections::HashMap;

    #[test]
    fn test_roundtrip_simple() {
        let constants = vec![
            SerializedConstant::Nil,
            SerializedConstant::Int(42),
            SerializedConstant::String("hello".to_string()),
        ];

        let instructions: Vec<(u8, u8, Option<i32>, Option<String>)> = vec![
            (0x01, 1, Some(1), None),   // LoadConst idx=1
            (0x09, 0, None, None),       // Return
        ];

        let functions: Vec<(&str, u32, bool, &[String], &[(u8, u8, Option<i32>, Option<String>)])> =
            vec![("__main__", 0, false, &[], &instructions)];

        let struct_defs = HashMap::new();

        let mut buf = Vec::new();
        write_vxobj(&mut buf, &constants, &functions, &struct_defs).unwrap();

        // 解析回来
        let parsed = parse_vxobj(&buf).unwrap();
        assert_eq!(parsed.version, 2);
        assert_eq!(parsed.constants.len(), 3);
        assert_eq!(parsed.functions.len(), 1);
        assert_eq!(parsed.functions[0].name, "__main__");
        assert_eq!(parsed.functions[0].instructions.len(), 2);
        assert_eq!(parsed.functions[0].instructions[0].op, 0x01);
    }
}
