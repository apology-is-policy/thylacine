// libutopia::parser::parse -- the U-5b recursive-descent parser
// over the U-5a token stream.
//
// === Strategic framing ===
//
// The parser consumes a `Vec<Token>` (produced by `tokenize()`)
// + the source length (for the synthetic Eof point span) and
// produces a `Script` AST. The parser TAKES OWNERSHIP of the
// tokens so it can `mem::take` heredoc body parts out of
// HeredocBody tokens (avoiding a clone of Vec<DqPart>). The
// extracted bodies live in a FIFO queue (`heredoc_bodies`) and
// are popped each time the parser encounters a HeredocStart in a
// redirect context.
//
// The grammar is recursive-descent; one method per non-terminal.
// Lookahead is mostly one token (peek_kind) with two-token
// lookahead in two places: detecting assignment at statement
// start (`IDENT Equal`) and Concat in word position
// (`value Caret value` with span adjacency).
//
// === Heredoc handling ===
//
// The lexer emits HeredocStart at the source position and
// HeredocBody after the next Newline. The parser pre-extracts
// all body parts into `heredoc_bodies` (FIFO) during Parser::new.
// When the parser encounters a HeredocStart in parse_redirect, it
// pops the next body from the queue. HeredocBody tokens
// themselves are treated as separators (skipped) during
// statement walking -- their parts are already inlined into the
// matching Redirect AST node.
//
// === Span tracking ===
//
// Every AST node carries a Span from first-token-start through
// last-token-end. Statement parsers compute this by capturing
// the start position before consuming the first token and the
// end position after consuming the last token.

use alloc::boxed::Box;
use alloc::collections::VecDeque;
use alloc::string::String;
use alloc::vec::Vec;

use super::ast::*;
use super::error::{ParseError, ParseErrorKind, ParseResult};
use super::expr::{parse_expr_tokens, split_value_tokens_on_whitespace};
use super::lexer::tokenize;
use super::span::Span;
use super::token::{DqPart, Token, TokenKind};

// ---------------------------------------------------------------------
// Public entries
// ---------------------------------------------------------------------

/// Parse a source string into a Script AST. Internally tokenizes
/// then runs the parser.
pub fn parse(source: &str) -> ParseResult<Script> {
    let tokens = tokenize(source)?;
    parse_tokens(tokens, source.len())
}

/// Maximum `()`/`{}`/`(( ))` bracket-nesting depth accepted within a single
/// token stream. Subshells, brace blocks, control-flow bodies, and arith
/// groups each recurse one level per opener; without a bound, deeply nested
/// input (`((((...))))`, `{{{...}}}`, `if{if{...}}`) overflows the 256 KiB EL0
/// stack into a guard-page snare:segv -> shell death (scripture 8.1). 64 is
/// far beyond any real script's nesting (the deepest realistic nesting is a
/// handful of levels). The `$(...)` command-substitution re-lex vector is
/// bounded separately in `parse_subscript` (it crosses a re-tokenize boundary
/// this single-stream pass cannot see).
const PARSE_MAX_NESTING: i32 = 64;

/// Reject a token stream whose `()`/`{}`/`(( ))` nesting exceeds
/// `PARSE_MAX_NESTING`, BEFORE the recursive-descent parser walks it. A single
/// linear pass tracking the running nesting depth (one `+1` per opener token =
/// one recursive-descent level); MAX-nesting, so sequential non-nested brackets
/// (`(a); (b); ...`) are unaffected. `(( ))` is a single DoubleLParen token =
/// one arith-group level, so it counts +1 like `(`.
fn check_token_nesting(tokens: &[Token]) -> ParseResult<()> {
    let mut depth: i32 = 0;
    for t in tokens {
        match t.kind {
            TokenKind::LParen | TokenKind::LBrace | TokenKind::DoubleLParen => depth += 1,
            TokenKind::RParen | TokenKind::RBrace | TokenKind::DoubleRParen => depth -= 1,
            _ => {}
        }
        if depth > PARSE_MAX_NESTING {
            return Err(ParseError {
                kind: ParseErrorKind::RecursionLimit,
                span: t.span,
            });
        }
    }
    Ok(())
}

/// Parse a pre-tokenized stream into a Script AST. Useful when the
/// caller already has the tokens (e.g., for incremental parsing or
/// editor integration).
pub fn parse_tokens(tokens: Vec<Token>, source_len: usize) -> ParseResult<Script> {
    check_token_nesting(&tokens)?;
    Parser::new(tokens, source_len).parse_script()
}

// ---------------------------------------------------------------------
// Parser state
// ---------------------------------------------------------------------

struct Parser {
    tokens: Vec<Token>,
    pos: usize,
    /// FIFO queue of heredoc bodies extracted from HeredocBody
    /// tokens during construction. Popped front when a redirect
    /// consumes a HeredocStart.
    heredoc_bodies: VecDeque<Vec<DqPart>>,
    /// `source.len()` -- used as the Eof point-span fallback.
    source_len: usize,
}

impl Parser {
    fn new(mut tokens: Vec<Token>, source_len: usize) -> Self {
        let mut bodies = VecDeque::new();
        for tok in tokens.iter_mut() {
            if let TokenKind::HeredocBody(parts) = &mut tok.kind {
                bodies.push_back(core::mem::take(parts));
            }
        }
        Self {
            tokens,
            pos: 0,
            heredoc_bodies: bodies,
            source_len,
        }
    }

    // -----------------------------------------------------------------
    // Cursor helpers
    // -----------------------------------------------------------------

    fn peek_token(&self) -> Option<&Token> {
        self.tokens.get(self.pos)
    }

    fn peek_kind(&self) -> Option<&TokenKind> {
        self.peek_token().map(|t| &t.kind)
    }

    fn peek_kind_at(&self, offset: usize) -> Option<&TokenKind> {
        self.tokens.get(self.pos + offset).map(|t| &t.kind)
    }

    fn at_eof(&self) -> bool {
        matches!(self.peek_kind(), None | Some(TokenKind::Eof))
    }

    /// Consume the current token and return it (cloned). Caller
    /// is responsible for not advancing past EOF.
    fn advance(&mut self) -> Token {
        let t = self.tokens[self.pos].clone();
        self.pos += 1;
        t
    }

    /// Consume a specific kind or emit an UnexpectedToken /
    /// UnexpectedEof error.
    fn expect_kind(&mut self, want: TokenKind, label: &'static str) -> ParseResult<Token> {
        match self.peek_kind() {
            Some(k) if core::mem::discriminant(k) == core::mem::discriminant(&want) => {
                Ok(self.advance())
            }
            Some(_) => Err(ParseError {
                kind: ParseErrorKind::UnexpectedToken { expected: label },
                span: self.current_span(),
            }),
            None => Err(ParseError {
                kind: ParseErrorKind::UnexpectedEof { expected: label },
                span: self.eof_span(),
            }),
        }
    }

    /// Consume a Word token whose text matches the identifier
    /// pattern (`[a-zA-Z_][a-zA-Z0-9_]*`). Returns the text.
    fn expect_ident(&mut self) -> ParseResult<String> {
        let span = self.current_span();
        let tok = self.peek_token().ok_or_else(|| ParseError {
            kind: ParseErrorKind::UnexpectedEof {
                expected: "identifier",
            },
            span: self.eof_span(),
        })?;
        if let TokenKind::Word(w) = &tok.kind {
            if is_valid_ident(w) {
                let owned = w.clone();
                self.pos += 1;
                return Ok(owned);
            }
        }
        Err(ParseError {
            kind: ParseErrorKind::ExpectedIdent,
            span,
        })
    }

    /// Skip separator tokens (Newline, Semicolon, HeredocBody).
    /// HeredocBody tokens are filler -- their parts have already
    /// been pre-extracted into `heredoc_bodies` and inlined into
    /// the matching Redirect AST node.
    fn skip_separators(&mut self) {
        while let Some(k) = self.peek_kind() {
            match k {
                TokenKind::Newline | TokenKind::Semicolon | TokenKind::HeredocBody(_) => {
                    self.pos += 1;
                }
                _ => break,
            }
        }
    }

    /// Tokens that can legally follow a statement (terminators).
    fn at_statement_terminator(&self) -> bool {
        matches!(
            self.peek_kind(),
            None | Some(TokenKind::Eof)
                | Some(TokenKind::Newline)
                | Some(TokenKind::Semicolon)
                | Some(TokenKind::RBrace)
                | Some(TokenKind::RParen)
                | Some(TokenKind::HeredocBody(_))
        )
    }

    /// Tokens that can legally follow a postfix `?` per the U-5b
    /// design lock (strict: `cmd ? arg` is a parse error).
    fn at_postfix_question_terminator(&self) -> bool {
        matches!(
            self.peek_kind(),
            None | Some(TokenKind::Eof)
                | Some(TokenKind::Newline)
                | Some(TokenKind::Semicolon)
                | Some(TokenKind::Pipe)
                | Some(TokenKind::PipeTolerate)
                | Some(TokenKind::AndAnd)
                | Some(TokenKind::OrOr)
                | Some(TokenKind::Ampersand)
                | Some(TokenKind::RBrace)
                | Some(TokenKind::RParen)
                | Some(TokenKind::HeredocBody(_))
        )
    }

    fn current_span(&self) -> Span {
        self.peek_token().map(|t| t.span).unwrap_or_else(|| self.eof_span())
    }

    fn eof_span(&self) -> Span {
        Span::point(self.source_len)
    }

    // -----------------------------------------------------------------
    // Top-level: parse_script
    // -----------------------------------------------------------------

    fn parse_script(&mut self) -> ParseResult<Script> {
        let mut statements = Vec::new();
        let script_start = 0;
        loop {
            self.skip_separators();
            if self.at_eof() {
                break;
            }
            let stmt = self.parse_statement()?;
            // After a statement, require a terminator (or EOF) so we
            // catch dangling tokens early.
            if !self.at_statement_terminator() {
                return Err(ParseError {
                    kind: ParseErrorKind::UnexpectedToken {
                        expected: "`;`, newline, or end of input",
                    },
                    span: self.current_span(),
                });
            }
            statements.push(stmt);
        }
        Ok(Script {
            statements,
            span: Span::new(script_start, self.source_len),
        })
    }

    // -----------------------------------------------------------------
    // Statement dispatch
    // -----------------------------------------------------------------

    fn parse_statement(&mut self) -> ParseResult<Statement> {
        match self.peek_kind() {
            Some(TokenKind::If) => self.parse_if(),
            Some(TokenKind::For) => self.parse_for(),
            Some(TokenKind::While) => self.parse_while(),
            Some(TokenKind::Fn) => self.parse_fn(),
            Some(TokenKind::Let) => self.parse_let(),
            Some(TokenKind::Return) => self.parse_return(),
            Some(TokenKind::Break) => self.parse_break_or_continue(true),
            Some(TokenKind::Continue) => self.parse_break_or_continue(false),
            // U-5d statement-form constructs.
            Some(TokenKind::Case) => self.parse_case_stmt(),
            Some(TokenKind::Try) => self.parse_try(),
            Some(TokenKind::Trace) => self.parse_trace(),
            Some(TokenKind::On) => self.parse_on_note(),
            Some(TokenKind::Mask) => self.parse_mask_note(),
            Some(TokenKind::Word(w)) if is_valid_ident(w) && self.is_assignment_start() => {
                self.parse_assign()
            }
            // Bare reserved keywords that aren't statement-starts:
            // Else (without preceding If), Catch (without preceding
            // Try), In (only valid inside For).
            Some(TokenKind::Else) | Some(TokenKind::Catch) | Some(TokenKind::In) => {
                Err(ParseError {
                    kind: ParseErrorKind::InvalidStatement,
                    span: self.current_span(),
                })
            }
            _ => self.parse_pipeline_statement(),
        }
    }

    /// Two-token lookahead for assignment: current is a Word with
    /// ident shape; next is Equal.
    fn is_assignment_start(&self) -> bool {
        matches!(self.peek_kind_at(1), Some(TokenKind::Equal))
    }

    // -----------------------------------------------------------------
    // Pipeline statement
    // -----------------------------------------------------------------

    fn parse_pipeline_statement(&mut self) -> ParseResult<Statement> {
        let pipeline = self.parse_pipeline()?;
        let span = pipeline.span;
        Ok(Statement {
            kind: StatementKind::Pipeline(pipeline),
            span,
        })
    }

    fn parse_pipeline(&mut self) -> ParseResult<Pipeline> {
        let start = self.current_span().start;
        let mut elements = Vec::new();
        elements.push(self.parse_pipeline_element()?);
        // Loop: consume `|` or `?|` and parse the next element.
        loop {
            match self.peek_kind() {
                Some(TokenKind::Pipe) => {
                    self.pos += 1;
                    self.skip_newlines_only();
                    let next = self.parse_pipeline_element()?;
                    elements.push(next);
                }
                Some(TokenKind::PipeTolerate) => {
                    // Mark the LEFT element as tolerated, then parse
                    // the next element.
                    if let Some(last) = elements.last_mut() {
                        last.tolerate_failure = true;
                    }
                    self.pos += 1;
                    self.skip_newlines_only();
                    let next = self.parse_pipeline_element()?;
                    elements.push(next);
                }
                _ => break,
            }
        }
        // Optional trailing `&` for background.
        let background = matches!(self.peek_kind(), Some(TokenKind::Ampersand));
        if background {
            self.pos += 1;
        }
        let end = elements
            .last()
            .map(|e| e.span.end)
            .unwrap_or(start);
        Ok(Pipeline {
            elements,
            background,
            span: Span::new(start, end),
        })
    }

    /// After a `|` or `?|`, the user is allowed to put a line
    /// continuation (Newline) before the next command. Bash + rc
    /// both support this. Consume Newlines (but NOT other
    /// separators).
    fn skip_newlines_only(&mut self) {
        while matches!(self.peek_kind(), Some(TokenKind::Newline)) {
            self.pos += 1;
        }
    }

    fn parse_pipeline_element(&mut self) -> ParseResult<PipelineElement> {
        let start = self.current_span().start;
        let command = self.parse_command()?;
        let end = command.span.end;
        Ok(PipelineElement {
            command,
            tolerate_failure: false,
            span: Span::new(start, end),
        })
    }

    // -----------------------------------------------------------------
    // Command dispatch
    // -----------------------------------------------------------------

    fn parse_command(&mut self) -> ParseResult<Command> {
        let start = self.current_span().start;
        let mut command = match self.peek_kind() {
            Some(TokenKind::LBrace) => self.parse_brace_block_command()?,
            Some(TokenKind::LParen) => self.parse_subshell_command()?,
            Some(TokenKind::DoubleLParen) => self.parse_arith_command()?,
            _ => self.parse_simple_command()?,
        };
        // Postfix `?` (fail-propagate) per scripture section 8.2.
        if matches!(self.peek_kind(), Some(TokenKind::Question)) {
            let q_tok = self.advance();
            command.fail_propagate = true;
            command.span = Span::new(start, q_tok.span.end);
            // Strict: requires a statement terminator.
            if !self.at_postfix_question_terminator() {
                return Err(ParseError {
                    kind: ParseErrorKind::UnexpectedTokenAfterFailPropagate,
                    span: self.current_span(),
                });
            }
        }
        Ok(command)
    }

    fn parse_brace_block_command(&mut self) -> ParseResult<Command> {
        let lb = self.advance(); // LBrace
        let body = self.parse_block_statements()?;
        let rb = self.expect_kind(TokenKind::RBrace, "`}`")?;
        Ok(Command {
            kind: CommandKind::BraceBlock(body),
            redirects: Vec::new(),
            fail_propagate: false,
            span: Span::new(lb.span.start, rb.span.end),
        })
    }

    fn parse_subshell_command(&mut self) -> ParseResult<Command> {
        let lp = self.advance(); // LParen
        let body = self.parse_block_statements_until(TokenKind::RParen)?;
        let rp = self.expect_kind(TokenKind::RParen, "`)`")?;
        Ok(Command {
            kind: CommandKind::Subshell(body),
            redirects: Vec::new(),
            fail_propagate: false,
            span: Span::new(lp.span.start, rp.span.end),
        })
    }

    fn parse_arith_command(&mut self) -> ParseResult<Command> {
        let lp = self.advance(); // DoubleLParen
        // Collect tokens until matching DoubleRParen, respecting
        // nested LParen/RParen pairs and DoubleLParen/DoubleRParen.
        let mut body = Vec::new();
        let mut depth: i32 = 2; // DoubleLParen counts as 2 opens
        while !self.at_eof() {
            let tok = self.peek_token().expect("at_eof check above");
            match &tok.kind {
                TokenKind::LParen => {
                    depth += 1;
                    body.push(self.advance());
                }
                TokenKind::DoubleLParen => {
                    depth += 2;
                    body.push(self.advance());
                }
                TokenKind::RParen => {
                    depth -= 1;
                    if depth == 0 {
                        let close = self.advance();
                        let expr = parse_expr_tokens(body, self.source_len, ExprContext::Arith)?;
                        return Ok(Command {
                            kind: CommandKind::Arith(expr),
                            redirects: Vec::new(),
                            fail_propagate: false,
                            span: Span::new(lp.span.start, close.span.end),
                        });
                    }
                    body.push(self.advance());
                }
                TokenKind::DoubleRParen => {
                    depth -= 2;
                    if depth == 0 {
                        let close = self.advance();
                        let expr = parse_expr_tokens(body, self.source_len, ExprContext::Arith)?;
                        return Ok(Command {
                            kind: CommandKind::Arith(expr),
                            redirects: Vec::new(),
                            fail_propagate: false,
                            span: Span::new(lp.span.start, close.span.end),
                        });
                    }
                    if depth < 0 {
                        return Err(ParseError {
                            kind: ParseErrorKind::UnexpectedToken { expected: "`))`" },
                            span: tok.span,
                        });
                    }
                    body.push(self.advance());
                }
                TokenKind::Eof => break,
                _ => body.push(self.advance()),
            }
        }
        Err(ParseError {
            kind: ParseErrorKind::UnexpectedEof { expected: "`))`" },
            span: self.eof_span(),
        })
    }

    // -----------------------------------------------------------------
    // Simple command (the most common kind: argv + redirects)
    // -----------------------------------------------------------------

    fn parse_simple_command(&mut self) -> ParseResult<Command> {
        let start = self.current_span().start;
        let mut words = Vec::new();
        let mut redirects = Vec::new();
        loop {
            match self.peek_kind() {
                Some(k) if is_value_token(k) => {
                    words.push(self.parse_word()?);
                }
                Some(TokenKind::Less)
                | Some(TokenKind::Greater)
                | Some(TokenKind::GreaterGreater)
                | Some(TokenKind::HeredocStart { .. }) => {
                    redirects.push(self.parse_redirect()?);
                }
                Some(TokenKind::Equal) if !words.is_empty() => {
                    return Err(ParseError {
                        kind: ParseErrorKind::UnexpectedEqualInCommand,
                        span: self.current_span(),
                    });
                }
                _ => break,
            }
        }
        if words.is_empty() && redirects.is_empty() {
            return Err(ParseError {
                kind: ParseErrorKind::EmptyPipelineElement,
                span: self.current_span(),
            });
        }
        let end = if let Some(last_redir) = redirects.last() {
            core::cmp::max(
                last_redir.span.end,
                words.last().map(|w| w.span().end).unwrap_or(start),
            )
        } else if let Some(last_word) = words.last() {
            last_word.span().end
        } else {
            start
        };
        let span = Span::new(start, end);
        let cmd = SimpleCommand { words, span };
        Ok(Command {
            kind: CommandKind::Simple(cmd),
            redirects,
            fail_propagate: false,
            span,
        })
    }

    /// Parse one word. A word is a single value-producing token,
    /// OR a sequence of value-producing tokens joined by `^`
    /// (Caret) where each adjacent pair is span-adjacent
    /// (no whitespace between).
    fn parse_word(&mut self) -> ParseResult<Word> {
        let first = self.advance();
        debug_assert!(is_value_token(&first.kind));
        // Assemble a maximal span-adjacent word. Two joining rules, both
        // requiring zero whitespace between the joined tokens:
        //   (a) a `~` fuses span-adjacent value tokens into ONE word, so
        //       `~/foo` is a single argv element and `a~b` stays one
        //       literal word. Position decides meaning at eval time: only
        //       a *leading* `~` (the word's first token) expands to $home;
        //       a `~` anywhere else is a literal tilde (eval_word).
        //   (b) the explicit `^` operator (scripture 6.7): span-adjacent
        //       Caret + span-adjacent value, with the Caret dropped from
        //       the AST.
        // `~` and `^` are the ONLY word-gluers; plain span-adjacent values
        // (`$a$b`) stay separate words, unchanged from before.
        let mut parts: Vec<Token> = alloc::vec![first];
        let mut has_tilde = matches!(parts[0].kind, TokenKind::Tilde);
        loop {
            let last_end = parts.last().expect("parts has >= 1").span.end;
            // Copy out the scalars the branches need so no borrow of the
            // peeked token survives across the `self` mutation below.
            let (adjacent, is_value, is_tilde, is_caret, next_end) = match self.peek_token() {
                Some(t) => (
                    t.span.start == last_end,
                    is_value_token(&t.kind),
                    matches!(t.kind, TokenKind::Tilde),
                    matches!(t.kind, TokenKind::Caret),
                    t.span.end,
                ),
                None => break,
            };
            // (a) tilde gluing: once a `~` is anywhere in the word, absorb
            // span-adjacent value tokens, so the whole tilde-prefix-and-path
            // is one argv element (`~/foo`, `~/$dir`, and the literal `a~b`).
            // The leading-vs-literal decision is made later at eval by token
            // position; here we only assemble the word.
            if adjacent && is_value && (has_tilde || is_tilde) {
                let value = self.advance();
                has_tilde |= matches!(value.kind, TokenKind::Tilde);
                parts.push(value);
                continue;
            }
            // (b) `^` concatenation: span-adjacent Caret + span-adjacent
            // value. Consume both (skip the Caret, store the value).
            if is_caret && adjacent {
                let maybe_value = match self.tokens.get(self.pos + 1) {
                    Some(t) => t,
                    None => break,
                };
                if !is_value_token(&maybe_value.kind) || maybe_value.span.start != next_end {
                    break;
                }
                self.pos += 1; // past the Caret
                let value = self.advance();
                has_tilde |= matches!(value.kind, TokenKind::Tilde);
                parts.push(value);
                continue;
            }
            break;
        }
        if parts.len() == 1 {
            Ok(Word::Single(parts.into_iter().next().expect("len 1")))
        } else {
            Ok(Word::Concat(parts))
        }
    }

    fn parse_redirect(&mut self) -> ParseResult<Redirect> {
        let start = self.current_span().start;
        match self.peek_kind() {
            Some(TokenKind::Less) => {
                self.pos += 1;
                let target = self.parse_redirect_target("`<` target")?;
                let end = target.span().end;
                Ok(Redirect {
                    kind: RedirectKind::Stdin,
                    target: Some(target),
                    span: Span::new(start, end),
                })
            }
            Some(TokenKind::Greater) => {
                self.pos += 1;
                let target = self.parse_redirect_target("`>` target")?;
                let end = target.span().end;
                Ok(Redirect {
                    kind: RedirectKind::Stdout,
                    target: Some(target),
                    span: Span::new(start, end),
                })
            }
            Some(TokenKind::GreaterGreater) => {
                self.pos += 1;
                let target = self.parse_redirect_target("`>>` target")?;
                let end = target.span().end;
                Ok(Redirect {
                    kind: RedirectKind::Append,
                    target: Some(target),
                    span: Span::new(start, end),
                })
            }
            Some(TokenKind::HeredocStart { .. }) => {
                let start_tok = self.advance();
                let (interp, strip_tabs) = match &start_tok.kind {
                    TokenKind::HeredocStart {
                        interp,
                        strip_tabs,
                        ..
                    } => (*interp, *strip_tabs),
                    _ => unreachable!(),
                };
                let body = self.heredoc_bodies.pop_front().unwrap_or_default();
                Ok(Redirect {
                    kind: RedirectKind::Heredoc {
                        interp,
                        strip_tabs,
                        body,
                    },
                    target: None,
                    span: start_tok.span,
                })
            }
            _ => Err(ParseError {
                kind: ParseErrorKind::UnexpectedToken {
                    expected: "redirect operator",
                },
                span: self.current_span(),
            }),
        }
    }

    fn parse_redirect_target(&mut self, label: &'static str) -> ParseResult<Word> {
        match self.peek_kind() {
            Some(k) if is_value_token(k) => self.parse_word(),
            _ => Err(ParseError {
                kind: ParseErrorKind::UnexpectedToken { expected: label },
                span: self.current_span(),
            }),
        }
    }

    // -----------------------------------------------------------------
    // Block bodies
    // -----------------------------------------------------------------

    /// Parse statements inside a brace block (`{ ... }`). The caller
    /// has consumed the opening `{`; this stops at the closing `}`
    /// (without consuming it).
    fn parse_block_statements(&mut self) -> ParseResult<Vec<Statement>> {
        let mut stmts = Vec::new();
        loop {
            self.skip_separators();
            match self.peek_kind() {
                Some(TokenKind::RBrace) | Some(TokenKind::Eof) | None => break,
                _ => {}
            }
            let stmt = self.parse_statement()?;
            if !self.at_statement_terminator() {
                return Err(ParseError {
                    kind: ParseErrorKind::UnexpectedToken {
                        expected: "`;`, newline, or `}`",
                    },
                    span: self.current_span(),
                });
            }
            stmts.push(stmt);
        }
        Ok(stmts)
    }

    /// Parse statements until a specific terminator token is
    /// encountered (without consuming it). Used by subshell.
    fn parse_block_statements_until(
        &mut self,
        end_token: TokenKind,
    ) -> ParseResult<Vec<Statement>> {
        let mut stmts = Vec::new();
        loop {
            self.skip_separators();
            match self.peek_kind() {
                Some(k) if core::mem::discriminant(k) == core::mem::discriminant(&end_token) => {
                    break
                }
                Some(TokenKind::Eof) | None => break,
                _ => {}
            }
            let stmt = self.parse_statement()?;
            if !self.at_statement_terminator() {
                return Err(ParseError {
                    kind: ParseErrorKind::UnexpectedToken {
                        expected: "`;`, newline, or `)`",
                    },
                    span: self.current_span(),
                });
            }
            stmts.push(stmt);
        }
        Ok(stmts)
    }

    // -----------------------------------------------------------------
    // Paren-delimited token collection (for if/while/for cond
    // expressions). Respects nested LParen/RParen and
    // DoubleLParen/DoubleRParen.
    // -----------------------------------------------------------------

    fn collect_paren_tokens(&mut self) -> ParseResult<Vec<Token>> {
        self.expect_kind(TokenKind::LParen, "`(`")?;
        let mut depth: i32 = 1;
        let mut collected = Vec::new();
        while !self.at_eof() {
            let tok = self.peek_token().expect("at_eof check above");
            match &tok.kind {
                TokenKind::LParen => {
                    depth += 1;
                    collected.push(self.advance());
                }
                TokenKind::DoubleLParen => {
                    depth += 2;
                    collected.push(self.advance());
                }
                TokenKind::RParen => {
                    depth -= 1;
                    if depth == 0 {
                        self.pos += 1; // consume the closing RParen
                        return Ok(collected);
                    }
                    collected.push(self.advance());
                }
                TokenKind::DoubleRParen => {
                    depth -= 2;
                    if depth == 0 {
                        self.pos += 1;
                        return Ok(collected);
                    }
                    if depth < 0 {
                        return Err(ParseError {
                            kind: ParseErrorKind::UnexpectedToken { expected: "`)`" },
                            span: tok.span,
                        });
                    }
                    collected.push(self.advance());
                }
                TokenKind::Eof => break,
                _ => collected.push(self.advance()),
            }
        }
        Err(ParseError {
            kind: ParseErrorKind::UnexpectedEof { expected: "`)`" },
            span: self.eof_span(),
        })
    }

    /// Collect "value tokens" -- everything from current position
    /// up to a top-level statement terminator. Tracks paren and
    /// brace depth so that `let files = (a b c)` collects the
    /// entire list. Does NOT consume the terminator.
    fn collect_value_tokens(&mut self) -> Vec<Token> {
        let mut collected = Vec::new();
        let mut paren_depth: i32 = 0;
        let mut brace_depth: i32 = 0;
        while !self.at_eof() {
            let tok = self.peek_token().expect("at_eof check above");
            if paren_depth == 0 && brace_depth == 0 {
                match &tok.kind {
                    TokenKind::Newline
                    | TokenKind::Semicolon
                    | TokenKind::Eof
                    | TokenKind::HeredocBody(_) => break,
                    TokenKind::Pipe
                    | TokenKind::PipeTolerate
                    | TokenKind::AndAnd
                    | TokenKind::OrOr
                    | TokenKind::Ampersand => break,
                    _ => {}
                }
            }
            match &tok.kind {
                TokenKind::LParen => paren_depth += 1,
                TokenKind::DoubleLParen => paren_depth += 2,
                TokenKind::RParen => paren_depth -= 1,
                TokenKind::DoubleRParen => paren_depth -= 2,
                TokenKind::LBrace => brace_depth += 1,
                TokenKind::RBrace => {
                    if brace_depth == 0 {
                        // We're inside an enclosing block; stop.
                        break;
                    }
                    brace_depth -= 1;
                }
                _ => {}
            }
            collected.push(self.advance());
        }
        collected
    }

    // -----------------------------------------------------------------
    // Control-flow: if / for / while / fn
    // -----------------------------------------------------------------

    fn parse_if(&mut self) -> ParseResult<Statement> {
        let if_tok = self.advance(); // If
        let cond_tokens = self.collect_paren_tokens()?;
        let cond = parse_expr_tokens(cond_tokens, self.source_len, ExprContext::Cond)?;
        self.skip_newlines_only();
        self.expect_kind(TokenKind::LBrace, "`{`")?;
        let then_branch = self.parse_block_statements()?;
        let mut last_end = self.expect_kind(TokenKind::RBrace, "`}`")?.span.end;
        let mut elif_branches: Vec<(Expr, Vec<Statement>)> = Vec::new();
        let mut else_branch = None;
        // Handle `else if ...` / `else { ... }` chain.
        loop {
            // The `else` might be on the same line as the `}` or on
            // the next line. Per rc convention (and POSIX), `} else {`
            // is required to be on the same logical line OR
            // separated by a Newline. Be permissive: allow either.
            // Look past optional Newline for `else`.
            let save_pos = self.pos;
            while matches!(self.peek_kind(), Some(TokenKind::Newline)) {
                self.pos += 1;
            }
            if !matches!(self.peek_kind(), Some(TokenKind::Else)) {
                // No else; restore position.
                self.pos = save_pos;
                break;
            }
            self.pos += 1; // consume Else
            self.skip_newlines_only();
            if matches!(self.peek_kind(), Some(TokenKind::If)) {
                self.pos += 1; // consume If
                let elif_tokens = self.collect_paren_tokens()?;
                let elif_cond =
                    parse_expr_tokens(elif_tokens, self.source_len, ExprContext::Cond)?;
                self.skip_newlines_only();
                self.expect_kind(TokenKind::LBrace, "`{`")?;
                let elif_body = self.parse_block_statements()?;
                last_end = self.expect_kind(TokenKind::RBrace, "`}`")?.span.end;
                elif_branches.push((elif_cond, elif_body));
                continue;
            }
            // Plain `else { ... }` -- final.
            self.expect_kind(TokenKind::LBrace, "`{`")?;
            let body = self.parse_block_statements()?;
            last_end = self.expect_kind(TokenKind::RBrace, "`}`")?.span.end;
            else_branch = Some(body);
            break;
        }
        let span = Span::new(if_tok.span.start, last_end);
        Ok(Statement {
            kind: StatementKind::If(Box::new(IfStmt {
                cond,
                then_branch,
                elif_branches,
                else_branch,
                span,
            })),
            span,
        })
    }

    fn parse_for(&mut self) -> ParseResult<Statement> {
        let for_tok = self.advance(); // For
        self.expect_kind(TokenKind::LParen, "`(`")?;
        let var_name = self.expect_ident()?;
        self.expect_kind(TokenKind::In, "`in`")?;
        // Collect the list expression until the matching outer RParen.
        let mut depth: i32 = 1;
        let mut list_tokens: Vec<Token> = Vec::new();
        while !self.at_eof() {
            let tok = self.peek_token().expect("at_eof above");
            match &tok.kind {
                TokenKind::LParen => {
                    depth += 1;
                    list_tokens.push(self.advance());
                }
                TokenKind::DoubleLParen => {
                    depth += 2;
                    list_tokens.push(self.advance());
                }
                TokenKind::RParen => {
                    depth -= 1;
                    if depth == 0 {
                        self.pos += 1; // consume outer RParen
                        break;
                    }
                    list_tokens.push(self.advance());
                }
                TokenKind::DoubleRParen => {
                    depth -= 2;
                    if depth == 0 {
                        self.pos += 1;
                        break;
                    }
                    if depth < 0 {
                        return Err(ParseError {
                            kind: ParseErrorKind::UnexpectedToken { expected: "`)`" },
                            span: tok.span,
                        });
                    }
                    list_tokens.push(self.advance());
                }
                _ => list_tokens.push(self.advance()),
            }
        }
        let list_expr = parse_expr_tokens(list_tokens, self.source_len, ExprContext::List)?;
        self.skip_newlines_only();
        self.expect_kind(TokenKind::LBrace, "`{`")?;
        let body = self.parse_block_statements()?;
        let close = self.expect_kind(TokenKind::RBrace, "`}`")?;
        let span = Span::new(for_tok.span.start, close.span.end);
        Ok(Statement {
            kind: StatementKind::For(Box::new(ForStmt {
                var_name,
                list_expr,
                body,
                span,
            })),
            span,
        })
    }

    fn parse_while(&mut self) -> ParseResult<Statement> {
        let w_tok = self.advance(); // While
        let cond_tokens = self.collect_paren_tokens()?;
        let cond = parse_expr_tokens(cond_tokens, self.source_len, ExprContext::Cond)?;
        self.skip_newlines_only();
        self.expect_kind(TokenKind::LBrace, "`{`")?;
        let body = self.parse_block_statements()?;
        let close = self.expect_kind(TokenKind::RBrace, "`}`")?;
        let span = Span::new(w_tok.span.start, close.span.end);
        Ok(Statement {
            kind: StatementKind::While(Box::new(WhileStmt { cond, body, span })),
            span,
        })
    }

    fn parse_fn(&mut self) -> ParseResult<Statement> {
        let fn_tok = self.advance(); // Fn
        let name = self.expect_ident()?;
        let mut args = Vec::new();
        // Collect optional positional-arg names (bare-word idents)
        // until we see LBrace.
        loop {
            match self.peek_kind() {
                Some(TokenKind::Word(w)) if is_valid_ident(w) => {
                    args.push(w.clone());
                    self.pos += 1;
                }
                _ => break,
            }
        }
        self.skip_newlines_only();
        self.expect_kind(TokenKind::LBrace, "`{`")?;
        let body = self.parse_block_statements()?;
        let close = self.expect_kind(TokenKind::RBrace, "`}`")?;
        let span = Span::new(fn_tok.span.start, close.span.end);
        Ok(Statement {
            kind: StatementKind::FnDecl(Box::new(FnDecl {
                name,
                args,
                body,
                span,
            })),
            span,
        })
    }

    // -----------------------------------------------------------------
    // Assignment: let / bare
    // -----------------------------------------------------------------

    fn parse_let(&mut self) -> ParseResult<Statement> {
        let let_tok = self.advance(); // Let
        let name = self.expect_ident()?;
        self.expect_kind(TokenKind::Equal, "`=`")?;
        let value_tokens = self.collect_value_tokens();
        let end = value_tokens
            .last()
            .map(|t| t.span.end)
            .unwrap_or(let_tok.span.end);
        let value = parse_expr_tokens(value_tokens, self.source_len, ExprContext::Value)?;
        let span = Span::new(let_tok.span.start, end);
        Ok(Statement {
            kind: StatementKind::Let(Box::new(LetStmt { name, value, span })),
            span,
        })
    }

    fn parse_assign(&mut self) -> ParseResult<Statement> {
        let name_tok = self.advance();
        let name = match name_tok.kind {
            TokenKind::Word(ref w) if is_valid_ident(w) => w.clone(),
            _ => {
                return Err(ParseError {
                    kind: ParseErrorKind::InvalidAssignmentName,
                    span: name_tok.span,
                });
            }
        };
        self.expect_kind(TokenKind::Equal, "`=`")?;
        let value_tokens = self.collect_value_tokens();
        let end = value_tokens
            .last()
            .map(|t| t.span.end)
            .unwrap_or(name_tok.span.end);
        let value = parse_expr_tokens(value_tokens, self.source_len, ExprContext::Value)?;
        let span = Span::new(name_tok.span.start, end);
        Ok(Statement {
            kind: StatementKind::Assign(Box::new(AssignStmt { name, value, span })),
            span,
        })
    }

    // -----------------------------------------------------------------
    // return / break / continue
    // -----------------------------------------------------------------

    fn parse_return(&mut self) -> ParseResult<Statement> {
        let ret_tok = self.advance(); // Return
        if self.at_statement_terminator() {
            return Ok(Statement {
                kind: StatementKind::Return(None),
                span: ret_tok.span,
            });
        }
        let value_tokens = self.collect_value_tokens();
        let end = value_tokens
            .last()
            .map(|t| t.span.end)
            .unwrap_or(ret_tok.span.end);
        let value = parse_expr_tokens(value_tokens, self.source_len, ExprContext::Return)?;
        let span = Span::new(ret_tok.span.start, end);
        Ok(Statement {
            kind: StatementKind::Return(Some(value)),
            span,
        })
    }

    fn parse_break_or_continue(&mut self, is_break: bool) -> ParseResult<Statement> {
        let tok = self.advance();
        let kind = if is_break {
            StatementKind::Break
        } else {
            StatementKind::Continue
        };
        Ok(Statement {
            kind,
            span: tok.span,
        })
    }

    // -----------------------------------------------------------------
    // Pattern matching (U-5d): `case $x { pat ... => body ; ... }`
    // -----------------------------------------------------------------

    fn parse_case_stmt(&mut self) -> ParseResult<Statement> {
        let case_tok = self.advance(); // Case
        // Scrutinee: a value-position expression, terminated by `{`.
        let scrutinee_tokens = self.collect_tokens_until_lbrace()?;
        if scrutinee_tokens.is_empty() {
            return Err(ParseError {
                kind: ParseErrorKind::EmptyExpression,
                span: self.current_span(),
            });
        }
        let scrutinee = parse_expr_tokens(scrutinee_tokens, self.source_len, ExprContext::Value)?;
        self.expect_kind(TokenKind::LBrace, "`{`")?;
        let mut arms = Vec::new();
        loop {
            self.skip_separators();
            if matches!(self.peek_kind(), Some(TokenKind::RBrace) | Some(TokenKind::Eof) | None) {
                break;
            }
            arms.push(self.parse_case_arm_stmt()?);
        }
        let close = self.expect_kind(TokenKind::RBrace, "`}`")?;
        let span = Span::new(case_tok.span.start, close.span.end);
        Ok(Statement {
            kind: StatementKind::Case(Box::new(CaseStmt {
                scrutinee,
                arms,
                span,
            })),
            span,
        })
    }

    fn parse_case_arm_stmt(&mut self) -> ParseResult<CaseArm> {
        let start = self.current_span().start;
        let pat_tokens = self.collect_case_arm_patterns()?;
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
        // Allow optional newlines between `=>` and the body so users
        // can write `... =>\n    body`.
        self.skip_newlines_only();
        let body = self.parse_statement()?;
        let end = body.span.end;
        Ok(CaseArm {
            patterns,
            body,
            span: Span::new(start, end),
        })
    }

    /// Walk tokens up to (and consuming) the `=>` at the SAME paren
    /// depth as the caller. The patterns may contain `(...)` groups;
    /// `FatArrow` inside parens is preserved as a literal token (it
    /// would never round-trip to a meaningful expression, but the
    /// nesting discipline keeps the algorithm simple).
    fn collect_case_arm_patterns(&mut self) -> ParseResult<Vec<Token>> {
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
                    TokenKind::Newline
                    | TokenKind::Semicolon
                    | TokenKind::RBrace
                    | TokenKind::Eof => {
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

    /// Collect tokens up to (but NOT consuming) the next `{` at depth
    /// 0. Used by `case $scrutinee { ... }` and by `on note 'name' {
    /// ... }` / `mask note ... { ... }` to gather the leading
    /// value-position tokens.
    fn collect_tokens_until_lbrace(&mut self) -> ParseResult<Vec<Token>> {
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
                    TokenKind::Newline
                    | TokenKind::Semicolon
                    | TokenKind::Eof
                    | TokenKind::RBrace => {
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

    // -----------------------------------------------------------------
    // try / catch (U-5d)
    // -----------------------------------------------------------------

    fn parse_try(&mut self) -> ParseResult<Statement> {
        let try_tok = self.advance(); // Try
        self.skip_newlines_only();
        self.expect_kind(TokenKind::LBrace, "`{`")?;
        let body = self.parse_block_statements()?;
        self.expect_kind(TokenKind::RBrace, "`}`")?;
        // Allow `} \n catch` across a newline; catch is REQUIRED per
        // the U-5d locked decision (scripture 7.7 always pairs them).
        self.skip_newlines_only();
        if !matches!(self.peek_kind(), Some(TokenKind::Catch)) {
            return Err(ParseError {
                kind: ParseErrorKind::UnexpectedToken {
                    expected: "`catch`",
                },
                span: self.current_span(),
            });
        }
        self.pos += 1; // consume Catch
        self.skip_newlines_only();
        self.expect_kind(TokenKind::LBrace, "`{`")?;
        let catch = self.parse_block_statements()?;
        let close = self.expect_kind(TokenKind::RBrace, "`}`")?;
        let span = Span::new(try_tok.span.start, close.span.end);
        Ok(Statement {
            kind: StatementKind::Try(Box::new(TryStmt { body, catch, span })),
            span,
        })
    }

    // -----------------------------------------------------------------
    // trace (U-5d)
    // -----------------------------------------------------------------

    fn parse_trace(&mut self) -> ParseResult<Statement> {
        let trace_tok = self.advance(); // Trace
        self.skip_newlines_only();
        self.expect_kind(TokenKind::LBrace, "`{`")?;
        let body = self.parse_block_statements()?;
        let close = self.expect_kind(TokenKind::RBrace, "`}`")?;
        let span = Span::new(trace_tok.span.start, close.span.end);
        Ok(Statement {
            kind: StatementKind::Trace(Box::new(TraceStmt { body, span })),
            span,
        })
    }

    // -----------------------------------------------------------------
    // on note / mask note (U-5d)
    // -----------------------------------------------------------------

    fn parse_on_note(&mut self) -> ParseResult<Statement> {
        let on_tok = self.advance(); // On
        self.expect_note_keyword()?;
        let (note_name, body, close_end) = self.parse_note_name_and_block()?;
        let span = Span::new(on_tok.span.start, close_end);
        Ok(Statement {
            kind: StatementKind::OnNote(Box::new(OnNoteStmt {
                note_name,
                body,
                span,
            })),
            span,
        })
    }

    fn parse_mask_note(&mut self) -> ParseResult<Statement> {
        let mask_tok = self.advance(); // Mask
        self.expect_note_keyword()?;
        let (note_name, body, close_end) = self.parse_note_name_and_block()?;
        let span = Span::new(mask_tok.span.start, close_end);
        Ok(Statement {
            kind: StatementKind::MaskNote(Box::new(MaskNoteStmt {
                note_name,
                body,
                span,
            })),
            span,
        })
    }

    /// Expect the bare `Word("note")` token (scripture spells the
    /// construct `on note 'name' { ... }`; `note` is NOT a reserved
    /// keyword, so we match it by text here).
    fn expect_note_keyword(&mut self) -> ParseResult<()> {
        let span = self.current_span();
        match self.peek_token() {
            Some(t) => match &t.kind {
                TokenKind::Word(w) if w == "note" => {
                    self.pos += 1;
                    Ok(())
                }
                _ => Err(ParseError {
                    kind: ParseErrorKind::UnexpectedToken { expected: "`note`" },
                    span,
                }),
            },
            None => Err(ParseError {
                kind: ParseErrorKind::UnexpectedEof { expected: "`note`" },
                span: self.eof_span(),
            }),
        }
    }

    /// Shared between `on note` and `mask note`: parse a name
    /// expression then a brace-delimited body. Returns (name, body,
    /// closing-brace.end).
    fn parse_note_name_and_block(&mut self) -> ParseResult<(Expr, Vec<Statement>, usize)> {
        let name_tokens = self.collect_tokens_until_lbrace()?;
        if name_tokens.is_empty() {
            return Err(ParseError {
                kind: ParseErrorKind::EmptyExpression,
                span: self.current_span(),
            });
        }
        let note_name = parse_expr_tokens(name_tokens, self.source_len, ExprContext::Value)?;
        self.expect_kind(TokenKind::LBrace, "`{`")?;
        let body = self.parse_block_statements()?;
        let close = self.expect_kind(TokenKind::RBrace, "`}`")?;
        Ok((note_name, body, close.span.end))
    }
}

// ---------------------------------------------------------------------
// Free helpers
// ---------------------------------------------------------------------

fn is_value_token(k: &TokenKind) -> bool {
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
            // A word-position `~` is a value: it expands to $home when it
            // leads a word (`~`, `~/foo`), and is a literal tilde mid-word
            // (`a~b`). Arithmetic context uses a SEPARATE predicate
            // (expr.rs::is_value_token_kind), so arith's bitwise-not `~`
            // is unaffected.
            | TokenKind::Tilde
    )
}

fn is_valid_ident(s: &str) -> bool {
    let mut chars = s.chars();
    match chars.next() {
        None => return false,
        Some(c) if c.is_ascii_alphabetic() || c == '_' => {}
        _ => return false,
    }
    chars.all(|c| c.is_ascii_alphanumeric() || c == '_')
}

// ---------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------

#[cfg(test)]
mod tests {
    use super::*;
    use alloc::string::ToString;
    use alloc::vec;

    fn parse_ok(src: &str) -> Script {
        parse(src).unwrap_or_else(|e| panic!("parse failed: {:?}", e))
    }

    fn parse_err(src: &str) -> ParseErrorKind {
        parse(src).unwrap_err().kind
    }

    #[test]
    fn empty_script_zero_statements() {
        let s = parse_ok("");
        assert_eq!(s.statements.len(), 0);
    }

    #[test]
    fn whitespace_only_zero_statements() {
        let s = parse_ok("  \n  \n\n");
        assert_eq!(s.statements.len(), 0);
    }

    #[test]
    fn single_word_command() {
        let s = parse_ok("ls");
        assert_eq!(s.statements.len(), 1);
        match &s.statements[0].kind {
            StatementKind::Pipeline(p) => {
                assert_eq!(p.elements.len(), 1);
                match &p.elements[0].command.kind {
                    CommandKind::Simple(sc) => assert_eq!(sc.words.len(), 1),
                    _ => panic!(),
                }
            }
            _ => panic!(),
        }
    }

    #[test]
    fn multi_word_command() {
        let s = parse_ok("cat /etc/hosts");
        match &s.statements[0].kind {
            StatementKind::Pipeline(p) => match &p.elements[0].command.kind {
                CommandKind::Simple(sc) => assert_eq!(sc.words.len(), 2),
                _ => panic!(),
            },
            _ => panic!(),
        }
    }

    /// The argv Words of the first simple command in `src`.
    fn first_words(src: &str) -> Vec<Word> {
        let s = parse_ok(src);
        match &s.statements[0].kind {
            StatementKind::Pipeline(p) => match &p.elements[0].command.kind {
                CommandKind::Simple(sc) => sc.words.clone(),
                _ => panic!("not a simple command"),
            },
            _ => panic!("not a pipeline"),
        }
    }

    #[test]
    fn tilde_bare_is_single_tilde() {
        // `cd ~` -> two words; the second is a lone `~` (Single Tilde),
        // which eval expands to $home.
        let w = first_words("cd ~");
        assert_eq!(w.len(), 2);
        assert!(matches!(&w[1], Word::Single(t) if t.kind == TokenKind::Tilde));
    }

    #[test]
    fn tilde_slash_path_glues_into_concat() {
        // `~/foo` is ONE argv element: Concat([Tilde, Word("/foo")]).
        let w = first_words("cat ~/foo");
        assert_eq!(w.len(), 2);
        match &w[1] {
            Word::Concat(parts) => {
                assert_eq!(parts.len(), 2);
                assert_eq!(parts[0].kind, TokenKind::Tilde);
                assert_eq!(parts[1].kind, TokenKind::Word("/foo".to_string()));
            }
            other => panic!("expected Concat, got {:?}", other),
        }
    }

    #[test]
    fn tilde_username_form_glues_one_word() {
        // `~alice` (no slash) is the username form: it still glues into ONE
        // word (Concat), but eval keeps it literal (no name service at v1.0).
        let w = first_words("ls ~alice");
        assert_eq!(w.len(), 2);
        match &w[1] {
            Word::Concat(parts) => {
                assert_eq!(parts.len(), 2);
                assert_eq!(parts[0].kind, TokenKind::Tilde);
                assert_eq!(parts[1].kind, TokenKind::Word("alice".to_string()));
            }
            other => panic!("expected Concat, got {:?}", other),
        }
    }

    #[test]
    fn tilde_slash_var_glues_whole_word() {
        // `~/$dir` is ONE word (maximal tilde absorb): Concat([Tilde, "/",
        // $dir]) -- the `~` keeps absorbing span-adjacent values past the
        // first, so a var suffix is not split off into its own word.
        let w = first_words("cd ~/$dir");
        assert_eq!(w.len(), 2);
        match &w[1] {
            Word::Concat(parts) => {
                assert_eq!(parts.len(), 3);
                assert_eq!(parts[0].kind, TokenKind::Tilde);
                assert_eq!(parts[1].kind, TokenKind::Word("/".to_string()));
                assert!(matches!(parts[2].kind, TokenKind::Var(_)));
            }
            other => panic!("expected 3-part Concat, got {:?}", other),
        }
    }

    #[test]
    fn tilde_midword_glues_literal() {
        // `a~b` stays ONE word: Concat([Word("a"), Tilde, Word("b")]).
        // The non-leading Tilde is literal at eval -> `a~b`.
        let w = first_words("echo a~b");
        assert_eq!(w.len(), 2);
        match &w[1] {
            Word::Concat(parts) => {
                assert_eq!(parts.len(), 3);
                assert_eq!(parts[0].kind, TokenKind::Word("a".to_string()));
                assert_eq!(parts[1].kind, TokenKind::Tilde);
                assert_eq!(parts[2].kind, TokenKind::Word("b".to_string()));
            }
            other => panic!("expected Concat, got {:?}", other),
        }
    }

    #[test]
    fn tilde_trailing_glues_literal() {
        // `echo a~` -> `a~` (Concat; the Tilde is not leading -> literal).
        let w = first_words("echo a~");
        assert_eq!(w.len(), 2);
        assert!(matches!(&w[1], Word::Concat(p) if p.len() == 2
            && p[0].kind == TokenKind::Word("a".to_string())
            && p[1].kind == TokenKind::Tilde));
    }

    #[test]
    fn tilde_as_command_name_parses() {
        // A leading `~` in command-name position parses (the home-relative
        // exec path): `~/bin/x arg` -> argv[0] = Concat([Tilde, ...]).
        let w = first_words("~/bin/x arg");
        assert_eq!(w.len(), 2);
        assert!(matches!(&w[0], Word::Concat(_)));
    }

    #[test]
    fn caret_concat_unaffected_by_tilde_gluing() {
        // The `^` operator still concats: `echo a^b` -> Concat of 2 parts.
        let w = first_words("echo a^b");
        assert_eq!(w.len(), 2);
        assert!(matches!(&w[1], Word::Concat(p) if p.len() == 2));
    }

    #[test]
    fn adjacent_values_without_glue_stay_separate() {
        // No `~`/`^` -> no implicit join: `$a$b` stays two words. Tilde
        // gluing must not disturb this pre-existing behavior.
        let w = first_words("echo $a$b");
        assert_eq!(w.len(), 3);
    }

    #[test]
    fn pipeline_with_pipe() {
        let s = parse_ok("ls | grep foo | wc -l");
        match &s.statements[0].kind {
            StatementKind::Pipeline(p) => {
                assert_eq!(p.elements.len(), 3);
                assert!(!p.elements[0].tolerate_failure);
                assert!(!p.elements[1].tolerate_failure);
                assert!(!p.background);
            }
            _ => panic!(),
        }
    }

    #[test]
    fn pipeline_tolerate_marks_left_element() {
        let s = parse_ok("cmd1 ?| cmd2 | cmd3");
        match &s.statements[0].kind {
            StatementKind::Pipeline(p) => {
                assert_eq!(p.elements.len(), 3);
                assert!(p.elements[0].tolerate_failure); // cmd1 tolerated
                assert!(!p.elements[1].tolerate_failure);
                assert!(!p.elements[2].tolerate_failure);
            }
            _ => panic!(),
        }
    }

    #[test]
    fn pipeline_background() {
        let s = parse_ok("sleep 100 &");
        match &s.statements[0].kind {
            StatementKind::Pipeline(p) => {
                assert!(p.background);
                assert_eq!(p.elements.len(), 1);
            }
            _ => panic!(),
        }
    }

    #[test]
    fn pipeline_continuation_across_newline() {
        let s = parse_ok("cmd1 |\ncmd2");
        match &s.statements[0].kind {
            StatementKind::Pipeline(p) => assert_eq!(p.elements.len(), 2),
            _ => panic!(),
        }
    }

    #[test]
    fn empty_pipe_element_errors() {
        match parse_err("cmd1 | | cmd2") {
            ParseErrorKind::EmptyPipelineElement => {}
            other => panic!("expected EmptyPipelineElement, got {:?}", other),
        }
    }

    #[test]
    fn semicolon_separates_statements() {
        let s = parse_ok("a; b; c");
        assert_eq!(s.statements.len(), 3);
    }

    #[test]
    fn newline_separates_statements() {
        let s = parse_ok("a\nb\nc");
        assert_eq!(s.statements.len(), 3);
    }

    #[test]
    fn trailing_semicolon_allowed() {
        let s = parse_ok("a; b;");
        assert_eq!(s.statements.len(), 2);
    }

    #[test]
    fn fail_propagate_postfix() {
        let s = parse_ok("build?");
        match &s.statements[0].kind {
            StatementKind::Pipeline(p) => {
                assert!(p.elements[0].command.fail_propagate);
            }
            _ => panic!(),
        }
    }

    #[test]
    fn fail_propagate_requires_terminator() {
        match parse_err("cmd ? arg") {
            ParseErrorKind::UnexpectedTokenAfterFailPropagate => {}
            other => panic!("expected UnexpectedTokenAfterFailPropagate, got {:?}", other),
        }
    }

    #[test]
    fn brace_block_command() {
        let s = parse_ok("{ a; b }");
        match &s.statements[0].kind {
            StatementKind::Pipeline(p) => match &p.elements[0].command.kind {
                CommandKind::BraceBlock(stmts) => assert_eq!(stmts.len(), 2),
                _ => panic!(),
            },
            _ => panic!(),
        }
    }

    #[test]
    fn subshell_command() {
        let s = parse_ok("(a; b)");
        match &s.statements[0].kind {
            StatementKind::Pipeline(p) => match &p.elements[0].command.kind {
                CommandKind::Subshell(stmts) => assert_eq!(stmts.len(), 2),
                _ => panic!(),
            },
            _ => panic!(),
        }
    }

    #[test]
    fn arith_command_lifts_body_to_expr() {
        let s = parse_ok("(( 1 + 2 ))");
        match &s.statements[0].kind {
            StatementKind::Pipeline(p) => match &p.elements[0].command.kind {
                // U-5c: body is now an Expr; `1 + 2` lifts to
                // BinOp(Add, Integer(1), Integer(2)).
                CommandKind::Arith(expr) => match &expr.kind {
                    ExprKind::BinOp(BinOp::Add, l, r) => match (&l.kind, &r.kind) {
                        (ExprKind::Integer(1), ExprKind::Integer(2)) => {}
                        other => panic!("expected Integer 1 + Integer 2, got {:?}", other),
                    },
                    other => panic!("expected BinOp(Add), got {:?}", other),
                },
                _ => panic!(),
            },
            _ => panic!(),
        }
    }

    #[test]
    fn redirect_stdout() {
        let s = parse_ok("cmd > out");
        match &s.statements[0].kind {
            StatementKind::Pipeline(p) => {
                let cmd = &p.elements[0].command;
                assert_eq!(cmd.redirects.len(), 1);
                assert!(matches!(cmd.redirects[0].kind, RedirectKind::Stdout));
            }
            _ => panic!(),
        }
    }

    #[test]
    fn redirect_stdin() {
        let s = parse_ok("cmd < in");
        match &s.statements[0].kind {
            StatementKind::Pipeline(p) => {
                let cmd = &p.elements[0].command;
                assert!(matches!(cmd.redirects[0].kind, RedirectKind::Stdin));
            }
            _ => panic!(),
        }
    }

    #[test]
    fn redirect_append() {
        let s = parse_ok("cmd >> out");
        match &s.statements[0].kind {
            StatementKind::Pipeline(p) => {
                let cmd = &p.elements[0].command;
                assert!(matches!(cmd.redirects[0].kind, RedirectKind::Append));
            }
            _ => panic!(),
        }
    }

    #[test]
    fn redirect_heredoc_inlines_body() {
        let s = parse_ok("cat <<EOF\nhello\nEOF\n");
        match &s.statements[0].kind {
            StatementKind::Pipeline(p) => {
                let cmd = &p.elements[0].command;
                assert_eq!(cmd.redirects.len(), 1);
                match &cmd.redirects[0].kind {
                    RedirectKind::Heredoc { interp, strip_tabs, body } => {
                        assert!(*interp);
                        assert!(!*strip_tabs);
                        assert_eq!(body.len(), 1);
                    }
                    _ => panic!(),
                }
            }
            _ => panic!(),
        }
    }

    #[test]
    fn redirect_interleaved_with_words() {
        let s = parse_ok("cmd arg1 > out arg2");
        match &s.statements[0].kind {
            StatementKind::Pipeline(p) => match &p.elements[0].command.kind {
                CommandKind::Simple(sc) => {
                    assert_eq!(sc.words.len(), 3);
                    assert_eq!(p.elements[0].command.redirects.len(), 1);
                }
                _ => panic!(),
            },
            _ => panic!(),
        }
    }

    #[test]
    fn equal_in_command_position_errors() {
        match parse_err("cmd =arg") {
            ParseErrorKind::UnexpectedEqualInCommand => {}
            other => panic!("expected UnexpectedEqualInCommand, got {:?}", other),
        }
    }

    #[test]
    fn bare_assignment() {
        let s = parse_ok("x = 5");
        match &s.statements[0].kind {
            StatementKind::Assign(a) => {
                assert_eq!(a.name, "x");
                // U-5c: value is now an Expr; `5` lifts to a Word.
                match &a.value.kind {
                    ExprKind::Word(w) => assert_eq!(w, "5"),
                    other => panic!("expected Word, got {:?}", other),
                }
            }
            _ => panic!(),
        }
    }

    #[test]
    fn let_assignment() {
        let s = parse_ok("let x = 5");
        match &s.statements[0].kind {
            StatementKind::Let(l) => {
                assert_eq!(l.name, "x");
                match &l.value.kind {
                    ExprKind::Word(w) => assert_eq!(w, "5"),
                    other => panic!("expected Word, got {:?}", other),
                }
            }
            _ => panic!(),
        }
    }

    #[test]
    fn list_assignment() {
        let s = parse_ok("let files = (a b c)");
        match &s.statements[0].kind {
            StatementKind::Let(l) => {
                assert_eq!(l.name, "files");
                // U-5c: value is now an Expr; `(a b c)` lifts to a
                // List of three Word elements.
                match &l.value.kind {
                    ExprKind::List(elems) => assert_eq!(elems.len(), 3),
                    other => panic!("expected List, got {:?}", other),
                }
            }
            _ => panic!(),
        }
    }

    #[test]
    fn if_basic() {
        let s = parse_ok("if (x == 0) { echo zero }");
        match &s.statements[0].kind {
            StatementKind::If(i) => {
                // U-5c: cond is now an Expr; `x == 0` lifts to
                // BinOp(Eq, Word("x"), Word("0")).
                match &i.cond.kind {
                    ExprKind::BinOp(BinOp::Eq, _, _) => {}
                    other => panic!("expected BinOp(Eq), got {:?}", other),
                }
                assert_eq!(i.then_branch.len(), 1);
                assert_eq!(i.elif_branches.len(), 0);
                assert!(i.else_branch.is_none());
            }
            _ => panic!(),
        }
    }

    #[test]
    fn if_else() {
        let s = parse_ok("if (x) { a } else { b }");
        match &s.statements[0].kind {
            StatementKind::If(i) => {
                assert!(i.else_branch.is_some());
                assert_eq!(i.else_branch.as_ref().unwrap().len(), 1);
            }
            _ => panic!(),
        }
    }

    #[test]
    fn if_elif_else() {
        let s = parse_ok("if (x == 0) { a } else if (x < 10) { b } else { c }");
        match &s.statements[0].kind {
            StatementKind::If(i) => {
                assert_eq!(i.elif_branches.len(), 1);
                assert!(i.else_branch.is_some());
            }
            _ => panic!(),
        }
    }

    #[test]
    fn for_loop() {
        let s = parse_ok("for (f in $files) { cat $f }");
        match &s.statements[0].kind {
            StatementKind::For(f) => {
                assert_eq!(f.var_name, "f");
                // U-5c: list_expr is now an Expr; `$files` lifts to
                // Var("files").
                match &f.list_expr.kind {
                    ExprKind::Var(v) => assert_eq!(v, "files"),
                    other => panic!("expected Var, got {:?}", other),
                }
                assert_eq!(f.body.len(), 1);
            }
            _ => panic!(),
        }
    }

    #[test]
    fn while_loop() {
        let s = parse_ok("while ($cond) { tick }");
        match &s.statements[0].kind {
            StatementKind::While(w) => {
                // U-5c: cond is now an Expr; `$cond` lifts to
                // Var("cond").
                match &w.cond.kind {
                    ExprKind::Var(v) => assert_eq!(v, "cond"),
                    other => panic!("expected Var, got {:?}", other),
                }
                assert_eq!(w.body.len(), 1);
            }
            _ => panic!(),
        }
    }

    #[test]
    fn fn_decl_no_args() {
        let s = parse_ok("fn greet { echo hello }");
        match &s.statements[0].kind {
            StatementKind::FnDecl(f) => {
                assert_eq!(f.name, "greet");
                assert!(f.args.is_empty());
                assert_eq!(f.body.len(), 1);
            }
            _ => panic!(),
        }
    }

    #[test]
    fn fn_decl_with_args() {
        let s = parse_ok("fn add a b { echo $a $b }");
        match &s.statements[0].kind {
            StatementKind::FnDecl(f) => {
                assert_eq!(f.name, "add");
                assert_eq!(f.args, vec!["a".to_string(), "b".to_string()]);
            }
            _ => panic!(),
        }
    }

    #[test]
    fn return_no_value() {
        let s = parse_ok("fn f { return }");
        match &s.statements[0].kind {
            StatementKind::FnDecl(f) => match &f.body[0].kind {
                StatementKind::Return(v) => assert!(v.is_none()),
                _ => panic!(),
            },
            _ => panic!(),
        }
    }

    #[test]
    fn return_with_value() {
        let s = parse_ok("fn f { return 42 }");
        match &s.statements[0].kind {
            StatementKind::FnDecl(f) => match &f.body[0].kind {
                // U-5c: Return value is now an Expr; `42` lifts to
                // Word("42").
                StatementKind::Return(Some(v)) => match &v.kind {
                    ExprKind::Word(w) => assert_eq!(w, "42"),
                    other => panic!("expected Word, got {:?}", other),
                },
                _ => panic!(),
            },
            _ => panic!(),
        }
    }

    #[test]
    fn break_and_continue() {
        let s = parse_ok("while ($c) { break }");
        match &s.statements[0].kind {
            StatementKind::While(w) => match &w.body[0].kind {
                StatementKind::Break => {}
                _ => panic!(),
            },
            _ => panic!(),
        }
        let s = parse_ok("while ($c) { continue }");
        match &s.statements[0].kind {
            StatementKind::While(w) => match &w.body[0].kind {
                StatementKind::Continue => {}
                _ => panic!(),
            },
            _ => panic!(),
        }
    }

    #[test]
    fn concat_via_caret() {
        let s = parse_ok("echo $a^$b");
        match &s.statements[0].kind {
            StatementKind::Pipeline(p) => match &p.elements[0].command.kind {
                CommandKind::Simple(sc) => {
                    assert_eq!(sc.words.len(), 2);
                    match &sc.words[1] {
                        Word::Concat(parts) => assert_eq!(parts.len(), 2),
                        _ => panic!("expected Concat"),
                    }
                }
                _ => panic!(),
            },
            _ => panic!(),
        }
    }

    #[test]
    fn concat_with_spaces_does_not_concat() {
        // `$a ^ $b` -> Var, Caret, Var -- not span-adjacent. The
        // parser will see Caret in command-arg position and (since
        // it's not a value/redirect) the loop stops. After the
        // simple command parse returns, the Caret is left in the
        // stream. parse_pipeline / parse_pipeline_statement returns;
        // parse_script's terminator check sees Caret and errors.
        let err = parse_err("echo $a ^ $b");
        assert!(matches!(
            err,
            ParseErrorKind::UnexpectedToken { .. }
        ));
    }

    #[test]
    fn quoted_keyword_is_string_not_keyword() {
        // `'if'` is SingleQuoted, NOT the If keyword.
        let s = parse_ok("echo 'if'");
        match &s.statements[0].kind {
            StatementKind::Pipeline(p) => match &p.elements[0].command.kind {
                CommandKind::Simple(sc) => assert_eq!(sc.words.len(), 2),
                _ => panic!(),
            },
            _ => panic!(),
        }
    }

    #[test]
    fn stray_else_errors() {
        match parse_err("else { foo }") {
            ParseErrorKind::InvalidStatement => {}
            other => panic!("expected InvalidStatement, got {:?}", other),
        }
    }

    #[test]
    fn span_covers_full_command() {
        let s = parse_ok("cat /etc/hosts");
        assert_eq!(s.statements[0].span.start, 0);
        assert_eq!(s.statements[0].span.end, 14);
    }

    #[test]
    fn multiple_statements_in_brace_block() {
        let s = parse_ok("{ a; b; c }");
        match &s.statements[0].kind {
            StatementKind::Pipeline(p) => match &p.elements[0].command.kind {
                CommandKind::BraceBlock(stmts) => assert_eq!(stmts.len(), 3),
                _ => panic!(),
            },
            _ => panic!(),
        }
    }

    #[test]
    fn brace_block_can_contain_control_flow() {
        let s = parse_ok("{ if (x) { a } else { b } }");
        match &s.statements[0].kind {
            StatementKind::Pipeline(p) => match &p.elements[0].command.kind {
                CommandKind::BraceBlock(stmts) => {
                    assert_eq!(stmts.len(), 1);
                    matches!(stmts[0].kind, StatementKind::If(_));
                }
                _ => panic!(),
            },
            _ => panic!(),
        }
    }

    #[test]
    fn heredoc_strip_tabs() {
        let s = parse_ok("cat <<-EOF\n\thello\n\tEOF\n");
        match &s.statements[0].kind {
            StatementKind::Pipeline(p) => match &p.elements[0].command.redirects[0].kind {
                RedirectKind::Heredoc {
                    strip_tabs, ..
                } => assert!(*strip_tabs),
                _ => panic!(),
            },
            _ => panic!(),
        }
    }

    #[test]
    fn nested_subshell_inside_subshell() {
        let s = parse_ok("(a; (b; c))");
        match &s.statements[0].kind {
            StatementKind::Pipeline(p) => match &p.elements[0].command.kind {
                CommandKind::Subshell(stmts) => assert_eq!(stmts.len(), 2),
                _ => panic!(),
            },
            _ => panic!(),
        }
    }

    #[test]
    fn fn_with_let_in_body() {
        let s = parse_ok("fn f { let x = 5; echo $x }");
        match &s.statements[0].kind {
            StatementKind::FnDecl(f) => {
                assert_eq!(f.body.len(), 2);
                matches!(f.body[0].kind, StatementKind::Let(_));
            }
            _ => panic!(),
        }
    }

    #[test]
    fn pipeline_inside_brace_block() {
        let s = parse_ok("{ ls | grep foo }");
        match &s.statements[0].kind {
            StatementKind::Pipeline(p) => match &p.elements[0].command.kind {
                CommandKind::BraceBlock(stmts) => match &stmts[0].kind {
                    StatementKind::Pipeline(inner_p) => assert_eq!(inner_p.elements.len(), 2),
                    _ => panic!(),
                },
                _ => panic!(),
            },
            _ => panic!(),
        }
    }

    #[test]
    fn unclosed_brace_errors() {
        match parse_err("{ a; b") {
            ParseErrorKind::UnexpectedEof { .. } => {}
            other => panic!("expected UnexpectedEof, got {:?}", other),
        }
    }

    #[test]
    fn unclosed_if_errors() {
        match parse_err("if (x) { a") {
            ParseErrorKind::UnexpectedEof { .. } => {}
            other => panic!("expected UnexpectedEof, got {:?}", other),
        }
    }

    // -----------------------------------------------------------------
    // U-5d: case / try / trace / on / mask
    // -----------------------------------------------------------------

    #[test]
    fn case_basic() {
        let s = parse_ok("case $x { *.c => echo C ; *.rs => echo Rust }");
        match &s.statements[0].kind {
            StatementKind::Case(c) => {
                assert_eq!(c.arms.len(), 2);
                assert_eq!(c.arms[0].patterns.len(), 1);
                match &c.arms[0].patterns[0].kind {
                    ExprKind::Word(w) => assert_eq!(w, "*.c"),
                    other => panic!("expected Word(*.c), got {:?}", other),
                }
                match &c.arms[1].patterns[0].kind {
                    ExprKind::Word(w) => assert_eq!(w, "*.rs"),
                    other => panic!("expected Word(*.rs), got {:?}", other),
                }
            }
            other => panic!("expected Case, got {:?}", other),
        }
    }

    #[test]
    fn case_multi_pattern_arm() {
        let s = parse_ok("case $x { *.c *.h => echo C ; * => echo other }");
        match &s.statements[0].kind {
            StatementKind::Case(c) => {
                assert_eq!(c.arms.len(), 2);
                assert_eq!(c.arms[0].patterns.len(), 2);
                match &c.arms[0].patterns[0].kind {
                    ExprKind::Word(w) => assert_eq!(w, "*.c"),
                    _ => panic!(),
                }
                match &c.arms[0].patterns[1].kind {
                    ExprKind::Word(w) => assert_eq!(w, "*.h"),
                    _ => panic!(),
                }
                // catchall arm
                assert_eq!(c.arms[1].patterns.len(), 1);
                match &c.arms[1].patterns[0].kind {
                    ExprKind::Word(w) => assert_eq!(w, "*"),
                    _ => panic!(),
                }
            }
            _ => panic!(),
        }
    }

    #[test]
    fn case_brace_block_body() {
        let s = parse_ok("case $x { *.c => { build $x ; install $x } }");
        match &s.statements[0].kind {
            StatementKind::Case(c) => {
                assert_eq!(c.arms.len(), 1);
                match &c.arms[0].body.kind {
                    StatementKind::Pipeline(p) => match &p.elements[0].command.kind {
                        CommandKind::BraceBlock(stmts) => assert_eq!(stmts.len(), 2),
                        other => panic!("expected BraceBlock, got {:?}", other),
                    },
                    _ => panic!(),
                }
            }
            _ => panic!(),
        }
    }

    #[test]
    fn case_quoted_pattern() {
        let s = parse_ok("case $x { 'foo' => echo foo ; * => echo other }");
        match &s.statements[0].kind {
            StatementKind::Case(c) => {
                assert_eq!(c.arms.len(), 2);
                match &c.arms[0].patterns[0].kind {
                    ExprKind::SingleQuoted(s) => assert_eq!(s, "foo"),
                    _ => panic!(),
                }
            }
            _ => panic!(),
        }
    }

    #[test]
    fn case_newline_separated_arms() {
        let s = parse_ok("case $x {\n  *.c => echo C\n  *.rs => echo Rust\n}");
        match &s.statements[0].kind {
            StatementKind::Case(c) => assert_eq!(c.arms.len(), 2),
            _ => panic!(),
        }
    }

    #[test]
    fn case_empty_pattern_errors() {
        // `=> body` with no patterns before -- error.
        match parse_err("case $x { => body }") {
            ParseErrorKind::UnexpectedToken { .. } => {} // collect_case_arm_patterns hits FatArrow first
            ParseErrorKind::EmptyCasePattern => {}
            other => panic!("expected EmptyCasePattern or UnexpectedToken, got {:?}", other),
        }
    }

    #[test]
    fn case_missing_fat_arrow_errors() {
        match parse_err("case $x { *.c echo }") {
            ParseErrorKind::UnexpectedToken { .. } => {}
            other => panic!("expected UnexpectedToken (=>), got {:?}", other),
        }
    }

    #[test]
    fn try_catch_basic() {
        let s = parse_ok("try { risky } catch { recover }");
        match &s.statements[0].kind {
            StatementKind::Try(t) => {
                assert_eq!(t.body.len(), 1);
                assert_eq!(t.catch.len(), 1);
            }
            _ => panic!(),
        }
    }

    #[test]
    fn try_catch_newline_separated() {
        let s = parse_ok("try {\n  a\n  b\n} catch {\n  c\n}");
        match &s.statements[0].kind {
            StatementKind::Try(t) => {
                assert_eq!(t.body.len(), 2);
                assert_eq!(t.catch.len(), 1);
            }
            _ => panic!(),
        }
    }

    #[test]
    fn try_without_catch_errors() {
        match parse_err("try { a }") {
            ParseErrorKind::UnexpectedToken { expected } => {
                assert!(expected.contains("catch"));
            }
            ParseErrorKind::UnexpectedEof { expected } => {
                assert!(expected.contains("catch"));
            }
            other => panic!("expected catch-related error, got {:?}", other),
        }
    }

    #[test]
    fn try_catch_close_brace_newline_then_catch() {
        // `} \n catch {` is allowed.
        let s = parse_ok("try { a }\ncatch { b }");
        match &s.statements[0].kind {
            StatementKind::Try(t) => {
                assert_eq!(t.body.len(), 1);
                assert_eq!(t.catch.len(), 1);
            }
            _ => panic!(),
        }
    }

    #[test]
    fn trace_block() {
        let s = parse_ok("trace { cmd1 ; cmd2 }");
        match &s.statements[0].kind {
            StatementKind::Trace(t) => assert_eq!(t.body.len(), 2),
            _ => panic!(),
        }
    }

    #[test]
    fn trace_empty_block() {
        let s = parse_ok("trace { }");
        match &s.statements[0].kind {
            StatementKind::Trace(t) => assert_eq!(t.body.len(), 0),
            _ => panic!(),
        }
    }

    #[test]
    fn on_note_basic() {
        let s = parse_ok("on note 'snare:int' { cleanup }");
        match &s.statements[0].kind {
            StatementKind::OnNote(o) => {
                match &o.note_name.kind {
                    ExprKind::SingleQuoted(s) => assert_eq!(s, "snare:int"),
                    other => panic!("expected SingleQuoted, got {:?}", other),
                }
                assert_eq!(o.body.len(), 1);
            }
            _ => panic!(),
        }
    }

    #[test]
    fn on_note_bare_word_name() {
        let s = parse_ok("on note snare:int { cleanup }");
        match &s.statements[0].kind {
            StatementKind::OnNote(o) => match &o.note_name.kind {
                ExprKind::Word(w) => assert_eq!(w, "snare:int"),
                _ => panic!(),
            },
            _ => panic!(),
        }
    }

    #[test]
    fn on_without_note_keyword_errors() {
        match parse_err("on banana { cleanup }") {
            ParseErrorKind::UnexpectedToken { expected } => {
                assert!(expected.contains("note"));
            }
            other => panic!("expected `note` error, got {:?}", other),
        }
    }

    #[test]
    fn mask_note_basic() {
        let s = parse_ok("mask note 'snare:int' { critical_section }");
        match &s.statements[0].kind {
            StatementKind::MaskNote(m) => {
                match &m.note_name.kind {
                    ExprKind::SingleQuoted(s) => assert_eq!(s, "snare:int"),
                    _ => panic!(),
                }
                assert_eq!(m.body.len(), 1);
            }
            _ => panic!(),
        }
    }

    #[test]
    fn case_as_expression_via_let() {
        let s = parse_ok("let kind = case $x { *.c => 'C' ; * => 'unknown' }");
        match &s.statements[0].kind {
            StatementKind::Let(l) => match &l.value.kind {
                ExprKind::Case(c) => {
                    assert_eq!(c.arms.len(), 2);
                    match &c.arms[0].value.kind {
                        ExprKind::SingleQuoted(s) => assert_eq!(s, "C"),
                        _ => panic!(),
                    }
                    match &c.arms[1].value.kind {
                        ExprKind::SingleQuoted(s) => assert_eq!(s, "unknown"),
                        _ => panic!(),
                    }
                }
                other => panic!("expected ExprKind::Case, got {:?}", other),
            },
            _ => panic!(),
        }
    }

    #[test]
    fn case_in_brace_block_body() {
        // case inside a function body works without special handling.
        let s = parse_ok("fn classify f { case $f { *.c => echo C ; * => echo other } }");
        match &s.statements[0].kind {
            StatementKind::FnDecl(f) => {
                assert_eq!(f.body.len(), 1);
                matches!(f.body[0].kind, StatementKind::Case(_));
            }
            _ => panic!(),
        }
    }

    #[test]
    fn else_without_if_still_errors() {
        match parse_err("else { a }") {
            ParseErrorKind::InvalidStatement => {}
            other => panic!("expected InvalidStatement, got {:?}", other),
        }
    }

    #[test]
    fn catch_without_try_still_errors() {
        match parse_err("catch { a }") {
            ParseErrorKind::InvalidStatement => {}
            other => panic!("expected InvalidStatement, got {:?}", other),
        }
    }
}
