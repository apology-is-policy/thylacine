//! kaua-term -- the Halcyon per-tile terminal (KT-1).
//!
//! Each Halcyon tile hosts one app (native `ut` or a Linux binary) on a pts and
//! runs a kaua-term PROCESS holding the master. The kaua-term is a headless
//! PARSER+producer: it feeds the master bytes through the shared VT parser
//! (`usr/lib/vt`) and turns the parser's state transitions into ONE ordered
//! record stream up to halcyond (HALCYON 14.3, the ratified feed-cells seam).
//! halcyond rasterizes + owns the transcript; the kaua-term never rasterizes,
//! so the hostile-input parser is crash-isolated in this process and halcyond's
//! renderer sees only trusted cells (KAUA-TERM.md 1b).
//!
//! This crate carries the host-testable PRODUCER core (the layer proven like the
//! vt parser -- pure logic, no I/O). The process (mint via `ptyhold`, spawn the
//! app, and the halcyond-owned Loom ring for both channels) lands with the ring
//! seam (KT-1.4), where the bidirectional transport gives it a purpose; the
//! producer here is what it drives.
//!
//! The seam contract (HALCYON 14.3): the record ORDER is load-bearing -- it
//! delimits Beacon zones -- so the producer emits in VT-stream order, flushing a
//! pending CellDiff before every ScrollOff/Control/Mode and at end-of-chunk.

#![no_std]

extern crate alloc;

use alloc::string::String;
use alloc::vec::Vec;
use vt::{Boundary, Cell, Vt};

/// The screen mode a tile is in: normal (ScrollOff feeds the transcript) or the
/// alt screen (a full live grid, no ScrollOff) -- the ?1049/47/1047 flip.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum ScreenMode {
    Normal,
    AltScreen,
}

/// An out-of-band control record (HALCYON 14.3): the events that are not cell
/// content but must arrive in stream order relative to it.
#[derive(Clone, Debug, PartialEq)]
pub enum Control {
    /// A Beacon frame (OSC 1936), re-synthesized as the complete `ESC ] ... ST`
    /// frame that `beacon::wire` parses -- forwarded RAW, uninterpreted, because
    /// halcyond keeps the Beacon parser (and its format-fuzz surface), R5.
    Osc1936Raw(Vec<u8>),
    /// BEL.
    Bell,
    /// OSC 0 / OSC 2 window title.
    Title(String),
    /// The hosted child exited with this code.
    Exit(i32),
    /// A down-channel Resize was applied (winsize set on the pts).
    WinsizeAck,
}

/// One ordered seam record, kaua-term -> halcyond (HALCYON 14.3). Cells are the
/// shared `vt::Cell` (self-contained glyph + inline style); halcyond interns to
/// its own TCell on ingest.
#[derive(Clone, Debug, PartialEq)]
pub enum Record {
    /// The live screen: position-keyed `(row, col, cell)` changes since the last
    /// CellDiff, plus the cursor `(row, col, visible)`. Intra-batch order is
    /// irrelevant (position-keyed); only the boundary order between records is.
    CellDiff {
        changed: Vec<(u16, u16, Cell)>,
        cursor: (u16, u16, bool),
    },
    /// Normal-mode lines that scrolled off the top -> the transcript. Coalesced:
    /// a bulk scroll is one record carrying every row that left, in order.
    ScrollOff { rows: Vec<Vec<Cell>> },
    /// An out-of-band control event.
    Control(Control),
    /// The screen-mode flip (alt-screen enter/leave).
    Mode(ScreenMode),
}

/// Turns a vt byte stream into the ordered seam record stream. Holds a shadow of
/// the last-emitted screen so each CellDiff carries only genuine changes, and a
/// scroll accumulator so consecutive Scroll boundaries coalesce into one
/// ScrollOff. Drive it with a `Vt` whose `set_capture_events(true)` is set.
pub struct Producer {
    shadow: Vec<Cell>,
    cols: usize,
    last_cursor: (u16, u16, bool),
    scroll_acc: Vec<Vec<Cell>>,
}

impl Producer {
    /// Seed the shadow from the vt's current (blank) screen, so the first
    /// CellDiff diffs the first paint against blank.
    pub fn new(vt: &Vt) -> Producer {
        Producer {
            shadow: vt.cells.clone(),
            cols: vt.cols,
            last_cursor: (vt.cy as u16, vt.cx as u16, vt.cursor_visible),
            scroll_acc: Vec::new(),
        }
    }

    /// Process one feed chunk (the caller must have set `vt.set_capture_events(true)`)
    /// and append records to `out` in VT-stream order.
    pub fn feed(&mut self, vt: &mut Vt, bytes: &[u8], out: &mut Vec<Record>) {
        let mut pos = 0;
        while let Some(b) = vt.feed_until(bytes, &mut pos) {
            match b {
                // Coalesce: hold rows until a non-scroll boundary or chunk end.
                Boundary::Scroll(row) => self.scroll_acc.push(row),
                Boundary::Bell => {
                    self.flush(vt, out);
                    out.push(Record::Control(Control::Bell));
                }
                Boundary::Osc(payload) => {
                    self.flush(vt, out);
                    if let Some(c) = classify_osc(&payload) {
                        out.push(Record::Control(c));
                    }
                }
                Boundary::AltEnter(outgoing, (mcx, mcy)) => {
                    // Flush the pre-swap main against the outgoing buffer with the
                    // MAIN's cursor (cells is already the blank alt, cursor homed),
                    // announce the mode, then reset the shadow to the blank alt.
                    self.flush_scroll(out);
                    self.emit_celldiff(&outgoing, mcx, mcy, vt.cursor_visible, out);
                    out.push(Record::Mode(ScreenMode::AltScreen));
                    self.reset_shadow(&vt.cells, vt.cx, vt.cy, vt.cursor_visible);
                }
                Boundary::AltLeave(restored) => {
                    // The alt live grid is discarded; the restored main is
                    // unchanged from before enter, so no diff -- just announce
                    // the mode and reset the shadow to the restored main.
                    self.flush_scroll(out);
                    out.push(Record::Mode(ScreenMode::Normal));
                    self.reset_shadow(&restored, vt.cx, vt.cy, vt.cursor_visible);
                }
            }
        }
        self.flush(vt, out);
    }

    /// After the caller resizes the vt (a down-channel Resize; the compositor is
    /// the geometry authority), resync the shadow to the new geometry and emit a
    /// FULL CellDiff of the resized screen (halcyond already knows the new dims).
    pub fn resized(&mut self, vt: &Vt, out: &mut Vec<Record>) {
        self.flush_scroll(out);
        self.cols = vt.cols;
        let cursor = (vt.cy as u16, vt.cx as u16, vt.cursor_visible);
        let changed = vt
            .cells
            .iter()
            .enumerate()
            .map(|(i, c)| ((i / self.cols) as u16, (i % self.cols) as u16, *c))
            .collect();
        self.shadow = vt.cells.clone();
        self.last_cursor = cursor;
        out.push(Record::CellDiff { changed, cursor });
    }

    fn flush(&mut self, vt: &Vt, out: &mut Vec<Record>) {
        self.flush_scroll(out);
        self.emit_celldiff(&vt.cells, vt.cx, vt.cy, vt.cursor_visible, out);
    }

    fn flush_scroll(&mut self, out: &mut Vec<Record>) {
        if !self.scroll_acc.is_empty() {
            let rows = core::mem::take(&mut self.scroll_acc);
            out.push(Record::ScrollOff { rows });
        }
    }

    // Diff `current` against the shadow; emit a CellDiff iff a cell changed OR
    // the cursor moved, then update the shadow + last cursor. A geometry
    // mismatch (should only happen via resize, which routes through resized())
    // is resynced without emitting garbage.
    fn emit_celldiff(
        &mut self,
        current: &[Cell],
        cx: usize,
        cy: usize,
        vis: bool,
        out: &mut Vec<Record>,
    ) {
        let cursor = (cy as u16, cx as u16, vis);
        if current.len() != self.shadow.len() {
            self.reset_shadow(current, cx, cy, vis);
            return;
        }
        let mut changed = Vec::new();
        for (i, (cur, sh)) in current.iter().zip(self.shadow.iter()).enumerate() {
            if cur != sh {
                changed.push(((i / self.cols) as u16, (i % self.cols) as u16, *cur));
            }
        }
        if changed.is_empty() && cursor == self.last_cursor {
            return;
        }
        if !changed.is_empty() {
            self.shadow.copy_from_slice(current);
        }
        self.last_cursor = cursor;
        out.push(Record::CellDiff { changed, cursor });
    }

    fn reset_shadow(&mut self, to: &[Cell], cx: usize, cy: usize, vis: bool) {
        self.shadow = to.to_vec();
        self.last_cursor = (cy as u16, cx as u16, vis);
    }
}

/// Route a raw OSC payload (the bytes between the introducer and the terminator)
/// to a Control. Titles (OSC 0/2) become Title; Beacon frames (OSC 1936) are
/// re-synthesized as the full `ESC ] <payload> ST` frame for `beacon::wire`;
/// every other OSC is dropped. The vt parser already consumes the 7770 aurora-
/// config channel, so it never reaches here.
fn classify_osc(payload: &[u8]) -> Option<Control> {
    let semi = payload.iter().position(|&b| b == b';')?;
    let (code, rest) = (&payload[..semi], &payload[semi + 1..]);
    match code {
        b"0" | b"2" => core::str::from_utf8(rest).ok().map(|s| Control::Title(String::from(s))),
        b"1936" => {
            let mut f = Vec::with_capacity(payload.len() + 3);
            f.extend_from_slice(b"\x1b]");
            f.extend_from_slice(payload);
            f.extend_from_slice(b"\x1b\\");
            Some(Control::Osc1936Raw(f))
        }
        _ => None,
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use alloc::vec; // the vec! macro (no_std crate; host tests only)

    // Convenience: drive one chunk through a fresh capture-on vt + producer.
    fn produce(cols: usize, rows: usize, chunk: &[u8]) -> Vec<Record> {
        let mut vt = Vt::new(cols, rows);
        vt.set_capture_events(true);
        let mut p = Producer::new(&vt);
        let mut out = Vec::new();
        p.feed(&mut vt, chunk, &mut out);
        out
    }

    fn cell_chars(rec: &Record) -> Vec<(u16, u16, char)> {
        match rec {
            Record::CellDiff { changed, .. } => changed.iter().map(|(r, c, cell)| (*r, *c, cell.ch)).collect(),
            other => panic!("expected CellDiff, got {other:?}"),
        }
    }

    #[test]
    fn first_paint_is_one_celldiff() {
        let recs = produce(6, 1, b"hi");
        assert_eq!(recs.len(), 1);
        assert_eq!(cell_chars(&recs[0]), vec![(0, 0, 'h'), (0, 1, 'i')]);
        match &recs[0] {
            Record::CellDiff { cursor, .. } => assert_eq!(*cursor, (0, 2, true)),
            _ => unreachable!(),
        }
    }

    #[test]
    fn no_change_emits_nothing() {
        let mut vt = Vt::new(6, 1);
        vt.set_capture_events(true);
        let mut p = Producer::new(&vt);
        let mut out = Vec::new();
        p.feed(&mut vt, b"hi", &mut out);
        out.clear();
        // A no-op feed (unknown-but-parsed escape, no cell/cursor change).
        p.feed(&mut vt, b"\x1b[0m", &mut out); // SGR reset, cursor unmoved
        assert!(out.is_empty(), "{out:?}");
    }

    #[test]
    fn bell_flushes_celldiff_and_orders() {
        // "A\x07B" -> CellDiff{A}, Control(Bell), CellDiff{B} -- B after the bell.
        let recs = produce(6, 1, b"A\x07B");
        assert_eq!(recs.len(), 3);
        assert_eq!(cell_chars(&recs[0]), vec![(0, 0, 'A')]);
        assert_eq!(recs[1], Record::Control(Control::Bell));
        assert_eq!(cell_chars(&recs[2]), vec![(0, 1, 'B')]);
    }

    #[test]
    fn title_becomes_a_control() {
        let recs = produce(6, 1, b"\x1b]0;win\x07");
        assert_eq!(recs, vec![Record::Control(Control::Title(String::from("win")))]);
    }

    #[test]
    fn osc1936_reframed_for_beacon() {
        let recs = produce(6, 1, b"\x1b]1936;v1;zone;k=prompt\x1b\\");
        // The full ESC ] ... ST frame, exactly what beacon::wire::parse consumes.
        assert_eq!(
            recs,
            vec![Record::Control(Control::Osc1936Raw(
                b"\x1b]1936;v1;zone;k=prompt\x1b\\".to_vec()
            ))]
        );
    }

    #[test]
    fn scrolloff_carries_the_leaving_line_then_celldiff() {
        // 4x2: fill both rows, park at the bottom, one LF scrolls "top!" off.
        let recs = produce(4, 2, b"\x1b[1;1Htop!\x1b[2;1Hbot!\x1b[2;1H\n");
        // Order: the changes up to the scroll are flushed as CellDiff first (the
        // contract's flush-before-boundary), then ScrollOff, then the final
        // post-scroll CellDiff. Assert a ScrollOff carrying exactly "top!".
        let so = recs
            .iter()
            .find_map(|r| match r {
                Record::ScrollOff { rows } => Some(rows),
                _ => None,
            })
            .expect("a ScrollOff");
        assert_eq!(so.len(), 1);
        let s: String = so[0].iter().map(|c| c.ch).collect();
        assert_eq!(s, "top!");
    }

    #[test]
    fn bulk_scroll_coalesces_into_one_scrolloff() {
        let mut vt = Vt::new(2, 3);
        vt.set_capture_events(true);
        let mut p = Producer::new(&vt);
        let mut out = Vec::new();
        p.feed(&mut vt, b"\x1b[1;1H0\x1b[2;1H1\x1b[3;1H2", &mut out); // rows 0,1,2
        out.clear();
        p.feed(&mut vt, b"\x1b[9S", &mut out); // SU 9, bounded to the 3-row band
        let so = out
            .iter()
            .find_map(|r| match r {
                Record::ScrollOff { rows } => Some(rows),
                _ => None,
            })
            .expect("a ScrollOff");
        let chars: Vec<char> = so.iter().map(|r| r[0].ch).collect();
        assert_eq!(chars, vec!['0', '1', '2']); // one record, all three rows, in order
    }

    #[test]
    fn alt_content_flushes_at_chunk_boundary() {
        // Interactive reality: each render is its own chunk. A chunk that paints
        // the main then enters+paints the alt must emit CellDiff{main},
        // Mode(Alt), CellDiff{alt}; a following chunk that leaves emits
        // Mode(Normal). (The main CellDiff carries the main's real cursor so a
        // later restore is correct -- see alt_enter_preserves_main_cursor.)
        let mut vt = Vt::new(6, 2);
        vt.set_capture_events(true);
        let mut p = Producer::new(&vt);
        let mut out = Vec::new();
        p.feed(&mut vt, b"main\x1b[?1049hX", &mut out); // ends on the alt screen
        let alt_idx = out.iter().position(|r| *r == Record::Mode(ScreenMode::AltScreen)).unwrap();
        // 'main' precedes the mode switch; 'X' (alt content) follows it.
        assert!(out[..alt_idx]
            .iter()
            .any(|r| matches!(r, Record::CellDiff { changed, .. } if changed.iter().any(|(_, _, c)| c.ch == 'm'))));
        assert!(out[alt_idx + 1..]
            .iter()
            .any(|r| matches!(r, Record::CellDiff { changed, .. } if changed.iter().any(|(_, _, c)| c.ch == 'X'))));
        out.clear();
        p.feed(&mut vt, b"\x1b[?1049l", &mut out);
        assert!(out.contains(&Record::Mode(ScreenMode::Normal)));
    }

    #[test]
    fn same_chunk_alt_enter_and_leave_discards_transient_content() {
        // enter+paint+leave in ONE chunk: the alt content is transient (a real
        // terminal shows nothing), so the only records are the two mode flips.
        let recs = produce(6, 2, b"main\x1b[?1049hX\x1b[?1049l");
        let modes: Vec<&Record> = recs.iter().filter(|r| matches!(r, Record::Mode(_))).collect();
        assert_eq!(
            modes,
            vec![&Record::Mode(ScreenMode::AltScreen), &Record::Mode(ScreenMode::Normal)]
        );
        // No CellDiff carries the transient 'X'.
        assert!(!recs
            .iter()
            .any(|r| matches!(r, Record::CellDiff { changed, .. } if changed.iter().any(|(_, _, c)| c.ch == 'X'))));
    }

    #[test]
    fn alt_enter_preserves_main_cursor() {
        // The main CellDiff flushed at alt-enter must carry the MAIN cursor
        // (col 4 after "main"), not the homed alt cursor (0,0), so halcyond can
        // restore it on alt-leave. Regression for the post-home-cursor bug.
        let mut vt = Vt::new(6, 2);
        vt.set_capture_events(true);
        let mut p = Producer::new(&vt);
        let mut out = Vec::new();
        p.feed(&mut vt, b"main\x1b[?1049h", &mut out);
        let alt_idx = out.iter().position(|r| *r == Record::Mode(ScreenMode::AltScreen)).unwrap();
        // The last CellDiff before the mode switch is the outgoing main.
        let main_cd = out[..alt_idx]
            .iter()
            .rev()
            .find(|r| matches!(r, Record::CellDiff { .. }))
            .expect("a main CellDiff before the alt switch");
        match main_cd {
            Record::CellDiff { cursor, .. } => assert_eq!(*cursor, (0, 4, true)), // (row 0, col 4)
            _ => unreachable!(),
        }
    }

    #[test]
    fn alt_screen_does_not_emit_scrolloff() {
        // On the alt screen a full scroll must NOT feed the transcript.
        let mut vt = Vt::new(2, 2);
        vt.set_capture_events(true);
        let mut p = Producer::new(&vt);
        let mut out = Vec::new();
        p.feed(&mut vt, b"\x1b[?1049h", &mut out); // enter alt
        out.clear();
        p.feed(&mut vt, b"\x1b[2;1Ha\nb\nc", &mut out); // scroll on the alt screen
        assert!(
            out.iter().all(|r| !matches!(r, Record::ScrollOff { .. })),
            "alt screen must not ScrollOff: {out:?}"
        );
    }

    #[test]
    fn resized_emits_full_redraw() {
        let mut vt = Vt::new(3, 2);
        vt.set_capture_events(true);
        let mut p = Producer::new(&vt);
        let mut out = Vec::new();
        p.feed(&mut vt, b"ab", &mut out);
        out.clear();
        vt.resize(4, 3); // the compositor grew the tile
        p.resized(&vt, &mut out);
        assert_eq!(out.len(), 1);
        match &out[0] {
            Record::CellDiff { changed, .. } => assert_eq!(changed.len(), 12), // full 4x3 grid
            other => panic!("expected a full CellDiff, got {other:?}"),
        }
    }

    #[test]
    fn cursor_only_move_still_emits() {
        let mut vt = Vt::new(6, 2);
        vt.set_capture_events(true);
        let mut p = Producer::new(&vt);
        let mut out = Vec::new();
        p.feed(&mut vt, b"hi", &mut out);
        out.clear();
        p.feed(&mut vt, b"\x1b[2;3H", &mut out); // move cursor, no cell change
        assert_eq!(out.len(), 1);
        match &out[0] {
            Record::CellDiff { changed, cursor } => {
                assert!(changed.is_empty());
                assert_eq!(*cursor, (1, 2, true));
            }
            other => panic!("expected a cursor-only CellDiff, got {other:?}"),
        }
    }
}
