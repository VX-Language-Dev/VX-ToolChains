// VX Language Macros - 编译时宏系统
// 提供高效的宏定义、注册和展开功能

use std::collections::HashMap;
use crate::parser::Expr;

/// 宏定义结构体
#[derive(Debug, Clone)]
pub struct Macro {
    pub name: String,
    pub params: Vec<String>,
    pub body: Vec<Box<Expr>>,
    pub line: usize,
    pub col: usize,
}

/// 带缓存的宏对象
#[derive(Debug, Clone)]
struct CachedMacro {
    macro_obj: Macro,
    expansion_cache: HashMap<String, Vec<Box<Expr>>>,
}

/// 宏注册表，管理所有宏的定义和展开
#[derive(Debug)]
pub struct MacroRegistry {
    macros: HashMap<String, CachedMacro>,
    /// 统计信息：展开次数
    expand_count: u64,
    /// 统计信息：缓存命中次数
    cache_hit_count: u64,
}

impl MacroRegistry {
    /// 创建新的宏注册表
    pub fn new() -> Self {
        Self {
            macros: HashMap::new(),
            expand_count: 0,
            cache_hit_count: 0,
        }
    }

    /// 注册宏定义
    pub fn register_macro(&mut self, mac: Macro) -> Result<(), String> {
        if self.macros.contains_key(&mac.name) {
            return Err(format!("宏 '{}' 已经存在", mac.name));
        }
        
        let cached_mac = CachedMacro {
            macro_obj: mac,
            expansion_cache: HashMap::new(),
        };
        
        self.macros.insert(cached_mac.macro_obj.name.clone(), cached_mac);
        Ok(())
    }

    /// 检查宏是否存在
    pub fn has_macro(&self, name: &str) -> bool {
        self.macros.contains_key(name)
    }

    /// 展开宏调用
    pub fn expand_macro(&mut self, name: &str, args: &[Box<Expr>]) -> Result<Vec<Box<Expr>>, String> {
        self.expand_count += 1;
        
        let cached_mac = self.macros.get_mut(name)
            .ok_or_else(|| format!("未找到宏: '{}'", name))?;

        // 验证参数数量
        if args.len() != cached_mac.macro_obj.params.len() {
            return Err(format!(
                "宏 '{}' 期望 {} 个参数，但提供了 {} 个", 
                name, 
                cached_mac.macro_obj.params.len(), 
                args.len()
            ));
        }

        // 生成参数签名用于缓存
        let args_signature = Self::generate_args_signature(args);

        // 检查缓存
        if let Some(cached_result) = cached_mac.expansion_cache.get(&args_signature) {
            self.cache_hit_count += 1;
            return Ok(cached_result.clone());
        }

        // 创建参数绑定映射
        let mut bindings = HashMap::new();
        for (param, arg) in cached_mac.macro_obj.params.iter().zip(args.iter()) {
            bindings.insert(param.clone(), arg.clone());
        }

        // 展开宏体，替换参数
        let expanded_body: Vec<Box<Expr>> = cached_mac.macro_obj.body.iter()
            .map(|expr| Self::substitute_params(expr, &bindings))
            .collect();

        // 缓存结果
        cached_mac.expansion_cache.insert(args_signature, expanded_body.clone());

        Ok(expanded_body)
    }

    /// 生成参数的唯一签名用于缓存
    fn generate_args_signature(args: &[Box<Expr>]) -> String {
        use std::fmt::Write;
        let mut signature = String::with_capacity(args.len() * 32);
        for (i, arg) in args.iter().enumerate() {
            if i > 0 {
                signature.push('|');
            }
            write!(&mut signature, "{:?}", arg).unwrap_or_default();
        }
        signature
    }

    /// 递归替换表达式中的参数引用
    fn substitute_params(expr: &Expr, bindings: &HashMap<String, Box<Expr>>) -> Box<Expr> {
        match expr {
            // 替换标识符（参数引用）
            Expr::Identifier(name, line, col) => {
                if let Some(replacement) = bindings.get(name) {
                    replacement.clone()
                } else {
                    Box::new(Expr::Identifier(name.clone(), *line, *col))
                }
            }
            
            // 递归处理二元操作
            Expr::BinaryOp(op, left, right, line, col) => {
                Box::new(Expr::BinaryOp(
                    op.clone(),
                    Self::substitute_params(left, bindings),
                    Self::substitute_params(right, bindings),
                    *line,
                    *col,
                ))
            }
            
            // 递归处理一元操作
            Expr::UnaryOp(op, operand, line, col) => {
                Box::new(Expr::UnaryOp(
                    op.clone(),
                    Self::substitute_params(operand, bindings),
                    *line,
                    *col,
                ))
            }
            
            // 递归处理函数调用
            Expr::CallExpr(func, args, line, col) => {
                let new_func = Self::substitute_params(func, bindings);
                let new_args: Vec<Box<Expr>> = args.iter()
                    .map(|arg| Self::substitute_params(arg, bindings))
                    .collect();
                
                Box::new(Expr::CallExpr(new_func, new_args, *line, *col))
            }
            
            // 递归处理变量声明
            Expr::VarDecl(name, type_hint, init_expr, is_mutable, line, col) => {
                let new_init = Self::substitute_params(init_expr, bindings);
                Box::new(Expr::VarDecl(
                    name.clone(),
                    type_hint.clone(),
                    new_init,
                    *is_mutable,
                    *line,
                    *col,
                ))
            }
            
            // 递归处理赋值语句
            Expr::Assign(target, name, value, line, col) => {
                Box::new(Expr::Assign(
                    Self::substitute_params(target, bindings),
                    name.clone(),
                    Self::substitute_params(value, bindings),
                    *line,
                    *col,
                ))
            }
            
            // 递归处理条件语句
            Expr::IfStmt(condition, then_branch, elif_branches, else_branch, line, col) => {
                let new_condition = Self::substitute_params(condition, bindings);
                let new_then_branch: Vec<Box<Expr>> = then_branch.iter()
                    .map(|stmt| Self::substitute_params(stmt, bindings))
                    .collect();
                let new_elif_branches: Vec<(Box<Expr>, Vec<Box<Expr>>)> = elif_branches.iter()
                    .map(|(cond, body)| {
                        let new_cond = Self::substitute_params(cond, bindings);
                        let new_body: Vec<Box<Expr>> = body.iter()
                            .map(|stmt| Self::substitute_params(stmt, bindings))
                            .collect();
                        (new_cond, new_body)
                    })
                    .collect();
                let new_else_branch = if let Some(ref branch) = else_branch {
                    Some(branch.iter()
                        .map(|stmt| Self::substitute_params(stmt, bindings))
                        .collect())
                } else {
                    None
                };
                
                Box::new(Expr::IfStmt(
                    new_condition,
                    new_then_branch,
                    new_elif_branches,
                    new_else_branch,
                    *line,
                    *col,
                ))
            }
            
            // 递归处理while循环
            Expr::WhileStmt(condition, body, line, col) => {
                let new_condition = Self::substitute_params(condition, bindings);
                let new_body: Vec<Box<Expr>> = body.iter()
                    .map(|stmt| Self::substitute_params(stmt, bindings))
                    .collect();
                
                Box::new(Expr::WhileStmt(new_condition, new_body, *line, *col))
            }
            
            // 递归处理for循环
            Expr::ForStmt(var, iterable, body, line, col) => {
                let new_iterable = Self::substitute_params(iterable, bindings);
                let new_body: Vec<Box<Expr>> = body.iter()
                    .map(|stmt| Self::substitute_params(stmt, bindings))
                    .collect();
                
                Box::new(Expr::ForStmt(var.clone(), new_iterable, new_body, *line, *col))
            }

            // 递归处理 match 语句
            Expr::MatchStmt(subject, arms, line, col) => {
                let new_subject = Self::substitute_params(subject, bindings);
                let new_arms: Vec<(Box<Expr>, Vec<Box<Expr>>)> = arms.iter()
                    .map(|(pattern, body)| {
                        let new_pattern = Self::substitute_params(pattern, bindings);
                        let new_body: Vec<Box<Expr>> = body.iter()
                            .map(|stmt| Self::substitute_params(stmt, bindings))
                            .collect();
                        (new_pattern, new_body)
                    })
                    .collect();

                Box::new(Expr::MatchStmt(new_subject, new_arms, *line, *col))
            }

            // 递归处理函数定义
            Expr::FuncDecl(name, gen_params, params, ret_type, body, line, col) => {
                let new_body: Vec<Box<Expr>> = body.iter()
                    .map(|stmt| Self::substitute_params(stmt, bindings))
                    .collect();
                
                Box::new(Expr::FuncDecl(
                    name.clone(),
                    gen_params.clone(),
                    params.clone(),
                    ret_type.clone(),
                    new_body,
                    *line,
                    *col,
                ))
            }
            
            // 递归处理返回语句
            Expr::ReturnStmt(value, line, col) => {
                let new_value = if let Some(ref v) = value {
                    Some(Self::substitute_params(v, bindings))
                } else {
                    None
                };
                Box::new(Expr::ReturnStmt(new_value, *line, *col))
            }
            
            // 递归处理数组字面量
            Expr::ArrayLiteral(items, line, col) => {
                let new_items: Vec<Box<Expr>> = items.iter()
                    .map(|item| Self::substitute_params(item, bindings))
                    .collect();
                Box::new(Expr::ArrayLiteral(new_items, *line, *col))
            }
            
            // 递归处理索引访问
            Expr::IndexAccess(array, index, line, col) => {
                Box::new(Expr::IndexAccess(
                    Self::substitute_params(array, bindings),
                    Self::substitute_params(index, bindings),
                    *line,
                    *col,
                ))
            }
            
            // 递归处理属性访问
            Expr::PropertyAccess(obj, prop, line, col) => {
                Box::new(Expr::PropertyAccess(
                    Self::substitute_params(obj, bindings),
                    prop.clone(),
                    *line,
                    *col,
                ))
            }
            
            // 其他表达式类型保持不变
            _ => Box::new(expr.clone()),
        }
    }

    /// 获取统计信息
    pub fn get_stats(&self) -> (u64, u64, f64) {
        let hit_rate = if self.expand_count > 0 {
            (self.cache_hit_count as f64 / self.expand_count as f64) * 100.0
        } else {
            0.0
        };
        (self.expand_count, self.cache_hit_count, hit_rate)
    }
}

impl Default for MacroRegistry {
    fn default() -> Self {
        Self::new()
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn test_register_and_expand_simple_macro() {
        let mut registry = MacroRegistry::new();
        
        // 创建一个简单的宏：debug_print(var_name, value)
        let body = vec![
            Box::new(Expr::CallExpr(
                Box::new(Expr::Identifier("out".to_string(), 1, 1)),
                vec![
                    Box::new(Expr::BinaryOp(
                        "+".to_string(),
                        Box::new(Expr::StringLiteral("DEBUG: ".to_string(), 1, 1)),
                        Box::new(Expr::Identifier("var_name".to_string(), 1, 1)),
                        1,
                        1,
                    )),
                ],
                1,
                1,
            )),
        ];
        
        let mac = Macro {
            name: "debug_print".to_string(),
            params: vec!["var_name".to_string(), "value".to_string()],
            body,
            line: 1,
            col: 1,
        };
        
        assert!(registry.register_macro(mac).is_ok());
        assert!(registry.has_macro("debug_print"));
    }

    #[test]
    fn test_expand_with_wrong_args_count() {
        let mut registry = MacroRegistry::new();
        
        let mac = Macro {
            name: "test".to_string(),
            params: vec!["a".to_string()],
            body: vec![],
            line: 1,
            col: 1,
        };
        
        registry.register_macro(mac).unwrap();
        
        // 尝试用错误的参数数量展开
        let result = registry.expand_macro("test", &[]);
        assert!(result.is_err());
    }

    #[test]
    fn test_expand_nonexistent_macro() {
        let mut registry = MacroRegistry::new();
        let result = registry.expand_macro("nonexistent", &[]);
        assert!(result.is_err());
    }
}
