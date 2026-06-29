use std::collections::HashMap;
use std::io::{self, Read, Write};

// ==================== VXOBJ v4 Format ====================
//
// VXOBJ v4: 跨平台中间文件格式，不包含任何可执行文件特征。
// 编译器输出 VXOBJ v4，链接器解析后生成目标平台的原生可执行文件。
//
// 格式结构:
//   [Header]
//     5 bytes magic: "VXOBJ"
//     4 bytes version (u32 BE): 4
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

const MAGIC: &[u8; 5] = b"VXOBJ";
const VERSION_V4: u32 = 4;

// VXOBJ v4 Section names
pub const SECTION_TYPE_IR: &str = "TypeIR";
pub const SECTION_DEBUG: &str = "Debug";
pub const SECTION_SOURCE_MAP: &str = "SourceMap";
pub const SECTION_EXTERNAL_DEPS: &str = "ExternalDeps";

#[derive(Debug, Clone)]
pub struct VxObjV4Header {
    pub version: u32,
    pub flags: u32,
    pub target_triple: String,
    pub sections: Vec<VxObjV4SectionIndex>,
}

#[derive(Debug, Clone)]
pub struct VxObjV4SectionIndex {
    pub name: String,
    pub offset: u32,
    pub size: u32,
}

#[derive(Debug, Clone)]
pub struct VxObjV4Container {
    pub header: VxObjV4Header,
    pub section_data: HashMap<String, Vec<u8>>,
}

impl VxObjV4Container {
    pub fn new(target_triple: &str) -> Self {
        Self {
            header: VxObjV4Header {
                version: VERSION_V4,
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
        self.header.sections.push(VxObjV4SectionIndex {
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

        let base_header_size = 5 + 4 + 4 + 4 + self.header.target_triple.len() as u32;
        let section_index_size: u32 = 4 + self.header.sections.iter()
            .map(|s| 4 + s.name.len() as u32 + 4 + 4)
            .sum::<u32>();
        let mut cur_off = base_header_size + section_index_size;

        // Write header
        w.write_all(MAGIC)?;
        write_u32_be(w, VERSION_V4)?;
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
        let mut magic = [0u8; 5];
        r.read_exact(&mut magic)?;
        if &magic != MAGIC {
            return Err(io::Error::new(io::ErrorKind::InvalidData, "Invalid VXOBJ magic"));
        }

        let version = read_u32_be(&mut r)?;
        if version != VERSION_V4 {
            return Err(io::Error::new(io::ErrorKind::InvalidData, format!("Unsupported VXOBJ version: {}", version)));
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
                return Err(io::Error::new(io::ErrorKind::InvalidData, "VXOBJ section data truncated"));
            }
            section_data.insert(name.clone(), data[offset as usize..end].to_vec());
            sections.push(VxObjV4SectionIndex { name, offset, size });
        }

        Ok(Self {
            header: VxObjV4Header { version, flags, target_triple, sections },
            section_data,
        })
    }
}

// ==================== External Dependencies ====================
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

    pub fn to_bytes(&self) -> Vec<u8> {
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

    pub fn from_bytes(data: &[u8]) -> Option<Self> {
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
        let name_end = data[pos..].iter().position(|&b| b == 0).unwrap_or(data.len() - pos);
        let name = String::from_utf8_lossy(&data[pos..pos + name_end]).to_string();
        pos += name_end + 1;

        if pos >= data.len() {
            break;
        }

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

// ==================== VXOBJ v4 Writer ====================

pub fn write_vxobj_v4(
    w: &mut dyn Write,
    target_triple: &str,
    type_ir_data: &[u8],
    debug_data: &[u8],
    source_map_data: &[u8],
    external_deps: &[ExternalDependency],
) -> io::Result<()> {
    let mut container = VxObjV4Container::new(target_triple);

    if !type_ir_data.is_empty() {
        container.set_section(SECTION_TYPE_IR, type_ir_data.to_vec());
    }
    if !debug_data.is_empty() {
        container.set_section(SECTION_DEBUG, debug_data.to_vec());
    }
    if !source_map_data.is_empty() {
        container.set_section(SECTION_SOURCE_MAP, source_map_data.to_vec());
    }
    if !external_deps.is_empty() {
        container.set_section(SECTION_EXTERNAL_DEPS, serialize_external_deps(external_deps));
        container.set_external_deps_flag(true);
    }

    container.write(w)
}

// ==================== Section Size Stats ====================

pub fn dump_section_stats(data: &[u8]) {
    match VxObjV4Container::parse(data) {
        Ok(container) => {
            println!("VXOBJ v4 container:");
            println!("  Target: {}", container.header.target_triple);
            println!("  Sections:");
            for sec in &container.header.sections {
                println!("    {:12} {} bytes", sec.name, sec.size);
            }
        }
        Err(e) => {
            println!("Parse error: {}", e);
        }
    }
}

// ==================== Low-Level I/O ====================

fn write_u32_be(w: &mut dyn Write, v: u32) -> io::Result<()> {
    w.write_all(&v.to_be_bytes())
}

fn write_string(w: &mut dyn Write, s: &str) -> io::Result<()> {
    let bytes = s.as_bytes();
    write_u32_be(w, bytes.len() as u32)?;
    w.write_all(bytes)
}

fn read_u32_be(r: &mut dyn Read) -> io::Result<u32> {
    let mut buf = [0u8; 4];
    r.read_exact(&mut buf)?;
    Ok(u32::from_be_bytes(buf))
}

fn read_string(r: &mut dyn Read) -> io::Result<String> {
    let len = read_u32_be(r)? as usize;
    let mut buf = vec![0u8; len];
    r.read_exact(&mut buf)?;
    Ok(String::from_utf8_lossy(&buf).to_string())
}
