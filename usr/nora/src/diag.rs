// nora::diag -- the editor's view of language-server diagnostics (Stage 8e-2).
//
// Deliberately PROTOCOL-FREE: the engine knows "line L, byte columns C..E,
// this severity, this message" and nothing about LSP. The binary owns the
// parley client and converts (including the LSP character-offset -> byte-offset
// conversion under the negotiated encoding, which needs the line text). So the
// engine stays pure + host-tested, and a second language server -- or a
// non-LSP source like `go vet` output -- feeds the same struct.
//
// Columns are CHARACTER columns -- the coordinate every `Pos` in nora uses
// (nora::text is char-addressed end to end so multi-byte UTF-8 can never split
// a grapheme). A producer holding byte offsets converts with
// `nora::text::byte_to_char_col`; getting this wrong is invisible on ASCII and
// lands the cursor off by (bytes - chars) on the first non-ASCII line, which is
// why the coordinate is named here rather than left to each caller.

use alloc::string::String;
use alloc::vec::Vec;

/// Diagnostic severity, ordered most-severe-first so `min` picks the winner
/// when several land on one line.
#[derive(Clone, Copy, Debug, PartialEq, Eq, PartialOrd, Ord)]
pub enum Severity {
    Error,
    Warning,
    Info,
    Hint,
}

/// One diagnostic, already clipped to a single line.
#[derive(Clone, Debug, PartialEq)]
pub struct LineDiag {
    /// 0-based logical row.
    pub line: usize,
    /// Character column of the span start within `line`.
    pub col: usize,
    /// Character column of the span end (exclusive). May equal `col` for a
    /// zero-width marker; never less.
    pub end_col: usize,
    pub severity: Severity,
    pub message: String,
}

/// The active diagnostics for the visible buffer.
///
/// Small by construction (a compiler stops after a handful of errors per file),
/// so a linear scan per rendered row is cheaper than any index -- and the row
/// loop is bounded by the viewport height, not the file length.
#[derive(Clone, Debug, Default)]
pub struct Diagnostics {
    items: Vec<LineDiag>,
}

impl Diagnostics {
    pub fn new() -> Diagnostics {
        Diagnostics { items: Vec::new() }
    }

    /// Replace the whole set (a server publishes the full list per file; an
    /// empty list is how it CLEARS them, so this must accept empty).
    pub fn set(&mut self, items: Vec<LineDiag>) {
        self.items = items;
    }

    pub fn clear(&mut self) {
        self.items.clear();
    }

    pub fn is_empty(&self) -> bool {
        self.items.is_empty()
    }

    pub fn items(&self) -> &[LineDiag] {
        &self.items
    }

    /// The most severe diagnostic on `line` (ties -> the earliest column, so
    /// the gutter and the status line agree on which one they are showing).
    pub fn for_line(&self, line: usize) -> Option<&LineDiag> {
        self.items
            .iter()
            .filter(|d| d.line == line)
            .min_by(|a, b| a.severity.cmp(&b.severity).then(a.col.cmp(&b.col)))
    }

    /// Severity to tint `line`'s gutter with, if any.
    pub fn severity_of_line(&self, line: usize) -> Option<Severity> {
        self.for_line(line).map(|d| d.severity)
    }

    /// `(errors, warnings)` across the buffer -- the status-line counter.
    /// Info/hint are deliberately not counted: the counter exists to answer
    /// "is my code broken", and folding hints in would keep it permanently lit.
    pub fn counts(&self) -> (usize, usize) {
        let mut e = 0;
        let mut w = 0;
        for d in &self.items {
            match d.severity {
                Severity::Error => e += 1,
                Severity::Warning => w += 1,
                _ => {}
            }
        }
        (e, w)
    }

    /// The next diagnostic strictly after `line`, wrapping to the first --
    /// `]d` navigation. `None` only when there are none at all.
    pub fn next_after(&self, line: usize) -> Option<&LineDiag> {
        let mut sorted: Vec<&LineDiag> = self.items.iter().collect();
        sorted.sort_by_key(|d| (d.line, d.col));
        sorted
            .iter()
            .find(|d| d.line > line)
            .or_else(|| sorted.first())
            .copied()
    }

    /// The previous diagnostic strictly before `line`, wrapping to the last.
    pub fn prev_before(&self, line: usize) -> Option<&LineDiag> {
        let mut sorted: Vec<&LineDiag> = self.items.iter().collect();
        sorted.sort_by_key(|d| (d.line, d.col));
        sorted
            .iter()
            .rev()
            .find(|d| d.line < line)
            .or_else(|| sorted.last())
            .copied()
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use alloc::string::ToString;

    fn d(line: usize, col: usize, sev: Severity, msg: &str) -> LineDiag {
        LineDiag { line, col, end_col: col + 3, severity: sev, message: msg.to_string() }
    }

    #[test]
    fn empty_by_default() {
        let dg = Diagnostics::new();
        assert!(dg.is_empty());
        assert_eq!(dg.for_line(0), None);
        assert_eq!(dg.severity_of_line(0), None);
        assert_eq!(dg.counts(), (0, 0));
        assert_eq!(dg.next_after(0), None);
        assert_eq!(dg.prev_before(0), None);
    }

    #[test]
    fn set_replaces_and_empty_clears() {
        let mut dg = Diagnostics::new();
        dg.set(alloc::vec![d(1, 0, Severity::Error, "boom")]);
        assert_eq!(dg.items().len(), 1);
        // A server clears by publishing an EMPTY list -- not by omitting it.
        dg.set(Vec::new());
        assert!(dg.is_empty());
    }

    #[test]
    fn most_severe_wins_on_a_line() {
        let mut dg = Diagnostics::new();
        dg.set(alloc::vec![
            d(4, 10, Severity::Warning, "warn"),
            d(4, 2, Severity::Error, "err"),
            d(4, 0, Severity::Hint, "hint"),
        ]);
        assert_eq!(dg.for_line(4).unwrap().message, "err");
        assert_eq!(dg.severity_of_line(4), Some(Severity::Error));
        assert_eq!(dg.severity_of_line(5), None);
    }

    #[test]
    fn ties_break_on_the_earliest_column() {
        let mut dg = Diagnostics::new();
        dg.set(alloc::vec![
            d(2, 9, Severity::Error, "later"),
            d(2, 1, Severity::Error, "earlier"),
        ]);
        assert_eq!(dg.for_line(2).unwrap().message, "earlier");
    }

    #[test]
    fn counts_only_errors_and_warnings() {
        let mut dg = Diagnostics::new();
        dg.set(alloc::vec![
            d(0, 0, Severity::Error, "e1"),
            d(1, 0, Severity::Error, "e2"),
            d(2, 0, Severity::Warning, "w"),
            d(3, 0, Severity::Info, "i"),
            d(4, 0, Severity::Hint, "h"),
        ]);
        assert_eq!(dg.counts(), (2, 1));
    }

    #[test]
    fn navigation_wraps_both_ways() {
        let mut dg = Diagnostics::new();
        dg.set(alloc::vec![
            d(9, 0, Severity::Error, "last"),
            d(2, 0, Severity::Error, "first"),
            d(5, 0, Severity::Warning, "mid"),
        ]);
        assert_eq!(dg.next_after(0).unwrap().message, "first");
        assert_eq!(dg.next_after(2).unwrap().message, "mid");
        assert_eq!(dg.next_after(5).unwrap().message, "last");
        // Past the last -> wrap to the first.
        assert_eq!(dg.next_after(9).unwrap().message, "first");
        assert_eq!(dg.next_after(999).unwrap().message, "first");

        assert_eq!(dg.prev_before(9).unwrap().message, "mid");
        assert_eq!(dg.prev_before(5).unwrap().message, "first");
        // Before the first -> wrap to the last.
        assert_eq!(dg.prev_before(2).unwrap().message, "last");
        assert_eq!(dg.prev_before(0).unwrap().message, "last");
    }

    #[test]
    fn severity_orders_most_severe_first() {
        assert!(Severity::Error < Severity::Warning);
        assert!(Severity::Warning < Severity::Info);
        assert!(Severity::Info < Severity::Hint);
    }
}
