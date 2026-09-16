//! Word wrapping at the plain tiers (MANUAL-DESIGN.md 4.3), as a paragraph's or
//! list item's runs arrive.

use alloc::string::String;

use crate::format::Run;
use crate::is_control;

/// Wraps one paragraph or list item at a time. `begin` starts one; runs are
/// `feed` as they arrive; `end` finishes it. Each finished line is passed to
/// the caller, without its LF. A code span does not break at its own spaces; a
/// word longer than the line is split. Width is counted in Unicode scalar values.
///
/// Lines are filled greedily, and a word is placed as soon as its placement is
/// decided, so the pending word never holds more than one line's width.
pub struct Wrap {
    /// Columns for text after the marker or its indentation.
    avail: usize,
    /// The marker's width: continuation lines are indented by it.
    pad: usize,
    line: String,
    /// Characters of text on `line` after the marker or indentation.
    len: usize,
    word: String,
    wlen: usize,
    /// The pending word no longer fits on the line it began beside, so it starts
    /// a line of its own.
    own_line: bool,
}

impl Default for Wrap {
    fn default() -> Wrap {
        Wrap::new()
    }
}

impl Wrap {
    pub fn new() -> Wrap {
        Wrap {
            avail: 1,
            pad: 0,
            line: String::new(),
            len: 0,
            word: String::new(),
            wlen: 0,
            own_line: false,
        }
    }

    /// Start a paragraph (`marker` empty) or a list item (`marker` its `- ` or
    /// `N. `) at `width` columns.
    pub fn begin(&mut self, marker: &str, width: usize) {
        self.pad = marker.chars().count();
        self.avail = width.saturating_sub(self.pad).max(1);
        self.line.clear();
        self.line.push_str(marker);
        self.len = 0;
        self.word.clear();
        self.wlen = 0;
        self.own_line = false;
    }

    /// A run's text. Control characters become U+FFFD (4.4).
    pub fn feed(&mut self, kind: Run, text: &str, emit: &mut dyn FnMut(&str)) {
        for c in text.chars() {
            if c == ' ' && kind != Run::Code {
                self.end_word();
            } else {
                self.push(if is_control(c) { '\u{fffd}' } else { c }, emit);
            }
        }
    }

    /// Finish the paragraph or item: its last line, which may hold only the marker.
    pub fn end(&mut self, emit: &mut dyn FnMut(&str)) {
        self.end_word();
        emit(&self.line);
    }

    fn push(&mut self, c: char, emit: &mut dyn FnMut(&str)) {
        self.word.push(c);
        self.wlen += 1;
        if !self.own_line && self.len > 0 && self.len + 1 + self.wlen > self.avail {
            emit(&self.line);
            self.new_line();
            self.own_line = true;
        }
        if (self.own_line || self.len == 0) && self.wlen > self.avail {
            let split = self
                .word
                .char_indices()
                .nth(self.avail)
                .map_or(self.word.len(), |(i, _)| i);
            self.line.push_str(&self.word[..split]);
            emit(&self.line);
            self.new_line();
            self.word.drain(..split);
            self.wlen -= self.avail;
            self.own_line = true;
        }
    }

    fn end_word(&mut self) {
        if self.wlen == 0 {
            return;
        }
        if !self.own_line && self.len > 0 {
            self.line.push(' ');
            self.line.push_str(&self.word);
            self.len += 1 + self.wlen;
        } else {
            self.line.push_str(&self.word);
            self.len = self.wlen;
        }
        self.word.clear();
        self.wlen = 0;
        self.own_line = false;
    }

    fn new_line(&mut self) {
        self.line.clear();
        for _ in 0..self.pad {
            self.line.push(' ');
        }
        self.len = 0;
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use alloc::vec;
    use alloc::vec::Vec;

    fn wrap(marker: &str, runs: &[(Run, &str)], width: usize) -> Vec<String> {
        let mut w = Wrap::new();
        let mut lines = Vec::new();
        w.begin(marker, width);
        for (kind, text) in runs {
            w.feed(*kind, text, &mut |l| lines.push(String::from(l)));
        }
        w.end(&mut |l| lines.push(String::from(l)));
        lines
    }

    fn t(s: &str) -> (Run, &str) {
        (Run::Text, s)
    }

    /// The whole-paragraph algorithm the reader shipped with at fe79e6c8: split
    /// every word out first, then fill. The streaming `Wrap` must agree with it.
    fn reference(marker: &str, runs: &[(Run, &str)], width: usize) -> Vec<String> {
        let mut words: Vec<String> = Vec::new();
        let mut cur = String::new();
        for (kind, s) in runs {
            let s = crate::sanitize(s, false);
            if *kind == Run::Code {
                cur.push_str(&s);
                continue;
            }
            for c in s.chars() {
                if c == ' ' {
                    if !cur.is_empty() {
                        words.push(core::mem::take(&mut cur));
                    }
                } else {
                    cur.push(c);
                }
            }
        }
        if !cur.is_empty() {
            words.push(cur);
        }
        let indent = marker.chars().count();
        let avail = width.saturating_sub(indent).max(1);
        let pad: String = " ".repeat(indent);
        let mut lines: Vec<String> = Vec::new();
        let mut line = String::from(marker);
        let mut len = 0usize;
        for w in &words {
            let wl = w.chars().count();
            if len > 0 && len + 1 + wl <= avail {
                line.push(' ');
                line.push_str(w);
                len += 1 + wl;
                continue;
            }
            if len > 0 {
                lines.push(core::mem::replace(&mut line, pad.clone()));
            }
            let chars: Vec<char> = w.chars().collect();
            let mut start = 0;
            while chars.len() - start > avail {
                line.extend(&chars[start..start + avail]);
                lines.push(core::mem::replace(&mut line, pad.clone()));
                start += avail;
            }
            line.extend(&chars[start..]);
            len = chars.len() - start;
        }
        lines.push(line);
        lines
    }

    #[test]
    fn a_paragraph_fills_greedily() {
        let runs = [t(
            "The reader wraps a paragraph only on a console whose width it knows.",
        )];
        assert_eq!(
            wrap("", &runs, 20),
            vec![
                "The reader wraps a",
                "paragraph only on a",
                "console whose width",
                "it knows."
            ]
        );
    }

    #[test]
    fn a_list_item_hangs_under_its_marker() {
        let runs = [t("A bulleted item that is longer than one line.")];
        assert_eq!(
            wrap("- ", &runs, 20),
            vec!["- A bulleted item", "  that is longer", "  than one line."]
        );
        assert_eq!(
            wrap("10. ", &runs, 20),
            vec![
                "10. A bulleted item",
                "    that is longer",
                "    than one line."
            ]
        );
    }

    #[test]
    fn a_code_span_does_not_break_at_its_spaces() {
        let runs = [t("Run "), (Run::Code, "manual --check"), t(" first.")];
        assert_eq!(wrap("", &runs, 16), vec!["Run", "manual --check", "first."]);
    }

    #[test]
    fn inline_runs_join_without_spaces_where_the_source_has_none() {
        let runs = [(Run::Code, "/manual"), t("."), t(" Next")];
        assert_eq!(wrap("", &runs, 40), vec!["/manual. Next"]);
    }

    #[test]
    fn a_word_longer_than_the_line_is_split() {
        let runs = [t("see /a/very/long/path/name/here now")];
        assert_eq!(
            wrap("", &runs, 10),
            vec!["see", "/a/very/lo", "ng/path/na", "me/here", "now"]
        );
    }

    #[test]
    fn exact_fits_do_not_leave_an_empty_line() {
        let runs = [t("abcd efgh")];
        assert_eq!(wrap("", &runs, 4), vec!["abcd", "efgh"]);
        assert_eq!(wrap("", &runs, 9), vec!["abcd efgh"]);
    }

    #[test]
    fn repeated_spaces_collapse_and_multibyte_counts_as_one() {
        let runs = [t("caf\u{e9}  na\u{ef}ve   r\u{e9}sum\u{e9}")];
        assert_eq!(
            wrap("", &runs, 11),
            vec!["caf\u{e9} na\u{ef}ve", "r\u{e9}sum\u{e9}"]
        );
    }

    #[test]
    fn a_marker_wider_than_the_line_still_progresses() {
        let runs = [t("ab")];
        assert_eq!(wrap("123. ", &runs, 3), vec!["123. a", "     b"]);
    }

    #[test]
    fn controls_become_replacement_characters() {
        let runs = [t("a\x1bb c"), (Run::Code, "d\x07 e")];
        assert_eq!(wrap("", &runs, 40), vec!["a\u{fffd}b cd\u{fffd} e"]);
    }

    /// The streaming fill agrees with the whole-paragraph reference on every
    /// input a small alphabet of words, spaces, code spans and wide characters
    /// can make, at widths from 0 to 12 and with each marker.
    #[test]
    fn agrees_with_the_whole_paragraph_reference() {
        let pieces: [(Run, &str); 12] = [
            t("a"),
            t("bb"),
            t("ccc "),
            t(" "),
            t("  dd"),
            t("eeeeeeeeeeeeeeeeeee"),
            t("\u{e9}\u{65e5}"),
            (Run::Code, "x y"),
            (Run::Code, ""),
            (Run::Code, "zzzzzzzzzzzzzz"),
            (Run::Emph, "e m"),
            (Run::Strong, "\x01"),
        ];
        let mut seed = 0x2545_f491_4f6c_dd1du64;
        let mut next = move || {
            seed ^= seed << 13;
            seed ^= seed >> 7;
            seed ^= seed << 17;
            seed
        };
        let mut compared = 0usize;
        for _ in 0..20_000 {
            let n = (next() % 9) as usize;
            let runs: Vec<(Run, &str)> = (0..n)
                .map(|_| pieces[(next() % pieces.len() as u64) as usize])
                .collect();
            for marker in ["", "- ", "10. "] {
                for width in 0..=12 {
                    assert_eq!(
                        wrap(marker, &runs, width),
                        reference(marker, &runs, width),
                        "marker {:?} width {} runs {:?}",
                        marker,
                        width,
                        runs
                    );
                    compared += 1;
                }
            }
        }
        assert!(compared > 700_000);
    }
}
