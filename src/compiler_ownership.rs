// ==================== 所有权检查器 ====================

use std::collections::HashMap;
use crate::parser::Expr;

#[derive(Debug, Clone, PartialEq)]
pub enum OwnershipState {
    Owned,
    Moved,
    Borrowed,
    Freed,
}

pub struct OwnershipChecker {
    source: String,
    scopes: Vec<HashMap<String, OwnershipState>>,
    heap_vars: std::collections::HashSet<String>,
    borrows: HashMap<String, String>,
    pub errors: Vec<String>,
}

impl OwnershipChecker {
    pub fn new(source: &str) -> Self {
        Self {
            source: source.to_string(),
            scopes: vec![HashMap::new()],
            heap_vars: std::collections::HashSet::new(),
            borrows: HashMap::new(),
            errors: Vec::new(),
        }
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
                self.errors.push(format!(
                    "堆变量 '{}' 在作用域结束时未被显式释放（可能内存泄漏），请调用 free({})",
                    var, var
                ));
            }
        }
        let to_remove: Vec<String> = self
            .borrows
            .iter()
            .filter(|(_, o)| scope.contains_key(o.as_str()))
            .map(|(b, _)| b.clone())
            .collect();
        for b in to_remove {
            self.borrows.remove(&b);
        }
    }
    pub fn declare_var(&mut self, name: &str, is_heap: bool) {
        self.scopes
            .last_mut()
            .unwrap()
            .insert(name.to_string(), OwnershipState::Owned);
        if is_heap {
            self.heap_vars.insert(name.to_string());
        }
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
            if s == OwnershipState::Borrowed {
                self.errors.push(format!(
                    "变量 '{}' 存在活跃借用，无法释放（违反借用规则）",
                    name
                ));
                return false;
            }
            let active: Vec<_> = self
                .borrows
                .iter()
                .filter(|(_, o)| *o == name)
                .map(|(b, _)| b.clone())
                .collect();
            if !active.is_empty() {
                self.errors.push(format!(
                    "变量 '{}' 存在活跃借用 {:?}，无法释放",
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
        self.declare_var(dst, is_heap);
    }
    pub fn do_borrow(&mut self, owner: &str, borrower: &str) {
        self.borrows.insert(borrower.to_string(), owner.to_string());
        self.set_state(owner, OwnershipState::Borrowed);
        self.declare_var(borrower, false);
    }
    pub fn check_assign(&mut self, name: &str, value: &Expr, line: usize, col: usize) {
        match value {
            Expr::NewzExpr(_, _, _, _, _) => {
                if self.get_state(name) == Some(OwnershipState::Owned)
                    && self.heap_vars.contains(name)
                {
                    self.errors.push(format!(
                        "变量 '{}' 持有堆所有权，赋值前请先释放（内存泄漏风险）",
                        name
                    ));
                }
                self.declare_var(name, true);
            }
            Expr::Identifier(src, _, _) => {
                if self.get_state(src) == Some(OwnershipState::Owned)
                    && self.heap_vars.contains(src)
                {
                    self.do_move(src, name);
                } else {
                    self.check_use(src, line, col);
                }
            }
            Expr::MoveExpr(target, _, _) => {
                if let Expr::Identifier(src, _, _) = target.as_ref() {
                    self.do_move(src, name);
                } else {
                    self.errors.push("move 只能应用于标识符".to_string());
                }
            }
            Expr::AddressOf(operand, _, _) => {
                if let Expr::Identifier(src, _, _) = operand.as_ref() {
                    self.do_borrow(src, name);
                }
            }
            _ => {
                if self.get_state(name) == Some(OwnershipState::Owned)
                    && self.heap_vars.contains(name)
                {
                    self.errors.push(format!(
                        "变量 '{}' 持有堆所有权，覆盖赋值将导致内存泄漏",
                        name
                    ));
                }
            }
        }
    }
    fn get_src_line(&self, line: usize) -> String {
        if line > 0 && line <= self.source.lines().count() {
            self.source.lines().nth(line - 1).unwrap().to_string()
        } else {
            String::new()
        }
    }
    pub fn check_ast(&mut self, ast: &[crate::parser::Stmt]) {
        for stmt in ast {
            self._check_stmt(stmt);
        }
    }
    fn _check_stmt(&mut self, s: &crate::parser::Stmt) {
        match s {
            Expr::VarDecl(name, _, value, _, line, col) => match value.as_ref() {
                Expr::NewzExpr(_, _, _, _, _) => self.declare_var(name, true),
                Expr::Identifier(src, _, _) => {
                    if self.get_state(src) == Some(OwnershipState::Owned)
                        && self.heap_vars.contains(src)
                    {
                        self.do_move(src, name);
                    } else {
                        self.check_use(src, *line, *col);
                        self.declare_var(name, false);
                    }
                }
                Expr::MoveExpr(target, _, _) => {
                    if let Expr::Identifier(src, _, _) = target.as_ref() {
                        if self.check_move(src, *line, *col) {
                            self.do_move(src, name);
                        }
                    }
                }
                Expr::AddressOf(operand, _, _) => {
                    if let Expr::Identifier(src, _, _) = operand.as_ref() {
                        self.do_borrow(src, name);
                        self.declare_var(name, false);
                    }
                }
                _ => {
                    self.declare_var(name, false);
                }
            },
            Expr::Assign(target, _, value, line, col) => {
                if let Expr::Identifier(name, _, _) = target.as_ref() {
                    self.check_assign(name, value, *line, *col);
                }
            }
            Expr::FreeStmt(target, line, col) => {
                if let Expr::Identifier(name, _, _) = target.as_ref() {
                    if !self.heap_vars.contains(name.as_str()) {
                        self.errors
                            .push(format!("变量 '{}' 不是堆指针，无法释放", name));
                    } else if self.check_free(name, *line, *col) {
                        self.do_free(name);
                    }
                } else if let Expr::Deref(op, _, _) = target.as_ref() {
                    if let Expr::Identifier(name, _, _) = op.as_ref() {
                        if self.check_free(name, *line, *col) {
                            self.do_free(name);
                        }
                    }
                } else {
                    self.errors
                        .push("free 只能应用于堆指针标识符或解引用".to_string());
                }
            }
            Expr::ExprStmt(expr, line, col) => {
                if let Expr::Identifier(name, _, _) = expr.as_ref() {
                    self.check_use(name, *line, *col);
                }
            }
            Expr::IfStmt(cond, body, elifs, else_body, _, _) => {
                self.push_scope();
                self._check_expr(cond);
                for stmt in body {
                    self._check_stmt(stmt);
                }
                self.pop_scope();
                for (c, b) in elifs {
                    self.push_scope();
                    self._check_expr(c);
                    for stmt in b {
                        self._check_stmt(stmt);
                    }
                    self.pop_scope();
                }
                if let Some(b) = else_body {
                    self.push_scope();
                    for stmt in b {
                        self._check_stmt(stmt);
                    }
                    self.pop_scope();
                }
            }
            Expr::WhileStmt(cond, body, _, _) => {
                self.push_scope();
                self._check_expr(cond);
                for stmt in body {
                    self._check_stmt(stmt);
                }
                self.pop_scope();
            }
            Expr::ForStmt(var, iter, body, _, _) => {
                self.push_scope();
                self.declare_var(var, false);
                self._check_expr(iter);
                for stmt in body {
                    self._check_stmt(stmt);
                }
                self.pop_scope();
            }
            Expr::FuncDecl(_, params, _, body, _, _) => {
                self.push_scope();
                for (p, _) in params {
                    self.declare_var(p, false);
                }
                for stmt in body {
                    self._check_stmt(stmt);
                }
                self.pop_scope();
            }
            Expr::ReturnStmt(val, _line, _col) => {
                if let Some(box_e) = val.as_ref() {
                    if let Expr::Identifier(src, _, _) = box_e.as_ref() {
                        if self.get_state(src) == Some(OwnershipState::Owned)
                            && self.heap_vars.contains(src)
                        {
                            self.errors.push(format!(
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
    fn _check_expr(&mut self, e: &Expr) {
        match e {
            Expr::Identifier(name, l, c) => {
                self.check_use(name, *l, *c);
            }
            Expr::BinaryOp(_, left, right, _, _) => {
                self._check_expr(left);
                self._check_expr(right);
            }
            Expr::UnaryOp(_, operand, _, _) => {
                self._check_expr(operand);
            }
            Expr::CallExpr(_, args, _, _) => {
                for a in args {
                    if let Expr::Identifier(name, l, c) = a.as_ref() {
                        self.check_use(name, *l, *c);
                    }
                }
            }
            Expr::PropertyAccess(obj, _, _, _) => {
                self._check_expr(obj);
            }
            Expr::IndexAccess(obj, index, _, _) => {
                self._check_expr(obj);
                self._check_expr(index);
            }
            Expr::Deref(op, _, _) => {
                if let Expr::Identifier(name, l, c) = op.as_ref() {
                    self.check_use(name, *l, *c);
                }
            }
            Expr::AddressOf(op, _, _) => {
                if let Expr::Identifier(name, l, c) = op.as_ref() {
                    self.check_use(name, *l, *c);
                }
            }
            _ => {}
        }
    }
}
