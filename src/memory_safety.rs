// ==================== 内存安全运行时 ====================
// 独立的内存安全管理模块
// 提供堆分配、指针验证、安全解引用、安全释放、析构等核心功能
//
// 在 Cargo 项目中通过 `mod memory_safety;` 引入
// 也可以直接复制到任何需要内存安全管理的 VX VM 项目中

use super::{VM, Value};

// ==================== 分配记录 ====================

/// 堆分配记录，用于跟踪每个堆对象的状态和生命周期
#[derive(Clone, Debug)]
pub struct AllocRecord {
    /// 代数 (generation)：每次释放时递增，用于检测悬垂指针
    pub generation: u32,
    /// 当前是否存活
    pub alive: bool,
    /// 实际的实例值
    pub instance: Value,
}

// ==================== VM 内存安全方法 ====================

impl VM {
    // -------------------- 堆分配 --------------------

    /// 在堆上分配一个新的对象
    /// 
    /// # 参数
    /// - `class_name`: 类名
    /// - `instance`: 实例值
    /// 
    /// # 返回
    /// 新分配的记录 ID
    pub(crate) fn alloc_heap(&mut self, _class_name: String, instance: Value) -> u64 {
        let id = self.next_alloc_id;
        self.next_alloc_id += 1;
        self.alloc_registry.insert(
            id,
            AllocRecord {
                generation: 0,
                alive: true,
                instance,
            },
        );
        if !self.frames.is_empty() {
            self.current_frame_mut().owned_allocs.push(id);
        }
        id
    }

    // -------------------- 指针验证 --------------------

    /// 验证指针是否仍然有效
    /// 
    /// 检测以下安全性问题：
    /// - use-after-free（悬垂指针）
    /// - 代数不匹配（野指针）
    /// - 指针指向不存在的分配记录
    /// 
    /// # 返回
    /// 如果指针有效返回 `Ok(true)`，否则返回 `Err(错误信息)`
    pub(crate) fn validate_pointer(&self, ptr: &Value) -> Result<bool, String> {
        let (alloc_id, generation) = match ptr {
            Value::Pointer {
                alloc_id,
                generation,
                ..
            } => (*alloc_id, *generation),
            _ => {
                return self.runtime_error("Expected a pointer for dereference/free operation");
            }
        };

        if let Some(rec) = self.alloc_registry.get(&alloc_id) {
            if !rec.alive {
                return self.runtime_error(&format!(
                    "Dangling pointer: allocation {} has been freed (use-after-free)",
                    alloc_id
                ));
            }
            if rec.generation != generation {
                return self.runtime_error(&format!(
                    "Stale pointer: generation mismatch for allocation {} (expected gen {}, got {})",
                    alloc_id, rec.generation, generation
                ));
            }
            Ok(true)
        } else {
            self.runtime_error(&format!(
                "Dangling pointer: allocation {} does not exist (use-after-free)",
                alloc_id
            ))
        }
    }

    // -------------------- 安全解引用 --------------------

    /// 安全解引用指针，返回指针指向的实例
    /// 
    /// 内部调用 `validate_pointer` 进行安全检查
    pub(crate) fn deref_pointer(&self, ptr: &Value) -> Result<Value, String> {
        if !self.validate_pointer(ptr)? {
            return Ok(Value::Nil);
        }
        if let Value::Pointer { alloc_id, .. } = ptr {
            Ok(self.alloc_registry
                .get(alloc_id)
                .map(|r| r.instance.clone())
                .unwrap_or(Value::Nil))
        } else {
            Ok(Value::Nil)
        }
    }

    // -------------------- 安全释放 --------------------

    /// 安全释放堆分配
    /// 
    /// 检测以下安全性问题：
    /// - double-free（重复释放）
    /// - 代数不匹配
    /// - 释放不存在的分配
    /// 
    /// 成功释放后：
    /// - 递增代数（generation）
    /// - 标记为非存活
    /// - 从当前帧的所有权列表中移除
    pub(crate) fn free_allocation(&mut self, alloc_id: u64, generation: u32) -> Result<(), String> {
        if let Some(rec) = self.alloc_registry.get(&alloc_id) {
            if !rec.alive {
                return self.runtime_error(&format!(
                    "Double-free: allocation {} has already been freed",
                    alloc_id
                ));
            }
            if rec.generation != generation {
                return self.runtime_error(&format!(
                    "Double-free: generation mismatch for allocation {}",
                    alloc_id
                ));
            }
        } else {
            return self.runtime_error(&format!(
                "Double-free: allocation {} does not exist",
                alloc_id
            ));
        }
        self.alloc_registry.remove(&alloc_id);
        if !self.frames.is_empty() {
            let owned = &mut self.current_frame_mut().owned_allocs;
            owned.retain(|&id| id != alloc_id);
        }
        Ok(())
    }

    // -------------------- 帧清理 / 析构 --------------------

    /// 清理指定调用帧的所有堆分配（作用域退出时调用）
    /// 
    /// 递增所有分配的代数并标记为非存活
    pub(crate) fn cleanup_frame_allocs(&mut self, owned_allocs: &[u64]) {
        for alloc_id in owned_allocs {
            self.alloc_registry.remove(alloc_id);
        }
    }
}