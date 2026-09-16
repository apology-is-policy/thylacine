//! Word wrapping at the plain tiers (MANUAL-DESIGN.md 4.3).

use alloc::string::String;
use alloc::vec::Vec;

use crate::format::Inline;
use crate::sanitize;

/// Wrap a paragraph or list item at `width` columns. `marker` begins the first
/// line (empty for a paragraph) and continuation lines are indented by its
/// width. A code span does not break at its own spaces; a word longer than the
/// line is split. Width is counted in Unicode scalar values.
pub fn wrap(marker: &str, runs: &[Inline], width: usize) -> Vec<String> {
    let mut words: Vec<String> = Vec::new();
    let mut cur = String::new();
    for r in runs {
        match r {
            Inline::Code(s) => cur.push_str(&sanitize(s, false)),
            Inline::Text(s) | Inline::Emph(s) | Inline::Strong(s) => {
                for c in sanitize(s, false).chars() {
                    if c == ' ' {
                        if !cur.is_empty() {
                            words.push(core::mem::take(&mut cur));
                        }
                    } else {
                        cur.push(c);
                    }
                }
            }
        }
    }
    if !cur.is_empty() {
        words.push(cur);
    }
    fill(marker, &words, width)
}

fn fill(marker: &str, words: &[String], width: usize) -> Vec<String> {
    let indent = marker.chars().count();
    let avail = width.saturating_sub(indent).max(1);
    let pad: String = " ".repeat(indent);
    let mut lines: Vec<String> = Vec::new();
    let mut line = String::from(marker);
    let mut len = 0usize;
    for w in words {
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

#[cfg(test)]
mod tests {
    use super::*;
    use alloc::vec;

    fn t(s: &str) -> Inline {
        Inline::Text(String::from(s))
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
        let runs = [
            t("Run "),
            Inline::Code(String::from("manual --check")),
            t(" first."),
        ];
        assert_eq!(wrap("", &runs, 16), vec!["Run", "manual --check", "first."]);
    }

    #[test]
    fn inline_runs_join_without_spaces_where_the_source_has_none() {
        let runs = [Inline::Code(String::from("/manual")), t("."), t(" Next")];
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
}
