// libutopia::parser::expr -- the U-5c expression parser.
//
// === Strategic framing ===
//
// U-5b lifted the rc-shape grammar into a statement-level AST but
// left every expression context (if condition, while condition, for
// list, let value, bare-assign value, return value, arith body) as
// a raw `Vec<Token>` placeholder. U-5c walks those placeholders into
// proper `Expr` trees. After U-5c, the AST has ONE canonical shape:
// every expression slot is an `Expr` (no `Vec<Token>` placeholders
// remain).
//
// The expression parser is a Pratt-style recursive-descent walker
// over an owned `Vec<Token>`. It's pure logic (no I/O, no syscalls);
// host-testable via `#[cfg(test)]` unit tests and runtime-validated
// via the `flow_parser_expr` probe in `/u-test`.
//
// The grammar follows scripture sections 6.1-6.14 + 7.3 + 17.10:
//
//   - **Atoms**: Word, SingleQuoted, DoubleQuoted, Var, VarLen,
//     VarNoSplit, Subst, Backtick, ProcSubIn, ProcSubOut, Regex.
//     Integer literal in arith context (Word("42") -> Integer(42)).
//
//   - **Var indexing**: `$var(N)` / `$var(M N)` per scripture 6.9.
//     Requires span-adjacent LParen after the Var token. Index
//     expression(s) parsed in arith context.
//
//   - **Concat**: `^` joining span-adjacent value sub-expressions
//     in non-arith contexts (scripture 6.7). The Word-level concat
//     (parse.rs::parse_word) handles command-arg concatenation;
//     this expression-level Concat handles concat inside `let x =`,
//     `if (...)`, `for (... in ...)`, etc.
//
//   - **List literal**: `(a b c)` in value position (scripture 6.3).
//
//   - **Arithmetic** (scripture 6.13): full Pratt-style precedence
//     climbing for the integer math operators. Operators
//     `+ - * / % ** & | ^ ~ << >> < <= > >= == != && ||`. Unary
//     `- ~ !`. Right-associative `**`.
//
//   - **String comparison** (scripture 6.14): `==` and `!=` as
//     comparison operators in cond and arith contexts.
//
//   - **Match operators** (scripture 7.3): `matches`, `in`, `=~`
//     as match-binary operators in cond context. Right operand is
//     the pattern (glob word, list, regex literal).
//
// === Context handling ===
//
// `ExprContext` (from ast.rs) discriminates four shapes:
//
//   - `Value` / `Return`: a single value (atom + concat) or a list
//     literal. NO top-level operators (use `(( ... ))` for math).
//     `let x = 5 + 3` is a parse error -- the user writes
//     `let x = (( 5 + 3 ))`.
//
//   - `Cond`: full boolean expression. All comparison, logical,
//     and match operators are recognized.
//
//   - `Arith`: full arithmetic. Words are integer literals; bare
//     identifiers are NOT primaries (vars must use `$` prefix).
//
//   - `List`: a list. Accepts a single value, multiple values
//     (implicit list), or an explicit `(a b c)`.
//
// === Substitution body lifting ===
//
// `Subst($(cmd))`, `Backtick(\`{cmd}\`)`, `ProcSubIn(<(cmd))`,
// `ProcSubOut(>(cmd))` carry their bodies as raw `String` from the
// lexer. The expression parser eagerly parses these bodies as
// sub-scripts (recursive `parse(&body)` call) so the AST is fully
// resolved at U-5c. The cost is small (substitution bodies are
// typically short); the benefit is that parse errors surface at
// parse time and the evaluator (U-6) sees a uniform AST.

use alloc::boxed::Box;
use alloc::string::String;
use alloc::vec::Vec;

use super::ast::{
    BinOp, CaseExpr, CaseExprArm, Expr, ExprContext, ExprKind, MatchOp, Script, UnOp,
};
use super::error::{ParseError, ParseErrorKind, ParseResult};
use super::lexer::tokenize;
use super::parse::parse_tokens;
use super::span::Span;
use super::token::{Token, TokenKind};

// ---------------------------------------------------------------------
// Public entry
// ---------------------------------------------------------------------

/// Parse a token stream into an `Expr` tree in the given context.
///
/// The token stream is the body of an expression context (between
/// `(` and `)` of an `if`, after `=` of an assignment, etc.) -- it
/// does NOT include the outer delimiters; those are consumed by the
/// statement parser before this is called.
///
/// `source_len` is used for EOF-position span fallbacks (matches
/// `parse_tokens`'s convention).
///
/// Empty token streams produce `ParseErrorKind::EmptyExpression`
/// when the context requires a value. Some contexts (e.g., Return
/// with no value) skip the call entirely.
pub fn parse_expr_tokens(
    tokens: Vec<Token>,
    source_len: usize,
    ctx: ExprContext,
) -> ParseResult<Expr> {
    if tokens.is_empty() {
        return Err(ParseError {
            kind: ParseErrorKind::EmptyExpression,
            span: Span::point(source_len),
        });
    }
    let mut p = ExprParser::new(tokens, source_len, ctx);
    let expr = p.parse_top()?;
    if !p.at_end() {
        return Err(ParseError {
            kind: ParseErrorKind::TrailingTokensInExpr,
            span: p.current_span(),
        });
    }
    Ok(expr)
}

// ---------------------------------------------------------------------
// Parser state
// ---------------------------------------------------------------------

struct ExprParser {
    tokens: Vec<Token>,
    pos: usize,
    source_len: usize,
    ctx: ExprContext,
}

impl ExprParser {
    fn new(tokens: Vec<Token>, source_len: usize, ctx: ExprContext) -> Self {
        let tokens = if ctx == ExprContext::Arith {
            retokenize_arith_stream(tokens)
        } else {
            tokens
        };
        Self {
            tokens,
            pos: 0,
            source_len,
            ctx,
        }
    }

    fn at_end(&self) -> bool {
        self.pos >= self.tokens.len()
    }

    fn peek(&self) -> Option<&Token> {
        self.tokens.get(self.pos)
    }

    fn peek_kind(&self) -> Option<&TokenKind> {
        self.peek().map(|t| &t.kind)
    }

    fn peek_kind_at(&self, off: usize) -> Option<&TokenKind> {
        self.tokens.get(self.pos + off).map(|t| &t.kind)
    }

    fn advance(&mut self) -> Token {
        let t = self.tokens[self.pos].clone();
        self.pos += 1;
        t
    }

    fn current_span(&self) -> Span {
        self.peek().map(|t| t.span).unwrap_or_else(|| self.eof_span())
    }

    fn eof_span(&self) -> Span {
        Span::point(self.source_len)
    }

    // -----------------------------------------------------------------
    // Top-level dispatch on context
    // -----------------------------------------------------------------

    fn parse_top(&mut self) -> ParseResult<Expr> {
        match self.ctx {
            ExprContext::Arith => self.parse_arith_top(),
            ExprContext::Cond => self.parse_or(),
            ExprContext::Value | ExprContext::Return => self.parse_value_top(),
            ExprContext::List => self.parse_list_top(),
        }
    }

    // -----------------------------------------------------------------
    // Value / Return context: a single value (atom + concat) or an
    // explicit list literal. NO top-level operators.
    // -----------------------------------------------------------------

    fn parse_value_top(&mut self) -> ParseResult<Expr> {
        // Case-as-expression (U-5d, scripture 7.2): `let x = case ...`.
        // The Value/Return path doesn't reach parse_primary, so check
        // for `Case` here too.
        if matches!(self.peek_kind(), Some(TokenKind::Case)) {
            return self.parse_case_expr();
        }
        // Explicit list `(a b c)` -- accept only if it's the entire
        // expression (the only top-level item).
        if let Some(TokenKind::LParen) = self.peek_kind() {
            // Look ahead: does this paren wrap the whole expression?
            // We treat ANY top-level `(` in value/return position as
            // a list literal per scripture 6.3 + U-5b lock #9.
            return self.parse_list_literal();
        }
        // Inline arith `(( ... ))`.
        if let Some(TokenKind::DoubleLParen) = self.peek_kind() {
            return self.parse_inline_arith();
        }
        // Single value (with span-adjacent concat).
        self.parse_concat_chain()
    }

    // -----------------------------------------------------------------
    // List context: the body of `for (var in expr)`. Accepts:
    //   - explicit `(a b c)` list literal
    //   - implicit list of space-separated value tokens
    //   - a single value (Var that resolves to a list at runtime)
    // -----------------------------------------------------------------

    fn parse_list_top(&mut self) -> ParseResult<Expr> {
        // Case-as-expression (U-5d) as a list element. Same rationale
        // as parse_value_top above.
        if matches!(self.peek_kind(), Some(TokenKind::Case)) {
            return self.parse_case_expr();
        }
        // Explicit `(...)` list literal.
        if let Some(TokenKind::LParen) = self.peek_kind() {
            return self.parse_list_literal();
        }
        // Inline arith `(( ... ))` as a (degenerate) list element.
        if let Some(TokenKind::DoubleLParen) = self.peek_kind() {
            return self.parse_inline_arith();
        }
        // Implicit list: collect concat-chains separated by whitespace
        // (i.e., parse one concat-chain, then if more value tokens
        // remain at top level, build an implicit List).
        let first = self.parse_concat_chain()?;
        if self.at_end() {
            return Ok(first);
        }
        // More tokens -- build an implicit list.
        let mut elems = alloc::vec![first];
        while !self.at_end() {
            elems.push(self.parse_concat_chain()?);
        }
        let start = elems.first().expect("at least one").span.start;
        let end = elems.last().expect("at least one").span.end;
        Ok(Expr {
            kind: ExprKind::List(elems),
            span: Span::new(start, end),
        })
    }

    // -----------------------------------------------------------------
    // Explicit list literal `(a b c)` -- caller has positioned at the
    // opening LParen.
    // -----------------------------------------------------------------

    fn parse_list_literal(&mut self) -> ParseResult<Expr> {
        let open = self.advance(); // LParen
        debug_assert!(matches!(open.kind, TokenKind::LParen));
        let mut elems = Vec::new();
        loop {
            match self.peek_kind() {
                Some(TokenKind::RParen) => break,
                None => {
                    return Err(ParseError {
                        kind: ParseErrorKind::UnexpectedEof { expected: "`)`" },
                        span: self.eof_span(),
                    });
                }
                _ => {
                    elems.push(self.parse_concat_chain()?);
                }
            }
        }
        let close = self.advance(); // RParen
        Ok(Expr {
            kind: ExprKind::List(elems),
            span: Span::new(open.span.start, close.span.end),
        })
    }

    // -----------------------------------------------------------------
    // Inline `(( ... ))` arith in non-arith context.
    // -----------------------------------------------------------------

    fn parse_inline_arith(&mut self) -> ParseResult<Expr> {
        let open = self.advance(); // DoubleLParen
        debug_assert!(matches!(open.kind, TokenKind::DoubleLParen));
        // Collect tokens until matching DoubleRParen (with paren
        // depth tracking).
        let mut body: Vec<Token> = Vec::new();
        let mut depth: i32 = 2;
        while !self.at_end() {
            let t = self.peek().expect("at_end above");
            match &t.kind {
                TokenKind::LParen => depth += 1,
                TokenKind::DoubleLParen => depth += 2,
                TokenKind::RParen => {
                    depth -= 1;
                    if depth == 0 {
                        let close = self.advance();
                        let inner = parse_expr_tokens(body, self.source_len, ExprContext::Arith)?;
                        return Ok(Expr {
                            kind: inner.kind,
                            span: Span::new(open.span.start, close.span.end),
                        });
                    }
                }
                TokenKind::DoubleRParen => {
                    depth -= 2;
                    if depth == 0 {
                        let close = self.advance();
                        let inner = parse_expr_tokens(body, self.source_len, ExprContext::Arith)?;
                        return Ok(Expr {
                            kind: inner.kind,
                            span: Span::new(open.span.start, close.span.end),
                        });
                    }
                    if depth < 0 {
                        return Err(ParseError {
                            kind: ParseErrorKind::UnexpectedToken { expected: "`))`" },
                            span: t.span,
                        });
                    }
                }
                _ => {}
            }
            body.push(self.advance());
        }
        Err(ParseError {
            kind: ParseErrorKind::UnexpectedEof { expected: "`))`" },
            span: self.eof_span(),
        })
    }

    // -----------------------------------------------------------------
    // Concat chain: span-adjacent value tokens joined by `^` in
    // non-arith contexts. The leading atom plus zero-or-more
    // (`^` adj-value) suffixes.
    //
    // This mirrors parse.rs::parse_word but operates over Expr
    // nodes instead of raw Tokens.
    // -----------------------------------------------------------------

    fn parse_concat_chain(&mut self) -> ParseResult<Expr> {
        let first = self.parse_value_atom()?;
        let mut parts: Vec<Expr> = alloc::vec![first];
        loop {
            let last_end = parts.last().expect("len >= 1").span.end;
            // Peek for a span-adjacent Caret.
            let caret = match self.peek() {
                Some(t) => t,
                None => break,
            };
            if !matches!(caret.kind, TokenKind::Caret) {
                break;
            }
            if caret.span.start != last_end {
                break;
            }
            let caret_end = caret.span.end;
            // Peek for a span-adjacent value token AFTER the caret.
            let next_tok = match self.tokens.get(self.pos + 1) {
                Some(t) => t,
                None => break,
            };
            if !is_value_token_kind(&next_tok.kind) {
                break;
            }
            if next_tok.span.start != caret_end {
                break;
            }
            // Consume the Caret + the next value atom.
            self.pos += 1; // past the Caret
            let next_expr = self.parse_value_atom()?;
            parts.push(next_expr);
        }
        if parts.len() == 1 {
            Ok(parts.into_iter().next().expect("len 1"))
        } else {
            let start = parts.first().expect("len > 0").span.start;
            let end = parts.last().expect("len > 0").span.end;
            Ok(Expr {
                kind: ExprKind::Concat(parts),
                span: Span::new(start, end),
            })
        }
    }

    // -----------------------------------------------------------------
    // Value atom: a single value-producing token (with optional
    // span-adjacent `(N)` or `(M N)` index/slice when the token is a
    // Var).
    // -----------------------------------------------------------------

    fn parse_value_atom(&mut self) -> ParseResult<Expr> {
        let tok = match self.peek() {
            Some(t) => t.clone(),
            None => {
                return Err(ParseError {
                    kind: ParseErrorKind::UnexpectedEof {
                        expected: "value expression",
                    },
                    span: self.eof_span(),
                });
            }
        };
        if !is_value_token_kind(&tok.kind) {
            return Err(ParseError {
                kind: ParseErrorKind::UnexpectedTokenInExpr {
                    expected: "value expression",
                },
                span: tok.span,
            });
        }
        self.pos += 1;
        let mut span = tok.span;
        let kind = match tok.kind {
            TokenKind::Word(s) => ExprKind::Word(s),
            TokenKind::SingleQuoted(s) => ExprKind::SingleQuoted(s),
            TokenKind::DoubleQuoted(parts) => ExprKind::DoubleQuoted(parts),
            TokenKind::Var(name) => {
                // Check for span-adjacent `(...)` -- VarIndex / VarSlice.
                let var_end = tok.span.end;
                if let Some(next) = self.peek() {
                    if matches!(next.kind, TokenKind::LParen) && next.span.start == var_end {
                        return self.finish_var_index(name, tok.span);
                    }
                }
                ExprKind::Var(name)
            }
            TokenKind::VarLen(name) => ExprKind::VarLen(name),
            TokenKind::VarNoSplit(name) => ExprKind::VarNoSplit(name),
            TokenKind::Subst(body) => {
                let sub = parse_subscript(&body, tok.span)?;
                ExprKind::Subst(Box::new(sub))
            }
            TokenKind::Backtick(body) => {
                let sub = parse_subscript(&body, tok.span)?;
                ExprKind::Backtick(Box::new(sub))
            }
            TokenKind::ProcSubIn(body) => {
                let sub = parse_subscript(&body, tok.span)?;
                ExprKind::ProcSubIn(Box::new(sub))
            }
            TokenKind::ProcSubOut(body) => {
                let sub = parse_subscript(&body, tok.span)?;
                ExprKind::ProcSubOut(Box::new(sub))
            }
            TokenKind::Regex(s) => ExprKind::Regex(s),
            _ => unreachable!("is_value_token_kind gates the dispatch above"),
        };
        // span is already tok.span for non-VarIndex atoms.
        // (VarIndex returns early via finish_var_index.)
        let _ = &mut span; // suppress unused-mut on the never-mutated path
        Ok(Expr { kind, span })
    }

    /// After a Var token + span-adjacent LParen, parse the index or
    /// slice form. Caller hasn't consumed the LParen.
    fn finish_var_index(&mut self, name: String, var_span: Span) -> ParseResult<Expr> {
        let lparen = self.advance(); // LParen
        debug_assert!(matches!(lparen.kind, TokenKind::LParen));
        // Collect tokens up to matching RParen.
        let mut body: Vec<Token> = Vec::new();
        let mut depth: i32 = 1;
        while !self.at_end() {
            let t = self.peek().expect("at_end above");
            match &t.kind {
                TokenKind::LParen => depth += 1,
                TokenKind::DoubleLParen => depth += 2,
                TokenKind::RParen => {
                    depth -= 1;
                    if depth == 0 {
                        let close = self.advance();
                        // Split body on whitespace-equivalent: at v1
                        // the index form is `$var(N)` or `$var(M N)`.
                        // We accept one or two arith expressions
                        // separated by free space (i.e., we use the
                        // same Pratt-arith parser to chew through;
                        // a chain like `1 2` is not a valid arith
                        // expr, so we split on top-level whitespace
                        // by scanning for an arith-expression
                        // terminator and then resuming).
                        return finish_var_index_with_body(
                            name, var_span, close.span.end, body, self.source_len,
                        );
                    }
                }
                TokenKind::DoubleRParen => {
                    depth -= 2;
                    if depth == 0 {
                        // The `))` -- treat as closing.
                        let close = self.advance();
                        return finish_var_index_with_body(
                            name, var_span, close.span.end, body, self.source_len,
                        );
                    }
                    if depth < 0 {
                        return Err(ParseError {
                            kind: ParseErrorKind::UnexpectedToken { expected: "`)`" },
                            span: t.span,
                        });
                    }
                }
                _ => {}
            }
            body.push(self.advance());
        }
        Err(ParseError {
            kind: ParseErrorKind::UnexpectedEof { expected: "`)`" },
            span: self.eof_span(),
        })
    }

    // -----------------------------------------------------------------
    // Arithmetic context: Pratt-style precedence climbing.
    //
    // Precedence (lowest to highest):
    //   1.  `||`
    //   2.  `&&`
    //   3.  `|`
    //   4.  `^` (xor)
    //   5.  `&`
    //   6.  `== !=`, `matches`, `in`, `=~`
    //   7.  `< <= > >=`
    //   8.  `<< >>`   (only when separated; `<<` raw triggers heredoc
    //                  at lex time, so the arith form requires
    //                  whitespace or precise lex tokens.)
    //   9.  `+ -`
    //   10. `* / %`
    //   11. `**`        (right-associative)
    //   12. unary `- ~ !`
    //   13. primary (atom + optional postfix index)
    //
    // The arithmetic parser uses the same atom dispatcher as the
    // value parser BUT in arith context:
    //   - Word("42") -> Integer(42) (parse the digits).
    //   - Other words -> InvalidArithLiteral error.
    //   - Var/$var(...) -> auto-deref (evaluator resolves to int).
    //
    // The cond context uses a subset of the arith Pratt: it lacks
    // the bit/shift levels and the integer literal path; otherwise
    // the structure is the same.
    // -----------------------------------------------------------------

    fn parse_arith_top(&mut self) -> ParseResult<Expr> {
        self.maybe_retokenize_at_pos();
        let result = self.parse_or()?;
        Ok(result)
    }

    fn parse_or(&mut self) -> ParseResult<Expr> {
        let mut left = self.parse_and()?;
        loop {
            match self.peek_kind() {
                Some(TokenKind::OrOr) => {
                    self.pos += 1;
                    self.maybe_retokenize_at_pos();
                    let right = self.parse_and()?;
                    left = bin(BinOp::Or, left, right);
                }
                _ => break,
            }
        }
        Ok(left)
    }

    fn parse_and(&mut self) -> ParseResult<Expr> {
        let mut left = self.parse_bit_or()?;
        loop {
            match self.peek_kind() {
                Some(TokenKind::AndAnd) => {
                    self.pos += 1;
                    self.maybe_retokenize_at_pos();
                    let right = self.parse_bit_or()?;
                    left = bin(BinOp::And, left, right);
                }
                _ => break,
            }
        }
        Ok(left)
    }

    fn parse_bit_or(&mut self) -> ParseResult<Expr> {
        // `|` is bit-OR only in arith context.
        if self.ctx != ExprContext::Arith {
            return self.parse_bit_xor();
        }
        let mut left = self.parse_bit_xor()?;
        loop {
            // The lexer emits `Pipe` for `|`. Inside `(( ... ))` the
            // body was collected by parse_arith_command without any
            // pipe-shape disambiguation; we accept the Pipe token
            // here and treat it as bit-OR.
            match self.peek_kind() {
                Some(TokenKind::Pipe) => {
                    self.pos += 1;
                    self.maybe_retokenize_at_pos();
                    let right = self.parse_bit_xor()?;
                    left = bin(BinOp::BitOr, left, right);
                }
                _ => break,
            }
        }
        Ok(left)
    }

    fn parse_bit_xor(&mut self) -> ParseResult<Expr> {
        // `^` is XOR in arith; concat in non-arith (handled at
        // value-atom level for non-arith).
        if self.ctx != ExprContext::Arith {
            return self.parse_bit_and();
        }
        let mut left = self.parse_bit_and()?;
        loop {
            match self.peek_kind() {
                Some(TokenKind::Caret) => {
                    self.pos += 1;
                    self.maybe_retokenize_at_pos();
                    let right = self.parse_bit_and()?;
                    left = bin(BinOp::BitXor, left, right);
                }
                _ => break,
            }
        }
        Ok(left)
    }

    fn parse_bit_and(&mut self) -> ParseResult<Expr> {
        if self.ctx != ExprContext::Arith {
            return self.parse_eq();
        }
        let mut left = self.parse_eq()?;
        loop {
            match self.peek_kind() {
                Some(TokenKind::Ampersand) => {
                    self.pos += 1;
                    self.maybe_retokenize_at_pos();
                    let right = self.parse_eq()?;
                    left = bin(BinOp::BitAnd, left, right);
                }
                _ => break,
            }
        }
        Ok(left)
    }

    fn parse_eq(&mut self) -> ParseResult<Expr> {
        let mut left = self.parse_rel()?;
        loop {
            let op = match self.peek_kind() {
                Some(TokenKind::DoubleEqual) => Some(EqOrMatch::Eq),
                Some(TokenKind::NotEqual) => Some(EqOrMatch::Ne),
                Some(TokenKind::EqualTilde) => Some(EqOrMatch::Regex),
                Some(TokenKind::Word(w)) if self.ctx == ExprContext::Cond && w == "matches" => {
                    Some(EqOrMatch::Matches)
                }
                Some(TokenKind::In) if self.ctx == ExprContext::Cond => Some(EqOrMatch::In),
                _ => None,
            };
            let Some(op) = op else { break };
            self.pos += 1;
            self.maybe_retokenize_at_pos();
            let right = self.parse_rel()?;
            left = match op {
                EqOrMatch::Eq => bin(BinOp::Eq, left, right),
                EqOrMatch::Ne => bin(BinOp::Ne, left, right),
                EqOrMatch::Matches => match_expr(MatchOp::Glob, left, right),
                EqOrMatch::In => match_expr(MatchOp::In, left, right),
                EqOrMatch::Regex => match_expr(MatchOp::Regex, left, right),
            };
        }
        Ok(left)
    }

    fn parse_rel(&mut self) -> ParseResult<Expr> {
        let mut left = self.parse_shift()?;
        loop {
            let op = match self.peek_kind() {
                Some(TokenKind::Less) => BinOp::Lt,
                Some(TokenKind::LessEqual) => BinOp::Le,
                Some(TokenKind::Greater) => BinOp::Gt,
                Some(TokenKind::GreaterEqual) => BinOp::Ge,
                _ => break,
            };
            self.pos += 1;
            self.maybe_retokenize_at_pos();
            let right = self.parse_shift()?;
            left = bin(op, left, right);
        }
        Ok(left)
    }

    fn parse_shift(&mut self) -> ParseResult<Expr> {
        // Shifts are arith-only and require the lexer to emit
        // distinct shift tokens; at U-5a the lexer doesn't have a
        // shift token (`<<` would be heredoc-start). We accept
        // `<<` / `>>` only when they appear as separate Less+Less
        // (parsed by the parent caller before reaching this level).
        // For v1.0 the practical result is: shifts in arith are
        // expressed via lex-friendly forms only. The level is
        // present in the precedence chain to leave structural
        // room for a v1.x arith-mode lexer.
        if self.ctx != ExprContext::Arith {
            return self.parse_add();
        }
        self.parse_add()
    }

    fn parse_add(&mut self) -> ParseResult<Expr> {
        if self.ctx != ExprContext::Arith {
            return self.parse_mul();
        }
        let mut left = self.parse_mul()?;
        loop {
            let op = match self.peek_kind() {
                Some(TokenKind::Plus) => BinOp::Add,
                Some(TokenKind::Minus) => BinOp::Sub,
                _ => break,
            };
            self.pos += 1;
            self.maybe_retokenize_at_pos();
            let right = self.parse_mul()?;
            left = bin(op, left, right);
        }
        Ok(left)
    }

    fn parse_mul(&mut self) -> ParseResult<Expr> {
        if self.ctx != ExprContext::Arith {
            return self.parse_pow();
        }
        let mut left = self.parse_pow()?;
        loop {
            let op = match self.peek_kind() {
                Some(TokenKind::Star) => BinOp::Mul,
                Some(TokenKind::Slash) => BinOp::Div,
                Some(TokenKind::Percent) => BinOp::Mod,
                _ => break,
            };
            self.pos += 1;
            self.maybe_retokenize_at_pos();
            let right = self.parse_pow()?;
            left = bin(op, left, right);
        }
        Ok(left)
    }

    fn parse_pow(&mut self) -> ParseResult<Expr> {
        if self.ctx != ExprContext::Arith {
            return self.parse_unary();
        }
        let left = self.parse_unary()?;
        // `**` lex doesn't have its own token at U-5a (it'd be two
        // Stars). Detect adjacent Star Star with span adjacency.
        if let (Some(t1), Some(t2)) = (self.peek(), self.peek_kind_at(1)) {
            if matches!(t1.kind, TokenKind::Star) && matches!(t2, TokenKind::Star) {
                let t1_end = t1.span.end;
                let t2_start = self.tokens[self.pos + 1].span.start;
                if t1_end == t2_start {
                    self.pos += 2;
                    self.maybe_retokenize_at_pos();
                    let right = self.parse_pow()?; // right-assoc
                    return Ok(bin(BinOp::Pow, left, right));
                }
            }
        }
        Ok(left)
    }

    fn parse_unary(&mut self) -> ParseResult<Expr> {
        // Unary operators: `! - ~`. `-` and `~` are arith-only;
        // `!` is logical and works in cond + arith.
        let op = match self.peek_kind() {
            Some(TokenKind::Bang) => Some(UnOp::Not),
            Some(TokenKind::Minus) if self.ctx == ExprContext::Arith => Some(UnOp::Neg),
            Some(TokenKind::Tilde) if self.ctx == ExprContext::Arith => Some(UnOp::BitNot),
            _ => None,
        };
        if let Some(op) = op {
            let tok = self.advance();
            self.maybe_retokenize_at_pos();
            let operand = self.parse_unary()?;
            let span = Span::new(tok.span.start, operand.span.end);
            return Ok(Expr {
                kind: ExprKind::UnOp(op, Box::new(operand)),
                span,
            });
        }
        self.parse_primary()
    }

    fn parse_primary(&mut self) -> ParseResult<Expr> {
        // Case-as-expression (scripture 7.2): the `case $x { pat =>
        // value ... }` form. Recognized as a primary in every
        // context so the AST has ONE canonical expression shape; the
        // evaluator decides what to do with the resulting value.
        if matches!(self.peek_kind(), Some(TokenKind::Case)) {
            return self.parse_case_expr();
        }
        // Parenthesized sub-expression `( expr )` -- valid in all
        // operator contexts.
        if let Some(TokenKind::LParen) = self.peek_kind() {
            // In arith context, `(...)` is grouping; in non-arith,
            // the value-top path already dispatched to list literal.
            // Here in arith: collect tokens, parse_expr_tokens
            // recursively.
            if self.ctx == ExprContext::Arith {
                let open = self.advance();
                let mut body: Vec<Token> = Vec::new();
                let mut depth: i32 = 1;
                while !self.at_end() {
                    let t = self.peek().expect("at_end above");
                    match &t.kind {
                        TokenKind::LParen => depth += 1,
                        TokenKind::DoubleLParen => depth += 2,
                        TokenKind::RParen => {
                            depth -= 1;
                            if depth == 0 {
                                let close = self.advance();
                                let inner = parse_expr_tokens(
                                    body,
                                    self.source_len,
                                    ExprContext::Arith,
                                )?;
                                return Ok(Expr {
                                    kind: inner.kind,
                                    span: Span::new(open.span.start, close.span.end),
                                });
                            }
                        }
                        TokenKind::DoubleRParen => {
                            depth -= 2;
                            if depth == 0 {
                                let close = self.advance();
                                let inner = parse_expr_tokens(
                                    body,
                                    self.source_len,
                                    ExprContext::Arith,
                                )?;
                                return Ok(Expr {
                                    kind: inner.kind,
                                    span: Span::new(open.span.start, close.span.end),
                                });
                            }
                            if depth < 0 {
                                return Err(ParseError {
                                    kind: ParseErrorKind::UnexpectedToken {
                                        expected: "`)`",
                                    },
                                    span: t.span,
                                });
                            }
                        }
                        _ => {}
                    }
                    body.push(self.advance());
                }
                return Err(ParseError {
                    kind: ParseErrorKind::UnexpectedEof { expected: "`)`" },
                    span: self.eof_span(),
                });
            }
            // In cond context, `(...)` is grouping.
            if self.ctx == ExprContext::Cond {
                let open = self.advance();
                let mut body: Vec<Token> = Vec::new();
                let mut depth: i32 = 1;
                while !self.at_end() {
                    let t = self.peek().expect("at_end above");
                    match &t.kind {
                        TokenKind::LParen => depth += 1,
                        TokenKind::DoubleLParen => depth += 2,
                        TokenKind::RParen => {
                            depth -= 1;
                            if depth == 0 {
                                let close = self.advance();
                                let inner = parse_expr_tokens(
                                    body,
                                    self.source_len,
                                    ExprContext::Cond,
                                )?;
                                return Ok(Expr {
                                    kind: inner.kind,
                                    span: Span::new(open.span.start, close.span.end),
                                });
                            }
                        }
                        TokenKind::DoubleRParen => {
                            depth -= 2;
                            if depth == 0 {
                                let close = self.advance();
                                let inner = parse_expr_tokens(
                                    body,
                                    self.source_len,
                                    ExprContext::Cond,
                                )?;
                                return Ok(Expr {
                                    kind: inner.kind,
                                    span: Span::new(open.span.start, close.span.end),
                                });
                            }
                            if depth < 0 {
                                return Err(ParseError {
                                    kind: ParseErrorKind::UnexpectedToken {
                                        expected: "`)`",
                                    },
                                    span: t.span,
                                });
                            }
                        }
                        _ => {}
                    }
                    body.push(self.advance());
                }
                return Err(ParseError {
                    kind: ParseErrorKind::UnexpectedEof { expected: "`)`" },
                    span: self.eof_span(),
                });
            }
        }
        // Atom dispatch.
        let tok = match self.peek() {
            Some(t) => t.clone(),
            None => {
                return Err(ParseError {
                    kind: ParseErrorKind::UnexpectedEof {
                        expected: "primary expression",
                    },
                    span: self.eof_span(),
                });
            }
        };
        match &tok.kind {
            TokenKind::Word(s) if self.ctx == ExprContext::Arith => {
                // Arith mode: try integer parse, else retokenize on
                // operator boundaries (digits + operator chars within
                // the word).
                self.pos += 1;
                if let Some(n) = parse_int(s) {
                    return Ok(Expr {
                        kind: ExprKind::Integer(n),
                        span: tok.span,
                    });
                }
                // Try retokenizing: if the Word splits cleanly into
                // digit-runs + arith-operator chars, parse the
                // resulting micro-stream.
                if let Some(micro) = retokenize_arith_word(s, tok.span.start) {
                    // Re-parse the micro stream as a complete arith
                    // expression using a fresh ExprParser.
                    let sub = parse_expr_tokens(micro, self.source_len, ExprContext::Arith)?;
                    return Ok(Expr {
                        kind: sub.kind,
                        span: tok.span,
                    });
                }
                return Err(ParseError {
                    kind: ParseErrorKind::InvalidArithLiteral,
                    span: tok.span,
                });
            }
            _ => {}
        }
        self.parse_value_atom()
    }

    // -----------------------------------------------------------------
    // Arith-word retokenization (called eagerly at construction).
    //
    // `maybe_retokenize_at_pos` is now a no-op because the entire
    // token stream is retokenized upfront at parser-construction
    // time (in `new()`) when the context is Arith. This avoids
    // the lifetime-tangle of splice-on-demand and ensures the
    // peek-ahead in the precedence climbing path sees the right
    // operator tokens.
    //
    // The retokenization is purely a robustness aid: arith-mode
    // lex awareness is documented as deferred to v1.x. The
    // retokenization here covers the cases:
    //   - `(( 1+2 ))` -> Word("1+2") -> Integer + Plus + Integer
    //   - `(( 1 + 2 ))` -> Word("1") Word("+") Word("2")
    //                   -> Integer + Plus + Integer
    //   - `(( -1 ))` -> Word("-1") -> Minus + Integer
    // -----------------------------------------------------------------

    fn maybe_retokenize_at_pos(&mut self) {
        // No-op: retokenization happens once at construction time
        // in `new()` when ctx == Arith. Kept as an identity hook so
        // call sites don't need to be deleted.
    }

    // -----------------------------------------------------------------
    // Case-as-expression (U-5d): `case $x { pat => value ... }`
    // -----------------------------------------------------------------
    //
    // Reached from `parse_primary` whenever the next token is `Case`.
    // Each arm's value is a single Value-context expression
    // terminated by Newline / Semicolon / RBrace at depth 0
    // (matching the rc-shape grammar's statement-terminator rules
    // applied to a single value).

    fn parse_case_expr(&mut self) -> ParseResult<Expr> {
        let case_tok = self.advance(); // Case
        debug_assert!(matches!(case_tok.kind, TokenKind::Case));
        // Scrutinee: a value-position expression terminated by `{`.
        let scrutinee_tokens = self.collect_until_lbrace_at_depth0()?;
        if scrutinee_tokens.is_empty() {
            return Err(ParseError {
                kind: ParseErrorKind::EmptyExpression,
                span: self.current_span(),
            });
        }
        let scrutinee = parse_expr_tokens(scrutinee_tokens, self.source_len, ExprContext::Value)?;
        if !matches!(self.peek_kind(), Some(TokenKind::LBrace)) {
            return Err(ParseError {
                kind: ParseErrorKind::UnexpectedToken { expected: "`{`" },
                span: self.current_span(),
            });
        }
        self.pos += 1; // consume LBrace
        let mut arms = Vec::new();
        loop {
            // Skip Newline + Semicolon separators between arms.
            while matches!(
                self.peek_kind(),
                Some(TokenKind::Newline) | Some(TokenKind::Semicolon)
            ) {
                self.pos += 1;
            }
            if matches!(self.peek_kind(), Some(TokenKind::RBrace)) || self.at_end() {
                break;
            }
            arms.push(self.parse_case_expr_arm()?);
        }
        if !matches!(self.peek_kind(), Some(TokenKind::RBrace)) {
            return Err(ParseError {
                kind: ParseErrorKind::UnexpectedEof { expected: "`}`" },
                span: self.eof_span(),
            });
        }
        let close = self.advance();
        let span = Span::new(case_tok.span.start, close.span.end);
        Ok(Expr {
            kind: ExprKind::Case(Box::new(CaseExpr {
                scrutinee: Box::new(scrutinee),
                arms,
                span,
            })),
            span,
        })
    }

    fn parse_case_expr_arm(&mut self) -> ParseResult<CaseExprArm> {
        let start = self.current_span().start;
        // Collect pattern tokens until `=>` at depth 0.
        let pat_tokens = self.collect_until_fat_arrow_at_depth0()?;
        let pieces = split_value_tokens_on_whitespace(pat_tokens);
        if pieces.is_empty() {
            return Err(ParseError {
                kind: ParseErrorKind::EmptyCasePattern,
                span: Span::new(start, self.current_span().start),
            });
        }
        let mut patterns: Vec<Expr> = Vec::with_capacity(pieces.len());
        for piece in pieces {
            patterns.push(parse_expr_tokens(piece, self.source_len, ExprContext::Value)?);
        }
        // Skip optional newlines between `=>` and the value.
        while matches!(self.peek_kind(), Some(TokenKind::Newline)) {
            self.pos += 1;
        }
        // Collect value tokens until top-level Newline / Semicolon /
        // RBrace.
        let value_tokens = self.collect_arm_value_tokens()?;
        if value_tokens.is_empty() {
            return Err(ParseError {
                kind: ParseErrorKind::EmptyExpression,
                span: self.current_span(),
            });
        }
        let end = value_tokens
            .last()
            .expect("non-empty checked above")
            .span
            .end;
        let value = parse_expr_tokens(value_tokens, self.source_len, ExprContext::Value)?;
        Ok(CaseExprArm {
            patterns,
            value,
            span: Span::new(start, end),
        })
    }

    /// Collect tokens up to (but NOT consuming) the next `{` at the
    /// caller's paren depth.
    fn collect_until_lbrace_at_depth0(&mut self) -> ParseResult<Vec<Token>> {
        let mut out = Vec::new();
        let mut depth: i32 = 0;
        loop {
            let k = match self.peek_kind() {
                Some(k) => k,
                None => {
                    return Err(ParseError {
                        kind: ParseErrorKind::UnexpectedEof { expected: "`{`" },
                        span: self.eof_span(),
                    });
                }
            };
            if depth == 0 {
                match k {
                    TokenKind::LBrace => return Ok(out),
                    TokenKind::Newline | TokenKind::Semicolon | TokenKind::RBrace => {
                        return Err(ParseError {
                            kind: ParseErrorKind::UnexpectedToken { expected: "`{`" },
                            span: self.current_span(),
                        });
                    }
                    _ => {}
                }
            }
            match k {
                TokenKind::LParen => depth += 1,
                TokenKind::DoubleLParen => depth += 2,
                TokenKind::RParen => depth -= 1,
                TokenKind::DoubleRParen => depth -= 2,
                _ => {}
            }
            if depth < 0 {
                return Err(ParseError {
                    kind: ParseErrorKind::UnexpectedToken { expected: "`{`" },
                    span: self.current_span(),
                });
            }
            out.push(self.advance());
        }
    }

    /// Collect tokens up to (and consuming) `=>` at the caller's paren
    /// depth.
    fn collect_until_fat_arrow_at_depth0(&mut self) -> ParseResult<Vec<Token>> {
        let mut out = Vec::new();
        let mut depth: i32 = 0;
        loop {
            let k = match self.peek_kind() {
                Some(k) => k,
                None => {
                    return Err(ParseError {
                        kind: ParseErrorKind::UnexpectedEof { expected: "`=>`" },
                        span: self.eof_span(),
                    });
                }
            };
            if depth == 0 {
                match k {
                    TokenKind::FatArrow => {
                        self.pos += 1;
                        return Ok(out);
                    }
                    TokenKind::Newline | TokenKind::Semicolon | TokenKind::RBrace => {
                        return Err(ParseError {
                            kind: ParseErrorKind::UnexpectedToken { expected: "`=>`" },
                            span: self.current_span(),
                        });
                    }
                    _ => {}
                }
            }
            match k {
                TokenKind::LParen => depth += 1,
                TokenKind::DoubleLParen => depth += 2,
                TokenKind::RParen => depth -= 1,
                TokenKind::DoubleRParen => depth -= 2,
                _ => {}
            }
            if depth < 0 {
                return Err(ParseError {
                    kind: ParseErrorKind::UnexpectedToken { expected: "`=>`" },
                    span: self.current_span(),
                });
            }
            out.push(self.advance());
        }
    }

    /// Collect tokens for a case-expression arm value: stops at
    /// top-level Newline / Semicolon / RBrace (without consuming
    /// the terminator).
    fn collect_arm_value_tokens(&mut self) -> ParseResult<Vec<Token>> {
        let mut out = Vec::new();
        let mut depth: i32 = 0;
        loop {
            let k = match self.peek_kind() {
                Some(k) => k,
                None => break,
            };
            if depth == 0 {
                match k {
                    TokenKind::Newline | TokenKind::Semicolon | TokenKind::RBrace => break,
                    _ => {}
                }
            }
            match k {
                TokenKind::LParen => depth += 1,
                TokenKind::DoubleLParen => depth += 2,
                TokenKind::RParen => depth -= 1,
                TokenKind::DoubleRParen => depth -= 2,
                TokenKind::LBrace => depth += 1, // nested case-expr {}
                TokenKind::RBrace if depth > 0 => depth -= 1,
                _ => {}
            }
            if depth < 0 {
                return Err(ParseError {
                    kind: ParseErrorKind::UnexpectedToken { expected: "`}`" },
                    span: self.current_span(),
                });
            }
            out.push(self.advance());
        }
        Ok(out)
    }
}

// ---------------------------------------------------------------------
// Free helpers
// ---------------------------------------------------------------------

enum EqOrMatch {
    Eq,
    Ne,
    Matches,
    In,
    Regex,
}

fn bin(op: BinOp, left: Expr, right: Expr) -> Expr {
    let span = Span::new(left.span.start, right.span.end);
    Expr {
        kind: ExprKind::BinOp(op, Box::new(left), Box::new(right)),
        span,
    }
}

fn match_expr(op: MatchOp, left: Expr, right: Expr) -> Expr {
    let span = Span::new(left.span.start, right.span.end);
    Expr {
        kind: ExprKind::Match(op, Box::new(left), Box::new(right)),
        span,
    }
}

fn is_value_token_kind(k: &TokenKind) -> bool {
    matches!(
        k,
        TokenKind::Word(_)
            | TokenKind::SingleQuoted(_)
            | TokenKind::DoubleQuoted(_)
            | TokenKind::Var(_)
            | TokenKind::VarLen(_)
            | TokenKind::VarNoSplit(_)
            | TokenKind::Subst(_)
            | TokenKind::Backtick(_)
            | TokenKind::ProcSubIn(_)
            | TokenKind::ProcSubOut(_)
            | TokenKind::Regex(_)
    )
}

fn parse_int(s: &str) -> Option<i64> {
    if s.is_empty() {
        return None;
    }
    // Allow optional leading `-` for negative literals embedded as a
    // single word (`Word("-1")`). The Pratt unary path handles
    // `Minus, Word("1")` separately.
    let (sign, rest) = if let Some(rest) = s.strip_prefix('-') {
        (-1i64, rest)
    } else if let Some(rest) = s.strip_prefix('+') {
        (1i64, rest)
    } else {
        (1i64, s)
    };
    if rest.is_empty() {
        return None;
    }
    let mut n: i64 = 0;
    for b in rest.bytes() {
        if !b.is_ascii_digit() {
            return None;
        }
        n = n.checked_mul(10)?.checked_add((b - b'0') as i64)?;
    }
    sign.checked_mul(n)
}

/// Retokenize an entire arith-mode token stream by replacing every
/// non-integer Word that contains only arith operator chars with
/// its split form (digit-Words + operator tokens). Runs once at
/// parser construction; the resulting stream is what the Pratt
/// climbing path walks.
///
/// Tokens that aren't Words are passed through unchanged. Words
/// that ARE valid integers (per `parse_int`) are passed through
/// unchanged so the primary path sees them as integer literals.
fn retokenize_arith_stream(tokens: Vec<Token>) -> Vec<Token> {
    let mut out: Vec<Token> = Vec::with_capacity(tokens.len());
    for tok in tokens {
        match &tok.kind {
            TokenKind::Word(text) if parse_int(text).is_none() => {
                let text_clone = text.clone();
                let base_start = tok.span.start;
                if let Some(micro) = retokenize_arith_word(&text_clone, base_start) {
                    if !micro.is_empty() {
                        out.extend(micro);
                        continue;
                    }
                }
                out.push(tok);
            }
            _ => out.push(tok),
        }
    }
    out
}

/// Retokenize an arith-mode Word's text into a stream of digit-Word
/// + operator tokens. Returns None if the word contains anything
/// other than digits + the recognized operator chars.
///
/// Recognized operator chars: `+ - * / %`. Other chars (alphabetic,
/// `_`, etc.) fail the retokenization.
///
/// Span calculation: each emitted token's span is anchored at
/// `base_start + offset_in_text`.
fn retokenize_arith_word(text: &str, base_start: usize) -> Option<Vec<Token>> {
    let bytes = text.as_bytes();
    let mut out: Vec<Token> = Vec::new();
    let mut i: usize = 0;
    while i < bytes.len() {
        let b = bytes[i];
        if b.is_ascii_digit() {
            let start = i;
            while i < bytes.len() && bytes[i].is_ascii_digit() {
                i += 1;
            }
            let s = &text[start..i];
            out.push(Token::new(
                TokenKind::Word(String::from(s)),
                Span::new(base_start + start, base_start + i),
            ));
            continue;
        }
        let kind = match b {
            b'+' => TokenKind::Plus,
            b'-' => TokenKind::Minus,
            b'*' => TokenKind::Star,
            b'/' => TokenKind::Slash,
            b'%' => TokenKind::Percent,
            _ => return None, // unsupported char in arith retokenization
        };
        out.push(Token::new(
            kind,
            Span::new(base_start + i, base_start + i + 1),
        ));
        i += 1;
    }
    Some(out)
}

/// Eagerly parse a substitution body (`$(cmd)`, `` `{cmd}` ``,
/// `<(cmd)`, `>(cmd)`) into a sub-Script.
///
/// The body's source coordinates are NOT translated to outer-source
/// coordinates; sub-script spans are relative to the body. If U-5d
/// or U-6 needs absolute coordinates for error reporting, the
/// outer span (passed here as `_outer_span`) anchors the
/// translation.
fn parse_subscript(body: &str, _outer_span: Span) -> ParseResult<Script> {
    let toks = tokenize(body)?;
    parse_tokens(toks, body.len())
}

/// Helper for VarIndex / VarSlice -- called when the closing `)`
/// has been consumed and `body` holds the tokens between the `(`
/// and `)`. Splits body on top-level whitespace boundaries to
/// produce one or two arith sub-expressions.
fn finish_var_index_with_body(
    name: String,
    var_span: Span,
    close_end: usize,
    body: Vec<Token>,
    source_len: usize,
) -> ParseResult<Expr> {
    // Empty body -> InvalidVarIndex.
    if body.is_empty() {
        return Err(ParseError {
            kind: ParseErrorKind::InvalidVarIndex,
            span: var_span,
        });
    }
    // Split body into top-level whitespace-separated sub-expressions.
    // Within a sub-expression, paren depth tracks nesting; at depth 0
    // a gap between two adjacent tokens (token[i].span.end !=
    // token[i+1].span.start) is the split point.
    let parts = split_value_tokens_on_whitespace(body);
    if parts.len() == 1 {
        let idx = parse_expr_tokens(parts.into_iter().next().expect("len 1"), source_len, ExprContext::Arith)?;
        return Ok(Expr {
            kind: ExprKind::VarIndex(name, Box::new(idx)),
            span: Span::new(var_span.start, close_end),
        });
    }
    if parts.len() == 2 {
        let mut it = parts.into_iter();
        let m = parse_expr_tokens(it.next().expect("first"), source_len, ExprContext::Arith)?;
        let n = parse_expr_tokens(it.next().expect("second"), source_len, ExprContext::Arith)?;
        return Ok(Expr {
            kind: ExprKind::VarSlice(name, Box::new(m), Box::new(n)),
            span: Span::new(var_span.start, close_end),
        });
    }
    Err(ParseError {
        kind: ParseErrorKind::InvalidVarIndex,
        span: var_span,
    })
}

/// Split a token stream into sub-expression slices on top-level
/// whitespace boundaries.
///
/// We detect whitespace by looking at the span gap between two
/// adjacent tokens at paren depth 0: when `tok[i].span.end !=
/// tok[i+1].span.start`, the user wrote whitespace between them.
///
/// Used at U-5c for `$var(N)` / `$var(M N)` index/slice splitting
/// and at U-5d for `case` arm pattern list splitting
/// (`*.c *.h => body`).
pub(super) fn split_value_tokens_on_whitespace(body: Vec<Token>) -> Vec<Vec<Token>> {
    if body.is_empty() {
        return Vec::new();
    }
    let mut parts: Vec<Vec<Token>> = Vec::new();
    let mut current: Vec<Token> = Vec::new();
    let mut depth: i32 = 0;
    for (i, tok) in body.iter().enumerate() {
        if depth == 0 && !current.is_empty() {
            let prev_end = current.last().expect("non-empty").span.end;
            if tok.span.start != prev_end {
                // Whitespace gap -- split.
                parts.push(core::mem::take(&mut current));
            }
        }
        match &tok.kind {
            TokenKind::LParen => depth += 1,
            TokenKind::DoubleLParen => depth += 2,
            TokenKind::RParen => depth -= 1,
            TokenKind::DoubleRParen => depth -= 2,
            _ => {}
        }
        let _ = i; // unused; preserves loop-variable shape for future use
        current.push(tok.clone());
    }
    if !current.is_empty() {
        parts.push(current);
    }
    parts
}

// ---------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------

#[cfg(test)]
mod tests {
    use super::*;
    use alloc::vec;

    fn lex(s: &str) -> Vec<Token> {
        tokenize(s).expect("lex ok")
    }

    fn expr_ok(src: &str, ctx: ExprContext) -> Expr {
        parse_expr_tokens(lex(src), src.len(), ctx)
            .unwrap_or_else(|e| panic!("parse_expr_tokens failed: {:?}", e))
    }

    fn expr_err(src: &str, ctx: ExprContext) -> ParseErrorKind {
        parse_expr_tokens(lex(src), src.len(), ctx).unwrap_err().kind
    }

    // -----------------------------------------------------------------
    // Value context tests
    // -----------------------------------------------------------------

    #[test]
    fn value_single_word() {
        let e = expr_ok("hello", ExprContext::Value);
        match &e.kind {
            ExprKind::Word(s) => assert_eq!(s, "hello"),
            other => panic!("expected Word, got {:?}", other),
        }
    }

    #[test]
    fn value_single_var() {
        let e = expr_ok("$x", ExprContext::Value);
        match &e.kind {
            ExprKind::Var(s) => assert_eq!(s, "x"),
            other => panic!("expected Var, got {:?}", other),
        }
    }

    #[test]
    fn value_concat() {
        let e = expr_ok("$a^$b^$c", ExprContext::Value);
        match &e.kind {
            ExprKind::Concat(parts) => assert_eq!(parts.len(), 3),
            other => panic!("expected Concat, got {:?}", other),
        }
    }

    #[test]
    fn value_concat_requires_adjacency() {
        // `$a ^ $b` -- with spaces, no concat; the leading `$a` is
        // the value, then trailing tokens fail TrailingTokensInExpr.
        let kind = expr_err("$a ^ $b", ExprContext::Value);
        assert!(
            matches!(kind, ParseErrorKind::TrailingTokensInExpr),
            "expected TrailingTokensInExpr, got {:?}",
            kind
        );
    }

    #[test]
    fn value_list_literal() {
        let e = expr_ok("(a b c)", ExprContext::Value);
        match &e.kind {
            ExprKind::List(elems) => assert_eq!(elems.len(), 3),
            other => panic!("expected List, got {:?}", other),
        }
    }

    #[test]
    fn value_empty_list() {
        let e = expr_ok("()", ExprContext::Value);
        match &e.kind {
            ExprKind::List(elems) => assert_eq!(elems.len(), 0),
            other => panic!("expected empty List, got {:?}", other),
        }
    }

    #[test]
    fn value_single_quoted() {
        let e = expr_ok("'hello world'", ExprContext::Value);
        match &e.kind {
            ExprKind::SingleQuoted(s) => assert_eq!(s, "hello world"),
            other => panic!("expected SingleQuoted, got {:?}", other),
        }
    }

    #[test]
    fn value_var_index() {
        let e = expr_ok("$files(1)", ExprContext::Value);
        match &e.kind {
            ExprKind::VarIndex(name, idx) => {
                assert_eq!(name, "files");
                match &idx.kind {
                    ExprKind::Integer(1) => {}
                    other => panic!("expected Integer(1), got {:?}", other),
                }
            }
            other => panic!("expected VarIndex, got {:?}", other),
        }
    }

    #[test]
    fn value_var_slice() {
        let e = expr_ok("$files(2 3)", ExprContext::Value);
        match &e.kind {
            ExprKind::VarSlice(name, m, n) => {
                assert_eq!(name, "files");
                match (&m.kind, &n.kind) {
                    (ExprKind::Integer(2), ExprKind::Integer(3)) => {}
                    other => panic!("expected Integer(2), Integer(3), got {:?}", other),
                }
            }
            other => panic!("expected VarSlice, got {:?}", other),
        }
    }

    #[test]
    fn value_varlen() {
        let e = expr_ok("$#files", ExprContext::Value);
        match &e.kind {
            ExprKind::VarLen(s) => assert_eq!(s, "files"),
            other => panic!("expected VarLen, got {:?}", other),
        }
    }

    #[test]
    fn value_inline_arith() {
        let e = expr_ok("(( 1 + 2 ))", ExprContext::Value);
        match &e.kind {
            ExprKind::BinOp(BinOp::Add, l, r) => match (&l.kind, &r.kind) {
                (ExprKind::Integer(1), ExprKind::Integer(2)) => {}
                other => panic!("expected Integer 1+2, got {:?}", other),
            },
            other => panic!("expected BinOp(Add), got {:?}", other),
        }
    }

    // -----------------------------------------------------------------
    // Arith context tests
    // -----------------------------------------------------------------

    #[test]
    fn arith_simple_add() {
        let e = expr_ok("1 + 2", ExprContext::Arith);
        match &e.kind {
            ExprKind::BinOp(BinOp::Add, l, r) => match (&l.kind, &r.kind) {
                (ExprKind::Integer(1), ExprKind::Integer(2)) => {}
                other => panic!("got {:?}", other),
            },
            other => panic!("got {:?}", other),
        }
    }

    #[test]
    fn arith_precedence_mul_over_add() {
        // 1 + 2 * 3 -> Add(1, Mul(2, 3))
        let e = expr_ok("1 + 2 * 3", ExprContext::Arith);
        match &e.kind {
            ExprKind::BinOp(BinOp::Add, l, r) => {
                match &l.kind {
                    ExprKind::Integer(1) => {}
                    other => panic!("got left {:?}", other),
                }
                match &r.kind {
                    ExprKind::BinOp(BinOp::Mul, _, _) => {}
                    other => panic!("got right {:?}", other),
                }
            }
            other => panic!("got {:?}", other),
        }
    }

    #[test]
    fn arith_paren_grouping() {
        // (1 + 2) * 3 -> Mul(Add(1, 2), 3)
        let e = expr_ok("(1 + 2) * 3", ExprContext::Arith);
        match &e.kind {
            ExprKind::BinOp(BinOp::Mul, l, r) => {
                match &l.kind {
                    ExprKind::BinOp(BinOp::Add, _, _) => {}
                    other => panic!("got left {:?}", other),
                }
                match &r.kind {
                    ExprKind::Integer(3) => {}
                    other => panic!("got right {:?}", other),
                }
            }
            other => panic!("got {:?}", other),
        }
    }

    #[test]
    fn arith_unary_neg() {
        let e = expr_ok("- 5", ExprContext::Arith);
        match &e.kind {
            ExprKind::UnOp(UnOp::Neg, inner) => match &inner.kind {
                ExprKind::Integer(5) => {}
                other => panic!("got {:?}", other),
            },
            other => panic!("got {:?}", other),
        }
    }

    #[test]
    fn arith_pow_right_assoc() {
        // 2 ** 3 ** 2 -> Pow(2, Pow(3, 2)) = 512
        let e = expr_ok("2 ** 3 ** 2", ExprContext::Arith);
        match &e.kind {
            ExprKind::BinOp(BinOp::Pow, l, r) => {
                match &l.kind {
                    ExprKind::Integer(2) => {}
                    other => panic!("got left {:?}", other),
                }
                match &r.kind {
                    ExprKind::BinOp(BinOp::Pow, _, _) => {}
                    other => panic!("got right {:?}", other),
                }
            }
            other => panic!("got {:?}", other),
        }
    }

    #[test]
    fn arith_var_deref() {
        let e = expr_ok("$x + $y", ExprContext::Arith);
        match &e.kind {
            ExprKind::BinOp(BinOp::Add, l, r) => match (&l.kind, &r.kind) {
                (ExprKind::Var(x), ExprKind::Var(y)) => {
                    assert_eq!(x, "x");
                    assert_eq!(y, "y");
                }
                other => panic!("got {:?}", other),
            },
            other => panic!("got {:?}", other),
        }
    }

    #[test]
    fn arith_invalid_literal_fails() {
        // `(( hello ))` -- the body is Word("hello"), not an integer.
        let kind = expr_err("hello", ExprContext::Arith);
        assert!(
            matches!(kind, ParseErrorKind::InvalidArithLiteral),
            "got {:?}",
            kind
        );
    }

    #[test]
    fn arith_no_whitespace_retokenize() {
        // `1+2` -- the lexer produces Word("1+2"); the arith parser
        // retokenizes into Word("1"), Plus, Word("2") and parses
        // Add(1, 2).
        let e = expr_ok("1+2", ExprContext::Arith);
        match &e.kind {
            ExprKind::BinOp(BinOp::Add, _, _) => {}
            other => panic!("got {:?}", other),
        }
    }

    #[test]
    fn arith_comparison() {
        let e = expr_ok("$x == 0", ExprContext::Arith);
        match &e.kind {
            ExprKind::BinOp(BinOp::Eq, _, _) => {}
            other => panic!("got {:?}", other),
        }
    }

    #[test]
    fn arith_bitand_bitor_bitxor() {
        let _and = expr_ok("$x & $y", ExprContext::Arith);
        let _or = expr_ok("$x | $y", ExprContext::Arith);
        let _xor = expr_ok("$x ^ $y", ExprContext::Arith);
        // Just ensure they parse.
    }

    // -----------------------------------------------------------------
    // Cond context tests
    // -----------------------------------------------------------------

    #[test]
    fn cond_simple_eq() {
        let e = expr_ok("$a == $b", ExprContext::Cond);
        match &e.kind {
            ExprKind::BinOp(BinOp::Eq, _, _) => {}
            other => panic!("got {:?}", other),
        }
    }

    #[test]
    fn cond_matches() {
        let e = expr_ok("$file matches *.c", ExprContext::Cond);
        match &e.kind {
            ExprKind::Match(MatchOp::Glob, _, _) => {}
            other => panic!("got {:?}", other),
        }
    }

    #[test]
    fn cond_in() {
        let e = expr_ok("$x in $files", ExprContext::Cond);
        match &e.kind {
            ExprKind::Match(MatchOp::In, _, _) => {}
            other => panic!("got {:?}", other),
        }
    }

    #[test]
    fn cond_regex() {
        let e = expr_ok("$x =~ /foo/", ExprContext::Cond);
        match &e.kind {
            ExprKind::Match(MatchOp::Regex, _, _) => {}
            other => panic!("got {:?}", other),
        }
    }

    #[test]
    fn cond_and_or() {
        let e = expr_ok("$a && $b || $c", ExprContext::Cond);
        // Parses as Or(And(a, b), c) due to standard precedence.
        match &e.kind {
            ExprKind::BinOp(BinOp::Or, l, _) => match &l.kind {
                ExprKind::BinOp(BinOp::And, _, _) => {}
                other => panic!("got {:?}", other),
            },
            other => panic!("got {:?}", other),
        }
    }

    #[test]
    fn cond_unary_not() {
        let e = expr_ok("! $x", ExprContext::Cond);
        match &e.kind {
            ExprKind::UnOp(UnOp::Not, _) => {}
            other => panic!("got {:?}", other),
        }
    }

    #[test]
    fn cond_paren_grouping() {
        let e = expr_ok("($a || $b) && $c", ExprContext::Cond);
        match &e.kind {
            ExprKind::BinOp(BinOp::And, l, _) => match &l.kind {
                ExprKind::BinOp(BinOp::Or, _, _) => {}
                other => panic!("got {:?}", other),
            },
            other => panic!("got {:?}", other),
        }
    }

    #[test]
    fn cond_relational() {
        let e = expr_ok("$x < $y", ExprContext::Cond);
        match &e.kind {
            ExprKind::BinOp(BinOp::Lt, _, _) => {}
            other => panic!("got {:?}", other),
        }
    }

    // -----------------------------------------------------------------
    // List context tests
    // -----------------------------------------------------------------

    #[test]
    fn list_single_var() {
        let e = expr_ok("$files", ExprContext::List);
        match &e.kind {
            ExprKind::Var(s) => assert_eq!(s, "files"),
            other => panic!("got {:?}", other),
        }
    }

    #[test]
    fn list_explicit_list() {
        let e = expr_ok("(a b c)", ExprContext::List);
        match &e.kind {
            ExprKind::List(parts) => assert_eq!(parts.len(), 3),
            other => panic!("got {:?}", other),
        }
    }

    #[test]
    fn list_implicit_list() {
        // `for (f in a b c)` -- implicit 3-element list.
        let e = expr_ok("a b c", ExprContext::List);
        match &e.kind {
            ExprKind::List(parts) => assert_eq!(parts.len(), 3),
            other => panic!("got {:?}", other),
        }
    }

    // -----------------------------------------------------------------
    // Empty / error cases
    // -----------------------------------------------------------------

    #[test]
    fn empty_expression_errors() {
        let result = parse_expr_tokens(Vec::new(), 0, ExprContext::Value);
        assert!(matches!(
            result.unwrap_err().kind,
            ParseErrorKind::EmptyExpression
        ));
    }

    #[test]
    fn arith_empty_paren_grouping_errors() {
        // `()` in arith is an empty grouping -- empty body parses to
        // EmptyExpression.
        let kind = expr_err("()", ExprContext::Arith);
        assert!(
            matches!(kind, ParseErrorKind::EmptyExpression),
            "got {:?}",
            kind
        );
    }

    #[test]
    fn invalid_var_index_too_many() {
        // `$files(1 2 3)` -- three integers, not 1-2 expected.
        let kind = expr_err("$files(1 2 3)", ExprContext::Value);
        assert!(
            matches!(kind, ParseErrorKind::InvalidVarIndex),
            "got {:?}",
            kind
        );
    }

    // -----------------------------------------------------------------
    // Substitution body lifting
    // -----------------------------------------------------------------

    #[test]
    fn subst_body_lifts_to_subscript() {
        let e = expr_ok("$(ls /etc)", ExprContext::Value);
        match &e.kind {
            ExprKind::Subst(script) => {
                assert_eq!(script.statements.len(), 1);
            }
            other => panic!("got {:?}", other),
        }
    }

    #[test]
    fn backtick_body_lifts_to_subscript() {
        let e = expr_ok("`{echo hi}", ExprContext::Value);
        match &e.kind {
            ExprKind::Backtick(script) => {
                assert_eq!(script.statements.len(), 1);
            }
            other => panic!("got {:?}", other),
        }
    }

    // -----------------------------------------------------------------
    // Span tracking
    // -----------------------------------------------------------------

    #[test]
    fn span_covers_whole_expression() {
        let e = expr_ok("$a + $b", ExprContext::Arith);
        assert_eq!(e.span.start, 0);
        assert_eq!(e.span.end, 7);
    }

    #[test]
    fn span_covers_concat() {
        let src = "$a^$b";
        let e = expr_ok(src, ExprContext::Value);
        assert_eq!(e.span.start, 0);
        assert_eq!(e.span.end, src.len());
    }

    #[test]
    fn span_covers_list() {
        let src = "(a b c)";
        let e = expr_ok(src, ExprContext::Value);
        assert_eq!(e.span.start, 0);
        assert_eq!(e.span.end, src.len());
    }

    // -----------------------------------------------------------------
    // parse_int helper
    // -----------------------------------------------------------------

    #[test]
    fn parse_int_basic() {
        assert_eq!(parse_int("0"), Some(0));
        assert_eq!(parse_int("42"), Some(42));
        assert_eq!(parse_int("-7"), Some(-7));
        assert_eq!(parse_int("+3"), Some(3));
        assert_eq!(parse_int(""), None);
        assert_eq!(parse_int("hello"), None);
        assert_eq!(parse_int("1.5"), None);
        assert_eq!(parse_int("-"), None);
    }

    // -----------------------------------------------------------------
    // retokenize_arith_word helper
    // -----------------------------------------------------------------

    #[test]
    fn retokenize_simple() {
        let r = retokenize_arith_word("1+2", 10).expect("ok");
        assert_eq!(r.len(), 3);
        // Token 0: Word("1") at span [10, 11)
        match &r[0].kind {
            TokenKind::Word(s) => assert_eq!(s, "1"),
            _ => panic!(),
        }
        assert_eq!(r[0].span.start, 10);
        assert_eq!(r[0].span.end, 11);
        // Token 1: Plus at span [11, 12)
        assert!(matches!(r[1].kind, TokenKind::Plus));
        // Token 2: Word("2") at span [12, 13)
        match &r[2].kind {
            TokenKind::Word(s) => assert_eq!(s, "2"),
            _ => panic!(),
        }
    }

    #[test]
    fn retokenize_rejects_alphabetic() {
        assert!(retokenize_arith_word("hello", 0).is_none());
        assert!(retokenize_arith_word("1+a", 0).is_none());
    }

    #[test]
    fn retokenize_multi_op() {
        let r = retokenize_arith_word("1*2+3", 0).expect("ok");
        assert_eq!(r.len(), 5);
        assert!(matches!(r[1].kind, TokenKind::Star));
        assert!(matches!(r[3].kind, TokenKind::Plus));
    }

    // -----------------------------------------------------------------
    // split_value_tokens_on_whitespace helper
    // -----------------------------------------------------------------

    #[test]
    fn split_one_index() {
        let tokens = lex("1");
        // Exclude the trailing Eof.
        let body: Vec<Token> = tokens.into_iter().filter(|t| !t.is_eof()).collect();
        let parts = split_value_tokens_on_whitespace(body);
        assert_eq!(parts.len(), 1);
        assert_eq!(parts[0].len(), 1);
    }

    #[test]
    fn split_two_indices_via_space() {
        let tokens = lex("2 3");
        let body: Vec<Token> = tokens.into_iter().filter(|t| !t.is_eof()).collect();
        let parts = split_value_tokens_on_whitespace(body);
        assert_eq!(parts.len(), 2);
    }

    // -----------------------------------------------------------------
    // Misc
    // -----------------------------------------------------------------

    #[test]
    fn value_double_quoted_preserves_parts() {
        let src = "\"hello $name\"";
        let e = expr_ok(src, ExprContext::Value);
        match &e.kind {
            ExprKind::DoubleQuoted(parts) => {
                assert!(!parts.is_empty());
            }
            other => panic!("got {:?}", other),
        }
    }

    #[test]
    fn value_regex_preserves_body() {
        // Regex is normally only after `=~`; we feed a Word followed
        // by `=~ /pat/` to exercise the Regex token through the
        // Match path.
        let e = expr_ok("$x =~ /foo/", ExprContext::Cond);
        match &e.kind {
            ExprKind::Match(MatchOp::Regex, _, right) => match &right.kind {
                ExprKind::Regex(s) => assert_eq!(s, "foo"),
                other => panic!("got {:?}", other),
            },
            other => panic!("got {:?}", other),
        }
    }

    #[test]
    fn list_implicit_concat_within_element() {
        // `pre^$x mid^$y` -- two list elements, each a Concat.
        let e = expr_ok("pre^$x mid^$y", ExprContext::List);
        match &e.kind {
            ExprKind::List(parts) => {
                assert_eq!(parts.len(), 2);
                for p in parts {
                    assert!(matches!(p.kind, ExprKind::Concat(_)));
                }
            }
            other => panic!("got {:?}", other),
        }
    }

    // suppress alloc::vec unused import warning since we do use it in
    // some tests
    #[allow(dead_code)]
    fn _vec_used() {
        let _: Vec<i32> = vec![1, 2, 3];
    }

    // -----------------------------------------------------------------
    // U-5d: case-as-expression
    // -----------------------------------------------------------------

    #[test]
    fn case_as_expr_basic() {
        let e = expr_ok(
            "case $f { *.c => 'C source' ; * => 'unknown' }",
            ExprContext::Value,
        );
        match &e.kind {
            ExprKind::Case(c) => {
                assert_eq!(c.arms.len(), 2);
                match &c.scrutinee.kind {
                    ExprKind::Var(v) => assert_eq!(v, "f"),
                    other => panic!("got {:?}", other),
                }
                match &c.arms[0].value.kind {
                    ExprKind::SingleQuoted(s) => assert_eq!(s, "C source"),
                    other => panic!("got {:?}", other),
                }
            }
            other => panic!("expected Case, got {:?}", other),
        }
    }

    #[test]
    fn case_as_expr_multi_pattern() {
        let e = expr_ok(
            "case $f { *.c *.h => 'C' ; * => 'other' }",
            ExprContext::Value,
        );
        match &e.kind {
            ExprKind::Case(c) => {
                assert_eq!(c.arms[0].patterns.len(), 2);
                match &c.arms[0].patterns[0].kind {
                    ExprKind::Word(w) => assert_eq!(w, "*.c"),
                    _ => panic!(),
                }
                match &c.arms[0].patterns[1].kind {
                    ExprKind::Word(w) => assert_eq!(w, "*.h"),
                    _ => panic!(),
                }
            }
            _ => panic!(),
        }
    }

    #[test]
    fn case_as_expr_newline_arms() {
        let e = expr_ok(
            "case $f {\n  *.c => 'C'\n  *.rs => 'Rust'\n  * => 'other'\n}",
            ExprContext::Value,
        );
        match &e.kind {
            ExprKind::Case(c) => assert_eq!(c.arms.len(), 3),
            _ => panic!(),
        }
    }

    #[test]
    fn case_as_expr_in_cond() {
        // case-as-expression works in Cond context too -- the
        // evaluator decides what to do with the result.
        let e = expr_ok(
            "case $x { 'yes' => 1 ; * => 0 } == 1",
            ExprContext::Cond,
        );
        match &e.kind {
            ExprKind::BinOp(BinOp::Eq, lhs, _) => {
                assert!(matches!(lhs.kind, ExprKind::Case(_)));
            }
            other => panic!("expected BinOp(Eq, Case, _), got {:?}", other),
        }
    }
}
