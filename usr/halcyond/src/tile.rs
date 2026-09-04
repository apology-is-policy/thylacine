// A tile's model -- the live grid + the scrollback transcript (HALCYON 14.11.1).
//
// One `Tile` per leaf terminal. It holds two structures, not one (14.11.1):
//
//   - the live grid   -- the current screen, what CellDiff mutates.
//   - the scrollback  -- the existing block/zone `Transcript`, now fed by
//                        ScrollOff (lines that left the top of the grid) and
//                        Beacon frames, not by a raw VT byte stream. It is pure
//                        history: everything that has scrolled off.
//
// They are separate because the grid spans zone boundaries (14.11.1): the last
// `rows` lines routinely straddle a prompt, so the grid cannot be "one zone's
// block". The grid is zone-agnostic; the transcript carries the zone structure.
//
// `apply` is the record -> model dispatch (14.11.2). Record order is load-bearing
// and guaranteed by the producer (a pending CellDiff is flushed before every
// ScrollOff/Control/Mode), so a zone frame lands at the exact point between the
// cells it separates. This module runs NO VT parser and cuts NO zones itself:
// the kaua-term pre-digested the VT, and the Beacon cut rides the SAME
// `Transcript::feed` the console path uses (14.11.4), so the format-fuzz surface
// (parsing an untrusted per-tile stream) is one parser, audited once.

use alloc::string::String;

use crate::grid::Grid;
use crate::transcript::Transcript;
use kaua_term::{Control, Record, ScreenMode};
use vt::Palette;

pub struct Tile {
    pub grid: Grid,
    pub scrollback: Transcript,
    pub mode: ScreenMode,
    /// OSC 0/2 title (the child's own; "" until it sets one).
    pub title: String,
    exit: Option<i32>,
    /// A pending bell affordance the render consumes once (no kernel bell).
    bell: bool,
}

impl Tile {
    pub fn new(cols: usize, rows: usize, pal: Palette) -> Tile {
        Tile {
            grid: Grid::new(cols, rows, pal.fg, pal.bg),
            scrollback: Transcript::new(pal),
            mode: ScreenMode::Normal,
            title: String::new(),
            exit: None,
            bell: false,
        }
    }

    /// The record -> model dispatch (14.11.2).
    pub fn apply(&mut self, rec: Record) {
        match rec {
            Record::CellDiff { changed, cursor } => self.grid.apply_celldiff(&changed, cursor),
            Record::ScrollOff { rows } => self.scrollback.push_scrolled_rows(&rows),
            Record::Control(c) => self.apply_control(c),
            Record::Mode(m) => self.mode = m,
        }
    }

    fn apply_control(&mut self, c: Control) {
        match c {
            // A Beacon frame is the COMPLETE ESC ] 1936 ; ... ST -- feed it to the
            // SAME beacon parser the console path uses; it drives the zone/block
            // cut + span state on the scrollback and touches no cells (14.11.4).
            Control::Osc1936Raw(frame) => self.scrollback.feed(&frame),
            Control::Title(t) => self.title = t,
            Control::Bell => self.bell = true,
            Control::Exit(code) => self.exit = Some(code),
            // The down-channel resize was applied on the pts; no model state here.
            Control::WinsizeAck => {}
        }
    }

    /// Resize the tile (halcyond drives geometry, 14.11.6): the grid reshapes
    /// now; the kaua-term replies with a full CellDiff. The scrollback is
    /// flow-based and reflows at layout, so it takes no dims here.
    pub fn resize(&mut self, cols: usize, rows: usize) {
        self.grid.resize(cols, rows);
    }

    /// `Some(code)` once the hosted child has exited (the teardown trigger,
    /// 14.11.10).
    pub fn exited(&self) -> Option<i32> {
        self.exit
    }

    /// Take + clear the pending bell affordance (the render rings it once).
    pub fn take_bell(&mut self) -> bool {
        core::mem::replace(&mut self.bell, false)
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::transcript::Item;
    use alloc::vec;
    use alloc::vec::Vec;
    use vt::Cell;

    fn cell(ch: char) -> Cell {
        Cell {
            ch,
            fg: 0x00FF00,
            bg: 0,
            attrs: 0,
        }
    }

    fn tile() -> Tile {
        Tile::new(20, 4, vt::BONFIRE)
    }

    #[test]
    fn celldiff_lands_on_the_grid_not_the_scrollback() {
        let mut t = tile();
        t.apply(Record::CellDiff {
            changed: vec![(0, 0, cell('h')), (0, 1, cell('i'))],
            cursor: (0, 2, true),
        });
        assert_eq!(t.grid.row(0)[0].ch, 'h');
        assert_eq!(t.grid.row(0)[1].ch, 'i');
        assert_eq!(t.grid.cursor(), (0, 2, true));
        // the grid is not history: the scrollback's open block stays empty.
        assert!(t.scrollback.open_block().items.is_empty());
    }

    #[test]
    fn scrolloff_appends_rows_to_the_scrollback_as_lines() {
        let mut t = tile();
        t.apply(Record::ScrollOff {
            rows: vec![vec![cell('a'), cell('b')], vec![cell('c')]],
        });
        let items = &t.scrollback.open_block().items;
        assert_eq!(items.len(), 2, "two scrolled rows -> two Line items");
        let line_txt = |it: &Item| -> Vec<char> {
            match it {
                Item::Line(l) => l.cells.iter().map(|c| c.ch).collect(),
                _ => Vec::new(),
            }
        };
        assert_eq!(line_txt(&items[0]), vec!['a', 'b']);
        assert_eq!(line_txt(&items[1]), vec!['c']);
    }

    #[test]
    fn mode_flip_is_recorded() {
        let mut t = tile();
        assert_eq!(t.mode, ScreenMode::Normal);
        t.apply(Record::Mode(ScreenMode::AltScreen));
        assert_eq!(t.mode, ScreenMode::AltScreen);
        t.apply(Record::Mode(ScreenMode::Normal));
        assert_eq!(t.mode, ScreenMode::Normal);
    }

    #[test]
    fn control_records_latch_title_exit_bell() {
        let mut t = tile();
        assert_eq!(t.title, "");
        assert_eq!(t.exited(), None);
        assert!(!t.take_bell());

        t.apply(Record::Control(Control::Title(String::from(
            "edit - foo.rs",
        ))));
        assert_eq!(t.title, "edit - foo.rs");

        t.apply(Record::Control(Control::Bell));
        assert!(t.take_bell(), "bell latched");
        assert!(!t.take_bell(), "bell cleared after one take");

        t.apply(Record::Control(Control::WinsizeAck)); // no-op, must not disturb state
        assert_eq!(t.exited(), None);

        t.apply(Record::Control(Control::Exit(0)));
        assert_eq!(t.exited(), Some(0));
    }

    #[test]
    fn beacon_frame_drives_the_scrollback_zone_cut_not_the_grid() {
        let mut t = tile();
        // an output zone with a cmd mark: the console path's own grammar. This
        // exercises the dispatch (Osc1936Raw -> scrollback.feed), reusing the
        // audited beacon parser; last_command is the observable effect.
        t.apply(Record::Control(Control::Osc1936Raw(
            b"\x1b]1936;v1;zone;k=output\x1b\\".to_vec(),
        )));
        t.apply(Record::Control(Control::Osc1936Raw(
            b"\x1b]1936;v1;mark;k=cmd;text=ls -l\x1b\\".to_vec(),
        )));
        assert_eq!(t.scrollback.last_command(), Some("ls -l"));
        // the grid is untouched by a control frame.
        assert_eq!(t.grid.row(0)[0].ch, ' ');
    }

    #[test]
    fn resize_reshapes_the_grid() {
        let mut t = tile();
        assert_eq!(t.grid.dims(), (20, 4));
        t.resize(40, 10);
        assert_eq!(t.grid.dims(), (40, 10));
    }

    #[test]
    fn record_order_a_zone_after_a_scrolloff_lands_in_the_right_block() {
        // stream order: two output lines scroll off, THEN a prompt zone opens,
        // THEN one more line scrolls off. The first two belong to the old block,
        // the third to the new prompt block (14.11.2 order guarantee).
        let mut t = tile();
        t.apply(Record::ScrollOff {
            rows: vec![vec![cell('1')], vec![cell('2')]],
        });
        assert_eq!(t.scrollback.open_block().items.len(), 2);
        t.apply(Record::Control(Control::Osc1936Raw(
            b"\x1b]1936;v1;zone;k=prompt\x1b\\".to_vec(),
        )));
        // the zone cut froze the old block and opened a fresh one.
        t.apply(Record::ScrollOff {
            rows: vec![vec![cell('3')]],
        });
        assert_eq!(
            t.scrollback.open_block().items.len(),
            1,
            "the post-zone scroll-off is alone in the new block"
        );
    }
}
