// ==================== TypeIR: 类型化中间表示 ====================
// 供 AOT 编译器消费，保留完整类型信息
//
// 设计原则:
// - 所有操作数都有静态已知类型
// - 所有函数调用点明确目标
// - 保留所有权/借用信息供自动并行化

use std::collections::HashMap;

// ==================== 类型系统 ====================

#[derive(Debug, Clone, PartialEq, Hash, Eq)]
pub enum Type {
    Void,
    Int,
    Float,
    Bool,
    String,
    Struct(String, Vec<(String, Type)>),
    Array(Box<Type>),
    Map(Box<Type>, Box<Type>),
    Func(Vec<Type>, Box<Type>),
    Pointer(Box<Type>),
    Generic(String, Vec<Type>),
    Unknown,
}

impl Type {
    pub fn is_numeric(&self) -> bool {
        matches!(self, Type::Int | Type::Float)
    }
    pub fn is_integral(&self) -> bool {
        matches!(self, Type::Int | Type::Bool)
    }
    pub fn size(&self) -> usize {
        match self {
            Type::Void => 0,
            Type::Int | Type::Float | Type::Bool => 8,
            Type::String => 16,
            Type::Pointer(_) => 8,
            _ => 8,
        }
    }
}

// ==================== Type-annotated Value ====================

#[derive(Debug, Clone)]
pub enum TypeValue {
    Int(i64),
    Float(f64),
    Bool(bool),
    String(String),
}

// ==================== Typed Instructions ====================

pub type VarId = u32;
pub type FuncId = u32;

#[derive(Debug, Clone)]
pub enum TypedInstruction {
    // Constants
    ConstInt(i64),
    ConstFloat(f64),
    ConstBool(bool),
    ConstString(String),
    ConstNil,

    // Variables
    LoadVar(VarId),
    StoreVar(VarId),

    // Arithmetic (typed)
    I32Add(VarId, VarId),
    I32Sub(VarId, VarId),
    I32Mul(VarId, VarId),
    I32Div(VarId, VarId),
    I32Mod(VarId, VarId),
    F64Add(VarId, VarId),
    F64Sub(VarId, VarId),
    F64Mul(VarId, VarId),
    F64Div(VarId, VarId),

    // Comparison (typed)
    I32Eq(VarId, VarId),
    I32Ne(VarId, VarId),
    I32Lt(VarId, VarId),
    I32Gt(VarId, VarId),
    I32Le(VarId, VarId),
    I32Ge(VarId, VarId),
    F64Eq(VarId, VarId),
    F64Ne(VarId, VarId),
    F64Lt(VarId, VarId),
    F64Gt(VarId, VarId),
    F64Le(VarId, VarId),
    F64Ge(VarId, VarId),

    // Unary
    I32Neg(VarId),
    F64Neg(VarId),
    BoolNot(VarId),

    // Bitwise / logical
    I32And(VarId, VarId),
    I32Or(VarId, VarId),

    // Control flow
    Jump(u32),
    JumpIfFalse(VarId, u32),
    JumpIfTrue(VarId, u32),

    // Functions
    // Call 增加可选的外部函数名：FuncId == u32::MAX 且未在模块 linkage 中登记时使用
    Call(FuncId, Vec<VarId>, Option<String>),
    CallIndirect(VarId, Vec<VarId>),
    Return(Option<VarId>),

    // Data structures
    MakeStruct(StructLayoutId, Vec<VarId>),
    GetField(VarId, u32),
    SetField(VarId, u32, VarId),
    MakeArray(VarId, Vec<VarId>),
    IndexGet(VarId, VarId),
    IndexSet(VarId, VarId, VarId),
    MakeMap(Vec<(VarId, VarId)>),

    // Memory / Ownership
    Alloc(Type),
    Free(VarId),
    OwnershipMove(VarId),
    Borrow(VarId),
    Deref(VarId),
    AliveCheck(VarId),

    // Stack ops
    Dup,
    Pop,
}

#[derive(Debug, Clone)]
pub struct StructLayoutId(pub u32);

// ==================== Type IR Function ====================

#[derive(Debug, Clone)]
pub struct TypeFunction {
    pub name: String,
    pub id: FuncId,
    pub params: Vec<(String, Type)>,
    pub return_type: Type,
    pub body: Vec<TypedInstruction>,
    pub local_types: HashMap<VarId, Type>,
    pub param_count: u32,
    pub has_return: bool,
    pub var_count: u32,
}

impl TypeFunction {
    pub fn new(name: &str, id: FuncId) -> Self {
        Self {
            name: name.to_string(),
            id,
            params: Vec::new(),
            return_type: Type::Void,
            body: Vec::new(),
            local_types: HashMap::new(),
            param_count: 0,
            has_return: false,
            var_count: 0,
        }
    }

    pub fn add_local(&mut self, ty: Type) -> VarId {
        let id = self.var_count;
        self.local_types.insert(id, ty);
        self.var_count += 1;
        id
    }

    pub fn get_type(&self, var: VarId) -> Option<&Type> {
        self.local_types.get(&var)
    }
}

// ==================== Type IR Module ====================

#[derive(Debug, Clone)]
pub enum Linkage {
    Internal,
    External(String),
}

#[derive(Debug, Clone, Default)]
pub struct TypeModule {
    pub functions: Vec<TypeFunction>,
    pub struct_layouts: Vec<(String, Vec<(String, Type)>)>,
    pub function_map: HashMap<FuncId, String>,
    pub linkage: HashMap<FuncId, Linkage>,
    pub entry_point: Option<FuncId>,
}

impl TypeModule {
    pub fn new() -> Self {
        Self::default()
    }

    pub fn get_function(&self, id: FuncId) -> Option<&TypeFunction> {
        self.functions.iter().find(|f| f.id == id)
    }

    pub fn get_function_id(&self, name: &str) -> Option<FuncId> {
        self.function_map.iter().find(|(_, v)| *v == name).map(|(k, _)| *k)
    }

    pub fn add_struct_layout(&mut self, name: &str, fields: Vec<(String, Type)>) -> StructLayoutId {
        let id = self.struct_layouts.len() as u32;
        self.struct_layouts.push((name.to_string(), fields));
        StructLayoutId(id)
    }
}

// ==================== Serialization ====================

pub fn serialize_type_module(module: &TypeModule) -> Vec<u8> {
    let mut buf = Vec::new();
    // Counts
    buf.extend(&(module.functions.len() as u32).to_be_bytes());
    buf.extend(&(module.struct_layouts.len() as u32).to_be_bytes());
    // Struct layouts
    for (name, fields) in &module.struct_layouts {
        write_str(&mut buf, name);
        buf.extend(&(fields.len() as u32).to_be_bytes());
        for (fname, ftype) in fields {
            write_str(&mut buf, fname);
            serialize_type(&mut buf, ftype);
        }
    }
    // Functions
    for func in &module.functions {
        write_str(&mut buf, &func.name);
            buf.extend(&func.id.to_be_bytes());
            buf.extend(&(func.params.len() as u32).to_be_bytes());
        buf.extend(&[if func.has_return { 1u8 } else { 0u8 }]);
        serialize_type(&mut buf, &func.return_type);
        for (pname, ptype) in &func.params {
            write_str(&mut buf, pname);
            serialize_type(&mut buf, ptype);
        }
        // var_count + local_types (VXOBJ v4)
        buf.extend(&func.var_count.to_be_bytes());
        buf.extend(&(func.local_types.len() as u32).to_be_bytes());
        for (vid, vty) in &func.local_types {
            buf.extend(&vid.to_be_bytes());
            serialize_type(&mut buf, vty);
        }
        buf.extend(&(func.body.len() as u32).to_be_bytes());
        for inst in &func.body {
            serialize_instruction(&mut buf, inst);
        }
    }
    // Linkage table (external symbols like built-ins)
    buf.extend(&(module.linkage.len() as u32).to_be_bytes());
    for (func_id, linkage) in &module.linkage {
        buf.extend(&func_id.to_be_bytes());
        match linkage {
            Linkage::Internal => buf.push(0),
            Linkage::External(name) => {
                buf.push(1);
                write_str(&mut buf, name);
            }
        }
    }
    buf
}

pub fn deserialize_type_module(data: &[u8]) -> Option<TypeModule> {
    deserialize_type_module_result(data).ok()
}

pub fn deserialize_type_module_result(data: &[u8]) -> Result<TypeModule, String> {
    let mut pos = 0;
    if data.len() < 8 { return Err("data too short".into()); }
    let num_funcs = read_u32_be_at(data, &mut pos).ok_or("failed to read num_funcs")?;
    let num_layouts = read_u32_be_at(data, &mut pos).ok_or("failed to read num_layouts")?;
    let mut module = TypeModule::new();
    for _ in 0..num_layouts {
        let name = read_str_at(data, &mut pos).ok_or("failed to read layout name")?;
        let num_fields = read_u32_be_at(data, &mut pos).ok_or("failed to read num_fields")? as usize;
        let mut fields = Vec::with_capacity(num_fields);
        for _ in 0..num_fields {
            let fname = read_str_at(data, &mut pos).ok_or("failed to read field name")?;
            let ftype = deserialize_type(data, &mut pos).ok_or("failed to deserialize field type")?;
            fields.push((fname, ftype));
        }
        module.struct_layouts.push((name, fields));
    }
    for _ in 0..num_funcs {
        let name = read_str_at(data, &mut pos).ok_or("failed to read func name")?;
        let id = read_u32_be_at(data, &mut pos).ok_or("failed to read func id")?;
        let param_count = read_u32_be_at(data, &mut pos).ok_or("failed to read param_count")?;
        let has_return = data.get(pos).copied().ok_or("missing has_return byte")? != 0;
        pos += 1;
        let return_type = deserialize_type(data, &mut pos).ok_or("failed to deserialize return type")?;
        let mut func = TypeFunction::new(&name, id);
        func.param_count = param_count;
        func.has_return = has_return;
        func.return_type = return_type;
        for _ in 0..param_count {
            let pname = read_str_at(data, &mut pos).ok_or("failed to read param name")?;
            let ptype = deserialize_type(data, &mut pos).ok_or("failed to deserialize param type")?;
            func.params.push((pname, ptype));
        }
        // var_count + local_types (VXOBJ v4)
        let var_count = read_u32_be_at(data, &mut pos).ok_or("failed to read var_count")?;
        func.var_count = var_count;
        let num_local_types = read_u32_be_at(data, &mut pos).ok_or("failed to read num_local_types")? as usize;
        for _ in 0..num_local_types {
            let vid = read_u32_be_at(data, &mut pos).ok_or("failed to read local type vid")?;
            let vty = deserialize_type(data, &mut pos).ok_or("failed to deserialize local type")?;
            func.local_types.insert(vid, vty);
        }
        let num_insts = read_u32_be_at(data, &mut pos).ok_or("failed to read num_insts")? as usize;
        for i in 0..num_insts {
            let inst = deserialize_instruction(data, &mut pos)
                .ok_or_else(|| format!("failed to deserialize instruction {} of func {} at pos {}", i, name, pos))?;
            func.body.push(inst);
        }
        module.functions.push(func);
        module.function_map.insert(id, name);
    }
    // Linkage table (optional for backward compatibility)
    if pos < data.len() {
        let num_linkages = read_u32_be_at(data, &mut pos).ok_or("failed to read num_linkages")?;
        for _ in 0..num_linkages {
            let func_id = read_u32_be_at(data, &mut pos).ok_or("failed to read linkage func_id")?;
            let tag = data.get(pos).copied().ok_or("missing linkage tag")?;
            pos += 1;
            let linkage = match tag {
                0 => Linkage::Internal,
                1 => {
                    let name = read_str_at(data, &mut pos).ok_or("failed to read linkage name")?;
                    Linkage::External(name)
                }
                _ => return Err(format!("unknown linkage tag {}", tag)),
            };
            module.linkage.insert(func_id, linkage);
        }
    }
    Ok(module)
}

fn serialize_type(buf: &mut Vec<u8>, ty: &Type) {
    match ty {
        Type::Void => buf.push(0),
        Type::Int => buf.push(1),
        Type::Float => buf.push(2),
        Type::Bool => buf.push(3),
        Type::String => buf.push(4),
        Type::Struct(name, fields) => {
            buf.push(5);
            write_str(buf, name);
            buf.extend(&(fields.len() as u32).to_be_bytes());
            for (fname, ftype) in fields {
                write_str(buf, fname);
                serialize_type(buf, ftype);
            }
        }
        Type::Array(inner) => { buf.push(6); serialize_type(buf, inner); }
        Type::Map(k, v) => { buf.push(7); serialize_type(buf, k); serialize_type(buf, v); }
        Type::Func(params, ret) => {
            buf.push(8);
            buf.extend(&(params.len() as u32).to_be_bytes());
            for p in params { serialize_type(buf, p); }
            serialize_type(buf, ret);
        }
        Type::Pointer(inner) => { buf.push(9); serialize_type(buf, inner); }
        Type::Generic(name, args) => {
            buf.push(10);
            write_str(buf, name);
            buf.extend(&(args.len() as u32).to_be_bytes());
            for a in args { serialize_type(buf, a); }
        }
        Type::Unknown => buf.push(255),
    }
}

fn deserialize_type(data: &[u8], pos: &mut usize) -> Option<Type> {
    let tag = data.get(*pos)?;
    *pos += 1;
    match tag {
        0 => Some(Type::Void),
        1 => Some(Type::Int),
        2 => Some(Type::Float),
        3 => Some(Type::Bool),
        4 => Some(Type::String),
        5 => {
            let name = read_str_at(data, pos)?;
            let len = read_u32_be_at(data, pos)? as usize;
            let mut fields = Vec::with_capacity(len);
            for _ in 0..len {
                let fname = read_str_at(data, pos)?;
                let ftype = deserialize_type(data, pos)?;
                fields.push((fname, ftype));
            }
            Some(Type::Struct(name, fields))
        }
        6 => Some(Type::Array(Box::new(deserialize_type(data, pos)?))),
        7 => Some(Type::Map(Box::new(deserialize_type(data, pos)?), Box::new(deserialize_type(data, pos)?))),
        8 => {
            let len = read_u32_be_at(data, pos)? as usize;
            let mut params = Vec::with_capacity(len);
            for _ in 0..len { params.push(deserialize_type(data, pos)?); }
            let ret = deserialize_type(data, pos)?;
            Some(Type::Func(params, Box::new(ret)))
        }
        9 => Some(Type::Pointer(Box::new(deserialize_type(data, pos)?))),
        10 => {
            let name = read_str_at(data, pos)?;
            let len = read_u32_be_at(data, pos)? as usize;
            let mut args = Vec::with_capacity(len);
            for _ in 0..len { args.push(deserialize_type(data, pos)?); }
            Some(Type::Generic(name, args))
        }
        255 => Some(Type::Unknown),
        _ => None,
    }
}

fn serialize_instruction(buf: &mut Vec<u8>, inst: &TypedInstruction) {
    use TypedInstruction::*;
    let (tag, payload) = match inst {
        ConstInt(v) => (0, Some(format!("i{}", v))),
        ConstFloat(v) => (1, Some(format!("f{}", v))),
        ConstBool(v) => (2, Some(format!("b{}", v))),
        ConstString(v) => (3, Some(format!("s{}", v))),
        ConstNil => (4, None),
        LoadVar(v) => (5, Some(format!("{}", v))),
        StoreVar(v) => (6, Some(format!("{}", v))),
        I32Add(a, b) => (10, Some(format!("{},{}", a, b))),
        I32Sub(a, b) => (11, Some(format!("{},{}", a, b))),
        I32Mul(a, b) => (12, Some(format!("{},{}", a, b))),
        I32Div(a, b) => (13, Some(format!("{},{}", a, b))),
        I32Mod(a, b) => (14, Some(format!("{},{}", a, b))),
        F64Add(a, b) => (15, Some(format!("{},{}", a, b))),
        F64Sub(a, b) => (16, Some(format!("{},{}", a, b))),
        F64Mul(a, b) => (17, Some(format!("{},{}", a, b))),
        F64Div(a, b) => (18, Some(format!("{},{}", a, b))),
        I32Eq(a, b) => (20, Some(format!("{},{}", a, b))),
        I32Ne(a, b) => (21, Some(format!("{},{}", a, b))),
        I32Lt(a, b) => (22, Some(format!("{},{}", a, b))),
        I32Gt(a, b) => (23, Some(format!("{},{}", a, b))),
        I32Le(a, b) => (24, Some(format!("{},{}", a, b))),
        I32Ge(a, b) => (25, Some(format!("{},{}", a, b))),
        F64Eq(a, b) => (26, Some(format!("{},{}", a, b))),
        F64Ne(a, b) => (27, Some(format!("{},{}", a, b))),
        F64Lt(a, b) => (28, Some(format!("{},{}", a, b))),
        F64Gt(a, b) => (29, Some(format!("{},{}", a, b))),
        F64Le(a, b) => (30, Some(format!("{},{}", a, b))),
        F64Ge(a, b) => (31, Some(format!("{},{}", a, b))),
        I32Neg(v) => (32, Some(format!("{}", v))),
        F64Neg(v) => (33, Some(format!("{}", v))),
        BoolNot(v) => (34, Some(format!("{}", v))),
        I32And(a, b) => (35, Some(format!("{},{}", a, b))),
        I32Or(a, b) => (36, Some(format!("{},{}", a, b))),
        Jump(t) => (40, Some(format!("{}", t))),
        JumpIfFalse(v, t) => (41, Some(format!("{},{}", v, t))),
        JumpIfTrue(v, t) => (42, Some(format!("{},{}", v, t))),
        Call(f, args, ext_name) => {
            let mut s = format!("f{}{}", f, args.iter().map(|a| format!(",{}", a)).collect::<String>());
            if let Some(name) = ext_name {
                s.push_str(&format!(";{}", name));
            }
            (50, Some(s))
        }
        CallIndirect(v, args) => {
            let s = format!("vi{}{}", v, args.iter().map(|a| format!(",{}", a)).collect::<String>());
            (51, Some(s))
        }
        Return(v) => match v { Some(id) => (52, Some(format!("{}", id))), None => (52, Some("".to_string())) },
        MakeStruct(id, args) => {
            let s = format!("s{}{}", id.0, args.iter().map(|a| format!(",{}", a)).collect::<String>());
            (60, Some(s))
        }
        GetField(o, idx) => (61, Some(format!("{},{}", o, idx))),
        SetField(o, idx, v) => (62, Some(format!("{},{},{}", o, idx, v))),
        MakeArray(base, args) => {
            // 格式: "<base>,<arg0>,<arg1>,..."  第一个元素为 base（基类型 VarId），其后为元素 VarId
            let mut parts = Vec::with_capacity(args.len() + 1);
            parts.push(base.to_string());
            for a in args {
                parts.push(a.to_string());
            }
            (63, Some(parts.join(",")))
        }
        IndexGet(a, i) => (64, Some(format!("{},{}", a, i))),
        IndexSet(a, i, v) => (65, Some(format!("{},{},{}", a, i, v))),
        MakeMap(pairs) => {
            // 格式: "<k0>,<v0>,<k1>,<v1>,..."  按键值对顺序
            let mut parts = Vec::with_capacity(pairs.len() * 2);
            for (k, v) in pairs {
                parts.push(k.to_string());
                parts.push(v.to_string());
            }
            (66, Some(parts.join(",")))
        }
        Alloc(_) => (70, None),
        Free(v) => (71, Some(format!("{}", v))),
        OwnershipMove(v) => (72, Some(format!("{}", v))),
        Borrow(v) => (73, Some(format!("{}", v))),
        Deref(v) => (74, Some(format!("{}", v))),
        AliveCheck(v) => (75, Some(format!("{}", v))),
        Dup => (80, None),
        Pop => (81, None),
    };
    buf.push(tag);
    if let Some(p) = payload {
        write_str(buf, &p);
    }
}

fn deserialize_instruction(data: &[u8], pos: &mut usize) -> Option<TypedInstruction> {
    use TypedInstruction::*;
    let tag = data.get(*pos).copied()?;
    *pos += 1;
    let read_vars = |s: &str| -> Option<(VarId, VarId)> {
        let parts: Vec<&str> = s.split(',').collect();
        if parts.len() < 2 { return None; }
        Some((parts[0].parse().ok()?, parts[1].parse().ok()?))
    };
    let read_single = |s: &str| -> Option<VarId> { s.parse().ok() };
    match tag {
        // 常量：序列化时附加前缀 i/f/b/s 用于区分类型
        0 => {
            let s = read_str_at(data, pos)?;
            let raw = s.strip_prefix('i').unwrap_or(&s);
            Some(ConstInt(raw.parse().ok()?))
        }
        1 => {
            let s = read_str_at(data, pos)?;
            let raw = s.strip_prefix('f').unwrap_or(&s);
            Some(ConstFloat(raw.parse().ok()?))
        }
        2 => {
            let s = read_str_at(data, pos)?;
            let raw = s.strip_prefix('b').unwrap_or(&s);
            Some(ConstBool(raw.parse().ok()?))
        }
        3 => {
            let s = read_str_at(data, pos)?;
            // 去掉前缀 's'，剩余部分即为字符串内容
            let stripped = s.strip_prefix('s').unwrap_or(&s);
            Some(ConstString(stripped.to_string()))
        }
        4 => Some(ConstNil),
        5 => { let v = read_str_at(data, pos)?.parse().ok()?; Some(LoadVar(v)) }
        6 => { let v = read_str_at(data, pos)?.parse().ok()?; Some(StoreVar(v)) }
        10 => { let (a, b) = read_vars(&read_str_at(data, pos)?)?; Some(I32Add(a, b)) }
        11 => { let (a, b) = read_vars(&read_str_at(data, pos)?)?; Some(I32Sub(a, b)) }
        12 => { let (a, b) = read_vars(&read_str_at(data, pos)?)?; Some(I32Mul(a, b)) }
        13 => { let (a, b) = read_vars(&read_str_at(data, pos)?)?; Some(I32Div(a, b)) }
        14 => { let (a, b) = read_vars(&read_str_at(data, pos)?)?; Some(I32Mod(a, b)) }
        15 => { let (a, b) = read_vars(&read_str_at(data, pos)?)?; Some(F64Add(a, b)) }
        16 => { let (a, b) = read_vars(&read_str_at(data, pos)?)?; Some(F64Sub(a, b)) }
        17 => { let (a, b) = read_vars(&read_str_at(data, pos)?)?; Some(F64Mul(a, b)) }
        18 => { let (a, b) = read_vars(&read_str_at(data, pos)?)?; Some(F64Div(a, b)) }
        20 => { let (a, b) = read_vars(&read_str_at(data, pos)?)?; Some(I32Eq(a, b)) }
        21 => { let (a, b) = read_vars(&read_str_at(data, pos)?)?; Some(I32Ne(a, b)) }
        22 => { let (a, b) = read_vars(&read_str_at(data, pos)?)?; Some(I32Lt(a, b)) }
        23 => { let (a, b) = read_vars(&read_str_at(data, pos)?)?; Some(I32Gt(a, b)) }
        24 => { let (a, b) = read_vars(&read_str_at(data, pos)?)?; Some(I32Le(a, b)) }
        25 => { let (a, b) = read_vars(&read_str_at(data, pos)?)?; Some(I32Ge(a, b)) }
        26 => { let (a, b) = read_vars(&read_str_at(data, pos)?)?; Some(F64Eq(a, b)) }
        27 => { let (a, b) = read_vars(&read_str_at(data, pos)?)?; Some(F64Ne(a, b)) }
        28 => { let (a, b) = read_vars(&read_str_at(data, pos)?)?; Some(F64Lt(a, b)) }
        29 => { let (a, b) = read_vars(&read_str_at(data, pos)?)?; Some(F64Gt(a, b)) }
        30 => { let (a, b) = read_vars(&read_str_at(data, pos)?)?; Some(F64Le(a, b)) }
        31 => { let (a, b) = read_vars(&read_str_at(data, pos)?)?; Some(F64Ge(a, b)) }
        32 => { let v = read_single(&read_str_at(data, pos)?)?; Some(I32Neg(v)) }
        33 => { let v = read_single(&read_str_at(data, pos)?)?; Some(F64Neg(v)) }
        34 => { let v = read_single(&read_str_at(data, pos)?)?; Some(BoolNot(v)) }
        35 => { let (a, b) = read_vars(&read_str_at(data, pos)?)?; Some(I32And(a, b)) }
        36 => { let (a, b) = read_vars(&read_str_at(data, pos)?)?; Some(I32Or(a, b)) }
        40 => { let t = read_str_at(data, pos)?.parse().ok()?; Some(Jump(t)) }
        41 => { let (v, t) = read_vars(&read_str_at(data, pos)?)?; Some(JumpIfFalse(v, t)) }
        42 => { let (v, t) = read_vars(&read_str_at(data, pos)?)?; Some(JumpIfTrue(v, t)) }
        50 => {
            // 格式: "f<func>,<arg0>,<arg1>,...;<ext_name>" （分号后可选外部函数名）
            let s = read_str_at(data, pos)?;
            let (body, ext_name) = s.split_once(';').map(|(b, n)| (b, Some(n.to_string()))).unwrap_or((&s, None));
            let stripped = body.strip_prefix('f').unwrap_or(body);
            let mut parts = stripped.split(',');
            let f: VarId = parts.next()?.parse().ok()?;
            let args: Vec<VarId> = parts.map(|p| p.parse().ok()).collect::<Option<_>>()?;
            Some(Call(f, args, ext_name))
        }
        51 => {
            // 格式: "vi<indirect>,<arg0>,<arg1>,..."
            let s = read_str_at(data, pos)?;
            let stripped = s.strip_prefix("vi").unwrap_or(&s);
            let mut parts = stripped.split(',');
            let f: VarId = parts.next()?.parse().ok()?;
            let args: Vec<VarId> = parts.map(|p| p.parse().ok()).collect::<Option<_>>()?;
            Some(CallIndirect(f, args))
        }
        52 => match read_str_at(data, pos) { Some(s) => Some(Return(s.parse().ok())), None => Some(Return(None)) },
        60 => {
            // 格式: "s<id>,<arg0>,<arg1>,..."
            let s = read_str_at(data, pos)?;
            let stripped = s.strip_prefix('s').unwrap_or(&s);
            let mut parts = stripped.split(',');
            let id: u32 = parts.next()?.parse().ok()?;
            let args: Vec<VarId> = parts.map(|p| p.parse().ok()).collect::<Option<_>>()?;
            Some(MakeStruct(StructLayoutId(id), args))
        }
        61 => { let (o, i) = read_vars(&read_str_at(data, pos)?)?; Some(GetField(o, i)) }
        62 => { let s = read_str_at(data, pos)?; let parts: Vec<&str> = s.split(',').collect(); if parts.len() < 3 { return None }; Some(SetField(parts[0].parse().ok()?, parts[1].parse().ok()?, parts[2].parse().ok()?)) }
        64 => { let (a, i) = read_vars(&read_str_at(data, pos)?)?; Some(IndexGet(a, i)) }
        65 => { let s = read_str_at(data, pos)?; let parts: Vec<&str> = s.split(',').collect(); if parts.len() < 3 { return None }; Some(IndexSet(parts[0].parse().ok()?, parts[1].parse().ok()?, parts[2].parse().ok()?)) }
        63 => {
            // 格式: "<base>,<arg0>,<arg1>,..."  首元素为 base
            let s = read_str_at(data, pos)?;
            let parts: Vec<&str> = s.split(',').collect();
            if parts.is_empty() {
                return Some(MakeArray(0, vec![]));
            }
            let base: VarId = parts[0].parse().ok()?;
            let args: Vec<VarId> = parts[1..]
                .iter()
                .map(|p| p.parse().ok())
                .collect::<Option<_>>()?;
            Some(MakeArray(base, args))
        }
        66 => {
            // 格式: "<k0>,<v0>,<k1>,<v1>,..."  元素成对出现
            let s = read_str_at(data, pos)?;
            if s.is_empty() {
                return Some(MakeMap(vec![]));
            }
            let parts: Vec<&str> = s.split(',').collect();
            if parts.len() % 2 != 0 {
                return None;
            }
            let mut pairs = Vec::with_capacity(parts.len() / 2);
            for chunk in parts.chunks_exact(2) {
                let k: VarId = chunk[0].parse().ok()?;
                let v: VarId = chunk[1].parse().ok()?;
                pairs.push((k, v));
            }
            Some(MakeMap(pairs))
        }
        70 => Some(Alloc(Type::Unknown)),
        71 => { let v = read_single(&read_str_at(data, pos)?)?; Some(Free(v)) }
        72 => { let v = read_single(&read_str_at(data, pos)?)?; Some(OwnershipMove(v)) }
        73 => { let v = read_single(&read_str_at(data, pos)?)?; Some(Borrow(v)) }
        74 => { let v = read_single(&read_str_at(data, pos)?)?; Some(Deref(v)) }
        75 => { let v = read_single(&read_str_at(data, pos)?)?; Some(AliveCheck(v)) }
        80 => Some(Dup),
        81 => Some(Pop),
        _ => None,
    }
}

// ==================== From Bytecode IR Pass ====================

pub fn upgrade_from_bytecode(
    func_name: &str,
    func_id: FuncId,
    instructions: &[(u8, Option<i32>, Option<String>)],
) -> TypeFunction {
    let mut tf = TypeFunction::new(func_name, func_id);
    for (op, iarg, _sarg) in instructions {
        let inst = match (op, iarg) {
            (0x01, Some(_)) => TypedInstruction::ConstInt(0),
            (0x02, _) => TypedInstruction::ConstNil,
            (0x03, _) => TypedInstruction::ConstBool(true),
            (0x04, _) => TypedInstruction::ConstBool(false),
            (0x05, _) => TypedInstruction::LoadVar(0),
            (0x06, _) => TypedInstruction::StoreVar(0),
            (0x07, _) => TypedInstruction::StoreVar(0),
            (0x08, Some(n)) => TypedInstruction::Call(0, vec![0; *n as usize], None),
            (0x09, _) => TypedInstruction::Return(None),
            (0x0B, Some(t)) => TypedInstruction::Jump(*t as u32),
            (0x0C, Some(t)) => TypedInstruction::JumpIfFalse(0, *t as u32),
            (0x0D, Some(t)) => TypedInstruction::JumpIfTrue(0, *t as u32),
            (0x10, _) => TypedInstruction::I32Add(0, 0),
            (0x11, _) => TypedInstruction::I32Sub(0, 0),
            (0x12, _) => TypedInstruction::I32Mul(0, 0),
            (0x13, _) => TypedInstruction::I32Div(0, 0),
            (0x14, _) => TypedInstruction::I32Mod(0, 0),
            (0x26, _) => TypedInstruction::MakeArray(0, vec![]),
            (0x28, _) => TypedInstruction::IndexGet(0, 0),
            (0x29, _) => TypedInstruction::IndexSet(0, 0, 0),
            (0x37, _) => TypedInstruction::Dup,
            (0x38, _) => TypedInstruction::Pop,
            (0x3B, _) => TypedInstruction::OwnershipMove(0),
            (0x3D, _) => TypedInstruction::Borrow(0),
            (0x3E, _) => TypedInstruction::AliveCheck(0),
            _ => continue,
        };
        tf.body.push(inst);
    }
    tf
}

// ==================== Helpers ====================

fn write_str(buf: &mut Vec<u8>, s: &str) {
    let bytes = s.as_bytes();
    buf.extend(&(bytes.len() as u32).to_be_bytes());
    buf.extend(bytes);
}

fn read_str_at(data: &[u8], pos: &mut usize) -> Option<String> {
    let len = read_u32_be_at(data, pos)? as usize;
    if *pos + len > data.len() { return None; }
    let s = String::from_utf8_lossy(&data[*pos..*pos + len]).to_string();
    *pos += len;
    Some(s)
}

fn read_u32_be_at(data: &[u8], pos: &mut usize) -> Option<u32> {
    if *pos + 4 > data.len() { return None; }
    let v = u32::from_be_bytes(data[*pos..*pos + 4].try_into().ok()?);
    *pos += 4;
    Some(v)
}

// ==================== Tests ====================

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_type_serialize_roundtrip() {
        let mut module = TypeModule::new();
        let mut func = TypeFunction::new("test_func", 0);
        func.return_type = Type::Int;
        func.params.push(("x".to_string(), Type::Int));
        func.body.push(TypedInstruction::ConstInt(42));
        func.body.push(TypedInstruction::Return(Some(0)));
        module.functions.push(func);
        module.function_map.insert(0, "test_func".to_string());

        let data = serialize_type_module(&module);
        let parsed = deserialize_type_module(&data).unwrap();
        assert_eq!(parsed.functions.len(), 1);
        assert_eq!(parsed.functions[0].name, "test_func");
        assert_eq!(parsed.functions[0].return_type, Type::Int);
    }

    /// 修复回归: MakeArray 序列化时必须保留 base 与全部元素 VarId
    #[test]
    fn test_make_array_roundtrip() {
        let mut module = TypeModule::new();
        let mut func = TypeFunction::new("test_array", 0);
        func.body.push(TypedInstruction::MakeArray(7, vec![1, 2, 3, 4]));
        module.functions.push(func);
        module.function_map.insert(0, "test_array".to_string());

        let data = serialize_type_module(&module);
        let parsed = deserialize_type_module(&data).unwrap();
        match &parsed.functions[0].body[0] {
            TypedInstruction::MakeArray(base, args) => {
                assert_eq!(*base, 7, "base VarId 应被保留");
                assert_eq!(args, &vec![1, 2, 3, 4], "所有元素 VarId 应被保留");
            }
            other => panic!("反序列化后类型不匹配: {:?}", other),
        }
    }

    /// 修复回归: MakeArray 空数组 roundtrip
    #[test]
    fn test_make_array_empty_roundtrip() {
        let mut module = TypeModule::new();
        let mut func = TypeFunction::new("test_empty_array", 0);
        func.body.push(TypedInstruction::MakeArray(0, vec![]));
        module.functions.push(func);
        module.function_map.insert(0, "test_empty_array".to_string());

        let data = serialize_type_module(&module);
        let parsed = deserialize_type_module(&data).unwrap();
        match &parsed.functions[0].body[0] {
            TypedInstruction::MakeArray(base, args) => {
                assert_eq!(*base, 0);
                assert!(args.is_empty());
            }
            other => panic!("反序列化后类型不匹配: {:?}", other),
        }
    }

    /// 修复回归: MakeMap 序列化时必须保留所有键值对
    #[test]
    fn test_make_map_roundtrip() {
        let mut module = TypeModule::new();
        let mut func = TypeFunction::new("test_map", 0);
        func.body.push(TypedInstruction::MakeMap(vec![(10, 20), (30, 40)]));
        module.functions.push(func);
        module.function_map.insert(0, "test_map".to_string());

        let data = serialize_type_module(&module);
        let parsed = deserialize_type_module(&data).unwrap();
        match &parsed.functions[0].body[0] {
            TypedInstruction::MakeMap(pairs) => {
                assert_eq!(pairs.len(), 2);
                assert_eq!(pairs[0], (10, 20));
                assert_eq!(pairs[1], (30, 40));
            }
            other => panic!("反序列化后类型不匹配: {:?}", other),
        }
    }

    /// 修复回归: MakeMap 空 Map roundtrip
    #[test]
    fn test_make_map_empty_roundtrip() {
        let mut module = TypeModule::new();
        let mut func = TypeFunction::new("test_empty_map", 0);
        func.body.push(TypedInstruction::MakeMap(vec![]));
        module.functions.push(func);
        module.function_map.insert(0, "test_empty_map".to_string());

        let data = serialize_type_module(&module);
        let parsed = deserialize_type_module(&data).unwrap();
        match &parsed.functions[0].body[0] {
            TypedInstruction::MakeMap(pairs) => assert!(pairs.is_empty()),
            other => panic!("反序列化后类型不匹配: {:?}", other),
        }
    }
}
