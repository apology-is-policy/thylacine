//! Box-drawing furniture for listings (ls -l; future stat). Borders are built
//! to an EXACT visible width so the box aligns over multi-byte names and the
//! zero-width SGR color spans the caller injects (compute width on PLAIN text,
//! color at emit). U+2500 block; the QEMU serial host TTY + Aurora render it.
//!
//! A box is `total` visible columns wide on every line:
//!   top:     ┌─ {title} ─..─ {right} ─┐   (title left, right right-aligned)
//!   content: │ {plain or colored content}{pad} │
//!   bottom:  └─ {label} ─..─┘            (label left)
//! The caller sizes `total` to fit the widest content row + the title + the
//! label (see `fit`), then pads each content row with `pad(total, vis)` spaces.

use alloc::string::String;

pub const TL: char = '┌';
pub const TR: char = '┐';
pub const BL: char = '└';
pub const BR: char = '┘';
pub const H: char = '─';
pub const V: char = '│';

/// `n` horizontal bars.
pub fn rule(n: usize) -> String {
    core::iter::repeat_n(H, n).collect()
}

/// The box width that fits a widest content row of `content_vis` visible cols,
/// a top `title` + `right` label, and a bottom `label` -- each with >= 1 fill.
pub fn fit(content_vis: usize, title: &str, right: &str, label: &str) -> usize {
    let t = title.chars().count();
    let r = right.chars().count();
    let l = label.chars().count();
    let top_min = if r == 0 { t + 7 } else { t + r + 9 };
    let bottom_min = if l == 0 { 4 } else { l + 6 };
    (content_vis + 4).max(top_min).max(bottom_min)
}

/// A top border of total visible width `total`: `┌─ {title} ─..─ {right} ─┐`.
/// `right` empty -> `┌─ {title} ─..─┐`. Fill is >= 1 (the caller sizes via `fit`).
pub fn top(total: usize, title: &str, right: &str) -> String {
    let t = title.chars().count();
    let mut s = String::new();
    s.push(TL);
    s.push(H);
    s.push(' ');
    s.push_str(title);
    s.push(' ');
    if right.is_empty() {
        let fill = total.saturating_sub(t + 6).max(1);
        s.extend(core::iter::repeat_n(H, fill));
    } else {
        let r = right.chars().count();
        let fill = total.saturating_sub(t + r + 8).max(1);
        s.extend(core::iter::repeat_n(H, fill));
        s.push(' ');
        s.push_str(right);
        s.push(' ');
    }
    s.push(H);
    s.push(TR);
    s
}

/// A bottom border of total visible width `total`: `└─ {label} ─..─┘`. An empty
/// label gives a plain `└─..─┘` rule.
pub fn bottom(total: usize, label: &str) -> String {
    let mut s = String::new();
    s.push(BL);
    if label.is_empty() {
        s.extend(core::iter::repeat_n(H, total.saturating_sub(2)));
    } else {
        let l = label.chars().count();
        s.push(H);
        s.push(' ');
        s.push_str(label);
        s.push(' ');
        let fill = total.saturating_sub(l + 5).max(1);
        s.extend(core::iter::repeat_n(H, fill));
    }
    s.push(BR);
    s
}

/// Trailing spaces to pad a content line `│ {content}{pad} │` whose PLAIN
/// content is `vis` visible cols, to total width `total`.
#[inline]
pub fn pad(total: usize, vis: usize) -> usize {
    total.saturating_sub(vis + 4)
}

#[cfg(test)]
mod tests {
    use super::*;

    fn vis(s: &str) -> usize {
        s.chars().count()
    }

    #[test]
    fn rule_is_n_bars() {
        assert_eq!(rule(0), "");
        assert_eq!(vis(&rule(5)), 5);
        assert!(rule(3).chars().all(|c| c == H));
    }

    #[test]
    fn top_with_right_is_exact_width() {
        let total = fit(20, "/home/cora", "5 items", "graft = x");
        let line = top(total, "/home/cora", "5 items");
        assert_eq!(vis(&line), total);
        assert!(line.starts_with('┌'));
        assert!(line.ends_with('┐'));
        assert!(line.contains("/home/cora"));
        assert!(line.contains("5 items"));
    }

    #[test]
    fn top_without_right_is_exact_width() {
        let line = top(30, "title", "");
        assert_eq!(vis(&line), 30);
        assert!(line.ends_with('┐'));
    }

    #[test]
    fn bottom_with_label_is_exact_width() {
        let line = bottom(40, "graft = a live kernel namespace");
        assert_eq!(vis(&line), 40);
        assert!(line.starts_with('└'));
        assert!(line.ends_with('┘'));
    }

    #[test]
    fn bottom_empty_label_is_a_plain_rule() {
        let line = bottom(12, "");
        assert_eq!(vis(&line), 12);
        assert_eq!(line, "└──────────┘");
    }

    #[test]
    fn fit_makes_pad_nonnegative_and_content_fits() {
        // Box sized to a 50-col content row; the row pads to exactly total-4.
        let total = fit(50, "/d", "9 items", "leg");
        assert!(total >= 54);
        assert_eq!(pad(total, 50), total - 54);
        // A row exactly as wide as the inner area pads to zero.
        assert_eq!(pad(total, total - 4), 0);
    }

    #[test]
    fn content_line_aligns_with_the_borders() {
        // The full assembled line (│ + space + content + pad + space + │) is the
        // same visible width as the top border.
        let content = "drwxr-xr-x  system  -  graft  -  proc/";
        let total = fit(vis(content), "/", "1 items", "graft = x");
        let mut line = String::from("│ ");
        line.push_str(content);
        line.extend(core::iter::repeat_n(' ', pad(total, vis(content))));
        line.push_str(" │");
        assert_eq!(vis(&line), total);
        assert_eq!(vis(&top(total, "/", "1 items")), total);
    }
}
