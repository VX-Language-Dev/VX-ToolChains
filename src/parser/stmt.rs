// VX Language Compiler v3.0 - 语句解析
// 负责所有语句的递归下降解析 + parse_block + 顶层 parse

use crate::token::{TokenType, VXError};
use super::ast::{Expr, Stmt, e_line, e_col, expr_to_type_name};
use super::Parser;

impl Parser {
    pub fn parse_statement(&mut self) -> Result<Stmt, VXError> {
        self.skip_newlines();
        match self.current().kind {
            TokenType::Struct => self.parse_struct_decl(),
            TokenType::Class => self.parse_class_decl(),
            TokenType::Enum => self.parse_enum_decl(),
            TokenType::Union => self.parse_union_decl(),
            TokenType::Identifier if self.peek(1).kind == TokenType::Colon => self.parse_var_decl(),
            TokenType::Import => self.parse_import_stmt(),
            TokenType::Func => self.parse_func_decl(),
            TokenType::If => self.parse_if_stmt(),
            TokenType::While => self.parse_while_stmt(),
            TokenType::For => self.parse_for_stmt(),
            TokenType::Return => self.parse_return_stmt(),
            TokenType::Break => {
                let t = self.advance();
                Ok(Expr::BreakStmt(t.line, t.col))
            }
            TokenType::Continue => {
                let t = self.advance();
                Ok(Expr::ContinueStmt(t.line, t.col))
            }
            TokenType::Free => self.parse_free_stmt(),
            TokenType::VarT => self.parse_var_decl_inferred(),
            _ => {
                let e = self.parse_expression()?;
                Ok(Expr::ExprStmt(Box::new(e.clone()), e_line(&e), e_col(&e)))
            }
        }
    }

    fn parse_struct_decl(&mut self) -> Result<Stmt, VXError> {
        let t = self.advance();
        let (l, c) = (t.line, t.col);
        let n = self.expect(TokenType::Identifier, None)?.value;
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
                let fn_name = self.expect(TokenType::Identifier, None)?.value;
                self.expect(TokenType::Colon, None)?;
                let ft = self.parse_type()?;
                f.push((expr_to_type_name(&ft), fn_name));
            }
        }
        self.expect(TokenType::Dedent, None)?;
        Ok(Expr::StructDecl(n, f, m, l, c))
    }

    fn parse_class_decl(&mut self) -> Result<Stmt, VXError> {
        let t = self.advance();
        let (l, c) = (t.line, t.col);
        let n = self.expect(TokenType::Identifier, None)?.value;
        let (mut p, mut ii) = (None, vec![]);
        if self.current().kind == TokenType::Extends {
            self.advance();
            p = Some(self.expect(TokenType::Identifier, None)?.value);
        }
        if self.current().kind == TokenType::Implements {
            self.advance();
            ii.push(self.expect(TokenType::Identifier, None)?.value);
            while self.current().kind == TokenType::Comma {
                self.advance();
                ii.push(self.expect(TokenType::Identifier, None)?.value);
            }
        }
        self.expect(TokenType::Colon, None)?;
        self.skip_newlines();
        self.expect(TokenType::Indent, None)?;
        let mut f = vec![];
        let mut m = vec![];
        while !self.match_kind(&[TokenType::Dedent, TokenType::EOF]) {
            self.skip_newlines();
            if self.match_kind(&[TokenType::Dedent, TokenType::EOF]) {
                break;
            }
            let mut acc = "public".to_string();
            match self.current().kind {
                TokenType::Public => {
                    self.advance();
                }
                TokenType::Private => {
                    self.advance();
                    acc = "private".into();
                }
                TokenType::Protected => {
                    self.advance();
                    acc = "protected".into();
                }
                _ => {}
            }
            if self.current().kind == TokenType::Func {
                m.push(Box::new(self.parse_func_decl()?));
            } else {
                let fn_name = self.expect(TokenType::Identifier, None)?.value;
                self.expect(TokenType::Colon, None)?;
                let ft = self.parse_type()?;
                f.push((expr_to_type_name(&ft), fn_name, acc));
            }
        }
        self.expect(TokenType::Dedent, None)?;
        Ok(Expr::ClassDecl(n, f, m, p, ii, l, c))
    }

    fn parse_enum_decl(&mut self) -> Result<Stmt, VXError> {
        let t = self.advance();
        let (l, c) = (t.line, t.col);
        let n = self.expect(TokenType::Identifier, None)?.value;
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
            let vn = self.expect(TokenType::Identifier, None)?.value;
            let mut vv = auto;
            if self.current().kind == TokenType::Assign {
                self.advance();
                vv = self.expect(TokenType::Int, None)?.value.parse().unwrap();
            }
            v.push((vn, vv));
            auto = vv + 1;
        }
        self.expect(TokenType::Dedent, None)?;
        Ok(Expr::EnumDecl(n, v, l, c))
    }

    fn parse_union_decl(&mut self) -> Result<Stmt, VXError> {
        let t = self.advance();
        let (l, c) = (t.line, t.col);
        let n = self.expect(TokenType::Identifier, None)?.value;
        self.expect(TokenType::Colon, None)?;
        self.skip_newlines();
        self.expect(TokenType::Indent, None)?;
        let mut f = vec![];
        while !self.match_kind(&[TokenType::Dedent, TokenType::EOF]) {
            self.skip_newlines();
            if self.match_kind(&[TokenType::Dedent, TokenType::EOF]) {
                break;
            }
            let fn_name = self.expect(TokenType::Identifier, None)?.value;
            self.expect(TokenType::Colon, None)?;
            f.push((expr_to_type_name(&self.parse_type()?), fn_name));
        }
        self.expect(TokenType::Dedent, None)?;
        Ok(Expr::UnionDecl(n, f, l, c))
    }

    fn parse_var_decl(&mut self) -> Result<Stmt, VXError> {
        let nm = self.expect(TokenType::Identifier, None)?.value;
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
            false,
            l,
            c,
        ))
    }

    fn parse_var_decl_inferred(&mut self) -> Result<Stmt, VXError> {
        let t = self.advance(); // consume 'var'
        let nm = self.expect(TokenType::Identifier, None)?.value;
        let mut v = Expr::NilLiteral(t.line, t.col);
        if self.current().kind == TokenType::Assign {
            self.advance();
            v = self.parse_expression()?;
        }
        let (l, c) = (t.line, t.col);
        Ok(Expr::VarDecl(nm, None, Box::new(v), false, l, c))
    }

    fn parse_func_decl(&mut self) -> Result<Stmt, VXError> {
        let t = self.advance();
        let (l, c) = (t.line, t.col);
        let n = self.expect(TokenType::Identifier, None)?.value;
        self.expect(TokenType::LParen, None)?;
        let mut p = vec![];
        if !self.match_kind(&[TokenType::RParen]) {
            let pn = self.expect(TokenType::Identifier, None)?.value;
            self.expect(TokenType::Colon, None)?;
            let pt = expr_to_type_name(&self.parse_type()?);
            p.push((pn, pt));
            while self.current().kind == TokenType::Comma {
                self.advance();
                let pn = self.expect(TokenType::Identifier, None)?.value;
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
        Ok(Expr::FuncDecl(n, p, rt, b, l, c))
    }

    fn parse_if_stmt(&mut self) -> Result<Stmt, VXError> {
        let t = self.advance();
        let (l, c) = (t.line, t.col);
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

    fn parse_for_stmt(&mut self) -> Result<Stmt, VXError> {
        let t = self.advance();
        let (l, c) = (t.line, t.col);
        let var = self.expect(TokenType::Identifier, None)?.value;
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

    fn parse_free_stmt(&mut self) -> Result<Stmt, VXError> {
        let t = self.advance();
        let (l, c) = (t.line, t.col);
        let target = self.parse_expression()?;
        Ok(Expr::FreeStmt(Box::new(target), l, c))
    }

    fn parse_import_stmt(&mut self) -> Result<Stmt, VXError> {
        let t = self.advance();
        let (l, c) = (t.line, t.col);
        let nm = self.expect(TokenType::Identifier, None)?.value;
        let (mut al, mut di) = (None, None);
        while self.match_kind(&[TokenType::As, TokenType::Dirs]) {
            if self.current().kind == TokenType::As {
                self.advance();
                al = Some(self.expect(TokenType::Identifier, None)?.value);
            } else {
                self.advance();
                di = Some(self.expect(TokenType::String, None)?.value);
            }
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
        while !self.match_kind(&[TokenType::Dedent, TokenType::EOF]) {
            self.skip_newlines();
            if self.match_kind(&[TokenType::Dedent, TokenType::EOF]) {
                break;
            }
            st.push(Box::new(self.parse_statement()?));
        }
        if self.current().kind == TokenType::Dedent {
            self.advance();
        }
        Ok(st)
    }

    pub fn parse(&mut self) -> Result<Vec<Stmt>, VXError> {
        let mut st = vec![];
        while !self.match_kind(&[TokenType::EOF]) {
            self.skip_newlines();
            if self.match_kind(&[TokenType::EOF]) {
                break;
            }
            st.push(self.parse_statement()?);
        }
        Ok(st)
    }
}
