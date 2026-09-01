// Helix-modal selection, v0 (HALCYON.md section 4: Esc -> normal mode,
// navigate/select/yank anywhere in read-only scrollback, `i` back to the
// writable prompt). LINE-WISE in v0: the flat address space is every
// visible text row -- each Line item, and each TABLE ROW as one row (its
// yank text is the cells joined by two spaces -- the plain realization
// re-derived). Cell/glyph-granular selection over mixed metrics is the
// recorded refinement; the model here already addresses (block, item,
// row), so narrowing to columns later extends rather than replaces.

use alloc::string::String;
use alloc::vec::Vec;

use crate::transcript::{Item, Transcript};

/// One selectable row: `block` indexes the frozen deque (usize::MAX = the
/// open block), `item` the block's items, `row` the table row (usize::MAX
/// for a plain line).
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub struct FlatRow {
    pub block: usize,
    pub item: usize,
    pub row: usize,
}

/// Flatten the transcript's current text rows, oldest first.
pub fn flatten(t: &Transcript) -> Vec<FlatRow> {
    let mut out = Vec::new();
    for (bi, b) in t.frozen_blocks().iter().enumerate() {
        push_block_rows(&mut out, bi, &b.items);
    }
    push_block_rows(&mut out, usize::MAX, &t.open_block().items);
    out
}

fn push_block_rows(out: &mut Vec<FlatRow>, block: usize, items: &[Item]) {
    for (ii, item) in items.iter().enumerate() {
        match item {
            Item::Line(_) => out.push(FlatRow { block, item: ii, row: usize::MAX }),
            Item::Table(tb) => {
                for r in 0..tb.rows.len() {
                    out.push(FlatRow { block, item: ii, row: r });
                }
            }
            Item::Rule => {}
        }
    }
}

/// The row's text (yank currency). A table row re-derives its plain
/// realization: cells joined by two spaces.
pub fn row_text(t: &Transcript, fr: FlatRow) -> String {
    let items: &[Item] = if fr.block == usize::MAX {
        &t.open_block().items
    } else {
        match t.frozen_blocks().get(fr.block) {
            Some(b) => &b.items,
            None => return String::new(),
        }
    };
    match items.get(fr.item) {
        Some(Item::Line(l)) => l.cells.iter().map(|c| c.ch).collect(),
        Some(Item::Table(tb)) => match tb.rows.get(fr.row) {
            Some(row) => {
                let mut s = String::new();
                for (ci, cell) in row.iter().enumerate() {
                    if ci > 0 {
                        s.push_str("  ");
                    }
                    for c in cell.iter() {
                        s.push(c.ch);
                    }
                }
                s
            }
            None => String::new(),
        },
        _ => String::new(),
    }
}

/// The selection state over a flat row list. The cursor is a flat index;
/// `anchor` is Some while extending (`v`).
pub struct Sel {
    pub cursor: usize,
    pub anchor: Option<usize>,
}

impl Sel {
    /// A fresh cursor at the newest row.
    pub fn at_end(flat_len: usize) -> Sel {
        Sel { cursor: flat_len.saturating_sub(1), anchor: None }
    }

    /// Clamp into a (possibly changed) flat list. New output while in
    /// Normal mode grows the list; eviction shrinks it -- the cursor
    /// stays on a valid row either way.
    pub fn clamp(&mut self, flat_len: usize) {
        if flat_len == 0 {
            self.cursor = 0;
            self.anchor = None;
            return;
        }
        if self.cursor >= flat_len {
            self.cursor = flat_len - 1;
        }
        if let Some(a) = self.anchor {
            if a >= flat_len {
                self.anchor = Some(flat_len - 1);
            }
        }
    }

    pub fn mv(&mut self, delta: i32, flat_len: usize) {
        if flat_len == 0 {
            return;
        }
        let c = self.cursor as i64 + delta as i64;
        self.cursor = c.clamp(0, flat_len as i64 - 1) as usize;
    }

    pub fn toggle_anchor(&mut self) {
        self.anchor = match self.anchor {
            Some(_) => None,
            None => Some(self.cursor),
        };
    }

    /// The selected inclusive range (cursor alone when no anchor).
    pub fn range(&self) -> (usize, usize) {
        match self.anchor {
            Some(a) if a <= self.cursor => (a, self.cursor),
            Some(a) => (self.cursor, a),
            None => (self.cursor, self.cursor),
        }
    }

    /// Yank the selected rows' text, newline-joined (+ trailing newline --
    /// line-wise yank pastes as whole lines, the vim/helix convention).
    pub fn yank(&self, t: &Transcript, flat: &[FlatRow]) -> String {
        let (lo, hi) = self.range();
        let mut s = String::new();
        for fr in flat.iter().skip(lo).take(hi.saturating_sub(lo) + 1) {
            s.push_str(&row_text(t, *fr));
            s.push('\n');
        }
        s
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::transcript::Transcript;
    use beacon::wire::{self, Op};
    use vt::THEMES;

    fn corpus() -> Transcript {
        let mut t = Transcript::new(THEMES[1].1);
        let mut buf = alloc::vec::Vec::new();
        wire::open(&mut buf, Op::Zone, &[("k", "prompt")]);
        buf.extend_from_slice(b"$ ls\n");
        wire::close(&mut buf, Op::Zone);
        wire::open(&mut buf, Op::Zone, &[("k", "output")]);
        wire::open(&mut buf, Op::Table, &[("cols", "lr"), ("hdr", "1")]);
        for (a, b2) in [("NAME", "SIZE"), ("version", "42")] {
            wire::open(&mut buf, Op::Row, &[]);
            wire::open(&mut buf, Op::Cell, &[]);
            buf.extend_from_slice(a.as_bytes());
            wire::close(&mut buf, Op::Cell);
            wire::open(&mut buf, Op::Cell, &[]);
            buf.extend_from_slice(b2.as_bytes());
            wire::close(&mut buf, Op::Cell);
            wire::close(&mut buf, Op::Row);
            buf.extend_from_slice(b"\n");
        }
        wire::close(&mut buf, Op::Table);
        buf.extend_from_slice(b"done\n");
        wire::close(&mut buf, Op::Zone);
        t.feed(&buf);
        t
    }

    #[test]
    fn flatten_counts_lines_and_table_rows() {
        let t = corpus();
        let flat = flatten(&t);
        // prompt "$ ls" + 2 table rows + "done" = 4 rows.
        assert_eq!(flat.len(), 4, "{:?}", flat);
        assert_eq!(flat[1].row, 0, "table rows address by row");
        assert_eq!(flat[2].row, 1);
    }

    #[test]
    fn yank_spans_blocks_and_rederives_table_rows() {
        let t = corpus();
        let flat = flatten(&t);
        let mut sel = Sel::at_end(flat.len());
        assert_eq!(sel.cursor, 3);
        sel.mv(-3, flat.len());
        sel.toggle_anchor();
        sel.mv(3, flat.len());
        let y = sel.yank(&t, &flat);
        assert_eq!(y, "$ ls\nNAME  SIZE\nversion  42\ndone\n");
    }

    #[test]
    fn clamp_survives_growth_and_eviction() {
        let flat_len = 4usize;
        let mut sel = Sel::at_end(flat_len);
        sel.toggle_anchor();
        sel.clamp(2);
        assert_eq!(sel.cursor, 1);
        assert_eq!(sel.anchor, Some(1));
        sel.clamp(0);
        assert_eq!(sel.cursor, 0);
        assert_eq!(sel.anchor, None);
        sel.mv(-1, 0);
        assert_eq!(sel.cursor, 0, "empty list is inert");
    }

    #[test]
    fn range_normalizes_direction() {
        let mut sel = Sel { cursor: 5, anchor: Some(2) };
        assert_eq!(sel.range(), (2, 5));
        sel.cursor = 1;
        assert_eq!(sel.range(), (1, 2));
        sel.anchor = None;
        assert_eq!(sel.range(), (1, 1));
    }
}
