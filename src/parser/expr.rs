// VX Language Compiler v3.0 - 表达式解析
// 负责所有表达式的递归下降解析

use crate::token::{TokenType, VXError};
use super::ast::{Expr, e_line, e_col, expr_to_type_name};
use super::Parser;

impl Parser {
    pub fn parse_expression(&mut self) -> Result<Expr, VXError> {
        self.parse_assignment()
    }

    fn parse_assignment(&mut self) -> Result<Expr, VXError> {
        let e = self.parse_or()?;
        if self.match_kind(&[
            TokenType::Assign,
            TokenType::PlusAssign,
            TokenType::MinusAssign,
            TokenType::StarAssign,
            TokenType::SlashAssign,
            TokenType::PercentAssign,
            TokenType::PowerAssign,
        ]) {
            let (el, ec) = (e_line(&e), e_col(&e));
            if !matches!(
                &e,
                Expr::Identifier(..) | Expr::IndexAccess(..) | Expr::PropertyAccess(..)
            ) {
                return Err(VXError {
                    msg: "赋值目标必须是变量/索引/属性".to_string(),
                    line: el,
                    col: ec,
                    source: Some(self.source.clone()),
                });
            }
            let op = self.advance().value;
            let r = self.parse_assignment()?;
            return Ok(Expr::Assign(Box::new(e), op, Box::new(r), el, ec));
        }
        Ok(e)
    }

    fn parse_or(&mut self) -> Result<Expr, VXError> {
        let mut l = self.parse_and()?;
        while self.current().kind == TokenType::Or {
            // 窥探 or 后是否有合法表达式延续; 多行 if/while 条件中
            // `... or\n   ...:` 当下一行是 Dedent+Indent+Identifier 时, 也算延续
            if !self.peek_continuation_after_binary_op() {
                break;
            }
            let op = self.advance().value;
            // 消费 or 后若后续是 Newline/Dedent/Indent (多行续行), 静默跳过
            self.skip_newlines();
            while self.current().kind == TokenType::Dedent {
                self.advance();
                self.skip_newlines();
            }
            while self.current().kind == TokenType::Indent {
                self.advance();
                self.skip_newlines();
            }
            l = Expr::BinaryOp(
                op,
                Box::new(l.clone()),
                Box::new(self.parse_and()?),
                e_line(&l),
                e_col(&l),
            );
        }
        Ok(l)
    }

    fn parse_and(&mut self) -> Result<Expr, VXError> {
        let mut l = self.parse_equality()?;
        while self.current().kind == TokenType::And {
            if !self.peek_continuation_after_binary_op() {
                break;
            }
            let op = self.advance().value;
            self.skip_newlines();
            while self.current().kind == TokenType::Dedent {
                self.advance();
                self.skip_newlines();
            }
            while self.current().kind == TokenType::Indent {
                self.advance();
                self.skip_newlines();
            }
            l = Expr::BinaryOp(
                op,
                Box::new(l.clone()),
                Box::new(self.parse_equality()?),
                e_line(&l),
                e_col(&l),
            );
        }
        Ok(l)
    }

    fn parse_equality(&mut self) -> Result<Expr, VXError> {
        let mut l = self.parse_comparison()?;
        while self.match_kind(&[TokenType::Eq, TokenType::Ne]) {
            let op = self.advance().value;
            l = Expr::BinaryOp(
                op,
                Box::new(l.clone()),
                Box::new(self.parse_comparison()?),
                e_line(&l),
                e_col(&l),
            );
        }
        Ok(l)
    }

    fn parse_comparison(&mut self) -> Result<Expr, VXError> {
        let mut l = self.parse_additive()?;
        while self.match_kind(&[TokenType::Lt, TokenType::Gt, TokenType::Le, TokenType::Ge]) {
            let op = self.advance().value;
            l = Expr::BinaryOp(
                op,
                Box::new(l.clone()),
                Box::new(self.parse_additive()?),
                e_line(&l),
                e_col(&l),
            );
        }
        Ok(l)
    }

    fn parse_additive(&mut self) -> Result<Expr, VXError> {
        let mut l = self.parse_multiplicative()?;
        while self.match_kind(&[TokenType::Plus, TokenType::Minus]) {
            let op = self.advance().value;
            l = Expr::BinaryOp(
                op,
                Box::new(l.clone()),
                Box::new(self.parse_multiplicative()?),
                e_line(&l),
                e_col(&l),
            );
        }
        Ok(l)
    }

    fn parse_multiplicative(&mut self) -> Result<Expr, VXError> {
        let mut l = self.parse_power()?;
        while self.match_kind(&[TokenType::Star, TokenType::Slash, TokenType::Percent]) {
            let op = self.advance().value;
            l = Expr::BinaryOp(
                op,
                Box::new(l.clone()),
                Box::new(self.parse_power()?),
                e_line(&l),
                e_col(&l),
            );
        }
        Ok(l)
    }

    fn parse_power(&mut self) -> Result<Expr, VXError> {
        let l = self.parse_unary()?;
        if self.current().kind == TokenType::Power {
            let op = self.advance().value;
            return Ok(Expr::BinaryOp(
                op,
                Box::new(l.clone()),
                Box::new(self.parse_power()?),
                e_line(&l),
                e_col(&l),
            ));
        }
        Ok(l)
    }

    fn parse_unary(&mut self) -> Result<Expr, VXError> {
        let (l, c) = (self.current().line, self.current().col);
        if self.current().kind == TokenType::Ampersand {
            self.advance();
            let is_mut = self.current().kind == TokenType::Mut;
            if is_mut {
                self.advance();
            }
            return Ok(Expr::AddressOf(Box::new(self.parse_unary()?), is_mut, l, c));
        }
        if self.current().kind == TokenType::Star {
            self.advance();
            return Ok(Expr::Deref(Box::new(self.parse_unary()?), l, c));
        }
        if self.match_kind(&[TokenType::Minus, TokenType::Not]) {
            let op = self.advance().value;
            return Ok(Expr::UnaryOp(op, Box::new(self.parse_unary()?), l, c));
        }
        self.parse_postfix()
    }

    fn parse_postfix(&mut self) -> Result<Expr, VXError> {
        let mut e = self.parse_primary()?;
        loop {
            if self.current().kind == TokenType::LParen {
                e = self.parse_call(e)?;
            } else if self.current().kind == TokenType::LBracket {
                e = self.parse_index(e)?;
            } else if self.current().kind == TokenType::Dot {
                self.advance();
                // 属性名: 接受 Identifier 或关键字 token (如 .new / .int)
                let p = self.expect_identifier_or_keyword()?;
                e = Expr::PropertyAccess(Box::new(e), p.value, p.line, p.col);
            } else if self.current().kind == TokenType::Arrow {
                self.advance();
                // 指针成员名: 接受 Identifier 或关键字 token
                let m = self.expect_identifier_or_keyword()?;
                e = Expr::PointerMember(Box::new(e), m.value, m.line, m.col);
            } else {
                break;
            }
        }
        Ok(e)
    }

    fn parse_call(&mut self, callee: Expr) -> Result<Expr, VXError> {
        let (l, c) = (self.current().line, self.current().col);
        self.advance();
        let mut a = vec![];
        self.skip_newlines();
        // 跳过参数列表换行产生的 Indent/Dedent
        while self.current().kind == TokenType::Indent || self.current().kind == TokenType::Dedent {
            self.advance();
            self.skip_newlines();
        }
        if !self.match_kind(&[TokenType::RParen]) {
            a.push(Box::new(self.parse_expression()?));
            while self.current().kind == TokenType::Comma
                || self.current().kind == TokenType::Newline
            {
                if self.current().kind == TokenType::Comma {
                    self.advance();
                } else {
                    self.skip_newlines();
                }
                self.skip_newlines();
                while self.current().kind == TokenType::Indent
                    || self.current().kind == TokenType::Dedent
                {
                    self.advance();
                    self.skip_newlines();
                }
                if self.current().kind == TokenType::RParen {
                    break;
                }
                a.push(Box::new(self.parse_expression()?));
            }
        }
        self.skip_newlines();
        while self.current().kind == TokenType::Dedent {
            self.advance();
            self.skip_newlines();
        }
        self.expect(TokenType::RParen, None)?;
        Ok(Expr::CallExpr(Box::new(callee), a, l, c))
    }

    fn parse_index(&mut self, e: Expr) -> Result<Expr, VXError> {
        let (l, c) = (self.current().line, self.current().col);
        self.advance();
        let i = self.parse_expression()?;
        self.expect(TokenType::RBracket, None)?;
        Ok(Expr::IndexAccess(Box::new(e), Box::new(i), l, c))
    }

    fn parse_primary(&mut self) -> Result<Expr, VXError> {
        // 注意: 不在 parse_primary 中静默 skip Newline, 因为 Newline 之后可能
        // 是 Indent/Dedent, 而这些是结构边界 token, 不应被表达式吞掉。
        // 多行 if/while 条件 (or/and 续行) 的 Newline 处理在 parse_or/parse_and
        // 中通过 peek_continuation_after_binary_op 显式跳过。
        let t = self.current().clone();
        match t.kind {
            TokenType::Int => {
                self.advance();
                Ok(Expr::IntLiteral(t.value.parse().unwrap(), t.line, t.col))
            }
            TokenType::Float => {
                self.advance();
                Ok(Expr::FloatLiteral(t.value.parse().unwrap(), t.line, t.col))
            }
            TokenType::String => {
                self.advance();
                Ok(Expr::StringLiteral(t.value.clone(), t.line, t.col))
            }
            TokenType::True | TokenType::False => {
                self.advance();
                Ok(Expr::BoolLiteral(t.kind == TokenType::True, t.line, t.col))
            }
            TokenType::Nil => {
                self.advance();
                Ok(Expr::NilLiteral(t.line, t.col))
            }
            // this 已裁减为语法糖 → 解析器自动替换为 Identifier("this")
            // 如果词法器遇到 "this" 标识符, 在此统一处理：
            TokenType::New => self.parse_new_expr(),
            TokenType::Move => self.parse_move_expr(),
            TokenType::In => {
                self.advance();
                Ok(Expr::Identifier(t.value.clone(), t.line, t.col))
            }
            TokenType::Identifier => {
                self.advance();
                // 编译器自动将 "this" 标识符作为当前实例变量处理
                Ok(Expr::Identifier(t.value.clone(), t.line, t.col))
            }
            TokenType::LBracket => self.parse_array(),
            TokenType::LBrace => self.parse_map(),
            TokenType::LParen => {
                self.advance();
                let e = self.parse_expression()?;
                self.expect(TokenType::RParen, None)?;
                Ok(e)
            }
            _ => Err(VXError {
                msg: format!("意外token: {:?}", t.kind),
                line: t.line,
                col: t.col,
                source: Some(self.source.clone()),
            }),
        }
    }

    fn parse_new_expr(&mut self) -> Result<Expr, VXError> {
        let t = self.advance();
        let (l, c) = (t.line, t.col);
        let tn = self.expect_identifier_or_keyword()?.value;
        let mut ta = vec![];
        if self.current().kind == TokenType::Lt {
            self.advance();
            ta.push(Box::new(self.parse_type()?));
            while self.current().kind == TokenType::Comma {
                self.advance();
                ta.push(Box::new(self.parse_type()?));
            }
            self.expect(TokenType::Gt, None)?;
        }
        let mut a = vec![];
        if self.current().kind == TokenType::LParen {
            self.advance();
            if !self.match_kind(&[TokenType::RParen]) {
                a.push(Box::new(self.parse_expression()?));
                while self.current().kind == TokenType::Comma {
                    self.advance();
                    a.push(Box::new(self.parse_expression()?));
                }
            }
            self.expect(TokenType::RParen, None)?;
        }
        Ok(Expr::NewExpr(tn, ta, a, l, c))
    }

    // parse_newz_expr 已裁减 → 由标准库 mem::zeroed<T>() 函数调用替代
    // parse_vector_literal 已裁减 → 数组字面量 [1,2,3] 编译器自动转为 std::Vec<T>

    fn parse_move_expr(&mut self) -> Result<Expr, VXError> {
        let t = self.advance();
        let (l, c) = (t.line, t.col);
        Ok(Expr::MoveExpr(Box::new(self.parse_unary()?), l, c))
    }

    fn parse_array(&mut self) -> Result<Expr, VXError> {
        let (l, c) = (self.current().line, self.current().col);
        self.advance();
        let mut e = vec![];
        if !self.match_kind(&[TokenType::RBracket]) {
            e.push(Box::new(self.parse_expression()?));
            while self.current().kind == TokenType::Comma {
                self.advance();
                e.push(Box::new(self.parse_expression()?));
            }
        }
        self.expect(TokenType::RBracket, None)?;
        Ok(Expr::ArrayLiteral(e, l, c))
    }

    fn parse_map(&mut self) -> Result<Expr, VXError> {
        let (l, c) = (self.current().line, self.current().col);
        self.advance();
        let mut p = vec![];
        let mut e: Vec<Box<Expr>> = vec![];
        if !self.match_kind(&[TokenType::RBrace]) {
            let k = self.parse_expression()?;
            if self.current().kind == TokenType::Colon {
                self.advance();
                let v = self.parse_expression()?;
                p.push((Box::new(k), Box::new(v)));
                while self.current().kind == TokenType::Comma {
                    self.advance();
                    let kk = self.parse_expression()?;
                    self.expect(TokenType::Colon, None)?;
                    let vv = self.parse_expression()?;
                    p.push((Box::new(kk), Box::new(vv)));
                }
                self.expect(TokenType::RBrace, None)?;
                return Ok(Expr::MapLiteral(p, l, c));
            } else {
                e.push(Box::new(k));
            }
            while self.current().kind == TokenType::Comma {
                self.advance();
                e.push(Box::new(self.parse_expression()?));
            }
            self.expect(TokenType::RBrace, None)?;
            return Ok(Expr::ArrayLiteral(e, l, c));
        }
        Ok(Expr::MapLiteral(p, l, c))
    }

    pub fn parse_type(&mut self) -> Result<Expr, VXError> {
        let (l, c) = (self.current().line, self.current().col);
        // 5 原生标量类型 (int/float/double/bool/void)
        // string/vector 已裁减 → 作为标准库标识符 std::String / std::Vec<T>
        // var 动态类型已移除：VX 为纯静态类型语言
        if self.current().kind == TokenType::VarT {
            self.advance();
            return Err(VXError {
                msg: "var 动态类型已移除，VX 为纯静态类型语言，请使用具体类型（如 int、bool、pointer）".to_string(),
                line: l,
                col: c,
                source: Some(self.source.clone()),
            });
        }
        let nm = if self.match_kind(&[
            TokenType::IntT,
            TokenType::FloatT,
            TokenType::DoubleT,
            TokenType::BoolT,
            TokenType::VoidT,
        ]) {
            let t = self.advance().value;
            match t.as_str() {
                "int" | "float" | "double" | "bool" | "void" => t,
                _ => {
                    return Err(VXError {
                        msg: format!("未知类型: {}", t),
                        line: l,
                        col: c,
                        source: Some(self.source.clone()),
                    });
                }
            }
        } else if self.current().kind == TokenType::Identifier {
            self.advance().value
        } else {
            return Err(VXError {
                msg: "期望类型".to_string(),
                line: l,
                col: c,
                source: Some(self.source.clone()),
            });
        };

        let mut ta = vec![];
        if self.current().kind == TokenType::Lt {
            self.advance();
            ta.push(Box::new(self.parse_type()?));
            while self.current().kind == TokenType::Comma {
                self.advance();
                ta.push(Box::new(self.parse_type()?));
            }
            self.expect(TokenType::Gt, None)?;
        }
        if !ta.is_empty() {
            Ok(Expr::TypeExpr(
                format!(
                    "{}<{}>",
                    nm,
                    ta.iter()
                        .map(|t| expr_to_type_name(t.as_ref()))
                        .collect::<Vec<_>>()
                        .join(", ")
                ),
                l,
                c,
            ))
        } else {
            Ok(Expr::TypeExpr(nm, l, c))
        }
    }
}
