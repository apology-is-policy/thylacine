// nora::wrap -- soft-wrap visual-line math (pure, host-testable).
//
// With wrap ON, a logical line of L characters occupies ceil(L/tw) visual rows
// at text width `tw` (>= 1); a blank line is one visual row. The editor scrolls
// and maps the cursor in VISUAL rows (not logical lines) so a long wrapped line
// can never push the cursor off screen -- the (top_row, top_sub) anchor and the
// cursor's (row, sub) are both visual positions. These helpers are the shared
// walk used by `editor::scroll_to` (the scroll anchor) and `view` (the render +
// cursor mapping); keeping them in one place keeps scroll and render agreeing on
// the geometry (a disagreement would desync the on-screen cursor).
//
// A "visual position" is `(row, sub)` with `sub` in `[0, line_rows(row)-1]`; the
// chars shown on that row are `[sub*tw, sub*tw + tw)`.

use crate::text::TextBuffer;

/// Visual rows a line of `chars` characters occupies at width `tw`. `tw` is
/// clamped to >= 1; a zero-length line is one visual row.
pub fn line_rows(chars: usize, tw: u16) -> usize {
    let tw = tw.max(1) as usize;
    if chars == 0 {
        1
    } else {
        (chars + tw - 1) / tw
    }
}

/// Visual rows of logical line `row`.
pub fn row_rows(text: &TextBuffer, row: usize, tw: u16) -> usize {
    line_rows(text.char_len(row), tw)
}

/// The cursor's visual position `(row, sub)`. A cursor at `col == char_len` of a
/// line whose length is an exact multiple of `tw` clamps to the last sub-row
/// (so the end-of-full-line cursor shows at the right edge, not on a phantom
/// extra row).
pub fn cursor_visual(text: &TextBuffer, tw: u16) -> (usize, usize) {
    let (row, col) = text.cursor();
    let twc = tw.max(1) as usize;
    let rows = row_rows(text, row, tw);
    let sub = (col / twc).min(rows - 1);
    (row, sub)
}

/// The visual position one row after `pos`, or `None` at end of document.
pub fn forward(text: &TextBuffer, pos: (usize, usize), tw: u16) -> Option<(usize, usize)> {
    let (row, sub) = pos;
    if sub + 1 < row_rows(text, row, tw) {
        Some((row, sub + 1))
    } else if row + 1 < text.line_count() {
        Some((row + 1, 0))
    } else {
        None
    }
}

/// The visual position one row before `pos`, or `None` at start of document.
pub fn backward(text: &TextBuffer, pos: (usize, usize), tw: u16) -> Option<(usize, usize)> {
    let (row, sub) = pos;
    if sub > 0 {
        Some((row, sub - 1))
    } else if row > 0 {
        Some((row - 1, row_rows(text, row - 1, tw) - 1))
    } else {
        None
    }
}

/// Walk `n` visual rows backward from `pos`, clamping at the document start.
pub fn back_n(text: &TextBuffer, pos: (usize, usize), n: usize, tw: u16) -> (usize, usize) {
    let mut p = pos;
    for _ in 0..n {
        match backward(text, p, tw) {
            Some(q) => p = q,
            None => break,
        }
    }
    p
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn line_rows_is_ceil_div_with_blank_as_one() {
        assert_eq!(line_rows(0, 10), 1);
        assert_eq!(line_rows(1, 10), 1);
        assert_eq!(line_rows(10, 10), 1);
        assert_eq!(line_rows(11, 10), 2);
        assert_eq!(line_rows(20, 10), 2);
        assert_eq!(line_rows(21, 10), 3);
        // tw clamps to >= 1 (never divides by zero).
        assert_eq!(line_rows(5, 0), 5);
    }

    #[test]
    fn cursor_visual_clamps_full_line_end_to_last_subrow() {
        // line "0123456789" (10 chars), tw 5 -> 2 visual rows.
        let mut t = TextBuffer::new("0123456789");
        t.set_cursor(0, 3);
        assert_eq!(cursor_visual(&t, 5), (0, 0)); // col 3 -> sub 0
        t.set_cursor(0, 7);
        assert_eq!(cursor_visual(&t, 5), (0, 1)); // col 7 -> sub 1
        t.set_cursor(0, 10); // end of an exactly-2-row line
        assert_eq!(cursor_visual(&t, 5), (0, 1)); // clamps to last sub, not sub 2
    }

    #[test]
    fn forward_and_backward_walk_visual_rows() {
        // line 0: 12 chars -> 2 rows at tw 10; line 1: 3 chars -> 1 row.
        let t = TextBuffer::new("abcdefghijkl\nxyz");
        assert_eq!(forward(&t, (0, 0), 10), Some((0, 1)));
        assert_eq!(forward(&t, (0, 1), 10), Some((1, 0))); // crosses to line 1
        assert_eq!(forward(&t, (1, 0), 10), None); // end of document
        assert_eq!(backward(&t, (1, 0), 10), Some((0, 1)));
        assert_eq!(backward(&t, (0, 1), 10), Some((0, 0)));
        assert_eq!(backward(&t, (0, 0), 10), None); // start
    }

    #[test]
    fn back_n_clamps_at_start() {
        let t = TextBuffer::new("abcdefghijkl\nxyz"); // (0,0)(0,1)(1,0)
        assert_eq!(back_n(&t, (1, 0), 1, 10), (0, 1));
        assert_eq!(back_n(&t, (1, 0), 5, 10), (0, 0)); // clamps, no underflow
    }
}
