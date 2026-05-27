// libutopia::parser::span -- byte-range markers carried by every Token
// produced by the U-5a tokenizer.
//
// Per the U-5 open-design answer #3 (UTOPIA-SHELL-DESIGN.md section
// 19 lift, recorded in memory/project_next_session.md): every AST
// node and every Token tracks its source span. This pins the byte
// range each token covers so the parser (U-5b) can emit
// location-bearing diagnostics, and so future tooling (LSP-shaped
// servers, syntax-aware completers in U-7) can map cursor positions
// to AST nodes without re-tokenizing.
//
// The convention matches Rust's standard span model: `start` is the
// inclusive byte offset; `end` is the exclusive byte offset. An
// empty span (start == end) marks a point. For EOF tokens, the span
// is (source.len(), source.len()) -- a point past the last byte.
//
// `start` and `end` are byte offsets, NOT char offsets. The lexer
// is UTF-8-aware (the source is `&str`), but spans index into the
// underlying byte array so that &source[span.start..span.end]
// always returns valid UTF-8 text for the token.

use core::fmt;

#[derive(Copy, Clone, Eq, PartialEq, Hash)]
pub struct Span {
    pub start: usize,
    pub end: usize,
}

impl Span {
    pub const fn new(start: usize, end: usize) -> Self {
        Self { start, end }
    }

    pub const fn point(at: usize) -> Self {
        Self { start: at, end: at }
    }

    pub fn len(&self) -> usize {
        self.end - self.start
    }

    pub fn is_empty(&self) -> bool {
        self.start == self.end
    }

    pub fn contains(&self, offset: usize) -> bool {
        offset >= self.start && offset < self.end
    }

    pub fn join(self, other: Span) -> Span {
        Span::new(
            core::cmp::min(self.start, other.start),
            core::cmp::max(self.end, other.end),
        )
    }

    pub fn slice<'a>(&self, source: &'a str) -> &'a str {
        &source[self.start..self.end]
    }
}

impl fmt::Debug for Span {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "{}..{}", self.start, self.end)
    }
}
