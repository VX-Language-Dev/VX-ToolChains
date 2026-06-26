use std::collections::HashMap;
use std::io::{self, Read, Write};

// ==================== VXOBJ v3 Format ====================
//
// VXOBJ v3: Section-based container
//   [Header]
//     5 bytes magic: "VXOBJ"
//     4 bytes version (u32 BE): 3
//     N bytes TargetTriple (u32 len BE + UTF-8)
//   [Section Index Table]
//     4 bytes count (u32 BE)
//     For each section:
//       4 bytes name_len (u32 BE) + name bytes
//       4 bytes offset (u32 BE, from file start)
//       4 bytes size (u32 BE, compressed)
//       4 bytes raw_size (u32 BE, 0 = not compressed)
//       1 byte flags (bit 0: compressed)
//   [Sections] (TypeIR, Bytecode, DebugInfo, SourceMap, TypeMeta)
//
// Backward compatible: v2 files still parse correctly.

const MAGIC: &[u8; 5] = b"VXOBJ";
const VERSION_V2: u32 = 2;
const VERSION_V3: u32 = 3;

// ==================== VXCO v1: Cross-Platform Intermediate Format ====================
//
// VXCO (VX Compiler Object): 跨平台中间文件格式，不包含任何可执行文件特征
//
// 格式结构:
//   [Header]
//     4 bytes magic: "VXCO"
//     4 bytes version (u32 BE): 1
//     4 bytes flags (u32 BE): bit 0 = has_external_deps
//     4 bytes target_triple_len (u32 BE)
//     N bytes target_triple (UTF-8)
//   [Section Index Table]
//     4 bytes count (u32 BE)
//     For each section:
//       4 bytes name_len (u32 BE) + name bytes
//       4 bytes offset (u32 BE, from file start)
//       4 bytes size (u32 BE)
//   [Sections] (TypeIR, DebugInfo, SourceMap, ExternalDeps)
//
// 用途:
//   - 编译器输出 VXCO 格式，不嵌入任何运行时或可执行信息
//   - 链接器接收 VXCO，自动适配目标平台，生成原生机器码可执行文件
//
// 外部依赖机制:
//   - 当源代码引用外部动态链接库时，ExternalDeps section 包含依赖信息
//   - 链接器根据此信息自动采用动态链接方式
//   - 未引用外部库的源代码默认静态链接生成独立可执行文件

const VXCO_MAGIC: &[u8; 4] = b"VXCO";
const VXCO_VERSION: u32 = 1;

// VXCO Section names
pub const VXCO_SECTION_TYPE_IR: &str = "TypeIR";
pub const VXCO_SECTION_DEBUG: &str = "Debug";
pub const VXCO_SECTION_SOURCE_MAP: &str = "SourceMap";
pub const VXCO_SECTION_EXTERNAL_DEPS: &str = "ExternalDeps";

#[derive(Debug, Clone)]
pub struct VxcoHeader {
    pub version: u32,
    pub flags: u32,
    pub target_triple: String,
    pub sections: Vec<VxcoSectionIndex>,
}

#[derive(Debug, Clone)]
pub struct VxcoSectionIndex {
    pub name: String,
    pub offset: u32,
    pub size: u32,
}

#[derive(Debug, Clone)]
pub struct VxcoContainer {
    pub header: VxcoHeader,
    pub section_data: HashMap<String, Vec<u8>>,
}

impl VxcoContainer {
    pub fn new(target_triple: &str) -> Self {
        Self {
            header: VxcoHeader {
                version: VXCO_VERSION,
                flags: 0,
                target_triple: target_triple.to_string(),
                sections: Vec::new(),
            },
            section_data: HashMap::new(),
        }
    }

    pub fn set_section(&mut self, name: &str, data: Vec<u8>) {
        let size = data.len() as u32;
        self.section_data.insert(name.to_string(), data);
        self.header.sections.push(VxcoSectionIndex {
            name: name.to_string(),
            offset: 0,
            size,
        });
    }

    pub fn get_section(&self, name: &str) -> Option<&Vec<u8>> {
        self.section_data.get(name)
    }

    pub fn has_external_deps(&self) -> bool {
        (self.header.flags & 1) != 0
    }

    pub fn set_external_deps_flag(&mut self, has_deps: bool) {
        if has_deps {
            self.header.flags |= 1;
        } else {
            self.header.flags &= !1;
        }
    }

    pub fn write(&self, w: &mut dyn Write) -> io::Result<()> {
        let mut sections: Vec<(String, Vec<u8>)> = Vec::new();
        for sec in &self.header.sections {
            if let Some(data) = self.section_data.get(&sec.name) {
                sections.push((sec.name.clone(), data.clone()));
            }
        }

        let base_header_size = 4 + 4 + 4 + 4 + self.header.target_triple.len() as u32;
        let section_index_size: u32 = 4 + self.header.sections.iter()
            .map(|s| 4 + s.name.len() as u32 + 4 + 4)
            .sum::<u32>();
        let mut cur_off = base_header_size + section_index_size;

        // Write header
        w.write_all(VXCO_MAGIC)?;
        write_u32_be(w, VXCO_VERSION)?;
        write_u32_be(w, self.header.flags)?;
        write_string(w, &self.header.target_triple)?;

        // Write section index
        write_u32_be(w, self.header.sections.len() as u32)?;
        for (i, sec) in self.header.sections.iter().enumerate() {
            write_string(w, &sec.name)?;
            write_u32_be(w, cur_off)?;
            write_u32_be(w, sec.size)?;
            cur_off += sections[i].1.len() as u32;
        }

        // Write section data
        for (_, data) in &sections {
            w.write_all(data)?;
        }
        w.flush()?;
        Ok(())
    }

    pub fn parse(data: &[u8]) -> io::Result<Self> {
        let mut r = io::Cursor::new(data);

        // Read magic
        let mut magic = [0u8; 4];
        r.read_exact(&mut magic)?;
        if &magic != VXCO_MAGIC {
            return Err(io::Error::new(io::ErrorKind::InvalidData, "Invalid VXCO magic"));
        }

        let version = read_u32_be(&mut r)?;
        if version != VXCO_VERSION {
            return Err(io::Error::new(io::ErrorKind::InvalidData, format!("Unsupported VXCO version: {}", version)));
        }

        let flags = read_u32_be(&mut r)?;
        let target_triple = read_string(&mut r)?;

        let num_sections = read_u32_be(&mut r)?;
        let mut sections = Vec::with_capacity(num_sections as usize);
        let mut section_data = HashMap::new();

        for _ in 0..num_sections {
            let name = read_string(&mut r)?;
            let offset = read_u32_be(&mut r)?;
            let size = read_u32_be(&mut r)?;

            let end = (offset + size) as usize;
            if end > data.len() {
                return Err(io::Error::new(io::ErrorKind::InvalidData, "VXCO section data truncated"));
            }
            section_data.insert(name.clone(), data[offset as usize..end].to_vec());
            sections.push(VxcoSectionIndex { name, offset, size });
        }

        Ok(Self {
            header: VxcoHeader { version, flags, target_triple, sections },
            section_data,
        })
    }
}

// ==================== VXCO External Dependencies ====================
//
// 简单格式: null-separated list of entries
// 每个 entry 格式: "name\0path\0is_optional\0"
// - name: 库名称
// - path: 库路径（可选，为空表示系统库）
// - is_optional: "1" 表示可选，"0" 表示必需

#[derive(Debug, Clone)]
pub struct ExternalDependency {
    pub name: String,
    pub path: Option<String>,
    pub is_optional: bool,
}

impl ExternalDependency {
    pub fn new(name: &str) -> Self {
        Self {
            name: name.to_string(),
            path: None,
            is_optional: false,
        }
    }

    pub fn with_path(mut self, path: &str) -> Self {
        self.path = Some(path.to_string());
        self
    }

    pub fn set_optional(mut self, optional: bool) -> Self {
        self.is_optional = optional;
        self
    }

    fn to_bytes(&self) -> Vec<u8> {
        let mut result = self.name.as_bytes().to_vec();
        result.push(0);
        if let Some(ref path) = self.path {
            result.extend_from_slice(path.as_bytes());
        }
        result.push(0);
        result.push(if self.is_optional { b'1' } else { b'0' });
        result.push(0);
        result
    }

    fn from_bytes(data: &[u8]) -> Option<Self> {
        let parts: Vec<&[u8]> = data.split(|&b| b == 0).collect();
        if parts.is_empty() {
            return None;
        }
        let name = String::from_utf8_lossy(parts[0]).to_string();
        let path = if parts.len() > 1 && !parts[1].is_empty() {
            Some(String::from_utf8_lossy(parts[1]).to_string())
        } else {
            None
        };
        let is_optional = parts.len() > 2 && parts[2] == b"1";
        Some(ExternalDependency { name, path, is_optional })
    }
}

pub fn serialize_external_deps(deps: &[ExternalDependency]) -> Vec<u8> {
    let mut result = Vec::new();
    for dep in deps {
        result.extend_from_slice(&dep.to_bytes());
    }
    result
}

pub fn deserialize_external_deps(data: &[u8]) -> Vec<ExternalDependency> {
    let mut deps = Vec::new();
    let mut pos = 0;

    while pos < data.len() {
        // 读取 name (直到第一个 null)
        let name_end = data[pos..].iter().position(|&b| b == 0).unwrap_or(data.len() - pos);
        let name = String::from_utf8_lossy(&data[pos..pos + name_end]).to_string();
        pos += name_end + 1;

        if pos >= data.len() {
            break;
        }

        // 读取 path (直到第二个 null)
        let path_end = data[pos..].iter().position(|&b| b == 0).unwrap_or(data.len() - pos);
        let path = if path_end > 0 {
            Some(String::from_utf8_lossy(&data[pos..pos + path_end]).to_string())
        } else {
            None
        };
        pos += path_end + 1;

        if pos >= data.len() {
            break;
        }

        // 读取 is_optional (第三个字段)
        let is_optional = data[pos] == b'1';
        pos += 1;

        deps.push(ExternalDependency {
            name,
            path,
            is_optional,
        });
    }

    deps
}

// Section names
pub const SECTION_TYPE_IR: &str = "TypeIR";
pub const SECTION_BYTECODE: &str = "Bytecode";
pub const SECTION_DEBUG: &str = "Debug";
pub const SECTION_SOURCE_MAP: &str = "SourceMap";
pub const SECTION_TYPE_META: &str = "TypeMeta";

// ==================== Public Types ====================

pub type BytecodeInstructionTuple = (u8, u8, Option<i32>, Option<String>);

pub type VxobjFunctionRef<'a> = (&'a str, u32, bool, &'a [String], &'a [BytecodeInstructionTuple]);

#[derive(Debug, Clone)]
pub struct SectionIndex {
    pub name: String,
    pub offset: u32,
    pub size: u32,
    pub raw_size: u32,
    pub compressed: bool,
}

#[derive(Debug, Clone)]
pub struct VxObjV3Header {
    pub version: u32,
    pub target_triple: String,
    pub sections: Vec<SectionIndex>,
}

#[derive(Debug, Clone)]
pub struct SerializedConstant {
    pub tag: u8,
    pub data: Vec<u8>,
}

impl SerializedConstant {
    pub fn nil() -> Self { Self { tag: 0, data: vec![] } }
    pub fn bool(v: bool) -> Self { Self { tag: 4, data: vec![if v { 1 } else { 0 }] } }
    pub fn int(v: i64) -> Self { Self { tag: 1, data: v.to_be_bytes().to_vec() } }
    pub fn float(v: f64) -> Self { Self { tag: 2, data: v.to_be_bytes().to_vec() } }
    pub fn string(s: &str) -> Self { Self { tag: 3, data: s.as_bytes().to_vec() } }
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

#[derive(Debug, Clone)]
pub struct VxObjModule {
    pub version: u32,
    pub target_triple: String,
    pub constants: Vec<SerializedConstant>,
    pub functions: Vec<VxObjFunction>,
    pub struct_defs: HashMap<String, Vec<String>>,
}

// ==================== V3 Section Storage ====================

pub struct VxObjV3Container {
    pub header: VxObjV3Header,
    pub section_data: HashMap<String, Vec<u8>>,
}

impl VxObjV3Container {
    pub fn new(target_triple: &str) -> Self {
        Self {
            header: VxObjV3Header {
                version: VERSION_V3,
                target_triple: target_triple.to_string(),
                sections: Vec::new(),
            },
            section_data: HashMap::new(),
        }
    }

    pub fn set_section(&mut self, name: &str, data: Vec<u8>, compress: bool) {
        let (stored, raw_size) = if compress && data.len() > 64 {
            let compressed = lz4_flex::compress_prepend_size(&data);
            (compressed, data.len() as u32)
        } else {
            (data.clone(), 0)
        };
        self.section_data.insert(name.to_string(), stored.clone());
        self.header.sections.push(SectionIndex {
            name: name.to_string(),
            offset: 0,
            size: stored.len() as u32,
            raw_size,
            compressed: raw_size != 0,
        });
    }

    pub fn get_section(&self, name: &str) -> Option<Vec<u8>> {
        let idx = self.header.sections.iter().find(|s| s.name == name)?;
        let data = self.section_data.get(name)?;
        if idx.compressed {
            lz4_flex::decompress_size_prepended(data).ok()
        } else {
            Some(data.clone())
        }
    }

    pub fn write(&self, w: &mut dyn Write) -> io::Result<()> {
        // Collect all section data to compute offsets
        let mut sections: Vec<(String, Vec<u8>)> = Vec::new();
        for sec in &self.header.sections {
            let data = self.section_data.get(&sec.name).unwrap();
            sections.push((sec.name.clone(), data.clone()));
        }

        let base_header_size = 5u32 + 4 + 4 + self.header.target_triple.len() as u32;
        let section_index_size: u32 = 4 + self.header.sections.iter()
            .map(|s| 4 + s.name.len() as u32 + 4 + 4 + 4 + 1)
            .sum::<u32>();
        let mut cur_off = base_header_size + section_index_size;

        // Write header
        w.write_all(MAGIC)?;
        write_u32_be(w, VERSION_V3)?;
        write_string(w, &self.header.target_triple)?;

        // Write section index
        write_u32_be(w, self.header.sections.len() as u32)?;
        for (i, sec) in self.header.sections.iter().enumerate() {
            write_string(w, &sec.name)?;
            write_u32_be(w, cur_off)?;
            write_u32_be(w, sec.size)?;
            write_u32_be(w, sec.raw_size)?;
            w.write_all(&[if sec.compressed { 1u8 } else { 0u8 }])?;
            cur_off += sections[i].1.len() as u32;
        }

        // Write section data
        for (_, data) in &sections {
            w.write_all(data)?;
        }
        w.flush()?;
        Ok(())
    }

    pub fn parse(data: &[u8]) -> io::Result<Self> {
        let mut r = io::Cursor::new(data);
        let mut magic = [0u8; 5];
        r.read_exact(&mut magic)?;
        if &magic != MAGIC {
            return Err(io::Error::new(io::ErrorKind::InvalidData, "Invalid magic"));
        }
        let version = read_u32_be(&mut r)?;
        if version != VERSION_V3 {
            return Err(io::Error::new(io::ErrorKind::InvalidData, format!("Expected v3, got v{}", version)));
        }
        let target_triple = read_string(&mut r)?;

        let num_sections = read_u32_be(&mut r)?;
        let mut sections = Vec::with_capacity(num_sections as usize);
        let mut section_data = HashMap::new();
        for _ in 0..num_sections {
            let name = read_string(&mut r)?;
            let offset = read_u32_be(&mut r)?;
            let size = read_u32_be(&mut r)?;
            let raw_size = read_u32_be(&mut r)?;
            let flags = read_u8(&mut r)?;
            let compressed = (flags & 1) != 0;
            sections.push(SectionIndex { name: name.clone(), offset, size, raw_size, compressed });
        }

        for sec in &sections {
            let end = (sec.offset + sec.size) as usize;
            if end > data.len() {
                return Err(io::Error::new(io::ErrorKind::InvalidData, "Section data truncated"));
            }
            section_data.insert(sec.name.clone(), data[sec.offset as usize..end].to_vec());
        }

        Ok(Self {
            header: VxObjV3Header { version, target_triple, sections },
            section_data,
        })
    }
}

// ==================== V2 Backward Compat ====================

#[derive(Debug, Clone)]
pub enum ConstantValue {
    Nil,
    Bool(bool),
    Int(i64),
    Float(f64),
    String(String),
}

pub fn read_constant(r: &mut dyn Read) -> io::Result<ConstantValue> {
    let tag = read_u8(r)?;
    match tag {
        0 => Ok(ConstantValue::Nil),
        1 => Ok(ConstantValue::Int(read_u64_be(r)? as i64)),
        2 => Ok(ConstantValue::Float(read_f64_be(r)?)),
        3 => Ok(ConstantValue::String(read_string(r)?)),
        4 => Ok(ConstantValue::Bool(read_u8(r)? != 0)),
        _ => Err(io::Error::new(io::ErrorKind::InvalidData, format!("Unknown constant tag: {}", tag))),
    }
}

pub fn write_constant_value(w: &mut dyn Write, c: &ConstantValue) -> io::Result<()> {
    match c {
        ConstantValue::Nil => w.write_all(&[0]),
        ConstantValue::Bool(b) => { w.write_all(&[4])?; w.write_all(&[if *b { 1 } else { 0 }]) }
        ConstantValue::Int(v) => { w.write_all(&[1])?; w.write_all(&v.to_be_bytes()) }
        ConstantValue::Float(v) => { w.write_all(&[2])?; w.write_all(&v.to_be_bytes()) }
        ConstantValue::String(s) => { w.write_all(&[3])?; write_string(w, s) }
    }
}

// ==================== Low-Level I/O ====================

fn write_u32_be(w: &mut dyn Write, v: u32) -> io::Result<()> {
    w.write_all(&v.to_be_bytes())
}

fn write_i32_be(w: &mut dyn Write, v: i32) -> io::Result<()> {
    w.write_all(&v.to_be_bytes())
}

fn write_string(w: &mut dyn Write, s: &str) -> io::Result<()> {
    let bytes = s.as_bytes();
    write_u32_be(w, bytes.len() as u32)?;
    w.write_all(bytes)
}

fn read_u8(r: &mut dyn Read) -> io::Result<u8> {
    let mut buf = [0u8; 1];
    r.read_exact(&mut buf)?;
    Ok(buf[0])
}

fn read_u32_be(r: &mut dyn Read) -> io::Result<u32> {
    let mut buf = [0u8; 4];
    r.read_exact(&mut buf)?;
    Ok(u32::from_be_bytes(buf))
}

fn read_i32_be(r: &mut dyn Read) -> io::Result<i32> {
    read_u32_be(r).map(|v| v as i32)
}

fn read_u64_be(r: &mut dyn Read) -> io::Result<u64> {
    let mut buf = [0u8; 8];
    r.read_exact(&mut buf)?;
    Ok(u64::from_be_bytes(buf))
}

fn read_f64_be(r: &mut dyn Read) -> io::Result<f64> {
    let bits = read_u64_be(r)?;
    Ok(f64::from_bits(bits))
}

fn read_string(r: &mut dyn Read) -> io::Result<String> {
    let len = read_u32_be(r)? as usize;
    let mut buf = vec![0u8; len];
    r.read_exact(&mut buf)?;
    Ok(String::from_utf8_lossy(&buf).to_string())
}

// ==================== V2 Parsing (Backward Compat) ====================

pub fn parse_vxobj(data: &[u8]) -> io::Result<VxObjModule> {
    use std::io::Cursor;
    let mut r = Cursor::new(data);
    let mut magic = [0u8; 5];
    r.read_exact(&mut magic)?;
    if &magic != MAGIC {
        return Err(io::Error::new(io::ErrorKind::InvalidData, "Invalid magic"));
    }
    let version = read_u32_be(&mut r)?;

    if version == VERSION_V3 {
        return parse_v3_as_module(data);
    }
    if version != VERSION_V2 {
        return Err(io::Error::new(io::ErrorKind::InvalidData, format!("Unsupported version: {}", version)));
    }

    let num_constants = read_u32_be(&mut r)?;
    let mut constants = Vec::with_capacity(num_constants as usize);
    for _ in 0..num_constants {
        let tag = read_u8(&mut r)?;
        let data = match tag {
            0 => vec![],
            1 => {
                let mut buf = [0u8; 8];
                r.read_exact(&mut buf)?;
                buf.to_vec()
            }
            2 => {
                let mut buf = [0u8; 8];
                r.read_exact(&mut buf)?;
                buf.to_vec()
            }
            3 => {
                let len = read_u32_be(&mut r)?;
                let mut buf = vec![0u8; len as usize];
                r.read_exact(&mut buf)?;
                buf
            }
            4 => {
                let mut buf = [0u8; 1];
                r.read_exact(&mut buf)?;
                buf.to_vec()
            }
            _ => vec![],
        };
        constants.push(SerializedConstant { tag, data });
    }

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
        let num_local = read_u32_be(&mut r)?;
        for _ in 0..num_local {
            let _ = read_constant(&mut r)?;
        }
        let num_insts = read_u32_be(&mut r)?;
        let mut instructions = Vec::with_capacity(num_insts as usize);
        for _ in 0..num_insts {
            let op = read_u8(&mut r)?;
            let arg_type = read_u8(&mut r)?;
            let (iarg, sarg) = match arg_type {
                0 => (None, None),
                1 => (Some(read_i32_be(&mut r)?), None),
                2 => (None, Some(read_string(&mut r)?)),
                _ => return Err(io::Error::new(io::ErrorKind::InvalidData, format!("Unknown arg type: {}", arg_type))),
            };
            instructions.push(VxObjInstruction { op, arg_type, iarg, sarg });
        }
        functions.push(VxObjFunction { name, num_params, has_return, param_names, instructions });
    }

    let mut struct_defs = HashMap::new();
    if data.len() - (r.position() as usize) >= 4 {
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

    Ok(VxObjModule { version, target_triple: String::new(), constants, functions, struct_defs })
}

fn parse_v3_as_module(data: &[u8]) -> io::Result<VxObjModule> {
    let container = VxObjV3Container::parse(data)?;
    let target_triple = container.header.target_triple.clone();
    let mut module = VxObjModule {
        version: VERSION_V3,
        target_triple,
        constants: Vec::new(),
        functions: Vec::new(),
        struct_defs: HashMap::new(),
    };

    // Deserialize Bytecode section (backward compat with v2-style data)
    if let Some(bytecode_data) = container.get_section(SECTION_BYTECODE) {
        let parsed = parse_v2_raw(&bytecode_data)?;
        module.constants = parsed.constants;
        module.functions = parsed.functions;
        module.struct_defs = parsed.struct_defs;
    }

    Ok(module)
}

fn parse_v2_raw(data: &[u8]) -> io::Result<VxObjModule> {
    let mut r = io::Cursor::new(data);
    // Skip magic (5 bytes) + version (4 bytes) if present
    if data.len() >= 9 && &data[..5] == MAGIC {
        let mut dummy = [0u8; 9];
        r.read_exact(&mut dummy)?;
    }
    let num_constants = read_u32_be(&mut r)?;
    let mut constants = Vec::with_capacity(num_constants as usize);
    for _ in 0..num_constants {
        let tag = read_u8(&mut r)?;
        match tag {
            0 => constants.push(SerializedConstant::nil()),
            1 => constants.push(SerializedConstant::int(read_u64_be(&mut r)? as i64)),
            2 => constants.push(SerializedConstant::float(read_f64_be(&mut r)?)),
            3 => constants.push(SerializedConstant::string(&read_string(&mut r)?)),
            4 => constants.push(SerializedConstant::bool(read_u8(&mut r)? != 0)),
            _ => return Err(io::Error::new(io::ErrorKind::InvalidData, format!("Unknown tag: {}", tag))),
        }
    }
    let num_functions = read_u32_be(&mut r)?;
    let mut functions = Vec::with_capacity(num_functions as usize);
    for _ in 0..num_functions {
        let name = read_string(&mut r)?;
        let num_params = read_u32_be(&mut r)?;
        let has_return = read_u8(&mut r)? != 0;
        let num_pn = read_u32_be(&mut r)?;
        let mut param_names = Vec::with_capacity(num_pn as usize);
        for _ in 0..num_pn {
            param_names.push(read_string(&mut r)?);
        }
        let num_local = read_u32_be(&mut r)?;
        for _ in 0..num_local {
            let tag = read_u8(&mut r)?;
            match tag {
                0 => {}
                1 => { let _ = read_u64_be(&mut r)?; }
                2 => { let _ = read_f64_be(&mut r)?; }
                3 => { let _ = read_string(&mut r)?; }
                4 => { let _ = read_u8(&mut r)?; }
                _ => {}
            }
        }
        let num_insts = read_u32_be(&mut r)?;
        let mut instructions = Vec::with_capacity(num_insts as usize);
        for _ in 0..num_insts {
            let op = read_u8(&mut r)?;
            let arg_type = read_u8(&mut r)?;
            let (iarg, sarg) = match arg_type {
                0 => (None, None),
                1 => (Some(read_i32_be(&mut r)?), None),
                2 => (None, Some(read_string(&mut r)?)),
                _ => return Err(io::Error::new(io::ErrorKind::InvalidData, format!("Unknown arg type: {}", arg_type))),
            };
            instructions.push(VxObjInstruction { op, arg_type, iarg, sarg });
        }
        functions.push(VxObjFunction { name, num_params, has_return, param_names, instructions });
    }
    let mut struct_defs = HashMap::new();
    if data.len() - (r.position() as usize) >= 4 {
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
    Ok(VxObjModule { version: VERSION_V3, target_triple: String::new(), constants, functions, struct_defs })
}

// ==================== V2 Writer (Backward Compat) ====================

pub fn write_vxobj(
    w: &mut dyn Write,
    constants: &[SerializedConstant],
    functions: &[VxobjFunctionRef<'_>],
    struct_defs: &HashMap<String, Vec<String>>,
) -> io::Result<()> {
    w.write_all(MAGIC)?;
    write_u32_be(w, VERSION_V2)?;
    write_u32_be(w, constants.len() as u32)?;
    for c in constants {
        w.write_all(&[c.tag])?;
        match c.tag {
            1 => { w.write_all(&c.data)?; }
            2 => { w.write_all(&c.data)?; }
            3 => { write_u32_be(w, c.data.len() as u32)?; w.write_all(&c.data)?; }
            4 => w.write_all(&c.data)?,
            _ => {}
        }
    }
    write_u32_be(w, functions.len() as u32)?;
    for (name, num_params, has_return, param_names, insts) in functions {
        write_string(w, name)?;
        write_u32_be(w, *num_params)?;
        w.write_all(&[if *has_return { 1 } else { 0 }])?;
        write_u32_be(w, param_names.len() as u32)?;
        for pn in *param_names {
            write_string(w, pn)?;
        }
        write_u32_be(w, 0)?;
        write_u32_be(w, insts.len() as u32)?;
        for (op, arg_type, iarg, sarg) in *insts {
            w.write_all(&[*op])?;
            w.write_all(&[*arg_type])?;
            match arg_type {
                1 => { if let Some(v) = iarg { write_i32_be(w, *v)?; } }
                2 => { if let Some(ref s) = sarg { write_string(w, s)?; } }
                _ => {}
            }
        }
    }
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

/// 直接从 `&CompiledModule` 写入 VXOBJ v2，跳过中间 tuple 缓冲。
///
/// 等价于先调用 `convert_module_to_vxobj_parts` 然后 `write_vxobj`，
/// 但避免了在调用方构造临时 `Vec` 的两次内存分配。
pub fn write_vxobj_from_module(
    w: &mut dyn Write,
    module: &crate::compiler_bytecode::CompiledModule,
) -> io::Result<()> {
    use crate::compiler_bytecode::{BytecodeArg, ConstantValue};

    // 1. 转换常量
    let constants: Vec<SerializedConstant> = module
        .constants
        .iter()
        .map(|c| match c {
            ConstantValue::Nil => SerializedConstant::nil(),
            ConstantValue::Bool(b) => SerializedConstant::bool(*b),
            ConstantValue::Int(v) => SerializedConstant::int(*v),
            ConstantValue::Float(v) => SerializedConstant::float(*v),
            ConstantValue::String(s) => SerializedConstant::string(s),
        })
        .collect();

    // 2. 合并 struct / class 定义
    let mut struct_defs: HashMap<String, Vec<String>> = HashMap::with_capacity(
        module.structs.len() + module.classes.len(),
    );
    for (n, f) in &module.structs {
        struct_defs.insert(n.clone(), f.clone());
    }
    for (n, f) in &module.classes {
        struct_defs.insert(n.clone(), f.clone());
    }

    // 3. 转换函数指令为 SerializedConstant + 函数元组
    let functions: Vec<VxobjFunctionRefData> = module
        .functions
        .iter()
        .map(|f| {
            // 对每条指令把 BytecodeArg 转成 (op, arg_type, iarg, sarg) 元组
            // 重要: 这是 owned Vec（不是借用），因为 ImportTuple 需要拼接字符串
            let insts: Vec<(u8, u8, Option<i32>, Option<String>)> = f
                .instructions
                .iter()
                .map(|inst| {
                    let (arg_type, iarg, sarg) = match &inst.arg {
                        BytecodeArg::None => (0u8, None, None),
                        BytecodeArg::Int(v) => (1u8, Some(*v), None),
                        BytecodeArg::String(s) => (2u8, None, Some(s.clone())),
                        BytecodeArg::ImportTuple(a, b, c) => {
                            let s = format!(
                                "{},{},{}",
                                b.as_deref().unwrap_or(""),
                                c.as_deref().unwrap_or(""),
                                a,
                            );
                            (2u8, None, Some(s))
                        }
                    };
                    (inst.op as u8, arg_type, iarg, sarg)
                })
                .collect();
            VxobjFunctionRefData {
                name: f.name.clone(),
                num_params: f.num_params as u32,
                has_return: f.has_return,
                param_names: f.param_names.clone(),
                insts,
            }
        })
        .collect();

    // 4. 构造借用切片元组
    let func_refs: Vec<VxobjFunctionRef<'_>> = functions
        .iter()
        .map(|fd| {
            (
                fd.name.as_str(),
                fd.num_params,
                fd.has_return,
                fd.param_names.as_slice(),
                fd.insts.as_slice(),
            )
        })
        .collect();

    write_vxobj(w, &constants, &func_refs, &struct_defs)
}

/// 内部数据结构：用于 `write_vxobj_from_module` 持有 owned 数据，
/// 再借用为 `VxobjFunctionRef` 元组传入 `write_vxobj`。
struct VxobjFunctionRefData {
    name: String,
    num_params: u32,
    has_return: bool,
    param_names: Vec<String>,
    insts: Vec<(u8, u8, Option<i32>, Option<String>)>,
}

// ==================== V3 Writer ====================

pub fn write_vxobj_v3(
    w: &mut dyn Write,
    target_triple: &str,
    type_ir_data: &[u8],
    bytecode_data: &[u8],
    debug_data: &[u8],
    source_map_data: &[u8],
    type_meta_data: &[u8],
) -> io::Result<()> {
    let mut container = VxObjV3Container::new(target_triple);
    if !type_ir_data.is_empty() {
        container.set_section(SECTION_TYPE_IR, type_ir_data.to_vec(), true);
    }
    if !bytecode_data.is_empty() {
        container.set_section(SECTION_BYTECODE, bytecode_data.to_vec(), true);
    }
    if !debug_data.is_empty() {
        container.set_section(SECTION_DEBUG, debug_data.to_vec(), true);
    }
    if !source_map_data.is_empty() {
        container.set_section(SECTION_SOURCE_MAP, source_map_data.to_vec(), true);
    }
    if !type_meta_data.is_empty() {
        container.set_section(SECTION_TYPE_META, type_meta_data.to_vec(), true);
    }
    container.write(w)
}

// ==================== VXCO Writer (Cross-Platform Intermediate Format) ====================

pub fn write_vxco(
    w: &mut dyn Write,
    target_triple: &str,
    type_ir_data: &[u8],
    debug_data: &[u8],
    source_map_data: &[u8],
    external_deps: &[ExternalDependency],
) -> io::Result<()> {
    let mut container = VxcoContainer::new(target_triple);

    if !type_ir_data.is_empty() {
        container.set_section(VXCO_SECTION_TYPE_IR, type_ir_data.to_vec());
    }
    if !debug_data.is_empty() {
        container.set_section(VXCO_SECTION_DEBUG, debug_data.to_vec());
    }
    if !source_map_data.is_empty() {
        container.set_section(VXCO_SECTION_SOURCE_MAP, source_map_data.to_vec());
    }
    if !external_deps.is_empty() {
        container.set_section(VXCO_SECTION_EXTERNAL_DEPS, serialize_external_deps(external_deps));
        container.set_external_deps_flag(true);
    }

    container.write(w)
}

// ==================== Section Size Stats ====================

pub fn dump_section_stats(data: &[u8]) {
    match VxObjV3Container::parse(data) {
        Ok(container) => {
            println!("VXOBJ v3 container:");
            println!("  Target: {}", container.header.target_triple);
            println!("  Sections:");
            for sec in &container.header.sections {
                let label = if sec.compressed {
                    format!("{} (compressed)", sec.size)
                } else {
                    sec.size.to_string()
                };
                let raw = if sec.raw_size > 0 {
                    format!(" -> {} raw", sec.raw_size)
                } else {
                    String::new()
                };
                println!("    {:12} {} bytes{}{}", sec.name, label, raw,
                    if sec.compressed && sec.raw_size > 0 {
                        format!(" ({:.1}%)", sec.size as f64 / sec.raw_size as f64 * 100.0)
                    } else { String::new() });
            }
        }
        Err(_) => {
            println!("Not a v3 container (trying v2)...");
            match parse_vxobj(data) {
                Ok(m) => {
                    println!("VXOBJ v2 module: {} functions, {} constants", m.functions.len(), m.constants.len());
                }
                Err(e) => {
                    println!("Parse error: {}", e);
                }
            }
        }
    }
}

// ==================== Tests ====================

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_v3_container_roundtrip() {
        let mut container = VxObjV3Container::new("x86_64-unknown-linux-gnu");
        container.set_section(SECTION_TYPE_IR, vec![1, 2, 3, 4], false);
        container.set_section(SECTION_BYTECODE, vec![10, 20, 30], true);
        let mut buf = Vec::new();
        container.write(&mut buf).unwrap();
        let parsed = VxObjV3Container::parse(&buf).unwrap();
        assert_eq!(parsed.header.target_triple, "x86_64-unknown-linux-gnu");
        assert_eq!(parsed.header.sections.len(), 2);
        assert_eq!(parsed.get_section(SECTION_TYPE_IR).unwrap(), vec![1, 2, 3, 4]);
        assert_eq!(parsed.get_section(SECTION_BYTECODE).unwrap(), vec![10, 20, 30]);
    }

    #[test]
    fn test_v2_backward_compat() {
        let constants = vec![
            SerializedConstant::nil(),
            SerializedConstant::int(42),
            SerializedConstant::string("hello"),
        ];
        let instructions = vec![(0x01, 1, Some(1), None), (0x09, 0, None, None)];
        let functions: Vec<VxobjFunctionRef<'_>> =
            vec![("__main__", 0u32, false, &[], &instructions)];
        let struct_defs = HashMap::new();
        let mut buf = Vec::new();
        write_vxobj(&mut buf, &constants, &functions, &struct_defs).unwrap();
        let parsed = parse_vxobj(&buf).unwrap();
        assert_eq!(parsed.version, 2);
        assert_eq!(parsed.functions[0].name, "__main__");
    }

    #[test]
    fn test_v3_contains_v2_data() {
        let mut bytecode_buf = Vec::new();
        let constants = vec![SerializedConstant::int(99)];
        let insts: Vec<(u8, u8, Option<i32>, Option<String>)> = vec![(0x01, 1, Some(0), None), (0x09, 0, None, None)];
        let functions: Vec<VxobjFunctionRef<'_>> =
            vec![("test", 0u32, false, &[] as &[String], insts.as_slice())];
        let struct_defs = HashMap::new();
        write_vxobj(&mut bytecode_buf, &constants, &functions, &struct_defs).unwrap();

        let mut buf = Vec::new();
        write_vxobj_v3(&mut buf, "x86_64-unknown-linux-gnu", &[], &bytecode_buf, &[], &[], &[]).unwrap();
        let parsed = parse_vxobj(&buf).unwrap();
        assert_eq!(parsed.version, 3);
        assert_eq!(parsed.functions.len(), 1);
        assert_eq!(parsed.functions[0].name, "test");
    }
}
