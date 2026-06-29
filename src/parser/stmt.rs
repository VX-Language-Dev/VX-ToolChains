// VX Language Compiler v3.0 - 语句解析
// 负责所有语句的递归下降解析 + parse_block + 顶层 parse

use crate::token::{TokenType, VXError};
use super::ast::{Expr, Stmt, e_line, e_col, expr_to_type_name};
use super::Parser;

impl Parser {
    pub fn parse_statement(&mut self) -> Result<Stmt, VXError> {
        self.skip_newlines();
        // 自举兼容: 跳过残留的 Indent/Dedent token (嵌套块结束时)
        while matches!(self.current().kind, TokenType::Indent | TokenType::Dedent) {
            self.advance();
            self.skip_newlines();
        }
        match self.current().kind {
            TokenType::Struct => self.parse_struct_decl(),
            TokenType::Class => self.parse_class_decl(),
            TokenType::Enum => self.parse_enum_decl(),
            TokenType::Union => self.parse_union_decl(),
            TokenType::Macro => self.parse_macro_def(),  // 宏定义
            TokenType::Hash => self.parse_macro_call_stmt(),  // 宏调用（作为语句）
            TokenType::Identifier if self.peek(1).kind == TokenType::Colon => self.parse_var_decl(),
            TokenType::Mut => self.parse_var_decl(),
            TokenType::Import => self.parse_import_stmt(),
            TokenType::Func => self.parse_func_decl(),
            TokenType::If => self.parse_if_stmt(),
            TokenType::Else | TokenType::Elif => {
                // 自举兼容: else/elif 出现在 parse_block while 循环中通常表示
                // 缩进跟踪错误; 这里跳过以让上层 parse_if_stmt 正确处理
                // (但 parse_if_stmt 自身在调用 parse_block 后会处理 else/elif,
                // 所以这里走到 default 是异常路径)
                return Err(VXError {
                    msg: format!("意外的 {:?} (else/elif 必须紧跟 if/elif 块)", self.current().kind),
                    line: self.current().line,
                    col: self.current().col,
                    source: Some(self.source.clone()),
                });
            }
            TokenType::Identifier if self.current().value == "match" => self.parse_match_stmt(),
            TokenType::While => self.parse_while_stmt(),
            TokenType::For => self.parse_for_stmt(),
            TokenType::Return => self.parse_return_stmt(),
            TokenType::Loop => self.parse_loop_stmt(),
            TokenType::Break => {
                let t = self.advance();
                let label = if self.current().kind == TokenType::Identifier
                    || self.current().kind == TokenType::Loop
                {
                    Some(self.advance().value)
                } else {
                    None
                };
                Ok(Expr::BreakStmt(label, t.line, t.col))
            }
            TokenType::Continue => {
                let t = self.advance();
                let label = if self.current().kind == TokenType::Identifier
                    || self.current().kind == TokenType::Loop
                {
                    Some(self.advance().value)
                } else {
                    None
                };
                Ok(Expr::ContinueStmt(label, t.line, t.col))
            }
            // Free 已裁减 → mem::free(ptr) 标准库函数调用, 作为普通函数调用处理
            TokenType::VarT => {
                let t = self.advance();
                Err(VXError {
                    msg: "var 类型推断已移除，VX 为纯静态类型语言，请使用 `name: Type = value` 语法".to_string(),
                    line: t.line,
                    col: t.col,
                    source: Some(self.source.clone()),
                })
            }
            _ => {
                let e = self.parse_expression()?;
                Ok(Expr::ExprStmt(Box::new(e.clone()), e_line(&e), e_col(&e)))
            }
        }
    }

    fn parse_struct_decl(&mut self) -> Result<Stmt, VXError> {
        let t = self.advance();
        let (l, c) = (t.line, t.col);
        let n = self.expect_identifier_or_keyword()?.value;
        let gp = self.parse_generic_params()?;
        self.expect(TokenType::Colon, None)?;
        self.skip_newlines();
        let mut f = vec![];
        let mut m = vec![];
        self.expect(TokenType::Indent, None)?;
        while !self.match_kind(&[TokenType::Dedent, TokenType::EOF]) {
            self.skip_newlines();
            if self.match_kind(&[TokenType::Dedent, TokenType::EOF]) {
                break;
            }
            if self.current().kind == TokenType::Func {
                m.push(Box::new(self.parse_func_decl()?));
            } else {
                let fn_name = self.expect_identifier_or_keyword()?.value;
                self.expect(TokenType::Colon, None)?;
                let ft = self.parse_type()?;
                f.push((expr_to_type_name(&ft), fn_name));
            }
        }
        // 自举兼容: 消费所有连续 Dedent
        while self.current().kind == TokenType::Dedent {
            self.advance();
        }
        Ok(Expr::StructDecl(n, gp, f, m, l, c))
    }

    fn parse_class_decl(&mut self) -> Result<Stmt, VXError> {
        let t = self.advance();
        let (l, c) = (t.line, t.col);
        let n = self.expect_identifier_or_keyword()?.value;
        let gp = self.parse_generic_params()?;
        // 冒号继承语法: class Dog : Animal, Canine :
        // 单冒号形式: class Foo :        (无父类，紧跟字段缩进块)
        // 双冒号形式: class Foo : Base : (父类 Base + 字段缩进块)
        let (mut p, mut ii) = (None, vec![]);
        if self.current().kind == TokenType::Colon {
            self.advance(); // 消费第一个冒号
            self.skip_newlines();
            if self.current().kind == TokenType::Identifier {
                // 父类列表: class Foo : Base, Trait :
                p = Some(self.expect_identifier_or_keyword()?.value);
                while self.current().kind == TokenType::Comma {
                    self.advance();
                    ii.push(self.expect_identifier_or_keyword()?.value);
                }
                if self.current().kind == TokenType::Colon {
                    self.advance();
                }
            }
            // 否则单冒号无父类，已消费冒号，直接进入缩进块
        }
        self.skip_newlines();
        self.expect(TokenType::Indent, None)?;
        let mut f = vec![];
        let mut m = vec![];
        while !self.match_kind(&[TokenType::Dedent, TokenType::EOF]) {
            self.skip_newlines();
            if self.match_kind(&[TokenType::Dedent, TokenType::EOF]) {
                break;
            }
            // 访问修饰符已裁减 → 默认 public, 不解析 private/protected 关键字
            let acc = "public".to_string();
            if self.current().kind == TokenType::Func {
                m.push(Box::new(self.parse_func_decl()?));
            } else {
                // 字段: 支持 name: Type / name = expr / name: Type = expr
                let fn_name = self.expect_identifier_or_keyword()?.value;
                if self.current().kind == TokenType::Colon {
                    self.advance();
                    let ft = self.parse_type()?;
                    // 可选 = 默认值
                    if self.current().kind == TokenType::Assign {
                        self.advance();
                        let _init = self.parse_expression()?;
                    }
                    f.push((expr_to_type_name(&ft), fn_name, acc));
                } else if self.current().kind == TokenType::Assign {
                    // VX 为纯静态类型语言，类字段必须显式声明类型
                    return Err(VXError {
                        msg: "类字段必须提供类型注解（VX 已移除 var 动态类型）".to_string(),
                        line: self.current().line,
                        col: self.current().col,
                        source: Some(self.source.clone()),
                    });
                } else {
                    return Err(VXError {
                        msg: "类字段声明需要类型注解或默认值".to_string(),
                        line: self.current().line,
                        col: self.current().col,
                        source: Some(self.source.clone()),
                    });
                }
            }
        }
        // 自举兼容: 消费所有连续 Dedent token (class body 可能嵌套多级缩进)
        while self.current().kind == TokenType::Dedent {
            self.advance();
        }
        Ok(Expr::ClassDecl(n, gp, f, m, p, ii, l, c))
    }

    fn parse_enum_decl(&mut self) -> Result<Stmt, VXError> {
        let t = self.advance();
        let (l, c) = (t.line, t.col);
        let n = self.expect_identifier_or_keyword()?.value;
        self.expect(TokenType::Colon, None)?;
        self.skip_newlines();
        self.expect(TokenType::Indent, None)?;
        let mut v = vec![];
        let mut auto = 0;
        while !self.match_kind(&[TokenType::Dedent, TokenType::EOF]) {
            self.skip_newlines();
            if self.match_kind(&[TokenType::Dedent, TokenType::EOF]) {
                break;
            }
            let vn = self.expect_identifier_or_keyword()?.value;
            let mut vv = auto;
            if self.current().kind == TokenType::Assign {
                self.advance();
                vv = self.expect(TokenType::Int, None)?.value.parse().unwrap();
            }
            v.push((vn, vv));
            auto = vv + 1;
        }
        while self.current().kind == TokenType::Dedent {
            self.advance();
        }
        Ok(Expr::EnumDecl(n, v, l, c))
    }

    fn parse_union_decl(&mut self) -> Result<Stmt, VXError> {
        let t = self.advance();
        let (l, c) = (t.line, t.col);
        let n = self.expect_identifier_or_keyword()?.value;
        self.expect(TokenType::Colon, None)?;
        self.skip_newlines();
        self.expect(TokenType::Indent, None)?;
        let mut f = vec![];
        while !self.match_kind(&[TokenType::Dedent, TokenType::EOF]) {
            self.skip_newlines();
            if self.match_kind(&[TokenType::Dedent, TokenType::EOF]) {
                break;
            }
            let fn_name = self.expect_identifier_or_keyword()?.value;
            self.expect(TokenType::Colon, None)?;
            f.push((expr_to_type_name(&self.parse_type()?), fn_name));
        }
        while self.current().kind == TokenType::Dedent {
            self.advance();
        }
        Ok(Expr::UnionDecl(n, f, l, c))
    }

    fn parse_var_decl(&mut self) -> Result<Stmt, VXError> {
        let is_mut = self.current().kind == TokenType::Mut;
        if is_mut {
            self.advance();
        }
        let nm = self.expect_identifier_or_keyword()?.value;
        self.expect(TokenType::Colon, None)?;
        let mut th = self.parse_type()?;
        let (l, c) = (e_line(&th), e_col(&th));
        while self.current().kind == TokenType::Star {
            self.advance();
            th = Expr::TypeExpr("pointer".into(), l, c);
        }
        let mut v = Expr::NilLiteral(l, c);
        if self.current().kind == TokenType::Assign {
            self.advance();
            v = self.parse_expression()?;
        }
        Ok(Expr::VarDecl(
            nm,
            Some(Box::new(th)),
            Box::new(v),
            is_mut,
            l,
            c,
        ))
    }

    fn parse_func_decl(&mut self) -> Result<Stmt, VXError> {
        let t = self.advance();
        let (l, c) = (t.line, t.col);
        let n = self.expect_identifier_or_keyword()?.value;
        let gp = self.parse_generic_params()?;
        self.expect(TokenType::LParen, None)?;
        let mut p = vec![];
        if !self.match_kind(&[TokenType::RParen]) {
            let pn = self.expect_identifier_or_keyword()?.value;
            self.expect(TokenType::Colon, None)?;
            let pt = expr_to_type_name(&self.parse_type()?);
            p.push((pn, pt));
            while self.current().kind == TokenType::Comma {
                self.advance();
                let pn = self.expect_identifier_or_keyword()?.value;
                self.expect(TokenType::Colon, None)?;
                let pt = expr_to_type_name(&self.parse_type()?);
                p.push((pn, pt));
            }
        }
        self.expect(TokenType::RParen, None)?;
        let mut rt = None;
        if self.current().kind == TokenType::Arrow {
            self.advance();
            rt = Some(expr_to_type_name(&self.parse_type()?));
        }
        let b = self.parse_block()?;
        Ok(Expr::FuncDecl(n, gp, p, rt, b, l, c))
    }

    fn parse_if_stmt(&mut self) -> Result<Stmt, VXError> {
        let t = self.advance();
        let (l, c) = (t.line, t.col);
        // 多行 if 条件由 parse_or/parse_and 中的 peek_continuation_after_binary_op
        // 机制处理, 这里直接调用 parse_expression 即可。
        let cond = self.parse_expression()?;
        let body = self.parse_block()?;
        let mut elifs: Vec<(Box<Expr>, Vec<Box<Stmt>>)> = vec![];
        self.skip_newlines();
        while self.current().kind == TokenType::Elif {
            self.advance();
            let ec = self.parse_expression()?;
            let eb = self.parse_block()?;
            elifs.push((Box::new(ec), eb));
            self.skip_newlines();
        }
        let mut ebody: Option<Vec<Box<Stmt>>> = None;
        if self.current().kind == TokenType::Else {
            self.advance();
            self.skip_newlines();
            ebody = Some(self.parse_block()?);
        }
        Ok(Expr::IfStmt(Box::new(cond), body, elifs, ebody, l, c))
    }

    fn parse_match_stmt(&mut self) -> Result<Stmt, VXError> {
        let t = self.advance(); // 消费上下文标识符 match
        let (l, c) = (t.line, t.col);
        let subject = self.parse_expression()?;
        let arms = self.parse_match_arms()?;
        Ok(Expr::MatchStmt(Box::new(subject), arms, l, c))
    }

    fn parse_match_arms(&mut self) -> Result<Vec<(Box<Expr>, Vec<Box<Stmt>>)>, VXError> {
        self.expect(TokenType::Colon, Some("期望 match 后的 ':'"))?;
        self.skip_newlines();
        if !self.match_kind(&[TokenType::Indent]) {
            let arm = self.parse_match_arm()?;
            return Ok(vec![arm]);
        }
        self.advance(); // 消费 Indent
        let mut arms: Vec<(Box<Expr>, Vec<Box<Stmt>>)> = vec![];
        while !self.match_kind(&[TokenType::Dedent, TokenType::EOF]) {
            self.skip_newlines();
            if self.match_kind(&[TokenType::Dedent, TokenType::EOF]) {
                break;
            }
            arms.push(self.parse_match_arm()?);
        }
        self.expect(TokenType::Dedent, None)?;
        Ok(arms)
    }

    fn parse_match_arm(&mut self) -> Result<(Box<Expr>, Vec<Box<Stmt>>), VXError> {
        let pattern = self.parse_expression()?;
        self.expect(TokenType::Colon, Some("期望分支模式后的 ':'"))?;
        let body = self.parse_block()?;
        Ok((Box::new(pattern), body))
    }

    fn parse_for_stmt(&mut self) -> Result<Stmt, VXError> {
        let t = self.advance();
        let (l, c) = (t.line, t.col);
        let var = self.expect_identifier_or_keyword()?.value;
        self.expect(TokenType::In, None)?;
        let it = self.parse_expression()?;
        let body = self.parse_block()?;
        Ok(Expr::ForStmt(var, Box::new(it), body, l, c))
    }

    fn parse_while_stmt(&mut self) -> Result<Stmt, VXError> {
        let t = self.advance();
        let (l, c) = (t.line, t.col);
        let cond = self.parse_expression()?;
        let body = self.parse_block()?;
        Ok(Expr::WhileStmt(Box::new(cond), body, l, c))
    }

    fn parse_loop_stmt(&mut self) -> Result<Stmt, VXError> {
        let t = self.advance();
        let (l, c) = (t.line, t.col);
        let label = if self.current().kind == TokenType::Identifier {
            Some(self.advance().value)
        } else {
            None
        };
        self.expect(TokenType::Colon, Some("期望 ':'"))?;
        let body = self.parse_block()?;
        Ok(Expr::LoopStmt(label, body, l, c))
    }

    fn parse_return_stmt(&mut self) -> Result<Stmt, VXError> {
        let t = self.advance();
        let (l, c) = (t.line, t.col);
        let v = if !self.match_kind(&[TokenType::Newline, TokenType::Dedent, TokenType::EOF]) {
            Some(Box::new(self.parse_expression()?))
        } else {
            None
        };
        Ok(Expr::ReturnStmt(v, l, c))
    }

    // parse_free_stmt 已裁减 → 标准库 mem::free(ptr) 函数调用

    fn parse_import_stmt(&mut self) -> Result<Stmt, VXError> {
        let t = self.advance();
        let (l, c) = (t.line, t.col);
        // 支持点分路径: import std.error / import bootstrap.lexer / import std.collections.vec
        // 在原生 lexer 中, 'std' 是 Identifier, '.' 是 Dot, 'error' 是 Identifier。
        // 这里循环吞掉 "Identifier ('.' Identifier)*" 序列, 合并为单一名 'std.error'。
        let mut nm = self.expect_identifier_or_keyword()?.value;
        while self.current().kind == TokenType::Dot {
            self.advance(); // 消费 '.'
            let next = self.expect_identifier_or_keyword()?.value;
            nm = format!("{}.{}", nm, next);
        }
        let mut al = None;
        // dirs 已裁减 → import 支持可变路径列表:
        //   import("a","b") as mod  → 多路径导入
        let mut di = vec![];
        // 移除 dirs 关键字检查, 仅保留 as 别名
        if self.current().kind == TokenType::As {
            self.advance();
            al = Some(self.expect_identifier_or_keyword()?.value);
        }
        // 如果导入名后跟有字符串字面量，收集为路径列表（旧 dirs 替代）
        while self.current().kind == TokenType::String {
            di.push(self.advance().value);
        }
        Ok(Expr::ImportStmt(nm, al, di, l, c))
    }

    pub fn parse_block(&mut self) -> Result<Vec<Box<Stmt>>, VXError> {
        let mut st: Vec<Box<Stmt>> = vec![];
        self.skip_newlines();
        if self.current().kind == TokenType::Colon {
            self.advance();
            self.skip_newlines();
            if !self.match_kind(&[TokenType::Indent]) {
                st.push(Box::new(self.parse_statement()?));
                return Ok(st);
            }
            self.advance();
        }
        while !self.match_kind(&[TokenType::Dedent, TokenType::EOF, TokenType::Else, TokenType::Elif]) {
            self.skip_newlines();
            if self.match_kind(&[TokenType::Dedent, TokenType::EOF, TokenType::Else, TokenType::Elif]) {
                break;
            }
            st.push(Box::new(self.parse_statement()?));
        }
        // parse_block 只应消费本层 block 结束的那一个 Dedent;
        // 外层 (class/func/if 等) 的 Dedent 留给调用者处理。
        if self.current().kind == TokenType::Dedent {
            self.advance();
        }
        Ok(st)
    }

    pub fn parse(&mut self) -> Result<Vec<Stmt>, VXError> {
        let mut st = vec![];
        while !self.match_kind(&[TokenType::EOF]) {
            self.skip_newlines();
            // 自举兼容: 顶层循环跳过残留的 Dedent (嵌套块结束时缩进回退)
            while self.current().kind == TokenType::Dedent {
                self.advance();
            }
            if self.match_kind(&[TokenType::EOF]) {
                break;
            }
            st.push(self.parse_statement()?);
        }
        Ok(st)
    }

    // ==================== 宏系统解析 ====================

    /// 解析宏定义: macro name(params) { body }
    fn parse_macro_def(&mut self) -> Result<Stmt, VXError> {
        let t = self.advance();  // 跳过 'macro' 关键字
        let (l, c) = (t.line, t.col);

        // 解析宏名称
        let name = self.expect_identifier_or_keyword()?.value;

        // 解析参数列表 (...)
        self.expect(TokenType::LParen, Some("期望 '('"))?;

        let mut params = Vec::new();
        if self.current().kind != TokenType::RParen {
            loop {
                let param_token = self.expect_identifier_or_keyword()?;
                params.push(param_token.value);

                if self.current().kind == TokenType::RParen {
                    break;
                }

                self.expect(TokenType::Comma, Some("期望 ',' 或 ')'"))?;
            }
        }

        self.expect(TokenType::RParen, Some("期望 ')'"))?;
        
        // 解析宏体 {...}
        self.expect(TokenType::LBrace, Some("期望 '{'"))?;
        
        let mut body = Vec::new();
        while self.current().kind != TokenType::RBrace && self.current().kind != TokenType::EOF {
            let stmt = self.parse_statement()?;
            body.push(Box::new(stmt));
        }
        
        self.expect(TokenType::RBrace, Some("期望 '}'"))?;
        
        Ok(Expr::MacroDef(name, params, body, l, c))
    }

    /// 解析宏调用语句: #macro_name(args)
    fn parse_macro_call_stmt(&mut self) -> Result<Stmt, VXError> {
        let t = self.advance();  // 跳过 '#'
        let (l, c) = (t.line, t.col);
        
        // 解析宏名称
        let name = self.expect_identifier_or_keyword()?.value;

        // 解析参数列表 (...)
        self.expect(TokenType::LParen, Some("期望 '('"))?;

        let mut args = Vec::new();
        if self.current().kind != TokenType::RParen {
            loop {
                let arg = self.parse_expression()?;
                args.push(Box::new(arg));

                if self.current().kind == TokenType::RParen {
                    break;
                }

                self.expect(TokenType::Comma, Some("期望 ',' 或 ')'"))?;
            }
        }
        
        self.expect(TokenType::RParen, Some("期望 ')'"))?;
        
        Ok(Expr::MacroCall(name, args, l, c))
    }
}
