// libutopia::parser::lexer -- the U-5a tokenizer for the rc-shape
// shell grammar (UTOPIA-SHELL-DESIGN.md sections 5-9).
//
// === Strategic framing ===
//
// The lexer is a single-pass, recursive-descent-style scanner over
// the input `&str`. It walks the input byte-by-byte (UTF-8 aware --
// non-ASCII byte sequences are treated as word chars and copied
// through verbatim), maintaining a state machine that produces a
// Vec<Token>. Pure logic; no I/O; host-testable.
//
// The lexer's job is to make the parser's job easy. Every
// unambiguous syntactic surface gets its own token kind (Pipe,
// PipeTolerate, DoubleEqual, ...). Where lex-time disambiguation is
// genuinely impossible without parse context (e.g., `^` in command
// position is concat; in arith it's XOR), the lexer emits the same
// token (Caret) and lets the parser handle context.
//
// === The eight syntactic surfaces ===
//
// 1. Plain whitespace + comments: skipped silently. `\<newline>`
//    is a line continuation (also silently consumed).
//
// 2. Bare words: contiguous sequences of word chars (alphanumeric,
//    `_`, `.`, `/`, `-`, `+`, `:`, `@`, `,`, and the glob meta
//    chars `*`, `[`, `]`). The `?` glyph is a word char when
//    followed by another word char (preserves `*.?s`); otherwise
//    a Question operator. `\<char>` inside a word includes
//    `<char>` literally (escape for the surrounding shell).
//    Reserved keywords are matched against the scanned word text;
//    a match emits the keyword token instead of Word.
//
// 3. Quoted strings: `'literal'` (rc convention: `''` is the only
//    escape -- a doubled quote produces a single `'`); `"interp"`
//    (recognizes `\n`, `\t`, `\\`, `\"`, `\$`; recognizes
//    `$var`, `$#var`, `$(cmd)` as interpolations).
//
// 4. Variable refs at top level: `$var`, `$#var`, `$"var`. The
//    `$"var` form is rc's clean answer to bash's `"$@"` mess
//    (scripture section 6.8).
//
// 5. Substitutions: `$(cmd)` (POSIX-shape) and `` `{cmd}` `` (rc
//    shape; requires the closing backtick). The body is stored
//    raw and re-tokenized by the parser when it descends.
//
// 6. Process substitution: `<(cmd)`, `>(cmd)`. Same raw-body
//    pattern.
//
// 7. Heredocs: `<<TAG`, `<<-TAG`, `<<"TAG"`. The body is
//    collected immediately following the line's newline, so the
//    lexer queues pending heredoc specs at `<<TAG` time and
//    drains them at the next `\n`.
//
// 8. Regex literals: `/pattern/` AFTER `=~`. The lexer sets a
//    one-shot flag when emitting EqualTilde; the next opening
//    `/` is scanned as a Regex literal (with `\/` -> `/` escape).
//
// === Span discipline ===
//
// Every emitted Token carries a Span covering its source bytes.
// Spans are byte offsets (not char offsets); &source[span.start
// ..span.end] always returns valid UTF-8 because the lexer
// advances by whole UTF-8 chars at every step.

use alloc::string::String;
use alloc::vec;
use alloc::vec::Vec;

use super::error::{ParseError, ParseErrorKind, ParseResult};
use super::span::Span;
use super::token::{DqPart, Token, TokenKind};

/// Tokenize the input. The returned Vec ends with a synthetic Eof
/// token whose span is the point (source.len(), source.len()).
pub fn tokenize(source: &str) -> ParseResult<Vec<Token>> {
    let mut lex = Lexer::new(source);
    lex.run()?;
    Ok(lex.tokens)
}

// ---------------------------------------------------------------------
// Lexer state
// ---------------------------------------------------------------------

struct Lexer<'a> {
    source: &'a str,
    bytes: &'a [u8],
    pos: usize,
    tokens: Vec<Token>,
    pending_heredocs: Vec<HeredocSpec>,
    /// Set by emitting EqualTilde; consumed by the next opening `/`
    /// to lex a Regex literal. Per scripture section 7.3.
    expect_regex_after: bool,
}

#[derive(Clone)]
struct HeredocSpec {
    tag: String,
    interp: bool,
    strip_tabs: bool,
    /// Span of the `<<TAG` start; used in error reporting if the
    /// body terminator isn't found.
    start_span: Span,
}

impl<'a> Lexer<'a> {
    fn new(source: &'a str) -> Self {
        Self {
            source,
            bytes: source.as_bytes(),
            pos: 0,
            tokens: Vec::new(),
            pending_heredocs: Vec::new(),
            expect_regex_after: false,
        }
    }

    fn run(&mut self) -> ParseResult<()> {
        loop {
            self.skip_whitespace_and_continuations_and_comments();
            if self.pos >= self.bytes.len() {
                break;
            }
            // Single-shot regex peek: if the previous emitted token
            // was EqualTilde AND the next non-whitespace byte is
            // `/`, scan as Regex. Whitespace was already skipped
            // above; we just need to check the current byte.
            if self.expect_regex_after {
                self.expect_regex_after = false;
                if self.peek_byte() == Some(b'/') {
                    self.scan_regex()?;
                    continue;
                }
            }
            self.scan_next_token()?;
        }
        // Drain any pending heredocs at EOF -- they're unterminated
        // (the final line wasn't a tag match).
        if let Some(spec) = self.pending_heredocs.first() {
            return Err(ParseError {
                kind: ParseErrorKind::UnterminatedHeredoc {
                    tag: spec.tag.clone(),
                },
                span: spec.start_span,
            });
        }
        // Synthetic EOF token.
        let end = self.source.len();
        self.tokens
            .push(Token::new(TokenKind::Eof, Span::new(end, end)));
        Ok(())
    }

    // -----------------------------------------------------------------
    // Top-level dispatch
    // -----------------------------------------------------------------

    fn scan_next_token(&mut self) -> ParseResult<()> {
        let b = self.bytes[self.pos];
        match b {
            b'\n' => self.scan_newline(),
            b';' => self.emit_single(TokenKind::Semicolon, 1),
            b'|' => match self.peek_byte_at(1) {
                Some(b'|') => self.emit_single(TokenKind::OrOr, 2),
                _ => self.emit_single(TokenKind::Pipe, 1),
            },
            b'&' => match self.peek_byte_at(1) {
                Some(b'&') => self.emit_single(TokenKind::AndAnd, 2),
                _ => self.emit_single(TokenKind::Ampersand, 1),
            },
            b'?' => match self.peek_byte_at(1) {
                Some(b'|') => self.emit_single(TokenKind::PipeTolerate, 2),
                Some(nb) if is_word_char_byte(nb) => self.scan_word(),
                _ => self.emit_single(TokenKind::Question, 1),
            },
            b'=' => match self.peek_byte_at(1) {
                Some(b'=') => self.emit_single(TokenKind::DoubleEqual, 2),
                Some(b'~') => {
                    let r = self.emit_single(TokenKind::EqualTilde, 2);
                    self.expect_regex_after = true;
                    r
                }
                Some(b'>') => self.emit_single(TokenKind::FatArrow, 2),
                _ => self.emit_single(TokenKind::Equal, 1),
            },
            b'!' => match self.peek_byte_at(1) {
                Some(b'=') => self.emit_single(TokenKind::NotEqual, 2),
                _ => self.emit_single(TokenKind::Bang, 1),
            },
            b'<' => self.scan_less(),
            b'>' => self.scan_greater(),
            b'(' => match self.peek_byte_at(1) {
                Some(b'(') => self.emit_single(TokenKind::DoubleLParen, 2),
                _ => self.emit_single(TokenKind::LParen, 1),
            },
            b')' => match self.peek_byte_at(1) {
                Some(b')') => self.emit_single(TokenKind::DoubleRParen, 2),
                _ => self.emit_single(TokenKind::RParen, 1),
            },
            b'{' => self.emit_single(TokenKind::LBrace, 1),
            b'}' => self.emit_single(TokenKind::RBrace, 1),
            b'^' => self.emit_single(TokenKind::Caret, 1),
            b'~' => self.emit_single(TokenKind::Tilde, 1),
            b'\'' => self.scan_single_quoted(),
            b'"' => self.scan_double_quoted(),
            b'$' => self.scan_dollar(),
            b'`' => self.scan_backtick(),
            // All other word chars start a word. Backslash-leading
            // (e.g. `\$` at top level) starts a word too -- the
            // escaped char becomes the word's first char.
            _ if is_word_char_byte(b) || b == b'\\' => self.scan_word(),
            // Anything else: control chars, DEL, etc. The legal
            // syntactic surface is fully covered above; anything
            // here is genuinely outside the rc-shape grammar.
            _ => {
                let start = self.pos;
                let ch = self.peek_char().unwrap_or('?');
                let char_len = self.peek_char_len();
                self.pos += char_len;
                Err(ParseError {
                    kind: ParseErrorKind::UnexpectedChar(ch),
                    span: Span::new(start, self.pos),
                })
            }
        }
    }

    // -----------------------------------------------------------------
    // Whitespace + comments + line continuation
    // -----------------------------------------------------------------

    fn skip_whitespace_and_continuations_and_comments(&mut self) {
        loop {
            if self.pos >= self.bytes.len() {
                return;
            }
            let b = self.bytes[self.pos];
            match b {
                b' ' | b'\t' => self.pos += 1,
                // \<newline> is line continuation (consume both)
                b'\\' if self.peek_byte_at(1) == Some(b'\n') => self.pos += 2,
                // # to end-of-line is a comment. The \n stays --
                // it's significant.
                b'#' => {
                    while self.pos < self.bytes.len() && self.bytes[self.pos] != b'\n' {
                        self.pos += 1;
                    }
                }
                _ => return,
            }
        }
    }

    // -----------------------------------------------------------------
    // Newline + heredoc body drain
    // -----------------------------------------------------------------

    fn scan_newline(&mut self) -> ParseResult<()> {
        let start = self.pos;
        self.pos += 1;
        self.tokens
            .push(Token::new(TokenKind::Newline, Span::new(start, self.pos)));

        // Drain pending heredocs in FIFO order. Each drains the
        // bytes from current pos until a terminator line, and
        // advances pos past the terminator's newline.
        let pending: Vec<HeredocSpec> = core::mem::take(&mut self.pending_heredocs);
        for spec in pending {
            self.collect_heredoc_body(&spec)?;
        }
        Ok(())
    }

    fn collect_heredoc_body(&mut self, spec: &HeredocSpec) -> ParseResult<()> {
        let body_start = self.pos;
        loop {
            if self.pos >= self.bytes.len() {
                return Err(ParseError {
                    kind: ParseErrorKind::UnterminatedHeredoc {
                        tag: spec.tag.clone(),
                    },
                    span: spec.start_span,
                });
            }
            let line_start = self.pos;
            while self.pos < self.bytes.len() && self.bytes[self.pos] != b'\n' {
                self.pos += 1;
            }
            let line_end = self.pos;
            let mut line = &self.source[line_start..line_end];
            if spec.strip_tabs {
                line = line.trim_start_matches('\t');
            }
            if line == spec.tag.as_str() {
                // Terminator. Body is [body_start, line_start).
                let body_bytes = &self.source[body_start..line_start];
                let parts = if spec.interp {
                    parse_dq_or_heredoc_body(body_bytes, body_start)?
                } else {
                    if body_bytes.is_empty() {
                        Vec::new()
                    } else {
                        vec![DqPart::Literal(String::from(body_bytes))]
                    }
                };
                let body_span = Span::new(body_start, line_start);
                self.tokens
                    .push(Token::new(TokenKind::HeredocBody(parts), body_span));
                // Consume the terminator's newline (if present).
                if self.pos < self.bytes.len() && self.bytes[self.pos] == b'\n' {
                    self.pos += 1;
                }
                return Ok(());
            }
            // Not the terminator; advance past the newline (if any).
            if self.pos < self.bytes.len() && self.bytes[self.pos] == b'\n' {
                self.pos += 1;
            }
        }
    }

    // -----------------------------------------------------------------
    // Word scan (with `?` disambiguation + backslash escapes)
    // -----------------------------------------------------------------

    fn scan_word(&mut self) -> ParseResult<()> {
        let start = self.pos;
        let mut text = String::new();

        while self.pos < self.bytes.len() {
            let b = self.bytes[self.pos];
            if b == b'\\' {
                // \<char> includes <char> literally. \<nl> is line
                // continuation (consume both, continue scanning
                // word).
                if self.pos + 1 >= self.bytes.len() {
                    text.push('\\');
                    self.pos += 1;
                    break;
                }
                let nb = self.bytes[self.pos + 1];
                if nb == b'\n' {
                    self.pos += 2;
                    continue;
                }
                self.pos += 1; // past the `\`
                let char_len = self.peek_char_len();
                text.push_str(&self.source[self.pos..self.pos + char_len]);
                self.pos += char_len;
                continue;
            }
            if b == b'?' {
                // Disambiguate. If next byte is a word char, include
                // `?` in the word (preserves globs like `*.?s`).
                // Otherwise stop -- the `?` becomes a Question (or
                // PipeTolerate if followed by `|`) at top-level
                // dispatch.
                let next = self.peek_byte_at(1);
                if next.map(is_word_char_byte).unwrap_or(false) {
                    text.push('?');
                    self.pos += 1;
                    continue;
                }
                break;
            }
            if is_word_char_byte(b) {
                let char_len = self.peek_char_len();
                text.push_str(&self.source[self.pos..self.pos + char_len]);
                self.pos += char_len;
                continue;
            }
            break;
        }

        if text.is_empty() {
            // Defensive: scan_word was called but no chars consumed.
            // This shouldn't happen with the dispatcher.
            return Err(ParseError {
                kind: ParseErrorKind::EmptyVarName,
                span: Span::new(start, self.pos),
            });
        }

        let span = Span::new(start, self.pos);
        let kind = TokenKind::reserved_word(&text).unwrap_or(TokenKind::Word(text));
        self.tokens.push(Token::new(kind, span));
        Ok(())
    }

    // -----------------------------------------------------------------
    // Single-quoted string ('text' with '' for embedded quote)
    // -----------------------------------------------------------------

    fn scan_single_quoted(&mut self) -> ParseResult<()> {
        let start = self.pos;
        self.pos += 1; // consume opening '
        let mut text = String::new();
        loop {
            if self.pos >= self.bytes.len() {
                return Err(ParseError {
                    kind: ParseErrorKind::UnterminatedSingleQuote,
                    span: Span::new(start, self.pos),
                });
            }
            let b = self.bytes[self.pos];
            if b == b'\'' {
                if self.peek_byte_at(1) == Some(b'\'') {
                    // Doubled quote escape: '' -> '
                    text.push('\'');
                    self.pos += 2;
                    continue;
                }
                self.pos += 1; // consume closing '
                let span = Span::new(start, self.pos);
                self.tokens
                    .push(Token::new(TokenKind::SingleQuoted(text), span));
                return Ok(());
            }
            let char_len = self.peek_char_len();
            text.push_str(&self.source[self.pos..self.pos + char_len]);
            self.pos += char_len;
        }
    }

    // -----------------------------------------------------------------
    // Double-quoted string ("text $var $(cmd) ...")
    // -----------------------------------------------------------------

    fn scan_double_quoted(&mut self) -> ParseResult<()> {
        let start = self.pos;
        self.pos += 1; // consume opening "

        // Find the matching closing " (respecting \" and $(...))
        // and parse the body via shared helper.
        let body_start = self.pos;
        let body_end = self.find_dq_closing(start)?;
        let body = &self.source[body_start..body_end];
        let parts = parse_dq_or_heredoc_body(body, body_start)?;
        self.pos = body_end + 1; // skip closing "
        let span = Span::new(start, self.pos);
        self.tokens
            .push(Token::new(TokenKind::DoubleQuoted(parts), span));
        Ok(())
    }

    /// Locate the unescaped `"` that closes the DQ string opened at
    /// `open_start`. The current `self.pos` points just past the
    /// opening `"`. Returns the byte index of the closing `"`.
    /// Skips `\"` and `\$` escapes and `$(...)` substitutions
    /// (recursive paren counting) so that a `)` inside a $(...)
    /// substitution doesn't accidentally terminate the string.
    fn find_dq_closing(&self, open_start: usize) -> ParseResult<usize> {
        let mut p = self.pos;
        while p < self.bytes.len() {
            let b = self.bytes[p];
            match b {
                b'"' => return Ok(p),
                b'\\' => {
                    if p + 1 < self.bytes.len() {
                        p += 2;
                    } else {
                        p += 1;
                    }
                }
                b'$' if self.bytes.get(p + 1) == Some(&b'(') => {
                    // Skip $(...) body
                    let after = match find_balanced_paren_close(self.source, p + 2) {
                        Some(close) => close + 1,
                        None => {
                            return Err(ParseError {
                                kind: ParseErrorKind::UnterminatedSubstitution,
                                span: Span::new(p, self.bytes.len()),
                            })
                        }
                    };
                    p = after;
                }
                _ => p += 1,
            }
        }
        Err(ParseError {
            kind: ParseErrorKind::UnterminatedDoubleQuote,
            span: Span::new(open_start, self.bytes.len()),
        })
    }

    // -----------------------------------------------------------------
    // $-forms: $var, $#var, $"var, $(cmd)
    // -----------------------------------------------------------------

    fn scan_dollar(&mut self) -> ParseResult<()> {
        let start = self.pos;
        debug_assert_eq!(self.bytes[self.pos], b'$');
        match self.peek_byte_at(1) {
            Some(b'#') => {
                self.pos += 2; // consume $#
                let name_start = self.pos;
                self.scan_var_name();
                if self.pos == name_start {
                    return Err(ParseError {
                        kind: ParseErrorKind::EmptyVarLenName,
                        span: Span::new(start, self.pos),
                    });
                }
                let name = String::from(&self.source[name_start..self.pos]);
                let span = Span::new(start, self.pos);
                self.tokens.push(Token::new(TokenKind::VarLen(name), span));
                Ok(())
            }
            Some(b'"') => {
                self.pos += 2; // consume $"
                let name_start = self.pos;
                self.scan_var_name();
                if self.pos == name_start {
                    return Err(ParseError {
                        kind: ParseErrorKind::EmptyVarNoSplitName,
                        span: Span::new(start, self.pos),
                    });
                }
                let name = String::from(&self.source[name_start..self.pos]);
                let span = Span::new(start, self.pos);
                self.tokens
                    .push(Token::new(TokenKind::VarNoSplit(name), span));
                Ok(())
            }
            Some(b'(') => {
                self.pos += 2; // consume $(
                let body_start = self.pos;
                let close = find_balanced_paren_close(self.source, body_start).ok_or_else(
                    || ParseError {
                        kind: ParseErrorKind::UnterminatedSubstitution,
                        span: Span::new(start, self.bytes.len()),
                    },
                )?;
                let body = String::from(&self.source[body_start..close]);
                self.pos = close + 1;
                let span = Span::new(start, self.pos);
                self.tokens.push(Token::new(TokenKind::Subst(body), span));
                Ok(())
            }
            Some(b) if is_var_name_start_byte(b) => {
                self.pos += 1; // consume $
                let name_start = self.pos;
                self.scan_var_name();
                let name = String::from(&self.source[name_start..self.pos]);
                let span = Span::new(start, self.pos);
                self.tokens.push(Token::new(TokenKind::Var(name), span));
                Ok(())
            }
            _ => Err(ParseError {
                kind: ParseErrorKind::EmptyVarName,
                span: Span::new(start, self.pos + 1),
            }),
        }
    }

    fn scan_var_name(&mut self) {
        while self.pos < self.bytes.len() && is_var_name_byte(self.bytes[self.pos]) {
            self.pos += 1;
        }
    }

    // -----------------------------------------------------------------
    // `{cmd}` backtick form
    // -----------------------------------------------------------------

    fn scan_backtick(&mut self) -> ParseResult<()> {
        let start = self.pos;
        debug_assert_eq!(self.bytes[self.pos], b'`');
        // Per scripture section 6.6: the form is `` `{cmd}` ``.
        // Require `{` immediately after the opening backtick.
        if self.peek_byte_at(1) != Some(b'{') {
            return Err(ParseError {
                kind: ParseErrorKind::UnterminatedBacktick,
                span: Span::new(start, self.pos + 1),
            });
        }
        self.pos += 2; // consume `{
        let body_start = self.pos;
        let close_brace =
            find_balanced_brace_close(self.source, body_start).ok_or_else(|| ParseError {
                kind: ParseErrorKind::UnterminatedBacktick,
                span: Span::new(start, self.bytes.len()),
            })?;
        let body = String::from(&self.source[body_start..close_brace]);
        // Closing backtick must follow the `}`.
        if self.bytes.get(close_brace + 1) != Some(&b'`') {
            return Err(ParseError {
                kind: ParseErrorKind::UnterminatedBacktick,
                span: Span::new(start, close_brace + 1),
            });
        }
        self.pos = close_brace + 2; // past `}` `` ` ``
        let span = Span::new(start, self.pos);
        self.tokens.push(Token::new(TokenKind::Backtick(body), span));
        Ok(())
    }

    // -----------------------------------------------------------------
    // < family: < <= << <<- <<" <(...)
    // -----------------------------------------------------------------

    fn scan_less(&mut self) -> ParseResult<()> {
        let start = self.pos;
        match self.peek_byte_at(1) {
            Some(b'=') => self.emit_single(TokenKind::LessEqual, 2),
            Some(b'(') => self.scan_proc_sub_in(start),
            Some(b'<') => self.scan_heredoc_start(start),
            _ => self.emit_single(TokenKind::Less, 1),
        }
    }

    fn scan_proc_sub_in(&mut self, start: usize) -> ParseResult<()> {
        self.pos += 2; // consume <(
        let body_start = self.pos;
        let close =
            find_balanced_paren_close(self.source, body_start).ok_or_else(|| ParseError {
                kind: ParseErrorKind::UnterminatedProcSub,
                span: Span::new(start, self.bytes.len()),
            })?;
        let body = String::from(&self.source[body_start..close]);
        self.pos = close + 1;
        let span = Span::new(start, self.pos);
        self.tokens
            .push(Token::new(TokenKind::ProcSubIn(body), span));
        Ok(())
    }

    fn scan_heredoc_start(&mut self, start: usize) -> ParseResult<()> {
        // self.pos is at the first `<`. Consume `<<`.
        self.pos += 2;
        let mut strip_tabs = false;
        let mut interp = true;
        if self.peek_byte() == Some(b'-') {
            strip_tabs = true;
            self.pos += 1;
        }
        // Allow optional horizontal whitespace between `<<` / `<<-` and the
        // tag, so `cat << EOF` works as well as `cat <<EOF` (the universal
        // heredoc form). A newline before a tag still falls through to the
        // InvalidHeredocStart / var-name-start check below.
        while matches!(self.peek_byte(), Some(b' ') | Some(b'\t')) {
            self.pos += 1;
        }
        // Tag follows. Either `"TAG"` (quoted) or TAG (unquoted).
        if self.peek_byte() == Some(b'"') {
            interp = false;
            self.pos += 1;
            let tag_start = self.pos;
            while self.pos < self.bytes.len() && self.bytes[self.pos] != b'"' {
                if self.bytes[self.pos] == b'\n' {
                    return Err(ParseError {
                        kind: ParseErrorKind::UnterminatedHeredocTag,
                        span: Span::new(start, self.pos),
                    });
                }
                self.pos += 1;
            }
            if self.pos >= self.bytes.len() {
                return Err(ParseError {
                    kind: ParseErrorKind::UnterminatedHeredocTag,
                    span: Span::new(start, self.pos),
                });
            }
            let tag = String::from(&self.source[tag_start..self.pos]);
            self.pos += 1; // consume closing "
            let span = Span::new(start, self.pos);
            self.pending_heredocs.push(HeredocSpec {
                tag: tag.clone(),
                interp,
                strip_tabs,
                start_span: span,
            });
            self.tokens.push(Token::new(
                TokenKind::HeredocStart {
                    tag,
                    interp,
                    strip_tabs,
                },
                span,
            ));
            return Ok(());
        }
        // Unquoted tag: must start with alphanumeric or `_`.
        match self.peek_byte() {
            Some(b) if is_var_name_start_byte(b) => {}
            _ => {
                return Err(ParseError {
                    kind: ParseErrorKind::InvalidHeredocStart,
                    span: Span::new(start, self.pos),
                });
            }
        }
        let tag_start = self.pos;
        while self.pos < self.bytes.len() && is_var_name_byte(self.bytes[self.pos]) {
            self.pos += 1;
        }
        let tag = String::from(&self.source[tag_start..self.pos]);
        let span = Span::new(start, self.pos);
        self.pending_heredocs.push(HeredocSpec {
            tag: tag.clone(),
            interp,
            strip_tabs,
            start_span: span,
        });
        self.tokens.push(Token::new(
            TokenKind::HeredocStart {
                tag,
                interp,
                strip_tabs,
            },
            span,
        ));
        Ok(())
    }

    // -----------------------------------------------------------------
    // > family: > >= >> >(...)
    // -----------------------------------------------------------------

    fn scan_greater(&mut self) -> ParseResult<()> {
        let start = self.pos;
        match self.peek_byte_at(1) {
            Some(b'=') => self.emit_single(TokenKind::GreaterEqual, 2),
            Some(b'>') => self.emit_single(TokenKind::GreaterGreater, 2),
            Some(b'(') => {
                self.pos += 2; // consume >(
                let body_start = self.pos;
                let close = find_balanced_paren_close(self.source, body_start).ok_or_else(
                    || ParseError {
                        kind: ParseErrorKind::UnterminatedProcSub,
                        span: Span::new(start, self.bytes.len()),
                    },
                )?;
                let body = String::from(&self.source[body_start..close]);
                self.pos = close + 1;
                let span = Span::new(start, self.pos);
                self.tokens
                    .push(Token::new(TokenKind::ProcSubOut(body), span));
                Ok(())
            }
            _ => self.emit_single(TokenKind::Greater, 1),
        }
    }

    // -----------------------------------------------------------------
    // Regex literal (after `=~`)
    // -----------------------------------------------------------------

    fn scan_regex(&mut self) -> ParseResult<()> {
        let start = self.pos;
        debug_assert_eq!(self.bytes[self.pos], b'/');
        self.pos += 1; // consume opening /
        let mut text = String::new();
        loop {
            if self.pos >= self.bytes.len() {
                return Err(ParseError {
                    kind: ParseErrorKind::UnterminatedRegex,
                    span: Span::new(start, self.pos),
                });
            }
            let b = self.bytes[self.pos];
            if b == b'\n' {
                return Err(ParseError {
                    kind: ParseErrorKind::UnterminatedRegex,
                    span: Span::new(start, self.pos),
                });
            }
            if b == b'\\' && self.peek_byte_at(1) == Some(b'/') {
                text.push('/');
                self.pos += 2;
                continue;
            }
            if b == b'/' {
                self.pos += 1; // consume closing /
                let span = Span::new(start, self.pos);
                self.tokens.push(Token::new(TokenKind::Regex(text), span));
                return Ok(());
            }
            let char_len = self.peek_char_len();
            text.push_str(&self.source[self.pos..self.pos + char_len]);
            self.pos += char_len;
        }
    }

    // -----------------------------------------------------------------
    // Helpers
    // -----------------------------------------------------------------

    fn peek_byte(&self) -> Option<u8> {
        self.bytes.get(self.pos).copied()
    }

    fn peek_byte_at(&self, offset: usize) -> Option<u8> {
        self.bytes.get(self.pos + offset).copied()
    }

    fn peek_char(&self) -> Option<char> {
        self.source[self.pos..].chars().next()
    }

    fn peek_char_len(&self) -> usize {
        self.source[self.pos..]
            .chars()
            .next()
            .map_or(1, |c| c.len_utf8())
    }

    fn emit_single(&mut self, kind: TokenKind, len: usize) -> ParseResult<()> {
        let start = self.pos;
        self.pos += len;
        self.tokens
            .push(Token::new(kind, Span::new(start, self.pos)));
        Ok(())
    }
}

// ---------------------------------------------------------------------
// Free helpers (used by both the Lexer and the DQ/heredoc body
// parser which operates on slices)
// ---------------------------------------------------------------------

/// True if `b` is a valid bare-word character at U-5a.
///
/// Non-ASCII bytes (b >= 0x80) are always word chars; the lexer
/// advances by whole UTF-8 chars so multibyte sequences are
/// preserved verbatim into Word tokens.
fn is_word_char_byte(b: u8) -> bool {
    matches!(
        b,
        b'a'..=b'z' | b'A'..=b'Z' | b'0'..=b'9'
            | b'_'
            | b'.'
            | b'/'
            | b'-'
            | b'+'
            | b':'
            | b'@'
            | b','
            | b'*'
            | b'['
            | b']'
    ) || b >= 0x80
}

/// True if `b` is a valid first char of a variable name. Names are
/// `[a-zA-Z_][a-zA-Z0-9_]*`.
fn is_var_name_start_byte(b: u8) -> bool {
    matches!(b, b'a'..=b'z' | b'A'..=b'Z' | b'_')
}

/// True if `b` is a valid continuation char of a variable name.
fn is_var_name_byte(b: u8) -> bool {
    matches!(b, b'a'..=b'z' | b'A'..=b'Z' | b'0'..=b'9' | b'_')
}

/// Find the byte index of the `)` that closes a paren opened with
/// `body_start` pointing just past the opening `(`. Handles
/// nesting of `(...)`, single + double quoted strings, and `\`
/// escapes. Returns None on EOF without close.
fn find_balanced_paren_close(s: &str, body_start: usize) -> Option<usize> {
    let bytes = s.as_bytes();
    let mut p = body_start;
    let mut depth: usize = 1;
    let mut in_single = false;
    let mut in_double = false;
    while p < bytes.len() {
        let b = bytes[p];
        if in_single {
            if b == b'\'' {
                if bytes.get(p + 1) == Some(&b'\'') {
                    p += 2;
                    continue;
                }
                in_single = false;
            }
            p += 1;
            continue;
        }
        if in_double {
            if b == b'\\' && p + 1 < bytes.len() {
                p += 2;
                continue;
            }
            if b == b'"' {
                in_double = false;
            }
            p += 1;
            continue;
        }
        match b {
            b'\'' => {
                in_single = true;
                p += 1;
            }
            b'"' => {
                in_double = true;
                p += 1;
            }
            b'(' => {
                depth += 1;
                p += 1;
            }
            b')' => {
                depth -= 1;
                if depth == 0 {
                    return Some(p);
                }
                p += 1;
            }
            b'\\' => {
                p += if p + 1 < bytes.len() { 2 } else { 1 };
            }
            _ => p += 1,
        }
    }
    None
}

/// Find the byte index of the `}` that closes a brace opened with
/// `body_start` pointing just past the opening `{`. Same balancing
/// discipline as find_balanced_paren_close (quotes + escapes
/// respected; nesting tracked).
fn find_balanced_brace_close(s: &str, body_start: usize) -> Option<usize> {
    let bytes = s.as_bytes();
    let mut p = body_start;
    let mut depth: usize = 1;
    let mut in_single = false;
    let mut in_double = false;
    while p < bytes.len() {
        let b = bytes[p];
        if in_single {
            if b == b'\'' {
                if bytes.get(p + 1) == Some(&b'\'') {
                    p += 2;
                    continue;
                }
                in_single = false;
            }
            p += 1;
            continue;
        }
        if in_double {
            if b == b'\\' && p + 1 < bytes.len() {
                p += 2;
                continue;
            }
            if b == b'"' {
                in_double = false;
            }
            p += 1;
            continue;
        }
        match b {
            b'\'' => {
                in_single = true;
                p += 1;
            }
            b'"' => {
                in_double = true;
                p += 1;
            }
            b'{' => {
                depth += 1;
                p += 1;
            }
            b'}' => {
                depth -= 1;
                if depth == 0 {
                    return Some(p);
                }
                p += 1;
            }
            b'\\' => {
                p += if p + 1 < bytes.len() { 2 } else { 1 };
            }
            _ => p += 1,
        }
    }
    None
}

/// Parse a body that has DQ-string-interior semantics (`\n \t \\ \"
/// \$` escapes; `$name`, `$#name`, `$(cmd)` interpolations). Used
/// for the body of both `"..."` strings and interpolating
/// heredocs.
///
/// `body` is the raw bytes between the delimiters (exclusive). The
/// returned Vec<DqPart> represents the parsed structure; adjacent
/// literals are NOT split (one Literal per run of literal text).
/// `base_offset_in_source` is the byte offset where `body` starts
/// in the original source; it's used in error span construction
/// (errors point into the original source, not into the body
/// slice).
fn parse_dq_or_heredoc_body(body: &str, base_offset_in_source: usize) -> ParseResult<Vec<DqPart>> {
    let bytes = body.as_bytes();
    let mut parts: Vec<DqPart> = Vec::new();
    let mut current_literal = String::new();
    let mut p = 0;
    while p < bytes.len() {
        let b = bytes[p];
        match b {
            b'\\' => {
                if p + 1 >= bytes.len() {
                    current_literal.push('\\');
                    p += 1;
                    continue;
                }
                let nb = bytes[p + 1];
                match nb {
                    b'n' => {
                        current_literal.push('\n');
                        p += 2;
                    }
                    b't' => {
                        current_literal.push('\t');
                        p += 2;
                    }
                    b'\\' => {
                        current_literal.push('\\');
                        p += 2;
                    }
                    b'"' => {
                        current_literal.push('"');
                        p += 2;
                    }
                    b'$' => {
                        current_literal.push('$');
                        p += 2;
                    }
                    b'\n' => {
                        // line continuation
                        p += 2;
                    }
                    _ => {
                        // Unknown escape: preserve both `\` and the
                        // next char verbatim (rc-style).
                        current_literal.push('\\');
                        // Advance by one UTF-8 char.
                        let nb_char_len = body[p + 1..].chars().next().map_or(1, |c| c.len_utf8());
                        current_literal.push_str(&body[p + 1..p + 1 + nb_char_len]);
                        p += 1 + nb_char_len;
                    }
                }
            }
            b'$' => {
                if !current_literal.is_empty() {
                    parts.push(DqPart::Literal(core::mem::take(&mut current_literal)));
                }
                let (maybe_part, consumed) = scan_dollar_in_slice(body, p, base_offset_in_source)?;
                if let Some(part) = maybe_part {
                    parts.push(part);
                    p += consumed;
                } else {
                    current_literal.push('$');
                    p += 1;
                }
            }
            _ => {
                let char_len = body[p..].chars().next().map_or(1, |c| c.len_utf8());
                current_literal.push_str(&body[p..p + char_len]);
                p += char_len;
            }
        }
    }
    if !current_literal.is_empty() {
        parts.push(DqPart::Literal(current_literal));
    }
    Ok(parts)
}

/// Parse a $-expansion at position `p` in `body`. Returns:
///   Ok((Some(part), n)) — a $-expansion was recognized; consumed
///                          `n` bytes.
///   Ok((None, 0))      — `$` was not followed by a recognizable
///                          form; caller treats the `$` as a
///                          literal.
///   Err(e)             — lex error (e.g., `$(...)` unterminated).
fn scan_dollar_in_slice(
    body: &str,
    p: usize,
    base_offset_in_source: usize,
) -> ParseResult<(Option<DqPart>, usize)> {
    let bytes = body.as_bytes();
    debug_assert_eq!(bytes[p], b'$');
    let nb = bytes.get(p + 1).copied();
    match nb {
        Some(b'#') => {
            let name_start = p + 2;
            let mut q = name_start;
            while q < bytes.len() && is_var_name_byte(bytes[q]) {
                q += 1;
            }
            if q == name_start {
                return Err(ParseError {
                    kind: ParseErrorKind::EmptyVarLenName,
                    span: Span::new(
                        base_offset_in_source + p,
                        base_offset_in_source + name_start,
                    ),
                });
            }
            let name = String::from(&body[name_start..q]);
            Ok((Some(DqPart::VarLen(name)), q - p))
        }
        Some(b'(') => {
            let inner_start = p + 2;
            let close = find_balanced_paren_close(body, inner_start).ok_or_else(|| ParseError {
                kind: ParseErrorKind::UnterminatedSubstitution,
                span: Span::new(
                    base_offset_in_source + p,
                    base_offset_in_source + body.len(),
                ),
            })?;
            let cmd_body = String::from(&body[inner_start..close]);
            Ok((Some(DqPart::Subst(cmd_body)), (close + 1) - p))
        }
        Some(b) if is_var_name_start_byte(b) => {
            let name_start = p + 1;
            let mut q = name_start;
            while q < bytes.len() && is_var_name_byte(bytes[q]) {
                q += 1;
            }
            let name = String::from(&body[name_start..q]);
            Ok((Some(DqPart::Var(name)), q - p))
        }
        _ => Ok((None, 0)),
    }
}

// ---------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------

#[cfg(test)]
mod tests {
    use super::*;
    use alloc::string::ToString;
    use alloc::vec;

    fn kinds(src: &str) -> Vec<TokenKind> {
        tokenize(src)
            .unwrap()
            .into_iter()
            .map(|t| t.kind)
            .collect()
    }

    fn kinds_no_eof(src: &str) -> Vec<TokenKind> {
        let mut v = kinds(src);
        // Drop the trailing Eof for readability.
        if matches!(v.last(), Some(TokenKind::Eof)) {
            v.pop();
        }
        v
    }

    fn err(src: &str) -> ParseErrorKind {
        tokenize(src).unwrap_err().kind
    }

    #[test]
    fn empty_input_emits_only_eof() {
        assert_eq!(kinds(""), vec![TokenKind::Eof]);
    }

    #[test]
    fn whitespace_only_is_eof() {
        assert_eq!(kinds("   \t  "), vec![TokenKind::Eof]);
    }

    #[test]
    fn single_bare_word() {
        assert_eq!(
            kinds_no_eof("hello"),
            vec![TokenKind::Word("hello".to_string())]
        );
    }

    #[test]
    fn multiple_words() {
        assert_eq!(
            kinds_no_eof("foo bar baz"),
            vec![
                TokenKind::Word("foo".into()),
                TokenKind::Word("bar".into()),
                TokenKind::Word("baz".into()),
            ]
        );
    }

    #[test]
    fn glob_metas_stay_in_word() {
        assert_eq!(
            kinds_no_eof("*.rs"),
            vec![TokenKind::Word("*.rs".into())]
        );
        assert_eq!(
            kinds_no_eof("foo?bar"),
            vec![TokenKind::Word("foo?bar".into())]
        );
        assert_eq!(
            kinds_no_eof("*.?s"),
            vec![TokenKind::Word("*.?s".into())]
        );
        assert_eq!(
            kinds_no_eof("**/*.md"),
            vec![TokenKind::Word("**/*.md".into())]
        );
        assert_eq!(
            kinds_no_eof("[abc].txt"),
            vec![TokenKind::Word("[abc].txt".into())]
        );
    }

    #[test]
    fn question_at_word_end_is_separator() {
        assert_eq!(
            kinds_no_eof("cmd?"),
            vec![TokenKind::Word("cmd".into()), TokenKind::Question]
        );
        assert_eq!(
            kinds_no_eof("cmd ?"),
            vec![TokenKind::Word("cmd".into()), TokenKind::Question]
        );
    }

    #[test]
    fn pipe_tolerate() {
        assert_eq!(
            kinds_no_eof("cmd ?|"),
            vec![TokenKind::Word("cmd".into()), TokenKind::PipeTolerate]
        );
        assert_eq!(
            kinds_no_eof("cmd?|"),
            vec![TokenKind::Word("cmd".into()), TokenKind::PipeTolerate]
        );
    }

    #[test]
    fn newline_emits_token() {
        assert_eq!(
            kinds_no_eof("a\nb"),
            vec![
                TokenKind::Word("a".into()),
                TokenKind::Newline,
                TokenKind::Word("b".into()),
            ]
        );
    }

    #[test]
    fn semicolon() {
        assert_eq!(
            kinds_no_eof("a;b"),
            vec![
                TokenKind::Word("a".into()),
                TokenKind::Semicolon,
                TokenKind::Word("b".into()),
            ]
        );
    }

    #[test]
    fn operators_two_char() {
        assert_eq!(
            kinds_no_eof("a == b"),
            vec![
                TokenKind::Word("a".into()),
                TokenKind::DoubleEqual,
                TokenKind::Word("b".into()),
            ]
        );
        assert_eq!(
            kinds_no_eof("a != b"),
            vec![
                TokenKind::Word("a".into()),
                TokenKind::NotEqual,
                TokenKind::Word("b".into()),
            ]
        );
        assert_eq!(
            kinds_no_eof("a <= b"),
            vec![
                TokenKind::Word("a".into()),
                TokenKind::LessEqual,
                TokenKind::Word("b".into()),
            ]
        );
        assert_eq!(
            kinds_no_eof("a >= b"),
            vec![
                TokenKind::Word("a".into()),
                TokenKind::GreaterEqual,
                TokenKind::Word("b".into()),
            ]
        );
        assert_eq!(
            kinds_no_eof("a && b || c"),
            vec![
                TokenKind::Word("a".into()),
                TokenKind::AndAnd,
                TokenKind::Word("b".into()),
                TokenKind::OrOr,
                TokenKind::Word("c".into()),
            ]
        );
    }

    #[test]
    fn fat_arrow_and_case_arm() {
        assert_eq!(
            kinds_no_eof("*.rs => echo rust"),
            vec![
                TokenKind::Word("*.rs".into()),
                TokenKind::FatArrow,
                TokenKind::Word("echo".into()),
                TokenKind::Word("rust".into()),
            ]
        );
    }

    #[test]
    fn paren_double_paren() {
        assert_eq!(
            kinds_no_eof("(( x + 1 ))"),
            vec![
                TokenKind::DoubleLParen,
                TokenKind::Word("x".into()),
                TokenKind::Word("+".into()),
                TokenKind::Word("1".into()),
                TokenKind::DoubleRParen,
            ]
        );
    }

    #[test]
    fn single_quoted_literal() {
        assert_eq!(
            kinds_no_eof("'hello world'"),
            vec![TokenKind::SingleQuoted("hello world".into())]
        );
    }

    #[test]
    fn single_quoted_with_doubled_quote() {
        assert_eq!(
            kinds_no_eof("'it''s a quote'"),
            vec![TokenKind::SingleQuoted("it's a quote".into())]
        );
    }

    #[test]
    fn single_quoted_unterminated_errors() {
        match err("'no close") {
            ParseErrorKind::UnterminatedSingleQuote => {}
            other => panic!("expected UnterminatedSingleQuote, got {:?}", other),
        }
    }

    #[test]
    fn double_quoted_plain() {
        assert_eq!(
            kinds_no_eof("\"hello world\""),
            vec![TokenKind::DoubleQuoted(vec![DqPart::Literal(
                "hello world".into()
            )])]
        );
    }

    #[test]
    fn double_quoted_with_var() {
        assert_eq!(
            kinds_no_eof("\"hello $user\""),
            vec![TokenKind::DoubleQuoted(vec![
                DqPart::Literal("hello ".into()),
                DqPart::Var("user".into()),
            ])]
        );
    }

    #[test]
    fn double_quoted_with_subst() {
        assert_eq!(
            kinds_no_eof("\"pwd is $(pwd)\""),
            vec![TokenKind::DoubleQuoted(vec![
                DqPart::Literal("pwd is ".into()),
                DqPart::Subst("pwd".into()),
            ])]
        );
    }

    #[test]
    fn double_quoted_with_varlen() {
        assert_eq!(
            kinds_no_eof("\"len is $#files\""),
            vec![TokenKind::DoubleQuoted(vec![
                DqPart::Literal("len is ".into()),
                DqPart::VarLen("files".into()),
            ])]
        );
    }

    #[test]
    fn double_quoted_escapes() {
        assert_eq!(
            kinds_no_eof("\"a\\nb\\t$x\""),
            vec![TokenKind::DoubleQuoted(vec![
                DqPart::Literal("a\nb\t".into()),
                DqPart::Var("x".into()),
            ])]
        );
        assert_eq!(
            kinds_no_eof("\"\\$literal\""),
            vec![TokenKind::DoubleQuoted(vec![DqPart::Literal(
                "$literal".into()
            )])]
        );
    }

    #[test]
    fn double_quoted_unterminated_errors() {
        match err("\"no close") {
            ParseErrorKind::UnterminatedDoubleQuote => {}
            other => panic!("expected UnterminatedDoubleQuote, got {:?}", other),
        }
    }

    #[test]
    fn double_quoted_with_nested_subst_with_paren() {
        // $(echo (a))  — the inner ( and ) balance inside $(...).
        assert_eq!(
            kinds_no_eof("\"$(echo (a))\""),
            vec![TokenKind::DoubleQuoted(vec![DqPart::Subst(
                "echo (a)".into()
            )])]
        );
    }

    #[test]
    fn dollar_var() {
        assert_eq!(
            kinds_no_eof("$user"),
            vec![TokenKind::Var("user".into())]
        );
    }

    #[test]
    fn dollar_var_len() {
        assert_eq!(
            kinds_no_eof("$#files"),
            vec![TokenKind::VarLen("files".into())]
        );
    }

    #[test]
    fn dollar_var_no_split() {
        assert_eq!(
            kinds_no_eof("$\"args"),
            vec![TokenKind::VarNoSplit("args".into())]
        );
    }

    #[test]
    fn dollar_subst() {
        assert_eq!(
            kinds_no_eof("$(pwd)"),
            vec![TokenKind::Subst("pwd".into())]
        );
    }

    #[test]
    fn dollar_subst_nested() {
        assert_eq!(
            kinds_no_eof("$(echo $(date))"),
            vec![TokenKind::Subst("echo $(date)".into())]
        );
    }

    #[test]
    fn dollar_subst_with_quoted_parens() {
        // ')' inside single quotes shouldn't terminate.
        assert_eq!(
            kinds_no_eof("$(echo ')')"),
            vec![TokenKind::Subst("echo ')'".into())]
        );
    }

    #[test]
    fn dollar_alone_errors() {
        match err("$ ") {
            ParseErrorKind::EmptyVarName => {}
            other => panic!("expected EmptyVarName, got {:?}", other),
        }
    }

    #[test]
    fn backtick_braced_substitution() {
        assert_eq!(
            kinds_no_eof("`{pwd}`"),
            vec![TokenKind::Backtick("pwd".into())]
        );
    }

    #[test]
    fn backtick_unterminated_errors() {
        match err("`{no close") {
            ParseErrorKind::UnterminatedBacktick => {}
            other => panic!("expected UnterminatedBacktick, got {:?}", other),
        }
    }

    #[test]
    fn process_substitution_in() {
        assert_eq!(
            kinds_no_eof("diff <(cmd1) <(cmd2)"),
            vec![
                TokenKind::Word("diff".into()),
                TokenKind::ProcSubIn("cmd1".into()),
                TokenKind::ProcSubIn("cmd2".into()),
            ]
        );
    }

    #[test]
    fn process_substitution_out() {
        assert_eq!(
            kinds_no_eof("tee >(filter)"),
            vec![
                TokenKind::Word("tee".into()),
                TokenKind::ProcSubOut("filter".into()),
            ]
        );
    }

    #[test]
    fn heredoc_simple() {
        let src = "cat <<EOF\nhello\nEOF\n";
        let toks = kinds_no_eof(src);
        assert_eq!(
            toks,
            vec![
                TokenKind::Word("cat".into()),
                TokenKind::HeredocStart {
                    tag: "EOF".into(),
                    interp: true,
                    strip_tabs: false,
                },
                TokenKind::Newline,
                TokenKind::HeredocBody(vec![DqPart::Literal("hello\n".into())]),
            ]
        );
    }

    #[test]
    fn heredoc_interp() {
        let src = "cat <<EOF\nhello $name\nEOF\n";
        let toks = kinds_no_eof(src);
        assert_eq!(
            toks,
            vec![
                TokenKind::Word("cat".into()),
                TokenKind::HeredocStart {
                    tag: "EOF".into(),
                    interp: true,
                    strip_tabs: false,
                },
                TokenKind::Newline,
                TokenKind::HeredocBody(vec![
                    DqPart::Literal("hello ".into()),
                    DqPart::Var("name".into()),
                    DqPart::Literal("\n".into()),
                ]),
            ]
        );
    }

    #[test]
    fn heredoc_quoted_no_interp() {
        let src = "cat <<\"EOF\"\nhello $name\nEOF\n";
        let toks = kinds_no_eof(src);
        assert_eq!(
            toks,
            vec![
                TokenKind::Word("cat".into()),
                TokenKind::HeredocStart {
                    tag: "EOF".into(),
                    interp: false,
                    strip_tabs: false,
                },
                TokenKind::Newline,
                TokenKind::HeredocBody(vec![DqPart::Literal("hello $name\n".into())]),
            ]
        );
    }

    #[test]
    fn heredoc_strip_tabs() {
        let src = "cat <<-EOF\n\thello\n\tEOF\n";
        let toks = kinds_no_eof(src);
        assert_eq!(
            toks,
            vec![
                TokenKind::Word("cat".into()),
                TokenKind::HeredocStart {
                    tag: "EOF".into(),
                    interp: true,
                    strip_tabs: true,
                },
                TokenKind::Newline,
                TokenKind::HeredocBody(vec![DqPart::Literal("\thello\n".into())]),
            ]
        );
    }

    #[test]
    fn heredoc_unterminated_errors() {
        match err("cat <<EOF\nhello\n") {
            ParseErrorKind::UnterminatedHeredoc { tag } => {
                assert_eq!(tag, "EOF");
            }
            other => panic!("expected UnterminatedHeredoc, got {:?}", other),
        }
    }

    #[test]
    fn regex_after_match_op() {
        assert_eq!(
            kinds_no_eof("$x =~ /^foo/"),
            vec![
                TokenKind::Var("x".into()),
                TokenKind::EqualTilde,
                TokenKind::Regex("^foo".into()),
            ]
        );
    }

    #[test]
    fn regex_escaped_slash() {
        assert_eq!(
            kinds_no_eof("$x =~ /a\\/b/"),
            vec![
                TokenKind::Var("x".into()),
                TokenKind::EqualTilde,
                TokenKind::Regex("a/b".into()),
            ]
        );
    }

    #[test]
    fn regex_unterminated_errors() {
        match err("$x =~ /abc") {
            ParseErrorKind::UnterminatedRegex => {}
            other => panic!("expected UnterminatedRegex, got {:?}", other),
        }
    }

    #[test]
    fn slash_not_after_match_op_is_word() {
        // `/etc/hosts` is a path, not a regex.
        assert_eq!(
            kinds_no_eof("cat /etc/hosts"),
            vec![
                TokenKind::Word("cat".into()),
                TokenKind::Word("/etc/hosts".into()),
            ]
        );
    }

    #[test]
    fn reserved_words_distinct_from_words() {
        assert_eq!(
            kinds_no_eof("if while else fn let case for in try catch return break continue on mask trace"),
            vec![
                TokenKind::If, TokenKind::While, TokenKind::Else, TokenKind::Fn,
                TokenKind::Let, TokenKind::Case, TokenKind::For, TokenKind::In,
                TokenKind::Try, TokenKind::Catch, TokenKind::Return, TokenKind::Break,
                TokenKind::Continue, TokenKind::On, TokenKind::Mask, TokenKind::Trace,
            ]
        );
    }

    #[test]
    fn quoted_keyword_stays_string() {
        assert_eq!(
            kinds_no_eof("'fn'"),
            vec![TokenKind::SingleQuoted("fn".into())]
        );
    }

    #[test]
    fn comment_skipped() {
        assert_eq!(
            kinds_no_eof("echo hi # this is a comment\necho bye"),
            vec![
                TokenKind::Word("echo".into()),
                TokenKind::Word("hi".into()),
                TokenKind::Newline,
                TokenKind::Word("echo".into()),
                TokenKind::Word("bye".into()),
            ]
        );
    }

    #[test]
    fn line_continuation_joins_lines() {
        assert_eq!(
            kinds_no_eof("foo \\\nbar"),
            vec![
                TokenKind::Word("foo".into()),
                TokenKind::Word("bar".into()),
            ]
        );
    }

    #[test]
    fn backslash_inside_word_escapes() {
        // \$ in a bare word is literal $
        assert_eq!(
            kinds_no_eof("\\$path"),
            vec![TokenKind::Word("$path".into())]
        );
    }

    #[test]
    fn utf8_in_word() {
        assert_eq!(
            kinds_no_eof("naïve"),
            vec![TokenKind::Word("naïve".into())]
        );
    }

    #[test]
    fn utf8_in_double_quoted() {
        assert_eq!(
            kinds_no_eof("\"héllo $world\""),
            vec![TokenKind::DoubleQuoted(vec![
                DqPart::Literal("héllo ".into()),
                DqPart::Var("world".into()),
            ])]
        );
    }

    #[test]
    fn redirects() {
        assert_eq!(
            kinds_no_eof("cmd > out"),
            vec![
                TokenKind::Word("cmd".into()),
                TokenKind::Greater,
                TokenKind::Word("out".into()),
            ]
        );
        assert_eq!(
            kinds_no_eof("cmd >> out"),
            vec![
                TokenKind::Word("cmd".into()),
                TokenKind::GreaterGreater,
                TokenKind::Word("out".into()),
            ]
        );
        assert_eq!(
            kinds_no_eof("cmd < in"),
            vec![
                TokenKind::Word("cmd".into()),
                TokenKind::Less,
                TokenKind::Word("in".into()),
            ]
        );
    }

    #[test]
    fn pipeline_with_tolerate() {
        assert_eq!(
            kinds_no_eof("a ?| b | c"),
            vec![
                TokenKind::Word("a".into()),
                TokenKind::PipeTolerate,
                TokenKind::Word("b".into()),
                TokenKind::Pipe,
                TokenKind::Word("c".into()),
            ]
        );
    }

    #[test]
    fn case_arm_pattern_with_fat_arrow() {
        assert_eq!(
            kinds_no_eof("*.c *.h => echo C"),
            vec![
                TokenKind::Word("*.c".into()),
                TokenKind::Word("*.h".into()),
                TokenKind::FatArrow,
                TokenKind::Word("echo".into()),
                TokenKind::Word("C".into()),
            ]
        );
    }

    #[test]
    fn span_covers_word() {
        let toks = tokenize("hello").unwrap();
        // [0]: Word, [1]: Eof
        assert_eq!(toks[0].span, Span::new(0, 5));
        assert_eq!(toks[1].span, Span::new(5, 5));
    }

    #[test]
    fn span_covers_double_quoted() {
        let toks = tokenize("\"hi\"").unwrap();
        assert_eq!(toks[0].span, Span::new(0, 4));
    }

    #[test]
    fn span_covers_subst() {
        let toks = tokenize("$(pwd)").unwrap();
        assert_eq!(toks[0].span, Span::new(0, 6));
    }

    #[test]
    fn full_command_example() {
        // A multi-construct line from scripture section 6.5
        let src = "let greeting = \"Hello, $user from $(pwd)\"";
        let toks = kinds_no_eof(src);
        assert_eq!(
            toks,
            vec![
                TokenKind::Let,
                TokenKind::Word("greeting".into()),
                TokenKind::Equal,
                TokenKind::DoubleQuoted(vec![
                    DqPart::Literal("Hello, ".into()),
                    DqPart::Var("user".into()),
                    DqPart::Literal(" from ".into()),
                    DqPart::Subst("pwd".into()),
                ]),
            ]
        );
    }
}
