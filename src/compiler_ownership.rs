// ==================== 所有权检查器 ====================
// 按照 Rust 内存模型实现：所有权 + 借用（可变/不可变）+ Copy 语义

use std::collections::{HashMap, HashSet};
use crate::parser::{Expr, get_src_line, expr_to_type_name};

#[derive(Debug, Clone, PartialEq)]
pub enum OwnershipState {
    Owned,
    Moved,
    Borrowed,
    Freed,
}

#[derive(Debug, Clone, PartialEq)]
pub enum BorrowKind {
    Immutable,
    Mutable,
}

pub struct OwnershipChecker {
    source: String,
    scopes: Vec<HashMap<String, OwnershipState>>,
    heap_vars: HashSet<String>,
    /// 使用 `mut` 关键字声明的变量集合（跨作用域，按名查找）
    mut_vars: HashSet<String>,
    /// borrower -> (owner, borrow_kind)
    borrows: HashMap<String, (String, BorrowKind)>,
    /// 变量名 -> 类型名（用于 Copy 判断）
    var_types: HashMap<String, String>,
    pub errors: Vec<String>,
    pub warnings: Vec<String>,
}

impl OwnershipChecker {
    pub fn new(source: &str) -> Self {
        Self {
            source: source.to_string(),
            scopes: vec![HashMap::new()],
            heap_vars: HashSet::new(),
            mut_vars: HashSet::new(),
            borrows: HashMap::new(),
            var_types: HashMap::new(),
            errors: Vec::new(),
            warnings: Vec::new(),
        }
    }

    /// 标量类型默认实现 Copy 语义，赋值/传参时复制而非移动所有权。
    fn is_copy_type(t: &str) -> bool {
        matches!(t, "int" | "float" | "double" | "bool")
    }

    pub fn push_scope(&mut self) {
        self.scopes.push(HashMap::new());
    }

    pub fn pop_scope(&mut self) {
        if self.scopes.len() <= 1 {
            return;
        }
        let scope = self.scopes.pop().unwrap();
        for (var, state) in &scope {
            if self.heap_vars.contains(var) && *state == OwnershipState::Owned {
                self.warnings.push(format!(
                    "堆变量 '{}' 在作用域结束时未被显式释放（可能内存泄漏），请调用 free({})",
                    var, var
                ));
            }
        }
        let to_remove: Vec<String> = self
            .borrows
            .iter()
            .filter(|(_, (o, _))| scope.contains_key(o.as_str()))
            .map(|(b, _)| b.clone())
            .collect();
        for b in to_remove {
            self.end_borrow(&b);
        }
    }

    pub fn declare_var(&mut self, name: &str, is_heap: bool, is_mut: bool, ty: &str) {
        self.scopes
            .last_mut()
            .unwrap()
            .insert(name.to_string(), OwnershipState::Owned);
        if is_heap {
            self.heap_vars.insert(name.to_string());
        }
        if is_mut {
            self.mut_vars.insert(name.to_string());
        }
        self.var_types.insert(name.to_string(), ty.to_string());
    }

    pub fn get_state(&self, name: &str) -> Option<OwnershipState> {
        for scope in self.scopes.iter().rev() {
            if let Some(s) = scope.get(name) {
                return Some(s.clone());
            }
        }
        None
    }

    pub fn set_state(&mut self, name: &str, state: OwnershipState) {
        for scope in self.scopes.iter_mut().rev() {
            if scope.contains_key(name) {
                scope.insert(name.to_string(), state);
                return;
            }
        }
    }

    pub fn is_var_mut(&self, name: &str) -> bool {
        self.mut_vars.contains(name)
    }

    fn var_type(&self, name: &str) -> Option<&str> {
        self.var_types.get(name).map(|s| s.as_str())
    }

    /// 返回某所有者当前所有活跃借用。
    fn active_borrows_of(&self, owner: &str) -> Vec<(&str, BorrowKind)> {
        self.borrows
            .iter()
            .filter(|(_, (o, _))| o == owner)
            .map(|(b, (_, k))| (b.as_str(), k.clone()))
            .collect()
    }

    pub fn check_use(&mut self, name: &str, line: usize, _col: usize) -> bool {
        if let Some(s) = self.get_state(name) {
            if s == OwnershipState::Moved {
                self.errors.push(format!(
                    "变量 '{}' 的所有权已被转移（use-after-move）\n {} | {}",
                    name,
                    line,
                    self.get_src_line(line)
                ));
                return false;
            }
            if s == OwnershipState::Freed {
                self.errors.push(format!(
                    "变量 '{}' 已被释放（use-after-free/悬垂指针）\n {} | {}",
                    name,
                    line,
                    self.get_src_line(line)
                ));
                return false;
            }
            if s == OwnershipState::Borrowed {
                // 若存在活跃可变借用，则所有者不可使用（Rust：&mut 借用期间原变量冻结）
                let has_mut = self
                    .active_borrows_of(name)
                    .iter()
                    .any(|(_, k)| *k == BorrowKind::Mutable);
                if has_mut {
                    self.errors.push(format!(
                        "变量 '{}' 存在活跃可变借用，无法使用\n {} | {}",
                        name,
                        line,
                        self.get_src_line(line)
                    ));
                    return false;
                }
            }
        }
        true
    }

    pub fn check_free(&mut self, name: &str, _line: usize, _col: usize) -> bool {
        if let Some(s) = self.get_state(name) {
            if s == OwnershipState::Moved {
                self.errors
                    .push(format!("变量 '{}' 所有权已转移，无法释放", name));
                return false;
            }
            if s == OwnershipState::Freed {
                self.errors
                    .push(format!("变量 '{}' 已被释放（双重释放/double-free）", name));
                return false;
            }
            let active: Vec<_> = self.active_borrows_of(name);
            if !active.is_empty() {
                self.errors.push(format!(
                    "变量 '{}' 存在活跃借用 {:?}，无法释放（违反借用规则）",
                    name, active
                ));
                return false;
            }
            return true;
        }
        self.errors.push(format!("未定义的变量 '{}'", name));
        false
    }

    pub fn do_free(&mut self, name: &str) {
        self.set_state(name, OwnershipState::Freed);
        self.heap_vars.remove(name);
    }

    pub fn check_move(&mut self, src: &str, _line: usize, _col: usize) -> bool {
        if let Some(s) = self.get_state(src) {
            if s == OwnershipState::Moved {
                self.errors
                    .push(format!("变量 '{}' 所有权已转移，无法再次移动", src));
                return false;
            }
            if s == OwnershipState::Freed {
                self.errors
                    .push(format!("变量 '{}' 已被释放，无法移动", src));
                return false;
            }
            if s == OwnershipState::Borrowed {
                self.errors
                    .push(format!("变量 '{}' 存在活跃借用，无法移动", src));
                return false;
            }
            return true;
        }
        self.errors.push(format!("未定义的变量 '{}'", src));
        false
    }

    pub fn do_move(&mut self, src: &str, dst: &str) {
        self.set_state(src, OwnershipState::Moved);
        let is_heap = self.heap_vars.contains(src);
        let ty = self
            .var_types
            .get(src)
            .cloned()
            .unwrap_or_else(|| "unknown".to_string());
        self.declare_var(dst, is_heap, false, &ty);
    }

    pub fn do_borrow(&mut self, owner: &str, borrower: &str, kind: BorrowKind) {
        // Rust 借用规则： aliasing XOR mutation
        let active = self.active_borrows_of(owner);
        match kind {
            BorrowKind::Immutable => {
                // 不可变借用：不允许与任何可变借用共存
                if active.iter().any(|(_, k)| *k == BorrowKind::Mutable) {
                    self.errors.push(format!(
                        "变量 '{}' 已存在可变借用，无法创建不可变借用（aliasing XOR mutation）",
                        owner
                    ));
                    return;
                }
            }
            BorrowKind::Mutable => {
                // 可变借用：不允许任何其它借用共存
                if !active.is_empty() {
                    self.errors.push(format!(
                        "变量 '{}' 已存在借用，无法创建可变借用（aliasing XOR mutation）",
                        owner
                    ));
                    return;
                }
            }
        }

        self.borrows
            .insert(borrower.to_string(), (owner.to_string(), kind));
        self.set_state(owner, OwnershipState::Borrowed);
        self.declare_var(borrower, false, false, "pointer");
    }

    fn end_borrow(&mut self, borrower: &str) {
        if let Some((owner, _)) = self.borrows.remove(borrower) {
            self.mut_vars.remove(borrower);
            self.var_types.remove(borrower);
            if self.active_borrows_of(&owner).is_empty() {
                self.set_state(&owner, OwnershipState::Owned);
            }
        }
    }

    pub fn check_assign(&mut self, target: &Expr, value: &Expr, line: usize, col: usize) {
        // 检查赋值目标是否可变
        let target_name = match target {
            Expr::Identifier(name, _, _) => Some(name.as_str()),
            _ => None,
        };
        if let Some(name) = target_name {
            if !self.is_var_mut(name) {
                self.errors.push(format!(
                    "不能对不可变变量 '{}' 赋值\n {} | {}",
                    name,
                    line,
                    self.get_src_line(line)
                ));
                return;
            }
        }

        // 若目标正被借用，则无法赋值
        if let Expr::Identifier(name, _, _) = target {
            if self.get_state(name) == Some(OwnershipState::Borrowed) {
                self.errors.push(format!(
                    "变量 '{}' 存在活跃借用，无法赋值",
                    name
                ));
                return;
            }
        }

        match value {
            Expr::NewExpr(_, _, _, _, _) => {
                if let Expr::Identifier(name, _, _) = target {
                    if self.get_state(name) == Some(OwnershipState::Owned)
                        && self.heap_vars.contains(name)
                    {
                        self.warnings.push(format!(
                            "变量 '{}' 持有堆所有权，赋值前请先释放（内存泄漏风险）",
                            name
                        ));
                    }
                    self.declare_var(name, true, true, "pointer");
                }
            }
            Expr::Identifier(src, _, _) => {
                if let Expr::Identifier(name, _, _) = target {
                    let src_is_copy = self
                        .var_type(src)
                        .map(|t| Self::is_copy_type(t))
                        .unwrap_or(false);
                    if self.get_state(src) == Some(OwnershipState::Owned)
                        && self.heap_vars.contains(src)
                        && !src_is_copy
                    {
                        self.do_move(src, name);
                    } else {
                        self.check_use(src, line, col);
                    }
                }
            }
            Expr::MoveExpr(target_expr, _, _) => {
                if let Expr::Identifier(src, _, _) = target_expr.as_ref() {
                    if let Expr::Identifier(name, _, _) = target {
                        if self.check_move(src, line, col) {
                            self.do_move(src, name);
                        }
                    }
                } else {
                    self.errors.push("move 只能应用于标识符".to_string());
                }
            }
            Expr::AddressOf(operand, is_mut, _, _) => {
                if let Expr::Identifier(src, _, _) = operand.as_ref() {
                    if let Expr::Identifier(name, _, _) = target {
                        let kind = if *is_mut {
                            BorrowKind::Mutable
                        } else {
                            BorrowKind::Immutable
                        };
                        self.do_borrow(src, name, kind);
                    }
                }
            }
            _ => {
                if let Expr::Identifier(name, _, _) = target {
                    if self.get_state(name) == Some(OwnershipState::Owned)
                        && self.heap_vars.contains(name)
                    {
                        self.warnings.push(format!(
                            "变量 '{}' 持有堆所有权，覆盖赋值将导致内存泄漏",
                            name
                        ));
                    }
                }
            }
        }
    }

    fn get_src_line(&self, line: usize) -> String {
        get_src_line(&self.source, line)
    }

    pub fn check_ast(&mut self, ast: &[crate::parser::Stmt]) {
        for stmt in ast {
            self.check_stmt(stmt);
        }
    }

    fn check_stmt(&mut self, s: &crate::parser::Stmt) {
        match s {
            Expr::VarDecl(name, ty, value, is_mut, line, col) => {
                let type_name = match ty {
                    Some(t) => expr_to_type_name(t),
                    None => {
                        self.errors.push(format!(
                            "[line {}, col {}] 变量 '{}' 缺少类型注解，VX 为纯静态类型语言",
                            line, col, name
                        ));
                        "unknown".to_string()
                    }
                };
                match value.as_ref() {
                    Expr::NewExpr(_, _, _, _, _) => {
                        self.declare_var(name, true, *is_mut, &type_name);
                    }
                    Expr::Identifier(src, _, _) => {
                        let src_is_copy = self
                            .var_type(src)
                            .map(|t| Self::is_copy_type(t))
                            .unwrap_or(false);
                        if self.get_state(src) == Some(OwnershipState::Owned)
                            && self.heap_vars.contains(src)
                            && !src_is_copy
                        {
                            self.do_move(src, name);
                            if *is_mut {
                                self.mut_vars.insert(name.clone());
                            }
                            self.var_types.insert(name.clone(), type_name);
                        } else {
                            self.check_use(src, *line, *col);
                            self.declare_var(name, false, *is_mut, &type_name);
                        }
                    }
                    Expr::MoveExpr(target, _, _) => {
                        if let Expr::Identifier(src, _, _) = target.as_ref() {
                            if self.check_move(src, *line, *col) {
                                self.do_move(src, name);
                                if *is_mut {
                                    self.mut_vars.insert(name.clone());
                                }
                                self.var_types.insert(name.clone(), type_name);
                            }
                        }
                    }
                    Expr::AddressOf(operand, borrow_mut, l, _c) => {
                        if let Expr::Identifier(src, _, _) = operand.as_ref() {
                            if *borrow_mut && !self.is_var_mut(src) {
                                self.errors.push(format!(
                                    "不能从不可变变量 '{}' 创建可变借用\n {} | {}",
                                    src,
                                    l,
                                    self.get_src_line(*l)
                                ));
                            } else {
                                let kind = if *borrow_mut {
                                    BorrowKind::Mutable
                                } else {
                                    BorrowKind::Immutable
                                };
                                self.do_borrow(src, name, kind);
                            }
                        }
                    }
                    _ => {
                        self.declare_var(name, false, *is_mut, &type_name);
                    }
                }
            }
            Expr::Assign(target, _, value, line, col) => {
                self.check_assign(target, value, *line, *col);
            }
            Expr::ExprStmt(expr, line, col) => {
                self.check_expr(expr, *line, *col);
            }
            Expr::IfStmt(cond, body, elifs, else_body, _, _) => {
                self.push_scope();
                self.check_expr(cond, 0, 0);
                for stmt in body {
                    self.check_stmt(stmt);
                }
                self.pop_scope();
                for (c, b) in elifs {
                    self.push_scope();
                    self.check_expr(c, 0, 0);
                    for stmt in b {
                        self.check_stmt(stmt);
                    }
                    self.pop_scope();
                }
                if let Some(b) = else_body {
                    self.push_scope();
                    for stmt in b {
                        self.check_stmt(stmt);
                    }
                    self.pop_scope();
                }
            }
            Expr::WhileStmt(cond, body, _, _) => {
                self.push_scope();
                self.check_expr(cond, 0, 0);
                for stmt in body {
                    self.check_stmt(stmt);
                }
                self.pop_scope();
            }
            Expr::ForStmt(var, iter, body, _, _) => {
                self.push_scope();
                self.declare_var(var, false, true, "int");
                self.check_expr(iter, 0, 0);
                for stmt in body {
                    self.check_stmt(stmt);
                }
                self.pop_scope();
            }
            Expr::FuncDecl(_, _, params, _, body, _, _) => {
                self.push_scope();
                for (p, pt) in params {
                    self.declare_var(p, false, false, pt);
                }
                for stmt in body {
                    self.check_stmt(stmt);
                }
                self.pop_scope();
            }
            Expr::ReturnStmt(val, _line, _col) => {
                if let Some(box_e) = val.as_ref() {
                    if let Expr::Identifier(src, _, _) = box_e.as_ref() {
                        if self.get_state(src) == Some(OwnershipState::Owned)
                            && self.heap_vars.contains(src)
                        {
                            self.warnings.push(format!(
                                "返回堆变量 '{}' 会转移所有权，调用者需负责释放",
                                src
                            ));
                        }
                    }
                }
            }
            _ => {}
        }
    }

    fn check_expr(&mut self, e: &Expr, default_line: usize, default_col: usize) {
        match e {
            Expr::Identifier(name, l, c) => {
                self.check_use(name, *l, *c);
            }
            Expr::BinaryOp(_, left, right, _, _) => {
                self.check_expr(left, default_line, default_col);
                self.check_expr(right, default_line, default_col);
            }
            Expr::UnaryOp(_, operand, _, _) => {
                self.check_expr(operand, default_line, default_col);
            }
            Expr::CallExpr(callee, args, _, _) => {
                self.check_expr(callee, default_line, default_col);
                for a in args {
                    self.check_expr(a, default_line, default_col);
                }
            }
            Expr::PropertyAccess(obj, _, _, _) => {
                self.check_expr(obj, default_line, default_col);
            }
            Expr::IndexAccess(obj, index, _, _) => {
                self.check_expr(obj, default_line, default_col);
                self.check_expr(index, default_line, default_col);
            }
            Expr::Deref(op, _, _) => {
                self.check_expr(op, default_line, default_col);
            }
            Expr::Assign(target, _, value, line, col) => {
                self.check_assign(target, value, *line, *col);
            }
            Expr::AddressOf(op, is_mut, l, c) => {
                self.check_expr(op, *l, *c);
                // 创建匿名借用（表达式位置）需要检查原变量状态
                if let Expr::Identifier(name, _, _) = op.as_ref() {
                    if self.get_state(name) == Some(OwnershipState::Moved)
                        || self.get_state(name) == Some(OwnershipState::Freed)
                    {
                        self.errors.push(format!(
                            "变量 '{}' 已被移动/释放，无法借用\n {} | {}",
                            name,
                            l,
                            self.get_src_line(*l)
                        ));
                    }
                    if *is_mut && !self.is_var_mut(name) {
                        self.errors.push(format!(
                            "不能从不可变变量 '{}' 创建可变借用\n {} | {}",
                            name,
                            l,
                            self.get_src_line(*l)
                        ));
                    }
                }
            }
            _ => {}
        }
    }
}
