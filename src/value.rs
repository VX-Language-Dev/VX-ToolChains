// ==================== Value ====================

use std::collections::HashMap;

#[derive(Clone, Debug, PartialEq)]
pub enum Value {
    Nil,
    Int(i64),
    Float(f64),
    Bool(bool),
    String(String),
    Array(Vec<Value>),
    Map(HashMap<String, Value>),
    Instance {
        class_name: String,
        fields: HashMap<String, Value>,
    },
    Pointer {
        alloc_id: u64,
        generation: u32,
        class_name: String,
    },
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
            Value::Instance { class_name, .. } => class_name.clone(),
            Value::Pointer { class_name, .. } => class_name.clone(),
        }
    }

    pub fn nil() -> Self {
        Value::Nil
    }
    pub fn int(i: i64) -> Self {
        Value::Int(i)
    }
    pub fn float(f: f64) -> Self {
        Value::Float(f)
    }
    pub fn bool(b: bool) -> Self {
        Value::Bool(b)
    }
    pub fn string(s: String) -> Self {
        Value::String(s)
    }
    pub fn array() -> Self {
        Value::Array(Vec::new())
    }
    pub fn map() -> Self {
        Value::Map(HashMap::new())
    }
    pub fn instance(class_name: String) -> Self {
        Value::Instance {
            class_name,
            fields: HashMap::new(),
        }
    }
    pub fn pointer(alloc_id: u64, generation: u32, class_name: String) -> Self {
        Value::Pointer {
            alloc_id,
            generation,
            class_name,
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

    pub fn to_string(&self) -> String {
        match self {
            Value::Nil => "nil".to_string(),
            Value::Int(i) => i.to_string(),
            Value::Float(f) => {
                let s = f.to_string();
                if let Some(_dot_pos) = s.find('.') {
                    let last_nonzero = s.trim_end_matches('0').trim_end_matches('.').len();
                    s[..last_nonzero].to_string()
                } else {
                    s
                }
            }
            Value::Bool(b) => {
                if *b {
                    "true".to_string()
                } else {
                    "false".to_string()
                }
            }
            Value::String(s) => s.clone(),
            Value::Array(arr) => {
                let items: Vec<String> = arr.iter().map(|v| v.to_string()).collect();
                format!("[{}]", items.join(", "))
            }
            Value::Map(map) => {
                let items: Vec<String> = map
                    .iter()
                    .map(|(k, v)| format!("{}: {}", k, v.to_string()))
                    .collect();
                format!("{{{}}}", items.join(", "))
            }
            Value::Instance { class_name, fields } => {
                let items: Vec<String> = fields
                    .iter()
                    .map(|(k, v)| format!("{}={}", k, v.to_string()))
                    .collect();
                format!("{}({})", class_name, items.join(", "))
            }
            Value::Pointer {
                class_name,
                alloc_id,
                generation,
            } => {
                format!("{}*({}:{})", class_name, alloc_id, generation)
            }
        }
    }
}
