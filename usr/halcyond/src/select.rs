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

use crate::transcript::{Item, ScrolledRow, Transcript};

/// One selectable row: `block` indexes the frozen deque (usize::MAX = the
/// open block), `item` the block's items, `row` the table row (usize::MAX
/// for a plain line).
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub struct FlatRow {
    pub block: usize,
    pub item: usize,
    pub row: usize,
}

/// H-4d: the `FlatRow.block` of a LIVE GRID row -- the virtual trailing
/// block (HALCYON 14.11.5); `item` is the grid row, `row` is `usize::MAX`.
/// Its runs come from the tile's cell spans (`Tile::grid_runs`), not the
/// transcript.
pub const GRID_BLOCK: usize = usize::MAX - 1;

/// Flatten the transcript's current text rows, oldest first.
pub fn flatten(t: &Transcript) -> Vec<FlatRow> {
    let mut out = Vec::new();
    for (bi, b) in t.frozen_blocks().iter().enumerate() {
        push_block_rows(&mut out, bi, &b.items);
    }
    push_block_rows(&mut out, usize::MAX, &t.open_block().items);
    out
}

/// The transcript's rows followed by `grid_rows` live-grid rows (the
/// virtual trailing block): a tile's Normal mode walks both.
pub fn flatten_with_grid(t: &Transcript, grid_rows: usize) -> Vec<FlatRow> {
    let mut out = flatten(t);
    for r in 0..grid_rows {
        out.push(FlatRow {
            block: GRID_BLOCK,
            item: r,
            row: usize::MAX,
        });
    }
    out
}

/// Bring a Normal-mode flat list and its selection current with the
/// transcript: when it moved since `flat_seq`, or the grid is another height,
/// re-flatten (with `grid_rows` live-grid rows trailing, a tile's shape) and
/// rebase the selection. Both hosts call it before a key acts AND before a
/// frame paints -- a stale list indexes the wrong blocks. Returns the ends
/// the rebase could not follow on the grid (`Sel::rebase`).
pub fn refresh(
    t: &Transcript,
    grid_rows: Option<usize>,
    flat: &mut Vec<FlatRow>,
    flat_seq: &mut u64,
    sel: Option<&mut Sel>,
) -> (bool, bool) {
    let grid_now = flat
        .iter()
        .rev()
        .take_while(|r| r.block == GRID_BLOCK)
        .count();
    if *flat_seq == t.seq && grid_now == grid_rows.unwrap_or(0) {
        return (false, false);
    }
    *flat_seq = t.seq;
    *flat = match grid_rows {
        Some(n) => flatten_with_grid(t, n),
        None => flatten(t),
    };
    match sel {
        Some(s) => s.rebase(t, Stamp::of(t, flat), flat.len()),
        None => (false, false),
    }
}

/// A block's rows: as many per item as `Item::flat_rows` says -- the rule the
/// transcript counts its dropped rows by, so a rebase and a flatten agree. A
/// table addresses its rows; a line, a `pre` (PL-1b: one unit -- its laid
/// lines all carry src_row=MAX, so a mark bands the whole fence) and an
/// inline image (I-47: addressable for the obj-verb menu) are one row each;
/// a rule has none.
fn push_block_rows(out: &mut Vec<FlatRow>, block: usize, items: &[Item]) {
    for (ii, item) in items.iter().enumerate() {
        let table = matches!(item, Item::Table(_));
        for r in 0..item.flat_rows() {
            out.push(FlatRow {
                block,
                item: ii,
                row: if table { r } else { usize::MAX },
            });
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
        // PL-1b: a pre yanks its literal content (lines joined by newlines --
        // the plain realization, verbatim).
        Some(Item::Pre(lines)) => {
            let mut s = String::new();
            for (i, l) in lines.iter().enumerate() {
                if i > 0 {
                    s.push('\n');
                }
                for c in l.cells.iter() {
                    s.push(c.ch);
                }
            }
            s
        }
        _ => String::new(),
    }
}

/// Where a flat list stood when a selection last looked: the transcript's
/// front-drop, grid-leaving, re-cut and repaint counters, and how many
/// leading rows were the transcript's (a tile's live-grid rows follow them).
#[derive(Clone, Copy, Default, PartialEq, Eq, Debug)]
pub struct Stamp {
    pub dropped: u64,
    pub scrolled: u64,
    pub rewraps: u64,
    pub repaints: u64,
    pub hist: usize,
}

impl Stamp {
    pub fn of(t: &Transcript, flat: &[FlatRow]) -> Stamp {
        Stamp {
            dropped: t.rows_dropped(),
            scrolled: t.rows_left(),
            rewraps: t.rewraps(),
            repaints: t.repaints(),
            hist: flat
                .iter()
                .position(|r| r.block == GRID_BLOCK)
                .unwrap_or(flat.len()),
        }
    }
}

/// The selection state over a flat row list. The cursor is a flat index;
/// `anchor` is Some while extending (`v`).
pub struct Sel {
    pub cursor: usize,
    pub anchor: Option<usize>,
    /// H-3c: the selected obj run on the cursor row (its obj index), or
    /// none. Cleared by every row motion; set by `w`/`b` (menu::step_run).
    pub obj: Option<u16>,
    /// The flat list the indices were last current against.
    at: Stamp,
}

impl Sel {
    /// A cursor at flat row `cursor` of the list `at` describes.
    pub fn at(cursor: usize, at: Stamp) -> Sel {
        Sel {
            cursor,
            anchor: None,
            obj: None,
            at,
        }
    }

    /// A fresh cursor at the newest row.
    pub fn at_end(flat_len: usize, at: Stamp) -> Sel {
        Sel::at(flat_len.saturating_sub(1), at)
    }

    /// Follow the transcript to a fresh flatten. The indices are positions.
    /// A transcript row moves only when rows go from the FRONT (the budget,
    /// a forget), up by what went. A live-grid row that stays on the grid
    /// moves up one per row the grid has shown leaving -- at the repaint
    /// that shows it gone, not when the row arrives here -- (down one per
    /// row a resize's repaint brought back); one that left the grid is found
    /// where its text went -- the history row its line joined (a wrapped
    /// line's halves join one row; a clear moves the screen into history), or
    /// the grid's first row, where its line goes on, while the line is held
    /// (a program that rewrites that row first leaves the half a row of its
    /// own, and the selection on the first row, as on any row rewritten in
    /// place). A selection whose row went moves to the oldest row that
    /// remains; one whose grid row left the grid drops its run (a grid run's
    /// key is no obj index). While a re-cut is unsettled, grid rows keep
    /// their places: the rows arriving were cut at other widths, so their
    /// count is no distance, and a run goes at the first repaint. Returns the
    /// ends -- the cursor, the anchor -- on a grid row this cannot follow:
    /// across a re-cut or the repaint that settles it, pushed past the grid's
    /// last row by a repaint (the producer cut that row off), or counted as
    /// gone before its row arrived; the caller starts them again at the
    /// prompt.
    pub fn rebase(&mut self, t: &Transcript, now: Stamp, flat_len: usize) -> (bool, bool) {
        let was = core::mem::replace(&mut self.at, now);
        let recut = was.rewraps != now.rewraps;
        let held = !recut && now.rewraps % 2 == 1;
        let dropped =
            usize::try_from(now.dropped.saturating_sub(was.dropped)).unwrap_or(usize::MAX);
        let scrolled =
            usize::try_from(now.scrolled.saturating_sub(was.scrolled)).unwrap_or(usize::MAX);
        // Rows counted as gone can come back: a resize's repaint settled the
        // rows the producer kept, and every grid row sits that much lower.
        let back =
            usize::try_from(was.scrolled.saturating_sub(now.scrolled)).unwrap_or(usize::MAX);
        // Where row p's text is now (None: its row went), whether its run
        // goes (its row left the grid, or the grid moved under a held row),
        // and whether it is a grid row this cannot follow.
        let follow = |p: usize| -> (Option<usize>, bool, bool) {
            if p < was.hist {
                return (p.checked_sub(dropped), false, false);
            }
            let g = p - was.hist;
            if recut || held {
                let repainted = now.repaints != was.repaints;
                return (Some(now.hist.saturating_add(g)), held && repainted, recut);
            }
            if back > 0 {
                let r = now.hist.saturating_add(g).saturating_add(back);
                return (Some(r), false, r >= flat_len);
            }
            if g >= scrolled {
                return (Some(now.hist + (g - scrolled)), false, false);
            }
            let row = match t.scrolled_row(was.scrolled + g as u64, now.hist) {
                ScrolledRow::History(r) => Some(r),
                ScrolledRow::Held => Some(now.hist),
                // Counted as gone before it arrived: no count says where its
                // text is.
                ScrolledRow::Ahead => return (Some(now.hist), true, true),
                ScrolledRow::Gone => None,
                // Older than the transcript remembers: as if each row it
                // scrolled became one history row.
                ScrolledRow::Unknown => p
                    .saturating_add(now.hist)
                    .checked_sub(was.hist.saturating_add(scrolled)),
            };
            (row, true, false)
        };
        let (c, drop_run, lost_c) = follow(self.cursor);
        match c {
            Some(c) => {
                if drop_run {
                    self.obj = None;
                }
                self.cursor = c;
            }
            None => {
                self.cursor = 0;
                self.obj = None;
            }
        }
        let mut lost_a = false;
        self.anchor = self.anchor.map(|a| {
            let (r, _, lost) = follow(a);
            lost_a = lost;
            r.unwrap_or(0)
        });
        self.clamp(flat_len);
        (lost_c, lost_a)
    }

    /// The live-grid row each end -- the cursor, the anchor -- is on, in the
    /// list the selection was last current against (None: a history row, or
    /// no anchor).
    pub fn grid_rows(&self) -> (Option<usize>, Option<usize>) {
        let on = |p: usize| p.checked_sub(self.at.hist);
        (on(self.cursor), self.anchor.and_then(on))
    }

    /// Put each end `rows` names on that grid row of the list the selection
    /// is current against: a reflow slid the window under the text it is on.
    pub fn slide(&mut self, rows: (Option<usize>, Option<usize>), flat_len: usize) {
        if let Some(g) = rows.0 {
            self.cursor = self.at.hist.saturating_add(g);
        }
        if let (Some(g), Some(a)) = (rows.1, self.anchor.as_mut()) {
            *a = self.at.hist.saturating_add(g);
        }
        self.clamp(flat_len);
    }

    /// Start each end `ends` names again on grid row `crow` of the list the
    /// selection is current against: a reflow took the text such an end was
    /// on off its row.
    pub fn regrid(&mut self, ends: (bool, bool), crow: usize, flat_len: usize) {
        let row = self.at.hist.saturating_add(crow);
        if ends.0 {
            self.cursor = row;
            self.obj = None;
        }
        if ends.1 {
            if let Some(a) = self.anchor.as_mut() {
                *a = row;
            }
        }
        self.clamp(flat_len);
    }

    /// Rows inserted at flat index `at` (the console's placed image, which
    /// freezes in front of the open block's rows): every selected row at or
    /// past it moves down with its content, and the transcript's share of
    /// the list grows by what came.
    pub fn shift_from(&mut self, at: usize, n: usize) {
        if at <= self.at.hist {
            self.at.hist = self.at.hist.saturating_add(n);
        }
        if self.cursor >= at {
            self.cursor = self.cursor.saturating_add(n);
        }
        if let Some(a) = self.anchor.as_mut() {
            if *a >= at {
                *a = a.saturating_add(n);
            }
        }
    }

    /// Clamp into a (possibly shorter) flat list -- a bound, not an
    /// identity: after a front drop only `rebase` keeps the SAME row.
    pub fn clamp(&mut self, flat_len: usize) {
        if flat_len == 0 {
            self.cursor = 0;
            self.anchor = None;
            return;
        }
        if self.cursor >= flat_len {
            self.cursor = flat_len - 1;
            self.obj = None;
        }
        if let Some(a) = self.anchor {
            if a >= flat_len {
                self.anchor = Some(flat_len - 1);
            }
        }
    }

    pub fn mv(&mut self, delta: i32, flat_len: usize) {
        self.obj = None;
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

    fn corpus() -> Transcript {
        let mut t = Transcript::new(libhalcyon::theme::daylight_palette());
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
        let mut sel = Sel::at_end(flat.len(), Stamp::of(&t, &flat));
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
        let mut sel = Sel::at_end(flat_len, Stamp::default());
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
        let mut sel = Sel {
            cursor: 5,
            anchor: Some(2),
            obj: None,
            at: Stamp::default(),
        };
        assert_eq!(sel.range(), (2, 5));
        sel.cursor = 1;
        assert_eq!(sel.range(), (1, 2));
        sel.anchor = None;
        assert_eq!(sel.range(), (1, 1));
    }

    // --- TC-1b: a selection survives rows dropped in front of it -----------

    /// `n` output zones from `from` on, each a two-row table and a line --
    /// three flat rows per zone.
    fn zones(t: &mut Transcript, from: usize, n: usize) {
        for z in from..from + n {
            let mut buf = alloc::vec::Vec::new();
            wire::open(&mut buf, Op::Zone, &[("k", "output")]);
            wire::open(&mut buf, Op::Table, &[("cols", "l")]);
            for r in 0..2 {
                wire::open(&mut buf, Op::Row, &[]);
                wire::open(&mut buf, Op::Cell, &[]);
                buf.extend_from_slice(alloc::format!("z{} r{}", z, r).as_bytes());
                wire::close(&mut buf, Op::Cell);
                wire::close(&mut buf, Op::Row);
                buf.extend_from_slice(b"\n");
            }
            wire::close(&mut buf, Op::Table);
            buf.extend_from_slice(alloc::format!("z{} end\n", z).as_bytes());
            wire::close(&mut buf, Op::Zone);
            t.feed(&buf);
        }
    }

    fn capped(blocks: usize) -> Transcript {
        Transcript::with_caps(
            libhalcyon::theme::daylight_palette(),
            blocks,
            1 << 20,
            10_000,
        )
    }

    #[test]
    fn the_transcript_counts_dropped_rows_as_flatten_counts_them() {
        let mut t = capped(3);
        zones(&mut t, 0, 3);
        let before = flatten(&t).len();
        assert_eq!(before, 9, "three zones of a two-row table and a line");
        zones(&mut t, 3, 2);
        assert_eq!(t.rows_dropped(), 6, "the budget dropped two zones");
        assert_eq!(
            flatten(&t).len(),
            before + 6 - 6,
            "and the flat list lost exactly its front"
        );
        let per_block: usize = t
            .frozen_blocks()
            .iter()
            .map(|b| b.flat_rows())
            .sum::<usize>()
            + t.open_block().flat_rows();
        assert_eq!(per_block, flatten(&t).len(), "one row rule");
    }

    #[test]
    fn a_selection_keeps_its_rows_when_the_budget_evicts_in_front_of_them() {
        let mut t = capped(3);
        zones(&mut t, 0, 3);
        let (mut flat, mut seq) = (Vec::new(), u64::MAX);
        refresh(&t, None, &mut flat, &mut seq, None);
        let mut sel = Sel::at_end(flat.len(), Stamp::of(&t, &flat));
        sel.mv(-2, flat.len());
        sel.toggle_anchor();
        sel.mv(1, flat.len());
        sel.obj = Some(1);
        let yank = sel.yank(&t, &flat);
        assert_eq!(yank, "z2 r0\nz2 r1\n");
        zones(&mut t, 3, 1);
        refresh(&t, None, &mut flat, &mut seq, Some(&mut sel));
        assert_eq!(
            sel.yank(&t, &flat),
            yank,
            "the same rows, not the ones three below"
        );
        assert_eq!(sel.obj, Some(1), "a row that stayed keeps its selected run");
    }

    #[test]
    fn a_selection_whose_row_was_dropped_moves_to_the_oldest_row_left() {
        let mut t = capped(3);
        zones(&mut t, 0, 3);
        let (mut flat, mut seq) = (Vec::new(), u64::MAX);
        refresh(&t, None, &mut flat, &mut seq, None);
        let mut sel = Sel::at(1, Stamp::of(&t, &flat));
        sel.obj = Some(1);
        zones(&mut t, 3, 1);
        refresh(&t, None, &mut flat, &mut seq, Some(&mut sel));
        assert_eq!(sel.cursor, 0);
        assert_eq!(
            row_text(&t, flat[0]),
            "z1 r0",
            "the oldest row that remains"
        );
        assert_eq!(sel.obj, None, "a run on a row that went is not a run here");
    }

    #[test]
    fn a_forget_keeps_a_selected_grid_row_and_moves_a_history_one_to_the_top() {
        let mut t = Transcript::new(libhalcyon::theme::daylight_palette());
        zones(&mut t, 0, 2);
        let (mut flat, mut seq) = (Vec::new(), u64::MAX);
        refresh(&t, Some(4), &mut flat, &mut seq, None);
        assert_eq!(flat.len(), 6 + 4, "four live-grid rows trail six");
        let mut on_grid = Sel::at(flat.len() - 2, Stamp::of(&t, &flat));
        let mut in_history = Sel::at(2, Stamp::of(&t, &flat));
        t.forget(&alloc::collections::BTreeSet::new());
        refresh(&t, Some(4), &mut flat, &mut seq, Some(&mut on_grid));
        in_history.rebase(&t, Stamp::of(&t, &flat), flat.len());
        assert_eq!(flat.len(), 4, "only the live grid remains");
        assert_eq!(
            flat[on_grid.cursor],
            FlatRow {
                block: GRID_BLOCK,
                item: 2,
                row: usize::MAX
            },
            "the same grid row"
        );
        assert_eq!(
            in_history.cursor, 0,
            "a row the forget took: the first row left"
        );
    }

    #[test]
    fn a_placed_image_lands_in_front_of_the_open_rows_and_the_selection_follows() {
        // The console's one mid-list insert: `inject_image` freezes in front
        // of the open block's rows, and the renderer shifts the selection.
        let mut t = Transcript::new(libhalcyon::theme::daylight_palette());
        zones(&mut t, 0, 1);
        let mut buf = alloc::vec::Vec::new();
        wire::open(&mut buf, Op::Zone, &[("k", "output")]);
        buf.extend_from_slice(b"live 1\nlive 2\n");
        t.feed(&buf);
        let (mut flat, mut seq) = (Vec::new(), u64::MAX);
        refresh(&t, None, &mut flat, &mut seq, None);
        let mut sel = Sel::at(4, Stamp::of(&t, &flat));
        assert_eq!(row_text(&t, flat[sel.cursor]), "live 2");
        let at = t
            .frozen_blocks()
            .iter()
            .map(|b| b.flat_rows())
            .sum::<usize>();
        assert!(t.inject_image(1, 1, alloc::vec![0]));
        sel.shift_from(at, 1);
        // More output before the next paint: the newest row is no longer the
        // selected one, so no clamp can land it by luck.
        t.feed(b"live 3\n");
        refresh(&t, None, &mut flat, &mut seq, Some(&mut sel));
        assert_eq!(
            row_text(&t, flat[sel.cursor]),
            "live 2",
            "the selection moved with its row"
        );
    }

    #[test]
    fn shift_from_moves_only_the_rows_at_or_past_the_insert() {
        let mut sel = Sel::at(5, Stamp::default());
        sel.toggle_anchor();
        sel.mv(-3, 10);
        sel.shift_from(4, 1);
        assert_eq!((sel.cursor, sel.anchor), (2, Some(6)));
    }

    fn cells_transcript() -> (Transcript, crate::transcript::SpanMap) {
        let mut t = Transcript::new(libhalcyon::theme::daylight_palette());
        t.set_cells_mode(true);
        (t, crate::transcript::SpanMap::new())
    }

    fn grid_row(text: &str) -> Vec<vt::Cell> {
        text.chars()
            .map(|ch| vt::Cell {
                ch,
                fg: 0,
                bg: 0,
                attrs: 0,
                span: 0,
            })
            .collect()
    }

    const G0: FlatRow = FlatRow {
        block: GRID_BLOCK,
        item: 0,
        row: usize::MAX,
    };

    #[test]
    fn a_grid_row_selection_follows_its_content_through_a_wrapped_scroll() {
        // A history row, then three rows leave a four-row grid: the two halves
        // of a soft-wrapped line, which join as ONE history row, and a plain
        // line.
        let (mut t, spans) = cells_transcript();
        t.push_scrolled_rows(&[grid_row("before")], &[false], &spans);
        t.note_grid_moved();
        let (mut flat, mut seq) = (Vec::new(), u64::MAX);
        refresh(&t, Some(4), &mut flat, &mut seq, None);
        assert_eq!(flat.len(), 1 + 4);
        let at = Stamp::of(&t, &flat);
        let mut head = Sel::at(1, at);
        head.obj = Some(1);
        let mut tail = Sel::at(2, at);
        let mut stays = Sel::at(4, at);
        stays.obj = Some(1);
        t.push_scrolled_rows(
            &[
                grid_row("long line "),
                grid_row("continued"),
                grid_row("plain"),
            ],
            &[true, false, false],
            &spans,
        );
        // Until the producer's repaint shows them gone, the grid still shows
        // the rows that arrived: an end keeps its grid row, and its run.
        refresh(&t, Some(4), &mut flat, &mut seq, Some(&mut head));
        assert_eq!(flat[head.cursor], G0, "the grid shows the first half on row 0 until the repaint");
        assert_eq!(head.obj, Some(1), "on the row it was on");
        t.note_grid_moved();
        refresh(&t, Some(4), &mut flat, &mut seq, Some(&mut head));
        tail.rebase(&t, Stamp::of(&t, &flat), flat.len());
        stays.rebase(&t, Stamp::of(&t, &flat), flat.len());
        assert_eq!(
            flat.len(),
            3 + 4,
            "before, the joined line, plain; then the grid"
        );
        assert_eq!(
            row_text(&t, flat[head.cursor]),
            "long line continued",
            "the first half's line, not the one before it"
        );
        assert_eq!(head.obj, None, "a grid run's key names nothing in history");
        assert_eq!(row_text(&t, flat[tail.cursor]), "long line continued");
        assert_eq!(
            flat[stays.cursor], G0,
            "the same content, now on grid row 0"
        );
        assert_eq!(stays.obj, Some(1), "a grid row that stayed keeps its run");
    }

    #[test]
    fn a_held_half_rides_the_grids_first_row_until_its_line_is_done() {
        let (mut t, spans) = cells_transcript();
        t.push_scrolled_rows(&[grid_row("before")], &[false], &spans);
        t.note_grid_moved();
        let (mut flat, mut seq) = (Vec::new(), u64::MAX);
        refresh(&t, Some(4), &mut flat, &mut seq, None);
        let mut sel = Sel::at(1, Stamp::of(&t, &flat));
        // Grid row 0 leaves as half a line: its line goes on at the new row 0.
        t.push_scrolled_rows(&[grid_row("head ")], &[true], &spans);
        t.note_grid_moved();
        refresh(&t, Some(4), &mut flat, &mut seq, Some(&mut sel));
        assert_eq!(flat.len(), 1 + 4, "a held half is no row yet");
        assert_eq!(flat[sel.cursor], G0, "on the row its line goes on in");
        // Row 0 restarts: the half is a line of its own. No grid row moved,
        // but the list grew, and it must be re-read.
        t.flush_scroll_pending(&spans);
        refresh(&t, Some(4), &mut flat, &mut seq, Some(&mut sel));
        assert_eq!(flat.len(), 2 + 4, "the flushed half is a history row");
        assert_eq!(row_text(&t, flat[1]).trim_end(), "head");
        assert_eq!(flat[sel.cursor], G0, "still on the grid's first row");
    }

    #[test]
    fn a_grid_row_counted_gone_before_it_arrived_is_reported() {
        // A reflow drops two rows no ScrollOff has brought: a count that
        // moved past a row not here yet places no end on it.
        let (mut t, spans) = cells_transcript();
        t.push_scrolled_rows(&[grid_row("before")], &[false], &spans);
        t.note_grid_moved();
        let (mut flat, mut seq) = (Vec::new(), u64::MAX);
        refresh(&t, Some(4), &mut flat, &mut seq, None);
        let mut sel = Sel::at(2, Stamp::of(&t, &flat));
        let g1 = FlatRow {
            block: GRID_BLOCK,
            item: 1,
            row: usize::MAX,
        };
        assert_eq!(flat[sel.cursor], g1, "premise: the cursor is on grid row 1");
        t.note_grid_shed(2);
        let lost = refresh(&t, Some(4), &mut flat, &mut seq, Some(&mut sel));
        assert_eq!(lost, (true, false), "reported, not placed on the grid's first row");
    }

    #[test]
    fn a_budget_reshare_refreshes_the_list() {
        // A new tile shrinks this one's share: every block but the newest goes
        // at once, with no output to announce it.
        let mut t = capped(10);
        zones(&mut t, 0, 3);
        let (mut flat, mut seq) = (Vec::new(), u64::MAX);
        refresh(&t, None, &mut flat, &mut seq, None);
        let mut sel = Sel::at(6, Stamp::of(&t, &flat));
        assert_eq!(row_text(&t, flat[sel.cursor]), "z2 r0");
        t.set_max_cost(1);
        refresh(&t, None, &mut flat, &mut seq, Some(&mut sel));
        assert_eq!(flat.len(), 3, "the newest zone alone");
        assert_eq!(row_text(&t, flat[sel.cursor]), "z2 r0");
    }

    #[test]
    fn refresh_rereads_the_list_when_the_grid_changes_height() {
        let t = Transcript::new(libhalcyon::theme::daylight_palette());
        let (mut flat, mut seq) = (Vec::new(), u64::MAX);
        refresh(&t, Some(4), &mut flat, &mut seq, None);
        assert_eq!(flat.len(), 4);
        refresh(&t, Some(6), &mut flat, &mut seq, None);
        assert_eq!(flat.len(), 6, "no transcript change, but a taller grid");
    }
}
