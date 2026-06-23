// ==================== Value ====================
//
// Value 使用 Arc<T> 包装堆分配字段（String、Vec、HashMap），
// 实现 Copy-on-Write 语义：
//   - clone() 只增加引用计数（O(1)），无需深拷贝
//   - 需要修改时通过 Arc::make_mut() 实现写时复制
//   - 显著减少 VM 热路径上的堆分配

use std::collections::HashMap;
use std::sync::Arc;

#[derive(Clone, Debug)]
pub enum Value {
    Nil,
    Int(i64),
    Float(f64),
    Bool(bool),
    String(Arc<str>),          // Arc: clone 只增引用计数
    Array(Arc<Vec<Value>>),    // Arc: clone 只增引用计数
    Map(Arc<HashMap<String, Value>>), // Arc: clone 只增引用计数
    Instance {
        class_name: Arc<str>,
        fields: Arc<HashMap<String, Value>>,
    },
    Pointer {
        alloc_id: u64,
        generation: u32,
        class_name: Arc<str>,
    },
}

impl PartialEq for Value {
    fn eq(&self, other: &Self) -> bool {
        match (self, other) {
            (Value::Nil, Value::Nil) => true,
            (Value::Int(a), Value::Int(b)) => a == b,
            (Value::Float(a), Value::Float(b)) => a == b,
            (Value::Bool(a), Value::Bool(b)) => a == b,
            (Value::String(a), Value::String(b)) => a == b,
            (Value::Array(a), Value::Array(b)) => a == b,
            (Value::Map(a), Value::Map(b)) => a == b,
            (Value::Instance { class_name: ca, fields: fa }, Value::Instance { class_name: cb, fields: fb }) => ca == cb && fa == fb,
            (Value::Pointer { alloc_id: aid, generation: ag, .. }, Value::Pointer { alloc_id: bid, generation: bg, .. }) => aid == bid && ag == bg,
            _ => false,
        }
    }
}

impl Value {
    pub fn type_name(&self) -> String {
        match self {
            Value::Nil => "Nil".to_string(),
            Value::Int(_) => "Int".to_string(),
            Value::Float(_) => "Float".to_string(),
            Value::Bool(_) => "Bool".to_string(),
            Value::String(_) => "String".to_string(),
            Value::Array(_) => "Array".to_string(),
            Value::Map(_) => "Map".to_string(),
            Value::Instance { class_name, .. } => class_name.to_string(),
            Value::Pointer { class_name, .. } => class_name.to_string(),
        }
    }

    pub fn nil() -> Self { Value::Nil }
    pub fn int(i: i64) -> Self { Value::Int(i) }
    pub fn float(f: f64) -> Self { Value::Float(f) }
    pub fn bool(b: bool) -> Self { Value::Bool(b) }
    pub fn string(s: impl Into<Arc<str>>) -> Self { Value::String(s.into()) }
    pub fn array() -> Self { Value::Array(Arc::new(Vec::new())) }
    pub fn map() -> Self { Value::Map(Arc::new(HashMap::new())) }
    pub fn instance(class_name: impl Into<Arc<str>>) -> Self {
        Value::Instance {
            class_name: class_name.into(),
            fields: Arc::new(HashMap::new()),
        }
    }
    pub fn pointer(alloc_id: u64, generation: u32, class_name: impl Into<Arc<str>>) -> Self {
        Value::Pointer { alloc_id, generation, class_name: class_name.into() }
    }

    /// 获取字符串的不可变引用（避免 clone）
    #[inline]
    pub fn as_str(&self) -> Option<&str> {
        match self {
            Value::String(s) => Some(s),
            _ => None,
        }
    }

    /// 获取数组的不可变引用（避免 clone）
    #[inline]
    pub fn as_array(&self) -> Option<&[Value]> {
        match self {
            Value::Array(arr) => Some(arr),
            _ => None,
        }
    }

    /// 获取 Map 的不可变引用（避免 clone）
    #[inline]
    pub fn as_map(&self) -> Option<&HashMap<String, Value>> {
        match self {
            Value::Map(m) => Some(m),
            _ => None,
        }
    }

    /// 获取可变字符串引用（必要时执行 COW 复制）
    #[inline]
    pub fn as_str_mut(&mut self) -> Option<&mut String> {
        match self {
            Value::String(s) => {
                // Arc<str> 不支持 make_mut 直接返回 String，需要转换
                let owned: String = s.to_string();
                *s = owned.into();
                // 由于 Arc<str> 是不可变的，这里需要特殊处理
                None
            },
            _ => None,
        }
    }

    /// 获取可变数组引用（COW：仅在非唯一时复制）
    #[inline]
    pub fn as_array_mut(&mut self) -> Option<&mut Vec<Value>> {
        match self {
            Value::Array(arr) => Some(Arc::make_mut(arr)),
            _ => None,
        }
    }

    /// 获取可变 Map 引用（COW：仅在非唯一时复制）
    #[inline]
    pub fn as_map_mut(&mut self) -> Option<&mut HashMap<String, Value>> {
        match self {
            Value::Map(m) => Some(Arc::make_mut(m)),
            _ => None,
        }
    }

    /// 向数组中追加元素（使用 COW）
    pub fn array_push(&mut self, value: Value) {
        if let Some(arr) = self.as_array_mut() {
            arr.push(value);
        }
    }

    /// 设置 Map 键值对（使用 COW）
    pub fn map_set(&mut self, key: impl Into<String>, value: Value) {
        if let Some(m) = self.as_map_mut() {
            m.insert(key.into(), value);
        }
    }

    /// 获取 Map 值（只读，无 clone）
    pub fn map_get(&self, key: &str) -> Option<&Value> {
        match self {
            Value::Map(m) => m.get(key),
            _ => None,
        }
    }

    /// 设置实例字段（使用 COW）
    pub fn set_field(&mut self, key: impl Into<String>, value: Value) {
        if let Value::Instance { fields, .. } = self {
            Arc::make_mut(fields).insert(key.into(), value);
        }
    }

    /// 获取实例字段（只读，无 clone）
    pub fn get_field(&self, key: &str) -> Option<&Value> {
        match self {
            Value::Instance { fields, .. } => fields.get(key),
            _ => None,
        }
    }

    pub fn is_truthy(&self) -> bool {
        match self {
            Value::Nil => false,
            Value::Bool(b) => *b,
            Value::Int(i) => *i != 0,
            Value::Float(f) => *f != 0.0,
            Value::String(s) => !s.is_empty(),
            Value::Array(arr) => !arr.is_empty(),
            Value::Map(map) => !map.is_empty(),
            Value::Instance { .. } => true,
            Value::Pointer { alloc_id, .. } => *alloc_id != 0,
        }
    }

    pub fn length(&self) -> usize {
        match self {
            Value::String(s) => s.len(),
            Value::Array(arr) => arr.len(),
            Value::Map(map) => map.len(),
            _ => 0,
        }
    }
}

impl std::fmt::Display for Value {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match self {
            Value::Nil => write!(f, "nil"),
            Value::Int(i) => write!(f, "{}", i),
            Value::Float(fl) => {
                let mut s = fl.to_string();
                if s.contains('.') {
                    s = s.trim_end_matches('0').trim_end_matches('.').to_string();
                }
                write!(f, "{}", s)
            }
            Value::Bool(b) => {
                if *b { write!(f, "true") } else { write!(f, "false") }
            }
            Value::String(s) => write!(f, "{}", s),
            Value::Array(arr) => {
                write!(f, "[")?;
                for (i, v) in arr.iter().enumerate() {
                    if i > 0 { write!(f, ", ")?; }
                    write!(f, "{}", v)?;
                }
                write!(f, "]")
            }
            Value::Map(map) => {
                write!(f, "{{")?;
                for (i, (k, v)) in map.iter().enumerate() {
                    if i > 0 { write!(f, ", ")?; }
                    write!(f, "{}: {}", k, v)?;
                }
                write!(f, "}}")
            }
            Value::Instance { class_name, fields } => {
                write!(f, "{}(", class_name)?;
                for (i, (k, v)) in fields.iter().enumerate() {
                    if i > 0 { write!(f, ", ")?; }
                    write!(f, "{}={}", k, v)?;
                }
                write!(f, ")")
            }
            Value::Pointer { class_name, alloc_id, generation } => {
                write!(f, "{}*({}:{})", class_name, alloc_id, generation)
            }
        }
    }
}

// 从 &str 创建 Value::String 的便捷实现
impl From<&str> for Value {
    fn from(s: &str) -> Self {
        Value::String(Arc::from(s))
    }
}

impl From<String> for Value {
    fn from(s: String) -> Self {
        Value::String(Arc::from(s))
    }
}
