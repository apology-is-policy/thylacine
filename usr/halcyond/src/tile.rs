#[cfg(test)]
use crate::layout::layout_block;
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

use alloc::collections::{BTreeSet, VecDeque};
use alloc::string::String;
use alloc::vec::Vec;

use crate::grid::Grid;
use crate::layout::{
    block_gap_between, caret_in_block, laid_line_for, layout_block_media, render_block, LaidBlock,
    LaidLine, Sheet,
};
use crate::menu::{run_rect, ObjRun};
use crate::raster::{GlyphSource, FACE_BODY, FACE_MONO};
use crate::transcript::{
    BlockKind, SpanMap, SpanTag, Transcript, DEFAULT_MAX_BLOCKS, DEFAULT_MAX_COST,
    DEFAULT_MAX_LINES_PER_BLOCK,
};
use cartoon::{Cartoon, Op};
use kaua_term::{Control, Record, ScreenMode};
use vt::{Palette, ATTR_ITALIC, ATTR_REVERSE, ATTR_UNDERLINE};

/// PL-4b-ii: a render's cached proportional live tail -- the laid live block,
/// the per-grid-row provenance `(item, row within the item, start column)`
/// (the row is `usize::MAX` for a plain line; a rebuilt table's rows and a
/// pre's lines share one item and differ by row), and the tail's screen-y --
/// that a click inverts through (`Tile::live_laid`).
type LiveLaid = (LaidBlock, Vec<(usize, usize, usize)>, i32);

pub struct Tile {
    pub grid: Grid,
    pub media: crate::inlinecache::InlineCache,
    pub scrollback: Transcript,
    pub mode: ScreenMode,
    /// OSC 0/2 title (the child's own; "" until it sets one).
    pub title: String,
    /// HALCYON-INSTRUMENT 14.6: what became of the tile's process. `Live`
    /// paints as always; a retained tile paints no caret and, under the
    /// Instrument profile, its state's body mark -- the `Process ended`
    /// line, the `Connection lost` strip. Set by the session (the bin
    /// judges the stream); the legacy profile's frozen affordance is
    /// unchanged by it.
    pub fate: crate::chrome::Fate,
    /// HALCYON-INSTRUMENT section 10: the caret's blink step, as the LAST
    /// paint used it. `steps(2, start)` is two-valued, so this is the whole
    /// of the caret's animation state -- there is no per-caret origin,
    /// because a CSS animation with no restart trigger is free-running and
    /// the phase comes off the monotonic clock alone.
    ///
    /// TRUE is the resting value, and deliberately so: under section 9.5's
    /// reduced motion the caret is STATIC, which means painted, not absent.
    /// A tile nobody drives a clock into therefore paints exactly as it did
    /// before this field existed. Written by the session through
    /// [`Tile::set_caret_on`]; the console renderer paints no caret at all.
    pub caret_on: bool,
    exit: Option<i32>,
    /// A pending bell affordance the render consumes once (no kernel bell).
    bell: bool,
    /// HALCYON 14.13: a clear (ED 2 / ED 3 / RIS) erased the normal screen
    /// and nothing has scrolled off since. With history, the render lays the
    /// tail under its own top padding with the history ending at the view's
    /// top edge and a full view below it. The normal screen's, so an
    /// alt-screen excursion leaves it as it was.
    pinned: bool,
    /// The producer acknowledged a resize: the CellDiff after the ack is its
    /// repaint of the whole grid at the dims it applied.
    resize_acked: bool,
    /// The screen mode flipped and the producer's repaint of the screen it
    /// flipped to has not landed: the grid still shows the other one.
    screen_pending: bool,
    /// The frozen blocks' laid heights at `heights_width`, aligned to the
    /// scrollback's frozen deque (front-evicted, back-appended; block ids are
    /// strictly increasing along it). A frozen block's layout is width- and
    /// content-deterministic, so a few bytes per block are enough to position
    /// every block without laying it out -- a render then lays out ONLY the
    /// blocks that intersect the view. The old whole-history layout was a
    /// transient of ~1.8x the retained bytes, outside every budget: one tile
    /// with ~20K rows of history ended the whole session at its next paint.
    /// The entry carries the block's `exit` too: it is the ONE field a frozen
    /// block can still acquire (an exit mark floating in right after its
    /// zone closed lands on the last frozen block), and a non-zero code adds
    /// the badge line -- a height cached before it would misplace every block
    /// below. Any new post-freeze mutation must join this key.
    heights: VecDeque<(u64, Option<i64>, i32)>,
    heights_width: i32,
    /// H-4d: the last render's block placement -- (block id, or u64::MAX for
    /// the open block; screen y; height) for EVERY block, in transcript
    /// order -- the hit map for a click on an obj run and the anchor for the
    /// keyboard menu (the console's `frame`).
    pub frame: Vec<(u64, i32, i32)>,
    /// H-4d: serial -> span state, noted after every Beacon frame fed; the
    /// grid's cells and the scrolled-off rows resolve their spans through it.
    pub spans: SpanMap,
    /// Blocks laid out by the last `render` (the window's witness).
    pub laid_last: usize,
    /// Visual lines laid out by the last `render` (the transient's witness:
    /// bounded by the view plus the two whole blocks, never the history).
    pub laid_lines_last: usize,
    /// PL-4b-ii: the last proportional render's live tail -- (the laid live
    /// block, the per-grid-row provenance, the tail's screen-y) -- so a click
    /// on the tail (`grid_hit` / `grid_run_rect`) inverts through the SAME
    /// geometry the render painted, not the mono cell grid. None while the
    /// grid holds an app's frame (the tail is the mono `paint_grid`, hit by
    /// cell: the alt screen, and its last frame until the normal screen's
    /// repaint lands) and before the first render. Rebuilt every render
    /// (O(grid), never the history), so
    /// a stale cache never outlives one frame; a click uses the last frame's
    /// layout exactly as the block `frame` does.
    live_laid: Option<LiveLaid>,
    /// 7.7: whether the last render reserved the indicator's lane (the
    /// content overflowed the view). The layout width follows it, and a
    /// flip re-lays once in the frame that sees it.
    lane: bool,
    /// The lane passes the last render took (bounded at three; r2 B-F1).
    lane_passes: u8,
    /// HALCYON 14.3: the program's synchronized frame (DEC mode 2026) and
    /// the bound on how long it may hold this tile's paint. The records
    /// open and close it; the session asks it before painting.
    pub hold: vt::FrameHold,
}

impl Tile {
    /// HALCYON-INSTRUMENT 9.4 (I-7): re-theme this tile in place on a live
    /// theme change -- the live grid and the retained scrollback both remap
    /// their cell colours from `old` to `new` (`vt::remap_color`). The
    /// session ALSO tells the tile's pts host (`Input::Palette`), whose own
    /// re-emit will overwrite the live grid; this remap closes the window
    /// until that arrives and is the console path's only re-theme. A repaint
    /// is the caller's (`dirty`).
    pub fn set_palette(&mut self, old: Palette, new: Palette) {
        self.grid.remap_palette(old, new);
        self.scrollback.remap_palette(old, new);
    }

    pub fn new(cols: usize, rows: usize, pal: Palette) -> Tile {
        Tile::with_budget(cols, rows, pal, DEFAULT_MAX_COST)
    }

    /// A tile whose scrollback holds at most `max_cost` bytes -- a session's
    /// tiles share ONE budget (their sum, not each, must fit the heap).
    pub fn with_budget(cols: usize, rows: usize, pal: Palette, max_cost: usize) -> Tile {
        Tile {
            grid: Grid::new(cols, rows, pal.fg, pal.bg),
            scrollback: {
                // A tile's transcript is FRAME-fed (the text is grid cells):
                // structure is rebuilt from the tagged cells.
                let mut t = Transcript::with_caps(
                    pal,
                    DEFAULT_MAX_BLOCKS,
                    max_cost / 2,
                    DEFAULT_MAX_LINES_PER_BLOCK,
                );
                t.set_cells_mode(true);
                t
            },
            media: crate::inlinecache::InlineCache::new(max_cost / 2),
            mode: ScreenMode::Normal,
            title: String::new(),
            fate: crate::chrome::Fate::Live,
            caret_on: true,
            exit: None,
            bell: false,
            pinned: false,
            resize_acked: false,
            screen_pending: false,
            heights: VecDeque::new(),
            heights_width: 0,
            frame: Vec::new(),
            spans: SpanMap::new(),
            laid_last: 0,
            laid_lines_last: 0,
            live_laid: None,
            lane: false,
            lane_passes: 0,
            hold: vt::FrameHold::default(),
        }
    }

    /// H-4d: the block under screen `y` in the last render, with its y:
    /// the click hit map. None on the grid tail or the gaps.
    pub fn hit(&self, y: i32) -> Option<(u64, i32)> {
        self.frame
            .iter()
            .find(|f| y >= f.1 && y < f.1 + f.2)
            .map(|f| (f.0, f.1))
    }

    /// H-4d: the span a grid cell was written under (None: no obj).
    fn grid_tag(&self, r: usize, c: usize) -> Option<SpanTag> {
        let cell = *self.grid.row(r).get(c)?;
        let t = self.spans.get(cell.span)?;
        if t.obj == 0 {
            None
        } else {
            Some(t)
        }
    }

    /// The obj runs on live-grid row `r`, keyed by their start column + 1
    /// (a row-unique u16, the grid's analogue of a block's obj index): the
    /// virtual trailing block's `runs_on_row`.
    pub fn grid_runs(&self, r: usize) -> Vec<ObjRun> {
        let row = self.grid.row(r);
        let mut runs = Vec::new();
        let mut c = 0;
        while c < row.len() {
            match self.grid_tag(r, c) {
                None => c += 1,
                Some(t) => {
                    let start = c;
                    let mut text = String::new();
                    while c < row.len() && self.grid_tag(r, c) == Some(t) {
                        text.push(row[c].ch);
                        c += 1;
                    }
                    runs.push(ObjRun {
                        obj: (start as u16).saturating_add(1),
                        text,
                    });
                }
            }
        }
        runs
    }

    /// A grid run by key: (start col, cols, its span).
    pub fn grid_run(&self, r: usize, key: u16) -> Option<(usize, usize, SpanTag)> {
        let start = (key as usize).checked_sub(1)?;
        let t = self.grid_tag(r, start)?;
        let row = self.grid.row(r);
        let mut c = start;
        while c < row.len() && self.grid_tag(r, c) == Some(t) {
            c += 1;
        }
        Some((start, c - start, t))
    }

    /// The (type, resolved ref) a grid run presents.
    pub fn grid_run_obj(&self, r: usize, key: u16) -> Option<(&str, &str)> {
        let (_, _, t) = self.grid_run(r, key)?;
        self.scrollback.obj_in_block(t.block, t.obj)
    }

    /// The run key (start col + 1) for the obj at grid cell (r, col), walking
    /// left to the run's start; None if the cell carries no obj.
    fn run_key_at(&self, r: usize, col: usize) -> Option<u16> {
        let t = self.grid_tag(r, col)?;
        let mut start = col;
        while start > 0 && self.grid_tag(r, start - 1) == Some(t) {
            start -= 1;
        }
        Some((start as u16).saturating_add(1))
    }

    /// The obj run under a tail-relative point: (grid row, run key). While the
    /// grid holds a normal-screen frame the tail is proportional (PL-4b), so
    /// the click inverts through the last render's cached layout -- `x`/`y`
    /// relative to the tail's top-left (the caller subtracts the tail's
    /// screen-y): the laid line under `y`, its logical column under `x`, then
    /// the `prov` inverse back to the grid (row, col). While it holds an app's
    /// frame there is no cache, so it falls back to the mono cell grid (`cw` x
    /// `ch`), the geometry `paint_grid` uses.
    pub fn grid_hit(&self, x: i32, y: i32, cw: i32, ch: i32) -> Option<(usize, u16)> {
        if x < 0 || y < 0 {
            return None;
        }
        match &self.live_laid {
            Some((live_lb, prov, _)) => {
                let line = live_lb.lines.iter().find(|l| y >= l.y && y < l.y + l.h)?;
                let col = col_at_x(line, x)?;
                let cols = self.grid.dims().0;
                let (r, rc) = prov_inverse(prov, line.src_item, line.src_row, col, cols)?;
                self.run_key_at(r, rc).map(|k| (r, k))
            }
            None => {
                if cw <= 0 || ch <= 0 {
                    return None;
                }
                let (r, c) = ((y / ch) as usize, (x / cw) as usize);
                self.run_key_at(r, c).map(|k| (r, k))
            }
        }
    }

    /// The tail-relative display rect (x, y, w, h) of grid run (r, key): the
    /// proportional x-extent + laid-line y/h from the cached layout, or the
    /// mono cell rect (`cw` x `ch`) while the grid holds an app's frame /
    /// before a render. A soft-wrapped run reports its FIRST laid piece (this rect only
    /// rides the menu-witness say line; the menu anchors at the pointer). None
    /// when the cell carries no run.
    pub fn grid_run_rect(&self, r: usize, key: u16, cw: i32, ch: i32) -> Option<(i32, i32, i32, i32)> {
        let (c0, n, _) = self.grid_run(r, key)?;
        match &self.live_laid {
            Some((live_lb, prov, _)) => {
                let &(item, row, start) = prov.get(r)?;
                let (a, b) = (start + c0, start + c0 + n);
                for line in live_lb.lines.iter() {
                    if line.src_item != item || line.src_row != row {
                        continue;
                    }
                    let lo = line.segs.first().map(|s| s.src_col).unwrap_or(0);
                    let hi = line
                        .segs
                        .last()
                        .map(|s| s.src_col + s.refs.len())
                        .unwrap_or(lo);
                    let (aa, bb) = (a.max(lo), b.min(hi));
                    if aa >= bb {
                        continue;
                    }
                    let x0 = line_col_x(line, aa);
                    let x1 = line_col_x(line, bb);
                    return Some((x0, line.y, (x1 - x0).max(1), line.h));
                }
                None
            }
            None => Some((c0 as i32 * cw, r as i32 * ch, (n as i32 * cw).max(1), ch)),
        }
    }

    /// Text and raster retention split one per-pane allowance; a quota change
    /// invalidates all height entries whose inline references may have expired.
    pub fn set_content_budget(&mut self, bytes: usize) {
        self.scrollback.set_max_cost(bytes / 2);
        if self.media.set_limit(bytes / 2) { self.heights.clear(); }
    }

    /// TC-1b (HALCYON 14.13): the user's Super+K -- forget this tile's
    /// history and nothing else. What the span ring names survives as husks
    /// -- a superset of what the live cells resolve, as it must be: a diff in
    /// flight and a hidden main screen carry older serials -- and an inline
    /// image stays while an object that survived names it. The pin goes with
    /// the history it pinned against, and the last frame's placements with
    /// the blocks they placed.
    pub fn forget_history(&mut self) {
        let named = self.spans.named();
        // An image no object names yet is still to be captioned (`view`
        // uploads first): it is not history, so it stays.
        let captioned: BTreeSet<u128> = self
            .scrollback
            .objs()
            .filter_map(|(ty, refv)| crate::inlinecache::image_id(ty, refv))
            .collect();
        self.scrollback.forget(&named);
        let keep: BTreeSet<u128> = self
            .scrollback
            .objs()
            .filter_map(|(ty, refv)| crate::inlinecache::image_id(ty, refv))
            .collect();
        self.media
            .retain(|id| keep.contains(&id) || !captioned.contains(&id));
        self.pinned = false;
        self.heights.clear();
        self.frame.clear();
    }

    pub fn place_image(&mut self, id: u128, w: u32, h: u32, argb: Vec<u32>) -> bool {
        if !self.media.insert(id, w, h, argb) { return false; }
        self.heights.clear();
        self.scrollback.seq = self.scrollback.seq.wrapping_add(1);
        true
    }

    /// The record -> model dispatch (14.11.2).
    pub fn apply(&mut self, rec: Record) {
        match rec {
            Record::CellDiff {
                changed,
                cursor,
                wrapped,
                top_continues,
            } => {
                // A cell written after the open `rule` frame (its serial at
                // or past the rule's) is the line the rule precedes: the
                // episode ends here, not at an op (transcript::end_rule). A
                // scroll re-reports older cells with their OLD serials, so
                // it does not end it; a bare newline changes no cell.
                if let Some(rule) = self.scrollback.rule_open() {
                    if changed.iter().any(|(_, _, c)| c.span >= rule && c.span != 0) {
                        self.scrollback.end_rule();
                    }
                }
                self.grid.apply_celldiff(&changed, cursor, &wrapped, top_continues);
                self.screen_pending = false;
                // The producer's reply to a resize -- the ack, then every
                // cell of its grid -- at the dims the grid has now: the grid
                // is the producer's cut again. A reply to an earlier resize
                // (other dims) settles nothing, and one at another width
                // re-cut the producer's lines where the grid does not show
                // it: the ack names no resize, so a reply at these dims may
                // have come before it, from a resize the grid went back to.
                // Either screen: the producer re-cuts its main screen
                // beneath the alt screen too.
                let reply = core::mem::take(&mut self.resize_acked);
                let (cols, rows) = self.grid.dims();
                if reply && wrapped.len() == rows && changed.len() == cols * rows {
                    self.scrollback.settle_grid_shed();
                } else if reply && changed.len() != cols * wrapped.len() {
                    self.scrollback.note_grid_recut();
                }
                // The scrolled-off fragment the transcript holds continues
                // into row 0 only while the producer says so; the moment it
                // does not (row 0 restarted as a line of its own), the
                // fragment is complete as it stands and lands as a line --
                // never glued to whatever row scrolls off next. The alt
                // screen reports false throughout (it has no history) but
                // the main screen returns intact at alt-leave, so a fragment
                // rides out a TUI session and rejoins its row then.
                if !top_continues && self.mode == ScreenMode::Normal {
                    self.scrollback.flush_scroll_pending(&self.spans);
                }
                // The rows that scrolled off before this repaint are gone
                // from the grid only now: a selection on one stayed on it.
                self.scrollback.note_grid_moved();
            }
            Record::ScrollOff { rows, wrapped } => {
                // Output has filled the screen: the view flows again.
                self.pinned = false;
                self.scrollback
                    .push_scrolled_rows(&rows, &wrapped, &self.spans)
            }
            Record::Control(c) => self.apply_control(c),
            // The producer's top flag on the next normal-screen CellDiff says
            // whether a held soft-wrapped fragment still has its continuation
            // (PL-3); the mode flip itself decides nothing.
            Record::Mode(m) => {
                self.screen_pending |= m != self.mode;
                self.mode = m;
            }
        }
    }

    fn apply_control(&mut self, c: Control) {
        match c {
            // A Beacon frame is the COMPLETE ESC ] 1936 ; ... ST -- feed it to the
            // SAME beacon parser the console path uses; it drives the zone/block
            // cut + span state on the scrollback and touches no cells (14.11.4).
            Control::Osc1936Raw { serial, frame } => {
                self.scrollback.feed_frame(&frame, serial);
                // H-4d: the cells the producer writes next carry `serial`;
                // they mean THIS state (after the frame) -- incl. the
                // structure (pre / table cell / rule) the tile rebuilds from.
                self.spans.note(serial, self.scrollback.span_tag());
                // BEACON.md 12.12: a program's `mark k=prog` names the tile
                // exactly as an OSC title does -- one `title`, latest wins
                // across both channels (the record stream keeps their order).
                if let Some(p) = self.scrollback.take_prog() {
                    self.title = p;
                }
            }
            Control::Title(t) => self.title = t,
            // The cwd report, forwarded raw (BEACON.md 12.11): the transcript's
            // one decoder applies it, as on the console path.
            Control::Osc7Raw(body) => self.scrollback.apply_cwd_report(&body),
            Control::Bell => self.bell = true,
            Control::Exit(code) => {
                self.exit = Some(code);
                self.hold.cut();
            }
            // The down-channel resize was applied on the pts: the next
            // CellDiff is the producer's repaint of the whole grid.
            Control::WinsizeAck => self.resize_acked = true,
            // The erased rows arrived first, as history. An erase claimed on
            // the alt screen is not the normal screen's, whatever the
            // producer says.
            Control::ScreenErased => {
                if self.mode == ScreenMode::Normal {
                    self.pinned = true;
                }
            }
            // The frame's records apply as they arrive; only the paint waits.
            Control::SyncBegin => self.hold.open(),
            Control::SyncEnd => self.hold.close(),
        }
    }

    /// Resize the tile (halcyond drives geometry, 14.11.6): the grid reshapes
    /// now; the kaua-term replies with a full CellDiff. The scrollback is
    /// flow-based and reflows at layout, so it takes no dims here.
    pub fn resize(&mut self, cols: usize, rows: usize) {
        // The normal screen reflows (the transcript's content model: a
        // soft-wrapped row is half of one logical line); the alt screen is
        // the TUI's to repaint, and so is its last frame while the normal
        // screen's repaint is still on its way: no row of that frame left
        // the normal screen's grid. The rows the reflow drops have left the
        // grid now; the producer's ScrollOff delivers those not here already
        // later, and must not count them as leaving twice. A new width
        // re-cuts every line of the producer's main screen, beneath the alt
        // screen too.
        let reflow = self.normal_screen_shown();
        let recut = cols != self.grid.dims().0;
        let shed = self.grid.resize(cols, rows, reflow);
        self.scrollback.note_grid_shed(shed);
        if recut {
            self.scrollback.note_grid_recut();
        }
    }

    /// Bring a Normal-mode list and its selection current
    /// (`select::refresh`, this grid's rows trailing), and start each end
    /// the rebase could not follow again on the grid's cursor row, the
    /// prompt.
    pub fn refresh_selected(
        &self,
        flat: &mut Vec<crate::select::FlatRow>,
        flat_seq: &mut u64,
        mut sel: Option<&mut crate::select::Sel>,
    ) {
        let rows = self.grid.dims().1;
        let lost = crate::select::refresh(&self.scrollback, Some(rows), flat, flat_seq, sel.as_deref_mut());
        if let Some(s) = sel {
            if lost.0 || lost.1 {
                s.regrid(lost, self.grid.cursor().0, flat.len());
            }
        }
    }

    /// Resize under a Normal-mode selection over `flat` (current as of
    /// `flat_seq`). A width change re-cuts every line, so no grid row keeps
    /// its text; a height change keeps each row's text and slides the window
    /// (`vt::reflow` keeps the cursor row) by the rows it drops off the top.
    /// An end whose row lost its text -- any grid row at a width change, a
    /// row slid past or cut off at a height change -- starts again on the
    /// grid's cursor row, the prompt; any other grid end slides with its
    /// row. The ends are judged on the list brought current first: a stale
    /// one can hold a row that has since left.
    pub fn resize_selected(
        &mut self,
        cols: usize,
        rows: usize,
        flat: &mut Vec<crate::select::FlatRow>,
        flat_seq: &mut u64,
        mut sel: Option<&mut crate::select::Sel>,
    ) {
        let old_cols = self.grid.dims().0;
        self.refresh_selected(flat, flat_seq, sel.as_deref_mut());
        let at = sel.as_ref().map(|s| s.grid_rows());
        let left = self.scrollback.rows_left();
        self.resize(cols, rows);
        let shed = usize::try_from(self.scrollback.rows_left().saturating_sub(left))
            .unwrap_or(usize::MAX);
        self.refresh_selected(flat, flat_seq, sel.as_deref_mut());
        if let (Some(s), Some((c, a))) = (sel, at) {
            let lost = |g: usize| cols != old_cols || g < shed || g - shed >= rows;
            let kept = |g: Option<usize>| g.filter(|&g| !lost(g)).map(|g| g - shed);
            s.slide((kept(c), kept(a)), flat.len());
            let (crow, _, _) = self.grid.cursor();
            s.regrid((c.is_some_and(lost), a.is_some_and(lost)), crow, flat.len());
        }
    }

    /// The grid shows the normal screen: the tile's mode, and the producer's
    /// repaint of that screen has landed (until then the grid still shows
    /// the full-screen app's last frame, and Esc is still the app's).
    pub fn normal_screen_shown(&self) -> bool {
        self.mode == ScreenMode::Normal && !self.screen_pending
    }

    /// The grid holds a frame of the normal screen: the live one, or its last
    /// while a full-screen app's first paint is on its way. The render paints
    /// by this; the keys and the reflow ask `normal_screen_shown`.
    pub fn holds_normal_frame(&self) -> bool {
        (self.mode == ScreenMode::Normal) != self.screen_pending
    }

    /// Whether a key goes to the transcript's Normal mode, not the program:
    /// while the grid shows the normal screen, a press or a repeat in Normal
    /// mode (`in_normal`), and in Insert only Esc's press, which enters it.
    pub fn modal_key(&self, in_normal: bool, rune: u32, value: u32) -> bool {
        self.normal_screen_shown() && value >= 1 && (in_normal || (rune == 0x1b && value == 1))
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

    /// Does this tile paint a caret at all, before section 10's blink is
    /// applied? The grid's own cursor visibility (the child's DECTCEM, which
    /// 14.7 keeps as the policy across a focus loss) AND 14.6's rule that a
    /// retained tile has no caret under the Instrument profile.
    ///
    /// Public because the blink's DIRTY rule needs exactly this question: a
    /// step that no tile can show must not repaint anything. `render` asks it
    /// too, so the two cannot answer differently -- the alternative was a
    /// second copy of the conjunction in the session loop, which is the shape
    /// that has to be re-pointed by hand every time the painter's rule moves.
    pub fn paints_caret(&self, inst: bool) -> bool {
        self.grid.cursor().2 && !(inst && self.fate != crate::chrome::Fate::Live)
    }

    /// Advance the caret's blink step; true when the tile must be repainted
    /// to show it.
    ///
    /// The phase is stored unconditionally but only a tile that CAN show the
    /// step asks for a repaint. Storing it either way matters: a tile whose
    /// cursor is hidden while the step passes would otherwise keep the phase
    /// it last painted with, and light up out of step the moment the child
    /// shows its cursor again.
    pub fn set_caret_on(&mut self, on: bool, inst: bool) -> bool {
        let repaint = self.caret_on != on && self.paints_caret(inst);
        self.caret_on = on;
        repaint
    }

    /// Paint the tile into `cart` (HALCYON.md 14.11.3). Returns the total
    /// content height in px (for scroll clamping by the caller).
    ///
    /// A normal-screen frame (`holds_normal_frame`): the scrollback flow
    /// renders above, the live grid's content rows as the tail at the bottom,
    /// proportional, soft-wrapped rows joined; the content is bottom-anchored,
    /// raised by `scroll_up` px (0 = the grid sits at the view bottom, history
    /// off the top; scrolling up reveals history). An app's frame (the alt
    /// screen, and its last frame until the normal screen's repaint lands):
    /// the grid alone, full-tile from the top-left, scrollback frozen +
    /// hidden. The `cart` is `reset()` first, so the caller passes one reusable
    /// display list. Grid glyphs come from FACE_MONO (the tile is a terminal);
    /// the scrollback flows through the proportional `layout_block`/`render_block`
    /// exactly as the console transcript does.
    ///
    /// Layout is windowed: the frozen blocks' heights come from the cache
    /// (filled once per block per width), the exact content height and every
    /// block's screen-y follow from them, and only the blocks intersecting
    /// the view are laid out, each dropped after it renders. The open block
    /// (the one block no cache can position: it changes) is laid out whole
    /// every render and stays alive across the walk, so at most TWO laid
    /// blocks exist at once, and a block that merely touches the view is
    /// laid out whole -- the transient is O(view + 2 x the open-block cap),
    /// `OPEN_BLOCK_MAX_COST` bounding both, whatever the history holds and
    /// wherever the view scrolled.
    ///
    /// `scroll_up` is the view's offset from the bottom, in pixels; a Normal
    /// mode `mark` (the cursor row + its selected obj run) drags it so the
    /// row is visible (Helix: the view follows the cursor), and the clamped
    /// result is written back. The mark paints the row's band and, for a
    /// selected run, the ember underline (the console renderer's pass).
    pub fn render(
        &mut self,
        cart: &mut Cartoon,
        w: usize,
        h: usize,
        gs: &mut GlyphSource,
        sheet: &Sheet,
        scroll_up: &mut i32,
        mark: Option<Mark>,
    ) -> i32 {
        self.render_selected(cart, w, h, gs, sheet, scroll_up, mark, &[])
    }

    /// `render` with a Normal-mode selection: every row of `bands`
    /// (`selection_bands`) is banded as the cursor's row is, each once --
    /// the console renderer's `sel_rows`.
    #[allow(clippy::too_many_arguments)]
    pub fn render_selected(
        &mut self,
        cart: &mut Cartoon,
        w: usize,
        h: usize,
        gs: &mut GlyphSource,
        sheet: &Sheet,
        scroll_up: &mut i32,
        mark: Option<Mark>,
        bands: &[Band],
    ) -> i32 {
        cart.reset();
        // HALCYON-INSTRUMENT 14.7 under the Instrument profile: the raw
        // application grid fills the content rect in `terminal_bg`, the
        // right/bottom remainder included (the grid rounds down to whole
        // cells); the rich document sits on `open` (the sheet's ground),
        // and the legacy pane keeps its surface in both modes.
        let inst = sheet.profile == libhalcyon::instrument::Profile::Instrument;
        let ground = if inst && !self.holds_normal_frame() {
            sheet.theme.terminal.bg
        } else {
            sheet.ground
        };
        cart.ops.push(Op::Clear { color: ground });
        let (_cw, cell_h, _base) = gs.mono_cell();
        let grid_h = self.grid.dims().1 as i32 * cell_h;
        self.laid_last = 0;
        self.laid_lines_last = 0;
        self.frame.clear();
        // HALCYON-INSTRUMENT 14.6, under the Instrument profile only (the
        // legacy affordance is byte-identical): a DISCONNECTED tile's body
        // is prepended a notice strip, so the view starts below it; an
        // ENDED tile's content grows by its final line.
        let top = if inst && self.fate == crate::chrome::Fate::Disconnected {
            notice_strip_h(sheet, gs)
        } else {
            0
        };
        let ended_line = if inst {
            match self.fate {
                crate::chrome::Fate::Ended(code) => Some((code, ended_line_h(sheet, gs))),
                _ => None,
            }
        } else {
            None
        };

        if !self.holds_normal_frame() {
            // The tail is the mono grid -- the app's last frame too, until the
            // normal screen's repaint lands; a click hits it by cell, not
            // through a proportional cache -- drop any stale normal-mode
            // layout so `grid_hit` takes the mono path.
            self.live_laid = None;
            paint_grid(cart, &self.grid, 0, top, gs, sheet, ground);
            if top > 0 {
                paint_notice_strip(cart, w, top, sheet, gs);
            }
            return grid_h + top;
        }

        let widthi = w as i32;
        let view_end = h as i32;
        let viewh = h as i32 - top;
        // The gap after a block depends on the pair (a prompt runs into
        // its output as one entry), so it is read per index by the walks
        // below. A frozen block that laid nothing (cells mode keeps a
        // zone-less block alive for its obj table even when its text is
        // still on the grid) takes no gap either -- else every such block
        // is a phantom band.
        let frozen_kinds: Vec<BlockKind> = self
            .scrollback
            .frozen_blocks()
            .iter()
            .map(|b| b.kind)
            .collect();
        let open_kind = self.scrollback.open_block().kind;
        let gap_after = |i: usize, hgt: i32| -> i32 {
            if hgt == 0 {
                return 0;
            }
            let next = frozen_kinds.get(i + 1).copied().unwrap_or(open_kind);
            let this = frozen_kinds.get(i).copied().unwrap_or(open_kind);
            block_gap_between(this, next, sheet)
        };
        // 7.7: the indicator's lane is reserved INSIDE the viewport on
        // overflow, so the layout width follows the last frame's decision
        // and a flip re-lays once, here. Stable, because narrowing never
        // shortens wrapped content: what overflows at the full width
        // overflows at the narrower one, and what fits at the narrower fits
        // at the full. Nothing under legacy (the lane is 0).
        let lane = crate::indicator::lane(sheet);
        let mut passes = 0u8;
        let (lay_w, open_lb, live_lb, prov, total, tail_gap, content_h) = loop {
            passes += 1;
            let lay_w = widthi - if self.lane { lane } else { 0 };
            self.laid_last += self.sync_heights(lay_w, sheet, gs);
            // The exact content height from the cached heights: the top
            // padding, every frozen block plus its trailing gap, then the
            // open block (the newest, un-frozen history; no trailing gap --
            // the grid follows it directly as the live tail), the tail, and
            // the bottom padding.
            let mut total = sheet.pad_top;
            for (i, &(_, _, hgt)) in self.heights.iter().enumerate() {
                total += hgt + gap_after(i, hgt);
            }
            let open_lb = layout_block_media(self.scrollback.open_block(), lay_w, sheet, gs, Some(&self.media));
            self.laid_last += 1;
            self.laid_lines_last += open_lb.lines.len();
            total += open_lb.height;

            // PL-4: the live grid renders PROPORTIONALLY as the normal-mode
            // tail (HALCYON 14.13) -- its soft-wrapped rows joined into
            // logical lines and re-wrapped at the tile width, replacing the
            // fixed mono grid. Laid only through the content rows so a
            // screen of trailing blanks below the prompt is not painted
            // (the bottom-anchored view would else float the prompt
            // mid-tile). `prov` maps a grid row -> (logical line, start col).
            let live_cols = self.grid.dims().0;
            let live_rows = self.grid.content_rows();
            let live_wrapped = self.grid.wrapped();
            let (live_b, prov) = self.scrollback.live_block(
                self.grid.cells(),
                live_cols,
                live_rows,
                live_wrapped,
                &self.spans,
                self.grid.top_continues(),
            );
            let live_lb = layout_block_media(&live_b, lay_w, sheet, gs, Some(&self.media));
            self.laid_last += 1;
            self.laid_lines_last += live_lb.lines.len();

            // HALCYON 14.13: after a clear the live tail starts where a fresh
            // tile's does -- under its own top padding, the history ending at
            // the view's top edge with a full view reserved below it --
            // whatever the typeface metrics. A tile with no history is laid
            // that way already.
            let pin = self.pinned && total > sheet.pad_top;
            let tail_gap = if pin { sheet.pad_top } else { 0 };
            let natural = total
                + tail_gap
                + live_lb.height
                + ended_line.map_or(0, |(_, lh)| lh)
                + sheet.pad_bottom;
            let content_h = if pin {
                natural.max(viewh + total)
            } else {
                natural
            };
            let overflow = content_h > viewh;
            if lane == 0 {
                // No indicator on this sheet (legacy, or a switch back to
                // it): the flag clears so a later Instrument sheet decides
                // afresh rather than inheriting a stale reservation.
                self.lane = false;
                break (lay_w, open_lb, live_lb, prov, total, tail_gap, content_h);
            }
            if overflow == self.lane {
                break (lay_w, open_lb, live_lb, prov, total, tail_gap, content_h);
            }
            // BOUNDED (r2 B-F1): the loop rested on "narrowing never
            // shortens", and a layout rule that was not monotone in the
            // width spun it forever -- the renderer never presented again.
            // When the two widths disagree the lane WINS: a reserved lane
            // over content that fits paints no thumb and costs 8 px; an
            // unreserved one over content that overflows hides the thumb.
            // Pass 2 laid with the lane is final; pass 2 laid without it
            // lays once more WITH it (pass 3), which is final by the same
            // rule -- and the next frame starts from the lane, so the
            // picture is stable across frames, never a per-frame flip.
            if passes >= 2 {
                if self.lane {
                    break (lay_w, open_lb, live_lb, prov, total, tail_gap, content_h);
                }
                self.lane = true;
                continue;
            }
            self.lane = overflow;
        };
        self.lane_passes = passes;
        let live_cols = self.grid.dims().0;

        // The mark's row drags the view: locate its content-relative span
        // (a frozen block's from the cached heights; the open block's is
        // laid already) and adjust scroll_up so it is visible.
        if let Some(m) = mark {
            let mut rel = sheet.pad_top;
            let mut span: Option<(i32, i32)> = None;
            for (i, (b, &(_, _, hgt))) in self
                .scrollback
                .frozen_blocks()
                .iter()
                .zip(self.heights.iter())
                .enumerate()
            {
                if b.id == m.block {
                    let lb = layout_block_media(b, lay_w, sheet, gs, Some(&self.media));
                    span = Some(match laid_line_for(&lb, m.item, m.row) {
                        Some((ly, lh)) => (rel + ly, lh),
                        None => (rel, hgt.max(1)),
                    });
                    break;
                }
                rel += hgt + gap_after(i, hgt);
            }
            if span.is_none() && m.block == u64::MAX {
                span = Some(match laid_line_for(&open_lb, m.item, m.row) {
                    Some((ly, lh)) => (rel + ly, lh),
                    None => (rel, open_lb.height.max(1)),
                });
            }
            if span.is_none() && m.block == GRID_KEY {
                let sp = live_row_spans(&live_lb, &prov, m.item, live_cols);
                if let (Some(&(y0, _)), Some(&(y1, h1))) = (sp.first(), sp.last()) {
                    span = Some((total + tail_gap + y0, (y1 + h1) - y0));
                }
            }
            if let Some((r, lh)) = span {
                // Visible iff scroll_up <= from_bottom <= scroll_up + viewh - lh.
                let from_bottom = content_h - r - lh;
                if from_bottom < *scroll_up {
                    *scroll_up = from_bottom;
                } else if from_bottom > *scroll_up + viewh - lh {
                    *scroll_up = from_bottom - (viewh - lh).max(0);
                }
            }
        }
        *scroll_up = (*scroll_up).clamp(0, (content_h - viewh).max(0));
        let su = *scroll_up;
        let y0 = top
            + if content_h <= viewh {
                0
            } else {
                viewh - content_h + su
            };

        // Bottom-anchor [scrollback][grid]: walk the blocks by their cached
        // heights, laying out + rendering only those that intersect the view.
        let mut y = y0 + sheet.pad_top;
        for (i, (b, &(_, _, hgt))) in self
            .scrollback
            .frozen_blocks()
            .iter()
            .zip(self.heights.iter())
            .enumerate()
        {
            self.frame.push((b.id, y, hgt));
            if y + hgt >= 0 && y <= view_end {
                let lb = layout_block_media(b, lay_w, sheet, gs, Some(&self.media));
                debug_assert_eq!(lb.height, hgt, "a frozen block's height is deterministic");
                paint_mark(cart, &lb, y, w, sheet, mark.filter(|m| m.block == b.id));
                paint_bands(cart, &lb, y, w, sheet, bands, b.id, mark);
                render_block(cart, &lb, y, gs);
                paint_run(cart, &lb, y, sheet, mark.filter(|m| m.block == b.id));
                self.laid_last += 1;
                self.laid_lines_last += lb.lines.len();
            }
            y += hgt + gap_after(i, hgt);
        }
        self.frame.push((u64::MAX, y, open_lb.height));
        if y + open_lb.height >= 0 && y <= view_end {
            let m = mark.filter(|m| m.block == u64::MAX);
            paint_mark(cart, &open_lb, y, w, sheet, m);
            paint_bands(cart, &open_lb, y, w, sheet, bands, u64::MAX, mark);
            render_block(cart, &open_lb, y, gs);
            paint_run(cart, &open_lb, y, sheet, m);
        }
        y += open_lb.height + tail_gap;
        // `y` is now the grid tail's screen-y (== y0 + total + tail_gap). H-4d: the
        // live grid is the virtual trailing block (14.11.5) -- in the frame
        // under GRID_KEY, its marked row banded under the cells, its
        // selected run underlined over them.
        self.frame.push((GRID_KEY, y, live_lb.height));
        let gm = mark.filter(|m| m.block == GRID_KEY);
        // The marked grid row's band, proportional (a soft-wrapped row spans
        // several laid lines) -- painted UNDER the cells.
        if let Some(m) = gm {
            for (by, bh) in live_row_spans(&live_lb, &prov, m.item, live_cols) {
                cart.ops.push(Op::Rect {
                    x: 0,
                    y: y + by,
                    w: w as u32,
                    h: bh as u32,
                    color: sheet.sel_bg,
                });
            }
        }
        for bd in bands
            .iter()
            .filter(|bd| bd.block == GRID_KEY && gm.map_or(true, |m| m.item != bd.item))
        {
            for (by, bh) in live_row_spans(&live_lb, &prov, bd.item, live_cols) {
                cart.ops.push(Op::Rect {
                    x: 0,
                    y: y + by,
                    w: w as u32,
                    h: bh as u32,
                    color: sheet.sel_bg,
                });
            }
        }
        render_block(cart, &live_lb, y, gs);
        // The caret: ONE source of truth (the grid cursor), placed at the
        // proportional x of its character boundary (HALCYON 14.13; subsumes s2,
        // the stray cursor adrift from the rows).
        let (cr, cc, _) = self.grid.cursor();
        // Section 10's blink is the SECOND conjunct, and it is separate from
        // `paints_caret` on purpose: that predicate answers "is there a caret
        // here at all", which is what decides whether a blink step has to
        // repaint this tile, while `caret_on` answers "is it up right now".
        // Folding the two would make the dirty rule mark every tile at every
        // step, caret or no caret.
        let caret = self.paints_caret(inst) && self.caret_on;
        if caret {
            if let Some(&(item, row, start)) = prov.get(cr) {
                let (cx, cy, chh) = caret_in_block(&live_lb, item, row, start + cc, sheet);
                cart.ops.push(Op::Rect {
                    x: cx,
                    y: y + cy,
                    w: sheet.mark_w as u32,
                    h: chh as u32,
                    color: sheet.accent,
                });
            }
        }
        // The selected obj run underlined, proportional -- each laid piece of a
        // wrapped run over its real x-extent, painted OVER the cells.
        if let Some(Mark {
            item,
            obj: Some(key),
            ..
        }) = gm
        {
            if let Some((c0, n, _)) = self.grid_run(item, key) {
                for (by, x0, x1) in live_run_underline(&live_lb, &prov, item, c0, n, sheet.mark_w) {
                    cart.ops.push(Op::Rect {
                        x: x0,
                        y: y + by,
                        w: (x1 - x0).max(1) as u32,
                        h: sheet.mark_w as u32,
                        color: sheet.accent,
                    });
                }
            }
        }
        // 14.6: the ended tile's final line after the tail, `Process ended
        // \u{b7} exit n` in Sans 12 `secondary`; the disconnected tile's
        // notice strip over the top of the view.
        if let Some((code, lh)) = ended_line {
            let ly = y + live_lb.height;
            if ly + lh >= 0 && ly <= view_end {
                paint_ended_line(cart, code, ly, sheet, gs);
            }
        }
        if top > 0 {
            paint_notice_strip(cart, w, top, sheet, gs);
        }
        // 7.7: the position indicator, over everything, only while the
        // content overflows (the lane it sits in is already reserved).
        if self.lane {
            let scroll = (content_h - viewh) - su;
            if let Some((x, y, tw, th)) =
                crate::indicator::thumb_rect(widthi, top, viewh, content_h, scroll, sheet)
            {
                cart.ops.push(Op::Rect {
                    x,
                    y,
                    w: tw as u32,
                    h: th as u32,
                    color: sheet.inst.dim,
                });
            }
        }
        // Cache this frame's proportional tail for the click inverse (`y` is the
        // tail's screen-y). Moved in AFTER every read above (`live_lb` / `prov`
        // are done being borrowed); `grid_hit` / `grid_run_rect` invert through
        // it until the next render replaces it.
        self.live_laid = Some((live_lb, prov, y));
        content_h
    }

    /// Drop every cached height: a SHEET change (a display scale change,
    /// HALCYON-SCALE 6) re-sizes every block at the same width, which the
    /// width key alone cannot see.
    pub fn invalidate_heights(&mut self) {
        self.heights.clear();
        self.heights_width = 0;
    }

    /// Bring the height cache in line with the frozen deque at `width`:
    /// a width change invalidates everything; blocks evicted at the front
    /// drop off; blocks frozen since the last render are laid out ONCE for
    /// their height and dropped. Returns the number of blocks laid out.
    fn sync_heights(&mut self, width: i32, sheet: &Sheet, gs: &mut GlyphSource) -> usize {
        if self.heights_width != width {
            self.heights.clear();
            self.heights_width = width;
        }
        let frozen = self.scrollback.frozen_blocks();
        // Front eviction: ids are strictly increasing along the deque, so
        // every cached id below the oldest live block's is gone.
        if let Some(oldest) = frozen.front().map(|b| b.id) {
            while self.heights.front().is_some_and(|&(id, _, _)| id < oldest) {
                self.heights.pop_front();
            }
        } else {
            self.heights.clear();
        }
        // Pairwise alignment on (id, exit): any disagreement truncates the
        // cache there, and the tail is re-laid below -- a floating exit mark
        // landing on the last frozen block re-lays exactly that block.
        let mut keep = 0;
        for (cached, b) in self.heights.iter().zip(frozen.iter()) {
            if cached.0 != b.id || cached.1 != b.exit {
                break;
            }
            keep += 1;
        }
        self.heights.truncate(keep);
        let mut laid = 0;
        for b in frozen.iter().skip(self.heights.len()) {
            let lb = layout_block_media(b, width, sheet, gs, Some(&self.media));
            self.heights.push_back((b.id, b.exit, lb.height));
            laid += 1;
        }
        laid
    }
}

/// H-4d: the frame / `Mark` key of the live grid, the virtual trailing block
/// (`Mark.item` is then the grid row, `Mark.obj` a `grid_runs` key).
pub const GRID_KEY: u64 = u64::MAX - 1;

/// H-4d: a Normal-mode cursor position in a tile's transcript, as `render`
/// paints it: the block (id, u64::MAX for the open block, or GRID_KEY), the
/// row (an item, and a table row when the item is a table; the grid row
/// under GRID_KEY), and the selected obj run on it, if any.
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub struct Mark {
    pub block: u64,
    pub item: usize,
    pub row: usize,
    pub obj: Option<u16>,
}

/// One row of a Normal-mode selection, keyed as a `Mark` keys the cursor.
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub struct Band {
    pub block: u64,
    pub item: usize,
    pub row: usize,
}

/// The block key `render` matches a flat row against: a frozen block's id,
/// `u64::MAX` for the open block, `GRID_KEY` for a live-grid row; none for a
/// frozen block that is gone.
pub fn block_key(t: &Transcript, fr: crate::select::FlatRow) -> Option<u64> {
    if fr.block == crate::select::GRID_BLOCK {
        Some(GRID_KEY)
    } else if fr.block == usize::MAX {
        Some(u64::MAX)
    } else {
        t.frozen_blocks().get(fr.block).map(|b| b.id)
    }
}

/// Every row of an anchored selection as `render_selected` bands it; none
/// without an anchor, where the cursor's row is the `Mark`'s to band.
pub fn selection_bands(
    t: &Transcript,
    flat: &[crate::select::FlatRow],
    sel: &crate::select::Sel,
) -> Vec<Band> {
    if sel.anchor.is_none() {
        return Vec::new();
    }
    let (lo, hi) = sel.range();
    flat.iter()
        .skip(lo)
        .take(hi - lo + 1)
        .filter_map(|&fr| {
            Some(Band {
                block: block_key(t, fr)?,
                item: fr.item,
                row: fr.row,
            })
        })
        .collect()
}

/// The cursor row's band under the text (`sel_bg`, full width).
fn paint_mark(
    cart: &mut Cartoon,
    lb: &LaidBlock,
    y: i32,
    w: usize,
    sheet: &Sheet,
    m: Option<Mark>,
) {
    if let Some(m) = m {
        if let Some((ly, lh)) = laid_line_for(lb, m.item, m.row) {
            cart.ops.push(Op::Rect {
                x: 0,
                y: y + ly,
                w: w as u32,
                h: lh.max(0) as u32,
                color: sheet.sel_bg,
            });
        }
    }
}

/// The rows of `bands` in `block`, banded as `paint_mark` bands the cursor's
/// row -- which is left to it, so no row is banded twice.
#[allow(clippy::too_many_arguments)]
fn paint_bands(
    cart: &mut Cartoon,
    lb: &LaidBlock,
    y: i32,
    w: usize,
    sheet: &Sheet,
    bands: &[Band],
    block: u64,
    mark: Option<Mark>,
) {
    for bd in bands.iter().filter(|bd| bd.block == block) {
        if mark.is_some_and(|m| m.block == bd.block && m.item == bd.item && m.row == bd.row) {
            continue;
        }
        if let Some((ly, lh)) = laid_line_for(lb, bd.item, bd.row) {
            cart.ops.push(Op::Rect {
                x: 0,
                y: y + ly,
                w: w as u32,
                h: lh.max(0) as u32,
                color: sheet.sel_bg,
            });
        }
    }
}

/// The selected run's ember underline (the sheet's 2-px mark) over the text.
fn paint_run(cart: &mut Cartoon, lb: &LaidBlock, y: i32, sheet: &Sheet, m: Option<Mark>) {
    if let Some(Mark {
        item,
        row,
        obj: Some(obj),
        ..
    }) = m
    {
        if let Some(r) = run_rect(lb, item, row, obj) {
            cart.ops.push(Op::Rect {
                x: r.0,
                y: y + r.1 + r.3 - sheet.mark_w,
                w: r.2.max(1) as u32,
                h: sheet.mark_w as u32,
                color: sheet.accent,
            });
        }
    }
}

/// Paint the live grid's cells at screen origin `(x0, y0)` into `cart` (a mono
/// cell store: per-cell bg rect when it differs from the ground, then the glyph,
/// then the underline; the block cursor beam last). Out-of-range is impossible
/// -- `Grid::row` and `Grid::cursor` are already clamped (grid.rs), the tile
/// trust boundary (14.11.12).
/// PL-4: the x of column `col` within ONE laid line (line-scoped, for the
/// per-line banding + underline geometry). Past the line's content -> its end.
fn line_col_x(line: &LaidLine, col: usize) -> i32 {
    for seg in line.segs.iter() {
        let n = seg.refs.len();
        if col >= seg.src_col && col < seg.src_col + n {
            return seg.xs[col - seg.src_col];
        }
    }
    line.segs.last().map(|s| s.x_end).unwrap_or(0)
}

/// PL-4b: the logical column of laid line `line` under block-relative x `x` --
/// the seg whose [x, x_end) contains it, then the glyph cell within it (each
/// glyph owns [xs[i], xs[i+1])). None past the content: a click beyond the last
/// glyph, or in a gap between runs, hits no cell (so no run).
fn col_at_x(line: &LaidLine, x: i32) -> Option<usize> {
    for seg in line.segs.iter() {
        if x >= seg.x && x < seg.x_end {
            let n = seg.refs.len();
            for i in 0..n {
                let hi = if i + 1 < n { seg.xs[i + 1] } else { seg.x_end };
                if x < hi {
                    return Some(seg.src_col + i);
                }
            }
            return Some(seg.src_col + n.saturating_sub(1));
        }
    }
    None
}

/// PL-4b: the inverse of `prov` -- the grid (row, col-in-row) that logical
/// column `col` of laid line (`item`, `row`) came from. The joined rows of one
/// logical line hold disjoint column ranges ([start, start+cols)), so at most
/// one row matches; the rows of one rebuilt table (the lines of one pre) share
/// the item and are told apart by `row` -- matching on the item alone named
/// the FIRST row for every click on the structure. None if no row covers it.
fn prov_inverse(
    prov: &[(usize, usize, usize)],
    item: usize,
    row: usize,
    col: usize,
    cols: usize,
) -> Option<(usize, usize)> {
    for (r, &(it, rw, start)) in prov.iter().enumerate() {
        if it == item && rw == row && start <= col && col < start + cols {
            return Some((r, col - start));
        }
    }
    None
}

/// PL-4: the (block-relative y, h) spans of `live`'s laid lines that show grid
/// row `r`'s columns. A soft-wrapped row can span several laid lines; a laid
/// line shared by two grid rows bands for both (matching the mono full-row
/// band). Empty for a row past `prov`.
fn live_row_spans(
    live: &LaidBlock,
    prov: &[(usize, usize, usize)],
    r: usize,
    cols: usize,
) -> Vec<(i32, i32)> {
    let Some(&(item, row, start)) = prov.get(r) else {
        return Vec::new();
    };
    let end = start + cols;
    let mut spans = Vec::new();
    for line in live.lines.iter() {
        if line.src_item != item || line.src_row != row {
            continue;
        }
        let lo = line.segs.first().map(|s| s.src_col).unwrap_or(0);
        let hi = line
            .segs
            .last()
            .map(|s| s.src_col + s.refs.len())
            .unwrap_or(lo);
        if lo < end && hi > start {
            spans.push((line.y, line.h));
        }
    }
    spans
}

/// PL-4: the (block-relative line-bottom y, x0, x1) underline segments for grid
/// row `r`'s obj run at grid columns [rc0, rc0+n) -- one per laid line the run
/// crosses, so a wrapped run underlines each piece over its real x-extent.
fn live_run_underline(
    live: &LaidBlock,
    prov: &[(usize, usize, usize)],
    r: usize,
    rc0: usize,
    n: usize,
    mark_w: i32,
) -> Vec<(i32, i32, i32)> {
    let Some(&(item, row, start)) = prov.get(r) else {
        return Vec::new();
    };
    let c0 = start + rc0;
    let c1 = c0 + n;
    let mut out = Vec::new();
    for line in live.lines.iter() {
        if line.src_item != item || line.src_row != row {
            continue;
        }
        let lo = line.segs.first().map(|s| s.src_col).unwrap_or(0);
        let hi = line
            .segs
            .last()
            .map(|s| s.src_col + s.refs.len())
            .unwrap_or(lo);
        let a = c0.max(lo);
        let b = c1.min(hi);
        if a >= b {
            continue;
        }
        out.push((line.y + line.h - mark_w, line_col_x(line, a), line_col_x(line, b)));
    }
    out
}

/// HALCYON-INSTRUMENT 14.6 (logical): the state texts' size (Sans 12), the
/// notice strip's minimum height (32) and its paddings (8 / 12).
const STATE_PX: f32 = 12.0;
const NOTICE_MIN_H: i32 = 32;
const NOTICE_PAD_Y: i32 = 8;
const NOTICE_PAD_X: i32 = 12;
pub const NOTICE_TEXT: &str = "Connection lost. The last output is preserved.";

fn state_line(gs: &mut GlyphSource, sheet: &Sheet) -> (i32, i32) {
    let px = sheet.px(STATE_PX);
    gs.line_metrics(FACE_BODY, px)
        .map(|m| (m.ascent, m.ascent + m.descent))
        .unwrap_or((10, 14))
}

/// The disconnected tile's strip height: at least 32, else the line box
/// plus its two paddings.
pub fn notice_strip_h(sheet: &Sheet, gs: &mut GlyphSource) -> i32 {
    let (_, lh) = state_line(gs, sheet);
    sheet.ipx(NOTICE_MIN_H).max(lh + 2 * sheet.ipx(NOTICE_PAD_Y))
}

/// The ended tile's final line height: its line box plus the block gap.
pub fn ended_line_h(sheet: &Sheet, gs: &mut GlyphSource) -> i32 {
    let (_, lh) = state_line(gs, sheet);
    lh + sheet.block_gap
}

/// `Process ended \u{b7} exit n` at `y`, in `secondary`, at the text inset.
fn paint_ended_line(cart: &mut Cartoon, code: i32, y: i32, sheet: &Sheet, gs: &mut GlyphSource) {
    let px = sheet.px(STATE_PX);
    let (asc, _) = state_line(gs, sheet);
    let mut text = String::from("Process ended \u{b7} exit ");
    let _ = core::fmt::write(&mut text, format_args!("{}", code));
    let (refs, _) = gs.shape_run(FACE_BODY, px, text.chars());
    if !refs.is_empty() {
        cart.push_glyphs(gs.gen(), sheet.pad_x, y + sheet.block_gap + asc, sheet.inst.secondary, &refs);
    }
}

/// The disconnected tile's notice strip over the top `h` rows: `header`
/// ground, a 1 px `separator` below, the `error` `!` then the text in
/// `secondary`, at the strip's paddings, vertically centred.
fn paint_notice_strip(cart: &mut Cartoon, w: usize, h: i32, sheet: &Sheet, gs: &mut GlyphSource) {
    let i = &sheet.inst;
    cart.ops.push(Op::Rect {
        x: 0,
        y: 0,
        w: w as u32,
        h: h.max(0) as u32,
        color: i.header,
    });
    let hair = sheet.hairline.max(1);
    cart.ops.push(Op::Rect {
        x: 0,
        y: (h - hair).max(0),
        w: w as u32,
        h: hair as u32,
        color: i.separator,
    });
    let px = sheet.px(STATE_PX);
    let (asc, lh) = state_line(gs, sheet);
    let base = (h - lh) / 2 + asc;
    let x = sheet.ipx(NOTICE_PAD_X);
    let (bang, bw) = gs.shape_run(FACE_BODY, px, "!".chars());
    let gen = gs.gen();
    if !bang.is_empty() {
        cart.push_glyphs(gen, x, base, i.error, &bang);
    }
    let (refs, _) = gs.shape_run(FACE_BODY, px, NOTICE_TEXT.chars());
    if !refs.is_empty() {
        cart.push_glyphs(gen, x + bw + sheet.ipx(NOTICE_PAD_Y), base, i.secondary, &refs);
    }
}

fn paint_grid(
    cart: &mut Cartoon,
    grid: &Grid,
    x0: i32,
    y0: i32,
    gs: &mut GlyphSource,
    sheet: &Sheet,
    ground: u32,
) {
    let (cw, ch, base) = gs.mono_cell();
    let (cols, rows) = grid.dims();
    let gen = gs.gen(); // stable across this frame: glyph() inserts never regen
    for r in 0..rows {
        let cy = y0 + r as i32 * ch;
        for c in 0..cols {
            let cell = grid.row(r)[c];
            let cx = x0 + c as i32 * cw;
            let (fg, bg) = if cell.attrs & ATTR_REVERSE != 0 {
                (cell.bg, cell.fg)
            } else {
                (cell.fg, cell.bg)
            };
            // Both arms, matching the transcript's identical test: a cell that
            // never set a background carries the vt pen's default, which is
            // `[terminal] bg` and which a theme may set apart from the pane
            // ground. Testing only `sheet.ground` made a tile emit a Rect for
            // EVERY cell under such a theme (cols x rows per frame) where the
            // intent is "only cells with an explicit background" -- and painted
            // that terminal ground over the pane's, so the same content
            // rendered differently in a tile than in the console transcript.
            if bg != ground && bg != sheet.theme.terminal.bg {
                cart.ops.push(Op::Rect {
                    x: cx,
                    y: cy,
                    w: cw as u32,
                    h: ch as u32,
                    color: bg,
                });
            }
            if cell.ch != ' ' && cell.ch != '\0' {
                // The GRID mono (advance 10 at 100%): a full-screen program
                // owns its cells at the pts geometry, not the document's
                // island size. An SGR italic is the true Italic cell under
                // Instrument (the Regular's under legacy, where the role
                // resolves to it -- 7.2).
                let face = if cell.attrs & ATTR_ITALIC != 0 {
                    sheet.face_mono_italic
                } else {
                    FACE_MONO
                };
                if let Some(gref) = gs.glyph(face, sheet.mono_grid_px, cell.ch) {
                    cart.push_glyphs(gen, cx, cy + base, fg, &[gref]);
                }
            }
            if cell.attrs & ATTR_UNDERLINE != 0 {
                cart.ops.push(Op::Rect {
                    x: cx,
                    y: cy + ch - sheet.hairline,
                    w: cw as u32,
                    h: sheet.hairline as u32,
                    color: fg,
                });
            }
        }
    }
    let (curx, cury, vis) = grid.cursor();
    if vis {
        cart.ops.push(Op::Rect {
            x: x0 + curx as i32 * cw,
            y: y0 + cury as i32 * ch,
            w: sheet.mark_w as u32,
            h: ch as u32,
            color: sheet.accent,
        });
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::transcript::Item;
    use alloc::vec;
    use alloc::vec::Vec;
    use libhalcyon::theme::DAYLIGHT;
    use vt::Cell;

    fn cell(ch: char) -> Cell {
        Cell {
            ch,
            fg: 0x00FF00,
            bg: 0,
            attrs: 0,
            span: 0,
        }
    }

    fn tile() -> Tile {
        Tile::new(20, 4, vt::BONFIRE)
    }

    #[test]
    fn a_celldiff_clearing_the_top_flag_finalizes_the_held_fragment() {
        // A soft-wrapped row scrolls off (held as a fragment); the producer's
        // CellDiffs say row 0 continues it (still held, joined live); then
        // row 0 restarts -- the flag clears and the fragment lands as a line
        // of its own, never glued to whatever scrolls off next.
        let mut t = tile();
        let row: Vec<Cell> = "abcdefgh".chars().map(cell).collect();
        t.apply(Record::ScrollOff {
            rows: vec![row],
            wrapped: vec![true],
        });
        assert!(t.scrollback.open_block().items.is_empty(), "held");
        t.apply(Record::CellDiff {
            changed: vec![(0, 0, cell('i')), (0, 1, cell('j'))],
            cursor: (0, 2, true),
            wrapped: vec![false; 4],
            top_continues: true,
        });
        assert!(t.scrollback.open_block().items.is_empty(), "still held while row 0 continues it");
        assert!(t.grid.top_continues());
        t.apply(Record::CellDiff {
            changed: vec![(0, 0, cell('z'))],
            cursor: (0, 1, true),
            wrapped: vec![false; 4],
            top_continues: false,
        });
        assert!(!t.grid.top_continues());
        let items = &t.scrollback.open_block().items;
        assert_eq!(items.len(), 1, "the fragment is a line now");
        match &items[0] {
            crate::transcript::Item::Line(l) => {
                assert_eq!(l.cells.iter().map(|c| c.ch).collect::<String>(), "abcdefgh")
            }
            _ => panic!("a line"),
        }
    }

    #[test]
    fn a_held_fragment_rides_out_a_tui_session_and_rejoins_its_row_at_leave() {
        // The alt screen's CellDiffs carry a clear flag (no history there)
        // but the main screen comes back whole: the fragment held at entry
        // is still held at leave, and the restored main's flag joins it.
        let mut t = tile();
        let row: Vec<Cell> = "abcdefgh".chars().map(cell).collect();
        t.apply(Record::ScrollOff {
            rows: vec![row],
            wrapped: vec![true],
        });
        t.apply(Record::CellDiff {
            changed: vec![(0, 0, cell('i'))],
            cursor: (0, 1, true),
            wrapped: vec![false; 4],
            top_continues: true,
        });
        t.apply(Record::Mode(ScreenMode::AltScreen));
        t.apply(Record::CellDiff {
            changed: vec![(0, 0, cell('T'))],
            cursor: (0, 1, true),
            wrapped: vec![false; 4],
            top_continues: false,
        });
        assert!(t.scrollback.open_block().items.is_empty(), "held through the TUI");
        t.apply(Record::Mode(ScreenMode::Normal));
        t.apply(Record::CellDiff {
            changed: vec![(0, 0, cell('i'))],
            cursor: (0, 1, true),
            wrapped: vec![false; 4],
            top_continues: true,
        });
        assert!(t.scrollback.open_block().items.is_empty(), "still held: row 0 continues it");
        assert!(t.grid.top_continues());
        let (lb, prov) = t.scrollback.live_block(t.grid.cells(), 20, 1, &[false], &t.spans, true);
        match &lb.items[0] {
            crate::transcript::Item::Line(l) => {
                assert_eq!(l.cells.iter().map(|c| c.ch).collect::<String>(), "abcdefghi")
            }
            _ => panic!("a line"),
        }
        assert_eq!(prov[0].2, 8, "row 0 sits after the eight held cells");
    }

    #[test]
    fn celldiff_lands_on_the_grid_not_the_scrollback() {
        let mut t = tile();
        t.apply(Record::CellDiff {
            changed: vec![(0, 0, cell('h')), (0, 1, cell('i'))],
            cursor: (0, 2, true),
            wrapped: vec![],
            top_continues: false,
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
            wrapped: vec![false, false],
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

        t.apply(Record::Control(Control::WinsizeAck)); // arms the resize's settle, nothing else
        assert_eq!(t.exited(), None);

        t.apply(Record::Control(Control::Exit(0)));
        assert_eq!(t.exited(), Some(0));
    }

    // BEACON.md 12.11 on the tile path: the cwd report crosses the wire raw
    // and the transcript's one decoder applies it -- a good report sets the
    // directory, a foreign host's or a malformed one changes nothing, and
    // the grid is untouched either way.
    #[test]
    fn a_raw_cwd_report_sets_the_scrollback_directory_through_the_one_decoder() {
        let mut t = tile();
        assert_eq!(t.scrollback.cwd(), "");
        t.apply(Record::Control(Control::Osc7Raw(
            b"file://localhost/lib/aurora".to_vec(),
        )));
        assert_eq!(t.scrollback.cwd(), "/lib/aurora");
        t.apply(Record::Control(Control::Osc7Raw(b"file:///a%20b".to_vec())));
        assert_eq!(t.scrollback.cwd(), "/a b", "percent-decoded, an empty host is ours");
        t.apply(Record::Control(Control::Osc7Raw(
            b"file://otherhost/elsewhere".to_vec(),
        )));
        assert_eq!(t.scrollback.cwd(), "/a b", "another host's report is not ours");
        t.apply(Record::Control(Control::Osc7Raw(b"file://localhost/no\x01ctl".to_vec())));
        assert_eq!(t.scrollback.cwd(), "/a b", "a control byte: rejected");
        t.apply(Record::Control(Control::Osc7Raw(b"garbage".to_vec())));
        assert_eq!(t.scrollback.cwd(), "/a b", "not a file URL: dropped whole");
        assert!(
            t.grid.cells().iter().all(|c| c.ch == ' ' || c.ch == '\0'),
            "the report wrote no cell"
        );
    }

    // BEACON.md 12.12: `mark k=prog` names the tile like an OSC title does;
    // the two channels share one `title`, latest wins in record order; an
    // empty or oversize name is dropped whole.
    #[test]
    fn a_prog_mark_names_the_tile_and_an_osc_title_after_it_wins() {
        let mut t = tile();
        let mut f: Vec<u8> = Vec::new();
        beacon::wire::point(&mut f, beacon::wire::Op::Mark, &[("k", "prog"), ("text", "ut")]);
        t.apply(Record::Control(Control::Osc1936Raw {
            serial: 1,
            frame: f.clone(),
        }));
        assert_eq!(t.title, "ut");
        t.apply(Record::Control(Control::Title(String::from("nora"))));
        assert_eq!(t.title, "nora", "a program's OSC title after the mark wins");
        t.apply(Record::Control(Control::Osc1936Raw { serial: 2, frame: f }));
        assert_eq!(t.title, "ut", "the shell re-asserts at its next prompt");
        let mut e: Vec<u8> = Vec::new();
        beacon::wire::point(&mut e, beacon::wire::Op::Mark, &[("k", "prog"), ("text", "  ")]);
        t.apply(Record::Control(Control::Osc1936Raw { serial: 3, frame: e }));
        assert_eq!(t.title, "ut", "an empty name is dropped whole");
        let long = alloc::string::String::from_utf8(alloc::vec![b'x'; 300]).unwrap();
        let mut l: Vec<u8> = Vec::new();
        beacon::wire::point(&mut l, beacon::wire::Op::Mark, &[("k", "prog"), ("text", &long)]);
        t.apply(Record::Control(Control::Osc1936Raw { serial: 4, frame: l }));
        assert_eq!(t.title, "ut", "an oversize name is dropped whole");
    }

    #[test]
    fn beacon_frame_drives_the_scrollback_zone_cut_not_the_grid() {
        let mut t = tile();
        // an output zone with a cmd mark: the console path's own grammar. This
        // exercises the dispatch (Osc1936Raw -> scrollback.feed), reusing the
        // audited beacon parser; last_command is the observable effect.
        t.apply(Record::Control(Control::Osc1936Raw {
            serial: 0,
            frame: b"\x1b]1936;v1;zone;k=output\x1b\\".to_vec(),
        }));
        t.apply(Record::Control(Control::Osc1936Raw {
            serial: 0,
            frame: b"\x1b]1936;v1;mark;k=cmd;text=ls -l\x1b\\".to_vec(),
        }));
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
            wrapped: vec![false, false],
        });
        assert_eq!(t.scrollback.open_block().items.len(), 2);
        t.apply(Record::Control(Control::Osc1936Raw {
            serial: 0,
            frame: b"\x1b]1936;v1;zone;k=prompt\x1b\\".to_vec(),
        }));
        // the zone cut froze the old block and opened a fresh one.
        t.apply(Record::ScrollOff {
            rows: vec![vec![cell('3')]],
            wrapped: vec![false],
        });
        assert_eq!(
            t.scrollback.open_block().items.len(),
            1,
            "the post-zone scroll-off is alone in the new block"
        );
    }

    // The render (14.11.3): a Cartoon is composed with a real GlyphSource. These
    // are shape assertions (a Clear, then glyph runs, plus the height contract),
    // not pixel checks -- the pixels are the ls-gfx-session E2E's job.
    fn daylight_tile(cols: usize, rows: usize) -> Tile {
        Tile::new(cols, rows, libhalcyon::theme::daylight_palette())
    }

    #[test]
    fn render_alt_is_grid_only_and_emits_glyphs() {
        let mut gs = GlyphSource::new_vendored(512);
        let sheet = crate::layout::daylight_sheet(100);
        let (cw, ch, _) = gs.mono_cell();
        let mut t = daylight_tile(20, 4);
        t.apply(Record::Mode(ScreenMode::AltScreen));
        t.apply(Record::CellDiff {
            changed: vec![(0, 0, cell('h')), (0, 1, cell('i'))],
            cursor: (0, 2, true),
            wrapped: vec![],
            top_continues: false,
        });
        let mut cart = Cartoon::new();
        let (w, h) = ((20 * cw) as usize, (4 * ch) as usize);
        let content = t.render(&mut cart, w, h, &mut gs, &sheet, &mut 0, None);
        assert_eq!(content, 4 * ch, "alt-screen content height == grid height");
        assert!(matches!(cart.ops.first(), Some(Op::Clear { .. })));
        assert!(
            cart.ops.iter().any(|o| matches!(o, Op::Glyphs { .. })),
            "the grid's glyphs are emitted"
        );
    }

    #[test]
    fn render_normal_scrollback_adds_height_above_the_grid() {
        let mut gs = GlyphSource::new_vendored(512);
        let sheet = crate::layout::daylight_sheet(100);
        let (cw, ch, _) = gs.mono_cell();
        let mut t = daylight_tile(20, 4);
        t.apply(Record::CellDiff {
            changed: vec![(3, 0, cell('x'))],
            cursor: (3, 1, true),
            wrapped: vec![],
            top_continues: false,
        });
        let mut cart = Cartoon::new();
        let (w, h) = ((20 * cw) as usize, (4 * ch) as usize);
        let grid_only = t.render(&mut cart, w, h, &mut gs, &sheet, &mut 0, None);
        // Three lines scroll off -> the scrollback grows -> the content is taller
        // than the grid tail alone (the flow renders above it, 14.11.3).
        t.apply(Record::ScrollOff {
            rows: vec![vec![cell('a')], vec![cell('b')], vec![cell('c')]],
            wrapped: vec![false, false, false],
        });
        let with_hist = t.render(&mut cart, w, h, &mut gs, &sheet, &mut 0, None);
        assert!(
            with_hist > grid_only,
            "scrollback adds content height above the grid tail ({with_hist} > {grid_only})"
        );
    }

    /// DECTCEM survives the whole seam: a child's `ESC[?25l` reaches
    /// `paints_caret`, and `ESC[?25h` brings the caret back.
    ///
    /// Every link is real here -- the vt parses the escape, kaua-term's
    /// Producer diffs it, the record is SERIALIZED AND PARSED BACK (the one
    /// link with two independent sides, so a one-sided change to the cursor
    /// byte fails here rather than in a guest), the Grid stores it and the
    /// predicate the painter asks reads it. Written because a measurement that
    /// claimed this chain was broken turned out to be an observation carried
    /// over from a build that did not emit the escape: a screenshot only says
    /// what it showed when it was TAKEN, and the caret blinks, so no single
    /// frame can settle the question. This can, on every run, in milliseconds.
    #[test]
    fn dectcem_travels_the_whole_seam_to_the_caret_predicate() {
        let mut v = vt::Vt::new(20, 4);
        v.set_capture_events(true);
        let mut p = kaua_term::Producer::new(&v);
        let pal = libhalcyon::theme::daylight_palette();
        let mut t = Tile::new(20, 4, pal);

        // The chain, with the wire in the middle: feed bytes, ship every record
        // through encode/parse, apply. Returns whether a record was produced at
        // all -- a visibility-only change must produce one, or nothing the tile
        // holds could ever learn of it.
        let step = |t: &mut Tile, p: &mut kaua_term::Producer, v: &mut vt::Vt, bytes: &[u8]| {
            let mut out = Vec::new();
            p.feed(v, bytes, &mut out);
            let produced = !out.is_empty();
            for rec in out {
                let mut buf = Vec::new();
                kaua_term::wire::encode_record(&rec, &mut buf);
                // tag, u32 length, payload -- the frame `encode_record` writes.
                let back = kaua_term::wire::parse_record(buf[0], &buf[5..])
                    .expect("the producer's own record must parse");
                assert_eq!(back, rec, "the wire round-trip is lossless");
                t.apply(back);
            }
            produced
        };

        assert!(t.paints_caret(true), "a fresh tile paints a caret");

        // The hide, with NOTHING else in the chunk: no cell changes, so the
        // record exists only because the cursor tuple moved.
        assert!(
            step(&mut t, &mut p, &mut v, b"\x1b[?25l"),
            "a visibility-only change must still emit a record"
        );
        assert!(!v.cursor_visible, "the vt took the DEC-private 25");
        assert!(
            !t.paints_caret(true),
            "the hidden caret reaches the predicate"
        );
        assert!(!t.paints_caret(false), "under either profile");

        // What lantern writes next: its clear and a slide. The caret stays down
        // across a repaint -- the escape is not consumed by the cells that follow.
        step(&mut t, &mut p, &mut v, b"\x1b[0m\x1b[H\x1b[2Jslide one");
        assert_eq!(t.grid.row(0)[0].ch, 's', "the slide landed");
        assert!(!t.paints_caret(true), "and the caret is still down");

        // SGR 25 is blink-off, NOT show-cursor: the `?` is what carries the
        // DEC-private meaning, so the bare form must leave the caret hidden.
        step(&mut t, &mut p, &mut v, b"\x1b[25m");
        assert!(!t.paints_caret(true), "SGR 25 is not DECTCEM");

        // And the way out, which lantern writes on every exit path.
        step(&mut t, &mut p, &mut v, b"\x1b[?25h");
        assert!(t.paints_caret(true), "the show brings the caret back");
    }

    /// HALCYON 14.13: after a whole-screen erase the live tail starts where
    /// a fresh tile's does, with the history above it; the next ScrollOff
    /// hands the view back to the bottom-anchored flow.
    #[test]
    fn a_screen_erase_pins_the_live_tail_to_the_top_of_the_view() {
        let mut gs = GlyphSource::new_vendored(512);
        let sheet = crate::layout::daylight_sheet(100);
        let (cw, ch, _) = gs.mono_cell();
        let (w, h) = ((20 * cw) as usize, (6 * ch) as usize);
        let mut t = daylight_tile(20, 6);
        // History far taller than the view, then a one-line tail.
        let rows: Vec<Vec<Cell>> = (0..30u8)
            .map(|i| vec![cell(char::from(b'a' + i % 26))])
            .collect();
        t.apply(Record::ScrollOff {
            wrapped: vec![false; rows.len()],
            rows,
        });
        t.apply(Record::CellDiff {
            changed: vec![(0, 0, cell('s'))],
            cursor: (0, 1, true),
            wrapped: vec![],
            top_continues: false,
        });
        let tail_y = |t: &Tile| {
            t.live_laid
                .as_ref()
                .map(|l| l.2)
                .expect("a normal render caches the tail")
        };
        let mut cart = Cartoon::new();
        t.render(&mut cart, w, h, &mut gs, &sheet, &mut 0, None);
        let flowing = tail_y(&t);
        assert!(
            flowing > sheet.pad_top,
            "premise: unpinned, the history pushes the tail down the view ({flowing})"
        );

        t.apply(Record::Control(Control::ScreenErased));
        let content = t.render(&mut cart, w, h, &mut gs, &sheet, &mut 0, None);
        assert_eq!(
            tail_y(&t),
            sheet.pad_top,
            "pinned: the tail starts where a fresh tile's does"
        );
        assert!(
            t.frame
                .iter()
                .filter(|&&(id, _, _)| id != GRID_KEY)
                .all(|&(_, y, bh)| y + bh <= 0),
            "and no history shows above it: {:?}",
            t.frame
        );
        assert!(
            content > h as i32,
            "the history is still there, above the view"
        );
        let mut up = i32::MAX;
        t.render(&mut cart, w, h, &mut gs, &sheet, &mut up, None);
        assert_eq!(
            up,
            content - h as i32,
            "and all of it is reachable by scrolling up"
        );

        t.apply(Record::ScrollOff {
            rows: vec![vec![cell('z')]],
            wrapped: vec![false],
        });
        t.render(&mut cart, w, h, &mut gs, &sheet, &mut 0, None);
        assert_eq!(
            tail_y(&t),
            flowing,
            "released: the tail is back where the flow puts it"
        );
    }

    /// On the Instrument sheet (28 px of top padding at 100 %): a pinned view
    /// shows none of a short history, and the floor alone overflows the view,
    /// so the position lane is reserved -- there is history to scroll to.
    #[test]
    fn a_pinned_instrument_view_hides_a_short_history_and_reserves_the_lane() {
        let mut gs = GlyphSource::new_vendored(512);
        let s = inst_sheet();
        let (cw, ch, _) = gs.mono_cell();
        let (w, h) = ((60 * cw) as usize, (30 * ch) as usize);
        let mut t = history_tile(60, 30, 64);
        push_history(&mut t, 2, 1, 'h');
        t.apply(Record::CellDiff {
            changed: vec![(0, 0, cell('s'))],
            cursor: (0, 1, true),
            wrapped: vec![],
            top_continues: false,
        });
        let mut cart = Cartoon::new();
        let content = t.render(&mut cart, w, h, &mut gs, &s, &mut 0, None);
        assert!(content < h as i32 && !t.lane, "premise: it fits unpinned");
        t.apply(Record::Control(Control::ScreenErased));
        let mut cart = Cartoon::new();
        let content = t.render(&mut cart, w, h, &mut gs, &s, &mut 0, None);
        assert!(content > h as i32, "the floor overflows the view");
        assert!(t.lane, "so the lane is reserved");
        assert_eq!(
            t.live_laid.as_ref().map(|l| l.2),
            Some(s.pad_top),
            "the tail under the Instrument padding"
        );
        assert!(
            t.frame
                .iter()
                .filter(|&&(id, _, _)| id != GRID_KEY)
                .all(|&(_, y, bh)| y + bh <= 0),
            "no history above the tail: {:?}",
            t.frame
        );
    }

    /// A pinned tail taller than the view flows as any tall tail does: its
    /// end, then the bottom padding, meet the view's bottom edge.
    #[test]
    fn a_pinned_tail_taller_than_the_view_still_ends_at_its_bottom() {
        let mut gs = GlyphSource::new_vendored(512);
        let sheet = crate::layout::daylight_sheet(100);
        let (cw, ch, _) = gs.mono_cell();
        let (w, h) = ((20 * cw) as usize, (6 * ch) as usize);
        let mut t = history_tile(20, 20, 64);
        push_history(&mut t, 4, 1, 'h');
        t.apply(Record::CellDiff {
            changed: (0..20u16).map(|r| (r, 0, cell('t'))).collect(),
            cursor: (19, 1, true),
            wrapped: vec![],
            top_continues: false,
        });
        t.apply(Record::Control(Control::ScreenErased));
        let mut cart = Cartoon::new();
        t.render(&mut cart, w, h, &mut gs, &sheet, &mut 0, None);
        let (tail_h, tail_y) = t
            .live_laid
            .as_ref()
            .map(|l| (l.0.height, l.2))
            .expect("a normal render caches the tail");
        assert!(
            tail_h > h as i32,
            "premise: the tail is taller than the view"
        );
        assert_eq!(
            tail_y + tail_h + sheet.pad_bottom,
            h as i32,
            "the tail's end sits on the view's bottom edge"
        );
    }

    /// Pinned with no history there is nothing to scroll to: the tile lays
    /// out exactly as it did before the clear.
    #[test]
    fn a_pinned_tile_without_history_lays_out_as_a_fresh_one() {
        let mut gs = GlyphSource::new_vendored(512);
        let s = inst_sheet();
        let (cw, ch, _) = gs.mono_cell();
        let (w, h) = ((60 * cw) as usize, (30 * ch) as usize);
        let mut t = history_tile(60, 30, 64);
        t.apply(Record::CellDiff {
            changed: vec![(0, 0, cell('s'))],
            cursor: (0, 1, true),
            wrapped: vec![],
            top_continues: false,
        });
        let mut cart = Cartoon::new();
        let fresh = t.render(&mut cart, w, h, &mut gs, &s, &mut 0, None);
        let fresh_y = t.live_laid.as_ref().map(|l| l.2);
        t.apply(Record::Control(Control::ScreenErased));
        let mut cart = Cartoon::new();
        let pinned = t.render(&mut cart, w, h, &mut gs, &s, &mut 0, None);
        assert_eq!(
            (pinned, t.live_laid.as_ref().map(|l| l.2), t.lane),
            (fresh, fresh_y, false),
            "nothing to scroll to, so nothing moves"
        );
    }

    /// Scrolled up into the history of a pinned tile, a mark on the tail's row
    /// drags the view down until that row is inside it.
    #[test]
    fn a_mark_on_the_pinned_tail_drags_the_view_to_it() {
        let mut gs = GlyphSource::new_vendored(512);
        let sheet = crate::layout::daylight_sheet(100);
        let (cw, ch, _) = gs.mono_cell();
        let (w, h) = ((20 * cw) as usize, (12 * ch) as usize);
        let mut t = history_tile(20, 4, 1000);
        push_history(&mut t, 40, 3, 'm');
        t.apply(Record::CellDiff {
            changed: vec![(0, 0, cell('s'))],
            cursor: (0, 1, true),
            wrapped: vec![],
            top_continues: false,
        });
        t.apply(Record::Control(Control::ScreenErased));
        let mut cart = Cartoon::new();
        let mut su = i32::MAX;
        t.render(&mut cart, w, h, &mut gs, &sheet, &mut su, None);
        assert!(su > 0, "premise: scrolled up into the history");
        let mark = Mark {
            block: GRID_KEY,
            item: 0,
            row: usize::MAX,
            obj: None,
        };
        let mut cart = Cartoon::new();
        t.render(&mut cart, w, h, &mut gs, &sheet, &mut su, Some(mark));
        let band = cart
            .ops
            .iter()
            .find_map(|o| match o {
                Op::Rect {
                    y, h: bh, color, ..
                } if *color == sheet.sel_bg => Some((*y, *bh as i32)),
                _ => None,
            })
            .expect("the marked row paints its band");
        assert!(
            band.0 >= 0 && band.0 + band.1 <= h as i32,
            "the marked row is in view: {band:?} of {h}"
        );
    }

    /// A Normal-mode selection bands each row it covers -- the console
    /// renderer's `sel_rows` -- through the history and into the live grid;
    /// without an anchor only the cursor's row is banded.
    #[test]
    fn a_selection_bands_each_row_it_covers() {
        let mut gs = GlyphSource::new_vendored(512);
        let sheet = crate::layout::daylight_sheet(100);
        let (cw, ch, _) = gs.mono_cell();
        let (w, h) = ((20 * cw) as usize, (60 * ch) as usize);
        let mut t = history_tile(20, 4, 1000);
        push_history(&mut t, 4, 3, 'm');
        t.apply(Record::CellDiff {
            changed: vec![(0, 0, cell('s')), (1, 0, cell('t'))],
            cursor: (1, 1, true),
            wrapped: vec![],
            top_continues: false,
        });
        let flat = crate::select::flatten_with_grid(&t.scrollback, 4);
        let grid0 = flat
            .iter()
            .position(|fr| fr.block == crate::select::GRID_BLOCK)
            .expect("premise: the grid's rows trail the history's");
        assert!(
            grid0 >= 4,
            "premise: four history rows precede the grid ({grid0})"
        );
        let banded = |cart: &Cartoon| {
            cart.ops
                .iter()
                .filter(|o| matches!(o, Op::Rect { color, .. } if *color == sheet.sel_bg))
                .count()
        };
        // In the history: rows 1..=3, the cursor on row 3.
        let mut sel = crate::select::Sel::at(3, crate::select::Stamp::default());
        sel.anchor = Some(1);
        let bands = selection_bands(&t.scrollback, &flat, &sel);
        assert_eq!(bands.len(), 3, "every selected row is a band");
        let fr = flat[3];
        let mark = Mark {
            block: block_key(&t.scrollback, fr).unwrap(),
            item: fr.item,
            row: fr.row,
            obj: None,
        };
        let mut cart = Cartoon::new();
        t.render_selected(&mut cart, w, h, &mut gs, &sheet, &mut 0, Some(mark), &bands);
        assert_eq!(
            banded(&cart),
            3,
            "each selected row is banded once, the cursor's included"
        );
        // Across into the grid: the last history row and the grid's first two.
        sel.cursor = grid0 + 1;
        sel.anchor = Some(grid0 - 1);
        let bands = selection_bands(&t.scrollback, &flat, &sel);
        assert_eq!(bands.len(), 3);
        let mark = Mark {
            block: GRID_KEY,
            item: 1,
            row: usize::MAX,
            obj: None,
        };
        let mut cart = Cartoon::new();
        t.render_selected(&mut cart, w, h, &mut gs, &sheet, &mut 0, Some(mark), &bands);
        assert_eq!(banded(&cart), 3, "a history row and two grid rows");
        // The control, one variable away: no anchor, the cursor's row alone.
        sel.anchor = None;
        assert!(selection_bands(&t.scrollback, &flat, &sel).is_empty());
        let mut cart = Cartoon::new();
        t.render_selected(&mut cart, w, h, &mut gs, &sheet, &mut 0, Some(mark), &[]);
        assert_eq!(banded(&cart), 1);
    }

    #[test]
    fn the_pin_is_the_normal_screens_and_only_a_scrolloff_releases_it() {
        let mut t = tile();
        t.apply(Record::Mode(ScreenMode::AltScreen));
        t.apply(Record::Control(Control::ScreenErased));
        assert!(!t.pinned, "an erase claimed on the alt screen pins nothing");
        t.apply(Record::Mode(ScreenMode::Normal));
        t.apply(Record::Control(Control::ScreenErased));
        assert!(t.pinned, "a normal-screen erase pins");
        // `clear`, then an editor, then back: the pin belongs to the screen
        // the editor left alone.
        t.apply(Record::Mode(ScreenMode::AltScreen));
        t.apply(Record::CellDiff {
            changed: vec![(0, 0, cell('e'))],
            cursor: (0, 1, true),
            wrapped: vec![],
            top_continues: false,
        });
        t.apply(Record::Mode(ScreenMode::Normal));
        assert!(t.pinned, "an alt-screen excursion leaves it");
        t.apply(Record::Control(Control::Bell));
        t.apply(Record::CellDiff {
            changed: vec![(1, 0, cell('x'))],
            cursor: (1, 1, true),
            wrapped: vec![],
            top_continues: false,
        });
        assert!(t.pinned, "cells and other controls leave it");
        t.apply(Record::ScrollOff {
            rows: vec![vec![cell('x')]],
            wrapped: vec![false],
        });
        assert!(!t.pinned, "a ScrollOff releases it");
    }

    /// TC-1 across every link, in the operator's shape: output that has
    /// partly scrolled, the command that starts the deck, then its clear.
    // One chunk of program output through the whole seam: the vt, the
    // producer, the wire both ways, the tile.
    fn seam_step(t: &mut Tile, p: &mut kaua_term::Producer, v: &mut vt::Vt, bytes: &[u8]) {
        let mut out = Vec::new();
        p.feed(v, bytes, &mut out);
        for rec in out {
            let mut buf = Vec::new();
            kaua_term::wire::encode_record(&rec, &mut buf);
            let back = kaua_term::wire::parse_record(buf[0], &buf[5..])
                .expect("the producer's own record must parse");
            assert_eq!(back, rec, "the wire round-trip is lossless");
            t.apply(back);
        }
    }

    fn history_lines(t: &Tile) -> Vec<String> {
        t.scrollback
            .open_block()
            .items
            .iter()
            .filter_map(|it| match it {
                Item::Line(l) => Some(l.cells.iter().map(|c| c.ch).collect()),
                _ => None,
            })
            .collect()
    }

    /// The vt reports the erase, the producer orders it, the record crosses
    /// the wire, and the tile keeps the erased screen as history and pins.
    #[test]
    fn a_clear_crosses_the_whole_seam_keeping_the_erased_screen_as_history() {
        let mut v = vt::Vt::new(20, 4);
        v.set_capture_events(true);
        let mut p = kaua_term::Producer::new(&v);
        let mut t = Tile::new(20, 4, libhalcyon::theme::daylight_palette());
        // Four rows: 1..3 scroll off; 4..6 and the command line are the
        // screen, and the newline that runs the command scrolls 4 off too.
        seam_step(
            &mut t,
            &mut p,
            &mut v,
            b"1\r\n2\r\n3\r\n4\r\n5\r\n6\r\n% lantern deck",
        );
        seam_step(&mut t, &mut p, &mut v, b"\r\n\x1b[0m\x1b[H\x1b[2Jslide one");
        assert_eq!(
            history_lines(&t),
            ["1", "2", "3", "4", "5", "6", "% lantern deck"],
            "nothing the program erased is lost, the command that ran it included"
        );
        assert!(t.pinned, "and the view is pinned");
        assert_eq!(t.grid.row(0)[0].ch, 's', "the slide is the live screen");
    }

    /// ut's redraw after `clear`: its prompt at the top-left, then `\r ESC[J`
    /// from the block's top on every keystroke. None of it is a clear.
    #[test]
    fn typing_after_a_clear_files_no_keystroke_into_the_history() {
        let mut v = vt::Vt::new(20, 4);
        v.set_capture_events(true);
        let mut p = kaua_term::Producer::new(&v);
        let mut t = Tile::new(20, 4, libhalcyon::theme::daylight_palette());
        seam_step(&mut t, &mut p, &mut v, b"% clear\r\n\x1b[H\x1b[2J\x1b[3J% ");
        assert_eq!(
            history_lines(&t),
            ["% clear"],
            "premise: the clear kept the screen"
        );
        for typed in ["% l", "% la", "% lan"] {
            seam_step(
                &mut t,
                &mut p,
                &mut v,
                alloc::format!("\r\x1b[J{typed}").as_bytes(),
            );
        }
        assert_eq!(
            history_lines(&t),
            ["% clear"],
            "no keystroke reached the history"
        );
        assert!(t.pinned, "and the view stays pinned");
    }

    /// A fragment that scrolled off stops being continued when row 0 restarts;
    /// the next rows to leave, in the same chunk, must not glue to it.
    #[test]
    fn a_restarted_top_row_never_glues_to_the_fragment_above() {
        let mut v = vt::Vt::new(4, 2);
        v.set_capture_events(true);
        let mut p = kaua_term::Producer::new(&v);
        let mut t = Tile::new(4, 2, libhalcyon::theme::daylight_palette());
        seam_step(&mut t, &mut p, &mut v, b"abcdefghij\x1b[HXY\x1b[2J");
        assert_eq!(history_lines(&t), ["abcd", "XYgh", "ij"]);
    }

    /// A reset (RIS) rescues a tile a crashed TUI left on the alt screen: the
    /// tile returns to the normal screen, the main screen's text moves to
    /// history, and the view pins like any clear.
    #[test]
    fn a_reset_rescues_a_tile_left_on_the_alt_screen() {
        let mut v = vt::Vt::new(20, 4);
        v.set_capture_events(true);
        let mut p = kaua_term::Producer::new(&v);
        let mut t = Tile::new(20, 4, libhalcyon::theme::daylight_palette());
        seam_step(&mut t, &mut p, &mut v, b"% top\r\n\x1b[?1049hcrashed");
        assert_eq!(
            t.mode,
            ScreenMode::AltScreen,
            "premise: the tile shows the alt screen"
        );
        seam_step(&mut t, &mut p, &mut v, b"\x1bc");
        assert_eq!(
            t.mode,
            ScreenMode::Normal,
            "the reset returned the tile to the normal screen"
        );
        assert_eq!(
            history_lines(&t),
            ["% top"],
            "the main screen's text moved to history"
        );
        assert!(t.pinned, "and the view is pinned");
        assert!(
            t.grid.row(0).iter().all(|c| c.ch == ' '),
            "the live screen is blank"
        );
    }

    /// A line whose head scrolled off before a TUI took the alt screen stays
    /// one line when a reset, not the TUI's own leave, brings the main screen
    /// back and moves it into history.
    #[test]
    fn a_reset_on_the_alt_screen_keeps_a_held_line_whole() {
        let mut v = vt::Vt::new(4, 3);
        v.set_capture_events(true);
        let mut p = kaua_term::Producer::new(&v);
        let mut t = Tile::new(4, 3, libhalcyon::theme::daylight_palette());
        seam_step(&mut t, &mut p, &mut v, b"abcdefghijklm");
        seam_step(&mut t, &mut p, &mut v, b"\x1b[?1049hTUI");
        seam_step(&mut t, &mut p, &mut v, b"\x1bc");
        assert_eq!(history_lines(&t), ["abcdefghijklm"]);
    }

    /// In a one-row tile each row that leaves by autowrap is continued by the
    /// next one on row 0: the line lands whole.
    #[test]
    fn a_one_row_tile_keeps_an_autowrapped_line_whole() {
        let mut v = vt::Vt::new(4, 1);
        v.set_capture_events(true);
        let mut p = kaua_term::Producer::new(&v);
        let mut t = Tile::new(4, 1, libhalcyon::theme::daylight_palette());
        seam_step(&mut t, &mut p, &mut v, b"abcdefghij\r\n");
        assert_eq!(history_lines(&t), ["abcdefghij"]);
    }

    /// A restart that leaves the screen as the producer last sent it still
    /// ends the line above: the next line to leave does not glue to it.
    #[test]
    fn a_restart_that_changes_nothing_else_still_ends_the_line_above() {
        let mut v = vt::Vt::new(4, 2);
        v.set_capture_events(true);
        let mut p = kaua_term::Producer::new(&v);
        let mut t = Tile::new(4, 2, libhalcyon::theme::daylight_palette());
        seam_step(&mut t, &mut p, &mut v, b"abcde\r\n\x1b[H\x1b[1JXYZ\r\n\r\n");
        assert_eq!(history_lines(&t), ["abcd", "XYZ"]);
    }

    /// A clear moves row 0 into history before it reports the restart, so a
    /// line whose head scrolled off earlier lands whole -- ED 2, ED 3 and a
    /// reset alike.
    #[test]
    fn a_clear_keeps_the_line_row_zero_continues() {
        for clear in [&b"\x1b[2J"[..], b"\x1b[3J", b"\x1bc"] {
            let mut v = vt::Vt::new(4, 2);
            v.set_capture_events(true);
            let mut p = kaua_term::Producer::new(&v);
            let mut t = Tile::new(4, 2, libhalcyon::theme::daylight_palette());
            seam_step(&mut t, &mut p, &mut v, b"abcdef\r\n");
            seam_step(&mut t, &mut p, &mut v, clear);
            assert_eq!(history_lines(&t), ["abcdef"], "{clear:?}");
            assert!(t.pinned, "{clear:?}");
        }
    }

    #[test]
    fn render_normal_tail_is_proportional_with_a_caret() {
        // PL-4b: the normal-mode tail renders PROPORTIONAL (via live_block, not
        // the mono paint_grid); a 2px ember caret marks the grid cursor; and a
        // mostly-blank TALL grid TRIMS -- the content height is far below
        // rows*cell_h.
        let mut gs = GlyphSource::new_vendored(512);
        let sheet = crate::layout::daylight_sheet(100);
        let (_, ch, _) = gs.mono_cell();
        let mut t = daylight_tile(20, 24); // tall grid, one line of content
        t.apply(Record::CellDiff {
            changed: vec![(0, 0, cell('h')), (0, 1, cell('i'))],
            cursor: (0, 2, true),
            wrapped: vec![],
            top_continues: false,
        });
        let mut cart = Cartoon::new();
        let (w, h) = (20 * 8, (24 * ch) as usize);
        let content = t.render(&mut cart, w, h, &mut gs, &sheet, &mut 0, None);
        assert!(
            content < 24 * ch,
            "the trimmed proportional tail is far below the full mono grid ({content} < {})",
            24 * ch
        );
        let caret = cart.ops.iter().any(|op| {
            matches!(op, Op::Rect { w: 2, color, .. } if *color == DAYLIGHT.ember)
        });
        assert!(caret, "a 2px ember caret beam is painted at the grid cursor");
    }

    /// A tile whose scrollback freezes a block every `lines` rows (zone cuts)
    /// and holds at most `max_blocks` frozen blocks.
    fn history_tile(cols: usize, rows: usize, max_blocks: usize) -> Tile {
        Tile {
            grid: Grid::new(cols, rows, libhalcyon::theme::daylight_palette().fg, libhalcyon::theme::daylight_palette().bg),
            scrollback: {
                let mut t = Transcript::with_caps(
                    libhalcyon::theme::daylight_palette(),
                    max_blocks,
                    DEFAULT_MAX_COST,
                    DEFAULT_MAX_LINES_PER_BLOCK,
                );
                t.set_cells_mode(true);
                t
            },
            media: crate::inlinecache::InlineCache::new(DEFAULT_MAX_COST / 2),
            mode: ScreenMode::Normal,
            title: String::new(),
            fate: crate::chrome::Fate::Live,
            caret_on: true,
            exit: None,
            bell: false,
            pinned: false,
            resize_acked: false,
            screen_pending: false,
            heights: VecDeque::new(),
            heights_width: 0,
            frame: Vec::new(),
            spans: SpanMap::new(),
            laid_last: 0,
            laid_lines_last: 0,
            live_laid: None,
            lane: false,
            lane_passes: 0,
            hold: vt::FrameHold::default(),
        }
    }

    fn push_history(t: &mut Tile, blocks: usize, lines_per_block: usize, ch: char) {
        for _ in 0..blocks {
            let rows: Vec<Vec<Cell>> = (0..lines_per_block)
                .map(|_| vec![cell(ch), cell(ch), cell(ch)])
                .collect();
            t.apply(Record::ScrollOff {
                wrapped: vec![false; rows.len()],
                rows,
            });
            // a zone cut freezes the open block and opens the next one
            t.apply(Record::Control(Control::Osc1936Raw {
                serial: 0,
                frame: b"\x1b]1936;v1;zone;k=output\x1b\\".to_vec(),
            }));
        }
    }

    /// The exact content height by the OLD method (every block laid out).
    fn full_height(t: &Tile, w: usize, gs: &mut GlyphSource, sheet: &Sheet) -> i32 {
        let mut total = sheet.block_gap;
        let frozen = t.scrollback.frozen_blocks();
        for (i, b) in frozen.iter().enumerate() {
            let next = frozen
                .get(i + 1)
                .map(|n| n.kind)
                .unwrap_or(t.scrollback.open_block().kind);
            let h = layout_block(b, w as i32, sheet, gs).height;
            total += h + if h == 0 { 0 } else { block_gap_between(b.kind, next, sheet) };
        }
        total += layout_block(t.scrollback.open_block(), w as i32, sheet, gs).height;
        // PL-4: the tail is the proportional live grid (its content rows laid
        // as logical lines), not rows*cell_h.
        let (live_b, _) = t.scrollback.live_block(
            t.grid.cells(),
            t.grid.dims().0,
            t.grid.content_rows(),
            t.grid.wrapped(),
            &t.spans,
        false,
        );
        total + layout_block(&live_b, w as i32, sheet, gs).height
    }

    #[test]
    fn render_lays_out_only_the_blocks_in_view_once_the_heights_are_cached() {
        // B2-F1: a render's layout transient must be O(view), not O(history).
        let mut gs = GlyphSource::new_vendored(512);
        let sheet = crate::layout::daylight_sheet(100);
        let (cw, ch, _) = gs.mono_cell();
        let mut t = history_tile(20, 4, 1000);
        push_history(&mut t, 200, 3, 'h');
        assert_eq!(t.scrollback.frozen_blocks().len(), 200);
        let mut cart = Cartoon::new();
        let (w, h) = ((20 * cw) as usize, (4 * ch) as usize);

        // Cold: every frozen block is laid out ONCE for its height (and the
        // blocks in view again, plus the open block).
        let cold = t.render(&mut cart, w, h, &mut gs, &sheet, &mut 0, None);
        assert!(
            t.laid_last >= 200,
            "cold render fills the cache ({})",
            t.laid_last
        );
        assert_eq!(
            cold,
            full_height(&t, w, &mut gs, &sheet),
            "content height is exact"
        );

        // Warm: only the open block and the (at most two) frozen blocks that
        // touch a 4-row view are laid out -- not the 200 in history.
        let warm = t.render(&mut cart, w, h, &mut gs, &sheet, &mut 0, None);
        assert_eq!(warm, cold);
        assert!(
            t.laid_last <= 4,
            "warm render laid out {} blocks for a 4-row view",
            t.laid_last
        );
        // The transient in LINES: the in-view blocks (3 lines each) plus the
        // (empty) open block -- never the 600 lines of history.
        assert!(
            t.laid_lines_last <= 12,
            "warm render laid out {} lines for a 4-row view",
            t.laid_lines_last
        );

        // Scrolled to the very top: the window follows the scroll -- still a
        // handful of blocks, never the whole history.
        let mut su = i32::MAX;
        let top = t.render(&mut cart, w, h, &mut gs, &sheet, &mut su, None);
        assert_eq!(top, cold);
        assert!(
            (0..i32::MAX).contains(&su),
            "the clamped scroll offset is written back: {}",
            su
        );
        assert!(
            (1..=6).contains(&t.laid_last),
            "top-of-history render laid out {} blocks",
            t.laid_last
        );
        assert!(
            cart.ops.iter().any(|o| matches!(o, Op::Glyphs { .. })),
            "the oldest history renders at the top"
        );

        // New history since the last render: only the NEW frozen blocks join
        // the cache (plus the view's blocks).
        push_history(&mut t, 3, 3, 'j');
        let grown = t.render(&mut cart, w, h, &mut gs, &sheet, &mut 0, None);
        assert!(grown > cold);
        assert!(
            t.laid_last <= 3 + 4,
            "incremental cache fill laid out {}",
            t.laid_last
        );
        assert_eq!(grown, full_height(&t, w, &mut gs, &sheet));
    }

    #[test]
    fn height_cache_follows_eviction_and_width_changes() {
        let mut gs = GlyphSource::new_vendored(512);
        let sheet = crate::layout::daylight_sheet(100);
        let (cw, ch, _) = gs.mono_cell();
        // At most 5 frozen blocks: pushing 12 evicts 7 at the front.
        let mut t = history_tile(20, 4, 5);
        push_history(&mut t, 4, 2, 'a');
        let mut cart = Cartoon::new();
        let (w, h) = ((20 * cw) as usize, (4 * ch) as usize);
        let _ = t.render(&mut cart, w, h, &mut gs, &sheet, &mut 0, None);
        assert_eq!(t.heights.len(), 4);
        push_history(&mut t, 8, 2, 'b');
        assert_eq!(
            t.scrollback.frozen_blocks().len(),
            5,
            "the block cap evicted"
        );
        let got = t.render(&mut cart, w, h, &mut gs, &sheet, &mut 0, None);
        assert_eq!(t.heights.len(), 5);
        let ids: Vec<u64> = t.scrollback.frozen_blocks().iter().map(|b| b.id).collect();
        let cached: Vec<u64> = t.heights.iter().map(|e| e.0).collect();
        assert_eq!(cached, ids, "the cache is aligned to the frozen deque");
        assert_eq!(got, full_height(&t, w, &mut gs, &sheet));

        // A different width invalidates every cached height (a reflow).
        let w2 = (30 * cw) as usize;
        let narrow = t.render(&mut cart, w2, h, &mut gs, &sheet, &mut 0, None);
        assert!(
            t.laid_last >= 5,
            "a new width re-lays every block ({})",
            t.laid_last
        );
        assert_eq!(narrow, full_height(&t, w2, &mut gs, &sheet));
        assert_eq!(t.heights_width, w2 as i32);
    }

    #[test]
    fn a_floating_exit_mark_re_lays_the_frozen_block_it_lands_on() {
        // The one post-freeze mutation: an exit mark arriving right AFTER its
        // output zone closed lands on the last FROZEN block (the floating
        // order the transcript tolerates). A non-zero code adds the badge
        // line, so a height cached before it would misplace every block below.
        let mut gs = GlyphSource::new_vendored(512);
        let sheet = crate::layout::daylight_sheet(100);
        let (cw, ch, _) = gs.mono_cell();
        let mut t = history_tile(20, 4, 1000);
        t.apply(Record::Control(Control::Osc1936Raw {
            serial: 0,
            frame: b"\x1b]1936;v1;zone;k=output\x1b\\".to_vec(),
        }));
        t.apply(Record::ScrollOff {
            rows: vec![vec![cell('a')], vec![cell('b')]],
            wrapped: vec![false, false],
        });
        t.apply(Record::Control(Control::Osc1936Raw {
            serial: 0,
            frame: b"\x1b]1936;v1;/zone\x1b\\".to_vec(),
        }));
        assert_eq!(t.scrollback.frozen_blocks().len(), 1);
        let mut cart = Cartoon::new();
        let (w, h) = ((20 * cw) as usize, (4 * ch) as usize);
        let before = t.render(&mut cart, w, h, &mut gs, &sheet, &mut 0, None);
        assert_eq!(t.heights.len(), 1);
        assert_eq!(t.heights[0].1, None);
        // the floating exit mark: the open block is an empty Foreign one, so
        // the code lands on the frozen output block
        t.apply(Record::Control(Control::Osc1936Raw {
            serial: 0,
            frame: b"\x1b]1936;v1;mark;k=exit;code=2\x1b\\".to_vec(),
        }));
        assert_eq!(t.scrollback.frozen_blocks()[0].exit, Some(2));
        let after = t.render(&mut cart, w, h, &mut gs, &sheet, &mut 0, None);
        assert_eq!(t.heights[0].1, Some(2), "the cache re-keyed on the exit");
        assert!(
            after > before,
            "the badge line grew the content ({after} > {before})"
        );
        assert_eq!(after, full_height(&t, w, &mut gs, &sheet));
    }

    #[test]
    fn a_mark_drags_the_view_to_its_row_and_the_frame_maps_every_block() {
        // H-4d: the Normal-mode cursor is visible after every render (Helix:
        // the view follows the cursor), the block frame covers the whole
        // history in transcript order (the click hit map), and a marked row
        // paints its band.
        let mut gs = GlyphSource::new_vendored(512);
        let sheet = crate::layout::daylight_sheet(100);
        let (cw, ch, _) = gs.mono_cell();
        let mut t = history_tile(20, 4, 1000);
        push_history(&mut t, 40, 3, 'm');
        let ids: Vec<u64> = t.scrollback.frozen_blocks().iter().map(|b| b.id).collect();
        let mut cart = Cartoon::new();
        // A 12-row view over a 4-row grid: eight rows of history show.
        let (w, h) = ((20 * cw) as usize, (12 * ch) as usize);
        let viewh = h as i32;
        let band = |cart: &Cartoon| {
            cart.ops
                .iter()
                .any(|o| matches!(o, Op::Rect { color, .. } if *color == sheet.sel_bg))
        };

        // Unmarked, at the bottom: every block is in the frame, in order, y
        // ascending without overlap; the oldest is above the view, the
        // newest frozen one in it; no band.
        let mut su = 0;
        t.render(&mut cart, w, h, &mut gs, &sheet, &mut su, None);
        assert_eq!(su, 0);
        // ... then the open block, then the live grid (the virtual trailing block).
        let want: Vec<u64> = ids.iter().copied().chain([u64::MAX, GRID_KEY]).collect();
        assert_eq!(t.frame.iter().map(|f| f.0).collect::<Vec<_>>(), want);
        assert!(
            t.frame.windows(2).all(|p| p[0].1 + p[0].2 <= p[1].1),
            "frame y ascends without overlap"
        );
        assert!(
            t.frame[0].1 + t.frame[0].2 <= 0,
            "the oldest block is above the view"
        );
        let newest = t.frame[ids.len() - 1];
        assert!(
            newest.1 >= 0 && newest.1 + newest.2 <= viewh,
            "the newest frozen block is in view: y={} h={}",
            newest.1,
            newest.2
        );
        assert_eq!(t.hit(newest.1), Some((newest.0, newest.1)));
        assert_eq!(t.hit(newest.1 + newest.2 - 1), Some((newest.0, newest.1)));
        assert_eq!(
            t.hit(t.frame[0].1 - 1),
            None,
            "the leading gap hits nothing"
        );
        assert!(!band(&cart), "no band without a mark");

        // A mark on the oldest block's first line drags the view up to it and
        // paints the band.
        let mark = Mark {
            block: ids[0],
            item: 0,
            row: usize::MAX,
            obj: None,
        };
        t.render(&mut cart, w, h, &mut gs, &sheet, &mut su, Some(mark));
        assert!(su > 0, "the view scrolled up: {}", su);
        // The MARKED ROW is in view -- not the whole block. row=usize::MAX
        // resolves (via laid_line_for) to item 0's line, a proportional body
        // line now (14.13, ~15px), SHORTER than the mono cell (`ch`=22), so
        // `ch` is no longer a valid row-height proxy; a 3-line block trails
        // below the row and need not fit. Take the row's actual span.
        let oldest = t.frame[0];
        let olb = layout_block(
            t.scrollback.frozen_blocks().front().unwrap(),
            w as i32,
            &sheet,
            &mut gs,
        );
        let (ory, orh) = laid_line_for(&olb, 0, usize::MAX).unwrap();
        assert!(
            oldest.1 + ory >= 0 && oldest.1 + ory + orh <= viewh,
            "the oldest block's marked row is in view: rowy={} rowh={}",
            oldest.1 + ory,
            orh
        );
        assert!(band(&cart), "the marked row paints its band");

        // A mark back on the newest frozen block drags the view down again:
        // the marked row is visible, and the offset written back is within
        // the content.
        let mark = Mark {
            block: *ids.last().unwrap(),
            item: 0,
            row: usize::MAX,
            obj: None,
        };
        t.render(&mut cart, w, h, &mut gs, &sheet, &mut su, Some(mark));
        let newest = t.frame[ids.len() - 1];
        let nlb = layout_block(
            t.scrollback.frozen_blocks().back().unwrap(),
            w as i32,
            &sheet,
            &mut gs,
        );
        let (nry, nrh) = laid_line_for(&nlb, 0, usize::MAX).unwrap();
        assert!(
            newest.1 + nry >= 0 && newest.1 + nry + nrh <= viewh,
            "the newest block's marked row is back in view: rowy={} rowh={}",
            newest.1 + nry,
            nrh
        );
        assert!(
            (0..viewh * 40).contains(&su),
            "the offset is clamped: {}",
            su
        );
        assert!(band(&cart));
    }

    #[test]
    fn grid_cells_carry_their_obj_runs_and_scroll_them_into_the_landing_block() {
        // H-4d: a cell stamped with a frame's serial resolves to the span
        // state after that frame -- an obj run on the LIVE GRID (the virtual
        // trailing block), keyed by its start column; when the row scrolls
        // off into a LATER block than the obj's, the obj is copied into the
        // landing block so the row's run resolves there too.
        let cs = |ch: char, span: u32| Cell {
            ch,
            fg: 0xFFFFFF,
            bg: 0,
            attrs: 0,
            span,
        };
        let mut t = daylight_tile(8, 2);
        // Content before the obj, so the zone cut below freezes a block.
        t.apply(Record::ScrollOff {
            rows: vec![vec![cs('p', 0), cs('q', 0)]],
            wrapped: vec![false],
        });
        let b0 = t.scrollback.open_block().id;
        t.apply(Record::Control(Control::Osc1936Raw {
            serial: 1,
            frame: b"\x1b]1936;v1;obj;type=path;ref=/bin\x1b\\".to_vec(),
        }));
        assert_eq!(
            t.spans.get(1),
            Some(SpanTag {
                block: b0,
                obj: 1,
                em: 0,
                hdr: 0
            })
        );
        t.apply(Record::CellDiff {
            changed: vec![(0, 0, cs('b', 1)), (0, 1, cs('i', 1)), (0, 2, cs('n', 1))],
            cursor: (0, 3, true),
            wrapped: vec![],
            top_continues: false,
        });
        t.apply(Record::Control(Control::Osc1936Raw {
            serial: 2,
            frame: b"\x1b]1936;v1;/obj\x1b\\".to_vec(),
        }));
        t.apply(Record::CellDiff {
            changed: vec![(0, 4, cs('x', 2))],
            cursor: (0, 5, true),
            wrapped: vec![],
            top_continues: false,
        });
        let runs = t.grid_runs(0);
        assert_eq!(runs.len(), 1, "one run on the grid row: {:?}", runs);
        assert_eq!((runs[0].obj, runs[0].text.as_str()), (1, "bin"));
        assert_eq!(t.grid_run(0, 1).map(|(c, n, _)| (c, n)), Some((0, 3)));
        assert_eq!(t.grid_run_obj(0, 1), Some(("path", "/bin")));
        assert_eq!(t.grid_hit(15, 3, 10, 20), Some((0, 1)), "col 1 of the run");
        assert_eq!(t.grid_hit(45, 3, 10, 20), None, "the x after the close");
        assert!(t.grid_runs(1).is_empty());

        // A zone-open freezes b0 (it has content) and opens b1; the row then
        // scrolls off INTO b1 carrying b0's obj: copied, and resolvable.
        t.apply(Record::Control(Control::Osc1936Raw {
            serial: 3,
            frame: b"\x1b]1936;v1;zone;k=prompt\x1b\\".to_vec(),
        }));
        assert_ne!(t.scrollback.open_block().id, b0, "the zone cut froze b0");
        t.apply(Record::ScrollOff {
            rows: vec![vec![
                cs('b', 1),
                cs('i', 1),
                cs('n', 1),
                cs(' ', 0),
                cs('x', 2),
            ]],
            wrapped: vec![false],
        });
        let fr = crate::select::FlatRow {
            block: usize::MAX,
            item: 0,
            row: usize::MAX,
        };
        let runs = crate::menu::runs_on_row(&t.scrollback, fr);
        assert_eq!(runs.len(), 1, "the scrolled row keeps its run: {:?}", runs);
        assert_eq!(runs[0].text, "bin");
        assert_eq!(
            crate::menu::obj_of(&t.scrollback, usize::MAX, runs[0].obj),
            Some(("path", "/bin")),
            "the obj was copied into the landing block"
        );
        assert_eq!(t.scrollback.open_block().objs.len(), 1);
        assert_eq!(
            t.scrollback.block_by_id(b0).map(|b| b.objs.len()),
            Some(1),
            "the source block keeps its own"
        );
    }

    #[test]
    fn grid_hit_inverts_a_proportional_click_to_the_run() {
        // PL-4b-ii-b: after a proportional render, a click on the live tail
        // inverts through the CACHED layout (not the mono cell grid) to the
        // right grid run, and the run's display rect is its real x-extent.
        let cs = |ch: char, span: u32| Cell {
            ch,
            fg: 0xFFFFFF,
            bg: 0,
            attrs: 0,
            span,
        };
        let mut gs = GlyphSource::new_vendored(512);
        let sheet = crate::layout::daylight_sheet(100);
        let mut t = daylight_tile(16, 4);
        // An obj run "bin" on grid row 0 (cols 0..3, serial 1), then a plain
        // 'x' at col 4 (serial 2, obj closed) -- the mono test's shape.
        t.apply(Record::Control(Control::Osc1936Raw {
            serial: 1,
            frame: b"\x1b]1936;v1;obj;type=path;ref=/bin\x1b\\".to_vec(),
        }));
        t.apply(Record::CellDiff {
            changed: vec![(0, 0, cs('b', 1)), (0, 1, cs('i', 1)), (0, 2, cs('n', 1))],
            cursor: (0, 3, true),
            wrapped: vec![],
            top_continues: false,
        });
        t.apply(Record::Control(Control::Osc1936Raw {
            serial: 2,
            frame: b"\x1b]1936;v1;/obj\x1b\\".to_vec(),
        }));
        t.apply(Record::CellDiff {
            changed: vec![(0, 4, cs('x', 2))],
            cursor: (0, 5, true),
            wrapped: vec![],
            top_continues: false,
        });
        // Render populates the proportional cache (live_laid Some).
        let mut cart = Cartoon::new();
        let (w, h) = (16 * 8, 4 * 20);
        t.render(&mut cart, w, h, &mut gs, &sheet, &mut 0, None);

        // The run's tail-relative rect is proportional (a real x-extent, one
        // laid line high), and its centre inverts back to (row 0, key 1).
        let (rx, ry, rw, rh) = t.grid_run_rect(0, 1, 8, 20).expect("the run has a rect");
        assert!(rw > 0 && rh > 0, "a non-empty proportional rect: {rw}x{rh}");
        let (cx, cy) = (rx + rw / 2, ry + rh / 2);
        assert_eq!(
            t.grid_hit(cx, cy, 8, 20),
            Some((0, 1)),
            "a click on 'bin' inverts to its run"
        );
        assert_eq!(
            t.grid_run_obj(0, 1),
            Some(("path", "/bin")),
            "the returned key resolves the obj"
        );
        // A click far right of every glyph, or below the trimmed tail, hits no
        // run (the proportional inverse finds no cell there).
        assert_eq!(t.grid_hit(w as i32 - 1, cy, 8, 20), None, "past the content");
        assert_eq!(t.grid_hit(cx, h as i32 - 1, 8, 20), None, "below the tail");

        // Alt-screen drops the cache -> grid_hit falls back to the mono grid
        // (the same (row, key) by cell), and grid_run_rect to the mono cell.
        t.apply(Record::Mode(ScreenMode::AltScreen));
        repaint_unchanged(&mut t);
        t.render(&mut cart, w, h, &mut gs, &sheet, &mut 0, None);
        assert_eq!(
            t.grid_hit(5, 5, 8, 20),
            Some((0, 1)),
            "alt-screen: mono hit on 'bin'"
        );
        assert_eq!(
            t.grid_run_rect(0, 1, 8, 20),
            Some((0, 0, 24, 20)),
            "alt-screen: the mono 3-cell rect"
        );
    }

    #[test]
    fn render_blank_daylight_grid_skips_bg_rects() {
        // A blank Daylight grid: every cell's bg == the sheet ground, so no bg
        // Rect is emitted (only the Clear) -- the paint_grid ground-skip. The
        // cursor beam is one Rect, so exactly one Rect total (the cursor).
        let mut gs = GlyphSource::new_vendored(512);
        let sheet = crate::layout::daylight_sheet(100);
        let (cw, ch, _) = gs.mono_cell();
        let mut t = daylight_tile(8, 2);
        t.apply(Record::Mode(ScreenMode::AltScreen)); // grid only, no scrollback flow
        repaint_unchanged(&mut t);
        let mut cart = Cartoon::new();
        let (w, h) = ((8 * cw) as usize, (2 * ch) as usize);
        t.render(&mut cart, w, h, &mut gs, &sheet, &mut 0, None);
        let rects = cart
            .ops
            .iter()
            .filter(|o| matches!(o, Op::Rect { .. }))
            .count();
        assert_eq!(rects, 1, "only the cursor beam Rect (blank cells skip bg)");
    }

    // --- the composition-round audit F1: a click on a rebuilt structure ---
    //
    // In cells mode every row of one rebuilt table (every line of one pre)
    // shares an ITEM; the provenance must carry the ROW, and the laid runs
    // their SOURCE columns, or the inverse names the structure's first row
    // for every click on it (`ps`: a click on row N's pid opened row 0's; a
    // `kill` from that menu killed the wrong process).

    fn cs(ch: char, span: u32) -> Cell {
        Cell {
            ch,
            fg: 0xFFFFFF,
            bg: 0,
            attrs: 0,
            span,
        }
    }

    fn frame(t: &mut Tile, serial: u32, body: &[u8]) {
        let mut f = Vec::new();
        f.extend_from_slice(b"\x1b]1936;v1;");
        f.extend_from_slice(body);
        f.extend_from_slice(b"\x1b\\");
        t.apply(Record::Control(Control::Osc1936Raw { serial, frame: f }));
    }

    fn write(t: &mut Tile, cells: Vec<(u16, u16, Cell)>, cur: (u16, u16)) {
        t.apply(Record::CellDiff {
            changed: cells,
            cursor: (cur.0, cur.1, true),
            wrapped: vec![],
            top_continues: false,
        });
    }

    /// A `ps`-shaped Beacon table on the LIVE grid: two rows, an obj (pid)
    /// cell in column 0 of each. A click on row 1's pid opens row 1's
    /// object; row 1's run rect is row 1's laid line.
    #[test]
    fn live_grid_table_click_hits_the_clicked_row() {
        let mut gs = GlyphSource::new_vendored(512);
        let sheet = crate::layout::daylight_sheet(100);
        let mut t = Tile::new(40, 4, libhalcyon::theme::daylight_palette());
        frame(&mut t, 1, b"zone;k=output");
        frame(&mut t, 2, b"table;cols=lr;hdr=0");
        frame(&mut t, 3, b"row");
        frame(&mut t, 4, b"cell");
        frame(&mut t, 5, b"obj;type=pid;ref=100");
        write(&mut t, vec![(0, 0, cs('1', 5)), (0, 1, cs('0', 5)), (0, 2, cs('0', 5))], (0, 3));
        frame(&mut t, 6, b"/obj");
        frame(&mut t, 7, b"/cell");
        write(&mut t, vec![(0, 3, cs(' ', 7)), (0, 4, cs(' ', 7))], (0, 5));
        frame(&mut t, 8, b"cell");
        write(&mut t, vec![(0, 5, cs('u', 8)), (0, 6, cs('t', 8))], (0, 7));
        frame(&mut t, 9, b"/cell");
        frame(&mut t, 10, b"/row");
        frame(&mut t, 11, b"row");
        frame(&mut t, 12, b"cell");
        frame(&mut t, 13, b"obj;type=pid;ref=200");
        write(&mut t, vec![(1, 0, cs('2', 13)), (1, 1, cs('0', 13)), (1, 2, cs('0', 13))], (1, 3));
        frame(&mut t, 14, b"/obj");
        frame(&mut t, 15, b"/cell");
        write(&mut t, vec![(1, 3, cs(' ', 15)), (1, 4, cs(' ', 15))], (1, 5));
        frame(&mut t, 16, b"cell");
        write(&mut t, vec![(1, 5, cs('s', 16)), (1, 6, cs('h', 16))], (1, 7));
        frame(&mut t, 17, b"/cell");
        frame(&mut t, 18, b"/row");
        frame(&mut t, 19, b"/table");
        write(&mut t, vec![], (2, 0));

        // The keyboard path (cell spans) names each row's own object.
        assert_eq!(t.grid_run_obj(1, 1), Some(("pid", "200")));
        assert_eq!(t.grid_run_obj(0, 1), Some(("pid", "100")));

        let mut cart = Cartoon::new();
        let (cw, ch, _) = gs.mono_cell();
        let (w, h) = (320usize, (4 * ch) as usize);
        t.render(&mut cart, w, h, &mut gs, &sheet, &mut 0, None);

        // The live block IS a rebuilt table, both rows on one item.
        let (lb, prov) = t.scrollback.live_block(
            t.grid.cells(),
            40,
            t.grid.content_rows(),
            t.grid.wrapped(),
            &t.spans,
        false,
        );
        assert!(matches!(lb.items[0], Item::Table(_)), "the live grid rebuilt the table");
        assert_eq!((prov[0].0, prov[0].1), (0, 0));
        assert_eq!((prov[1].0, prov[1].1), (0, 1), "the same item, the next ROW");
        let laid = crate::layout::layout_block(&lb, w as i32, &sheet, &mut gs);
        let row1 = laid.lines.iter().find(|l| l.src_row == 1).expect("row 1 laid");
        let pid_seg = row1.segs.iter().find(|s| s.obj != 0).expect("row 1's pid seg");
        assert_eq!(pid_seg.src_col, 0, "the pid cell starts at grid column 0");
        let sh_seg = row1.segs.iter().find(|s| s.obj == 0).expect("row 1's second cell");
        assert_eq!(sh_seg.src_col, 5, "the second cell starts at its grid column");
        let (x, y) = ((pid_seg.x + pid_seg.x_end) / 2, row1.y + row1.h / 2);

        // The click on row 1's pid glyphs.
        let hit = t.grid_hit(x, y, cw, ch);
        let obj = hit.and_then(|(r, k)| t.grid_run_obj(r, k).map(|(a, b)| (r, a, b)));
        assert_eq!(
            obj,
            Some((1usize, "pid", "200")),
            "a click on row 1's pid opens row 1's object (hit {:?})",
            hit
        );
        // And row 1's run rect sits on row 1's laid line, not row 0's.
        let r0 = t.grid_run_rect(0, 1, cw, ch).expect("row 0 rect");
        let r1 = t.grid_run_rect(1, 1, cw, ch).expect("row 1 rect");
        assert_ne!(r0.1, r1.1, "row 0 and row 1 rects sit on different laid lines: {:?} vs {:?}", r0, r1);
        assert_eq!(r1.1, row1.y, "row 1's rect is row 1's laid line");
    }

    /// The `la` shape: a `pre` box on the live grid with an obj path per
    /// row. A click on row 1's path opens row 1's object.
    #[test]
    fn live_grid_pre_click_hits_the_clicked_row() {
        let mut gs = GlyphSource::new_vendored(512);
        let sheet = crate::layout::daylight_sheet(100);
        let mut t = Tile::new(40, 4, libhalcyon::theme::daylight_palette());
        frame(&mut t, 1, b"pre");
        write(&mut t, vec![(0, 0, cs('|', 1)), (0, 1, cs(' ', 1))], (0, 2));
        frame(&mut t, 2, b"obj;type=path;ref=/aa");
        write(&mut t, vec![(0, 2, cs('a', 2)), (0, 3, cs('a', 2))], (0, 4));
        frame(&mut t, 3, b"/obj");
        write(&mut t, vec![(0, 4, cs(' ', 3)), (0, 5, cs('|', 3))], (0, 6));
        write(&mut t, vec![(1, 0, cs('|', 3)), (1, 1, cs(' ', 3))], (1, 2));
        frame(&mut t, 4, b"obj;type=path;ref=/bb");
        write(&mut t, vec![(1, 2, cs('b', 4)), (1, 3, cs('b', 4))], (1, 4));
        frame(&mut t, 5, b"/obj");
        write(&mut t, vec![(1, 4, cs(' ', 5)), (1, 5, cs('|', 5))], (1, 6));
        frame(&mut t, 6, b"/pre");
        write(&mut t, vec![], (2, 0));
        assert_eq!(t.grid_run_obj(1, 3), Some(("path", "/bb")));

        let mut cart = Cartoon::new();
        let (cw, ch, _) = gs.mono_cell();
        let (w, h) = (320usize, (4 * ch) as usize);
        t.render(&mut cart, w, h, &mut gs, &sheet, &mut 0, None);
        let (lb, prov) = t.scrollback.live_block(
            t.grid.cells(),
            40,
            t.grid.content_rows(),
            t.grid.wrapped(),
            &t.spans,
        false,
        );
        assert!(matches!(lb.items[0], Item::Pre(_)), "the live grid rebuilt the pre");
        assert_eq!(prov[1].0, prov[0].0, "both grid rows map to the ONE pre item");
        assert_eq!((prov[0].1, prov[1].1), (0, 1), "each names its pre line");
        let laid = crate::layout::layout_block(&lb, w as i32, &sheet, &mut gs);
        let pre_lines: Vec<&crate::layout::LaidLine> =
            laid.lines.iter().filter(|l| l.src_item == 0).collect();
        assert_eq!(pre_lines.len(), 2);
        assert_eq!((pre_lines[0].src_row, pre_lines[1].src_row), (0, 1), "pre lines carry their row");
        let row1 = pre_lines[1];
        let seg = row1.segs.iter().find(|s| s.obj != 0).expect("row 1's obj seg");
        let (x, y) = ((seg.x + seg.x_end) / 2, row1.y + row1.h / 2);
        let hit = t.grid_hit(x, y, cw, ch);
        let obj = hit.and_then(|(r, k)| t.grid_run_obj(r, k).map(|(a, b)| (r, a, b)));
        assert_eq!(
            obj,
            Some((1usize, "path", "/bb")),
            "a click on row 1's path opens /bb (hit {:?})",
            hit
        );
    }

    /// Audit F3, through the tile: `rule`, a text line, a newline, then an
    /// `em` open starting the next line -- the text line's cells ended the
    /// rule episode, so the open carries no rule and one rule frame places
    /// ONE rule on the live grid.
    #[test]
    fn one_rule_frame_places_one_rule_on_the_live_grid() {
        let mut t = Tile::new(20, 4, libhalcyon::theme::daylight_palette());
        frame(&mut t, 1, b"rule");
        assert_eq!(t.scrollback.rule_open(), Some(1));
        write(&mut t, vec![(0, 0, cs('a', 1)), (0, 1, cs('b', 1))], (1, 0));
        assert_eq!(t.scrollback.rule_open(), None, "cells after the rule end its episode");
        frame(&mut t, 2, b"em;class=dim");
        write(&mut t, vec![(1, 0, cs('x', 2)), (1, 1, cs('y', 2))], (1, 2));
        frame(&mut t, 3, b"/em");
        let (lb, _) = t.scrollback.live_block(
            t.grid.cells(),
            20,
            t.grid.content_rows(),
            t.grid.wrapped(),
            &t.spans,
        false,
        );
        let rules = lb.items.iter().filter(|i| matches!(i, Item::Rule)).count();
        assert_eq!(rules, 1, "one rule frame, one rule; items {}", lb.items.len());
        assert!(matches!(lb.items[0], Item::Rule), "the rule precedes the text line");
    }
    /// HALCYON-INSTRUMENT 14.6: a retained tile paints its state under the
    /// Instrument profile -- no caret; an ended tile's `Process ended`
    /// line in `secondary`; a disconnected tile's notice strip (`header`
    /// ground at least 32 tall, the `error` `!`) -- and under the legacy
    /// profile a fate changes NOTHING (the frozen affordance is what it
    /// was, op for op).
    #[test]
    fn a_retained_tile_paints_its_state_under_instrument_and_nothing_new_under_legacy() {
        use crate::chrome::Fate;
        let mut gs = GlyphSource::new_vendored(512);
        let inst = crate::layout::sheet_for(
            &libhalcyon::instrument::Bundle::builtin(libhalcyon::instrument::Profile::Instrument),
            100,
            crate::layout::TEST_DISPLAY_W,
        );
        let legacy = crate::layout::daylight_sheet(100);
        let (_, ch, _) = gs.mono_cell();
        let mk = || {
            let mut t = daylight_tile(20, 8);
            t.apply(Record::CellDiff {
                changed: vec![(0, 0, cell('h')), (0, 1, cell('i'))],
                cursor: (0, 2, true),
                wrapped: vec![],
                top_continues: false,
            });
            t
        };
        let (w, h) = (20 * 8, (8 * ch) as usize);
        let render = |t: &mut Tile, sheet: &Sheet, gs: &mut GlyphSource| {
            let mut c = Cartoon::new();
            t.render(&mut c, w, h, gs, sheet, &mut 0, None);
            c
        };
        let caret = |c: &Cartoon, accent: u32| {
            c.ops.iter().any(|op| matches!(op, Op::Rect { w: 2, color, .. } if *color == accent))
        };
        // Legacy: op for op the same with a fate as without (the ops
        // projected to tuples -- `Op` carries no Debug or Eq).
        let key = |c: &Cartoon| -> (Vec<(u8, i64, i64, i64, i64, i64, i64)>, Vec<(u32, i32)>) {
            let ops = c
                .ops
                .iter()
                .map(|op| match *op {
                    Op::Clear { color } => (0, color as i64, 0, 0, 0, 0, 0),
                    Op::Rect { x, y, w, h, color } => (1, x as i64, y as i64, w as i64, h as i64, color as i64, 0),
                    Op::Glyphs { atlas_gen, baseline_x, baseline_y, color, start, count } => {
                        (2, atlas_gen as i64, baseline_x as i64, baseline_y as i64, color as i64, start as i64, count as i64)
                    }
                    Op::Image { blob_id, x, y, w, h } => (3, blob_id as i64, x as i64, y as i64, w as i64, h as i64, 0),
                    Op::Embed { surface_ref, x, y, w, h } => (4, surface_ref as i64, x as i64, y as i64, w as i64, h as i64, 0),
                    Op::RectAlpha { x, y, w, h, color, alpha } => (5, x as i64, y as i64, w as i64, h as i64, color as i64, alpha as i64),
                    // Seven fields into six slots: alpha and radius share
                    // the last, which is a projection for equality only.
                    Op::Glow { x, y, w, h, color, alpha, radius } => {
                        (6, x as i64, y as i64, w as i64, h as i64, color as i64, ((alpha as i64) << 32) | radius as i64)
                    }
                    Op::Blur { x, y, w, h, radius } => {
                        (7, x as i64, y as i64, w as i64, h as i64, 0, radius as i64)
                    }
                })
                .collect();
            let runs = c.runs.iter().map(|r| (r.glyph, r.advance)).collect();
            (ops, runs)
        };
        let live = render(&mut mk(), &legacy, &mut gs);
        for fate in [Fate::Ended(3), Fate::Disconnected, Fate::Crashed] {
            let mut t = mk();
            t.fate = fate;
            let c = render(&mut t, &legacy, &mut gs);
            assert_eq!(key(&c), key(&live), "legacy unchanged under {:?}", fate);
        }
        assert!(caret(&live, legacy.accent));
        // Instrument, live: the caret.
        let c = render(&mut mk(), &inst, &mut gs);
        assert!(caret(&c, inst.accent));
        assert!(!c.ops.iter().any(|op| matches!(op, Op::Glyphs { color, .. } if *color == inst.inst.secondary)));
        // Ended: no caret, the final line.
        let mut t = mk();
        t.fate = Fate::Ended(3);
        let c = render(&mut t, &inst, &mut gs);
        assert!(!caret(&c, inst.accent), "no caret on a retained tile");
        assert!(
            c.ops.iter().any(|op| matches!(op, Op::Glyphs { color, .. } if *color == inst.inst.secondary)),
            "the `Process ended` line in secondary"
        );
        // Disconnected: the strip over the top, no caret.
        let mut t = mk();
        t.fate = Fate::Disconnected;
        let c = render(&mut t, &inst, &mut gs);
        assert!(!caret(&c, inst.accent));
        assert!(
            c.ops.iter().any(|op| matches!(op, Op::Rect { x: 0, y: 0, h, color, .. } if *color == inst.inst.header && *h >= 32)),
            "the notice strip's ground"
        );
        assert!(c.ops.iter().any(|op| matches!(op, Op::Glyphs { color, .. } if *color == inst.inst.error)), "the `!`");
        assert!(c.ops.iter().any(|op| matches!(op, Op::Rect { h: 1, color, .. } if *color == inst.inst.separator)), "its rule");
        // Crashed: no caret, no strip, no line -- the metadata carries the word.
        let mut t = mk();
        t.fate = Fate::Crashed;
        let c = render(&mut t, &inst, &mut gs);
        assert!(!caret(&c, inst.accent));
        assert!(!c.ops.iter().any(|op| matches!(op, Op::Rect { x: 0, y: 0, color, .. } if *color == inst.inst.header)));
    }
    /// Section 10's caret blink, at the seam where a STEP becomes a REPAINT.
    ///
    /// The second block pins 14.6 to an expectation read off the DOCUMENT,
    /// and it has to. `paints_caret` is both what the dirty rule consults and
    /// what the painter calls, so asserting the two AGREE is asserting a
    /// function equals itself -- it cannot fail, whatever the predicate says.
    /// Only an independent `want` catches a predicate that drifted, and the
    /// drift that matters is the retained tile: get it wrong and every dead
    /// child's tile repaints about twice a second, forever, to show a caret
    /// it does not have.
    #[test]
    fn the_caret_blink_reaches_the_paint_and_its_predicate_matches_it() {
        use crate::chrome::Fate;
        let mut gs = GlyphSource::new_vendored(512);
        let inst = crate::layout::sheet_for(
            &libhalcyon::instrument::Bundle::builtin(libhalcyon::instrument::Profile::Instrument),
            100,
            crate::layout::TEST_DISPLAY_W,
        );
        let legacy = crate::layout::daylight_sheet(100);
        let (_, ch, _) = gs.mono_cell();
        let (w, h) = (20 * 8, (8 * ch) as usize);
        let mk = |cursor_on: bool| {
            let mut t = daylight_tile(20, 8);
            t.apply(Record::CellDiff {
                changed: vec![(0, 0, cell('h')), (0, 1, cell('i'))],
                cursor: (0, 2, cursor_on),
                wrapped: vec![],
                top_continues: false,
            });
            t
        };
        let beam = |t: &mut Tile, sheet: &Sheet, gs: &mut GlyphSource| {
            let mut c = Cartoon::new();
            t.render(&mut c, w, h, gs, sheet, &mut 0, None);
            c.ops
                .iter()
                .any(|op| matches!(op, Op::Rect { w: 2, color, .. } if *color == sheet.accent))
        };

        // Both directions of the step. The first assertion alone would pass
        // on a painter that ignored the field entirely.
        let mut t = mk(true);
        assert!(beam(&mut t, &legacy, &mut gs), "the resting value is UP -- 9.5's static caret is PAINTED");
        t.caret_on = false;
        assert!(!beam(&mut t, &legacy, &mut gs), "the off half of the step hides it");
        t.caret_on = true;
        assert!(beam(&mut t, &legacy, &mut gs), "and the next cycle brings it back");

        // 14.6 + DECTCEM, stated independently of the code that implements
        // them, then required of BOTH the predicate and the beam.
        for fate in [Fate::Live, Fate::Ended(3), Fate::Disconnected, Fate::Crashed] {
            for cursor_on in [true, false] {
                for (sheet, is_inst) in [(&legacy, false), (&inst, true)] {
                    let want = cursor_on && !(is_inst && fate != Fate::Live);
                    let mut t = mk(cursor_on);
                    t.fate = fate;
                    t.caret_on = true;
                    assert_eq!(
                        t.paints_caret(is_inst),
                        want,
                        "14.6 at {:?} cursor={} inst={}",
                        fate,
                        cursor_on,
                        is_inst
                    );
                    assert_eq!(
                        beam(&mut t, sheet, &mut gs),
                        want,
                        "the beam follows it at {:?} cursor={} inst={}",
                        fate,
                        cursor_on,
                        is_inst
                    );
                }
            }
        }

        // A step dirties only a tile that can show it, and stores the phase
        // either way.
        let mut live = mk(true);
        assert!(live.set_caret_on(false, true), "a live tile with a cursor repaints");
        assert!(!live.set_caret_on(false, true), "the same step twice is not a second repaint");
        let mut hidden = mk(false);
        assert!(!hidden.set_caret_on(false, true), "no cursor to mark, so no repaint");
        assert!(!hidden.caret_on, "but the phase was stored, so it reappears in step");
        // The same tile, the same step, opposite profiles, opposite answers:
        // 14.6 suppresses a retained tile's caret under Instrument ONLY.
        let mut retained = mk(true);
        retained.fate = Fate::Ended(0);
        assert!(!retained.set_caret_on(false, true), "a retained tile has no caret under Instrument");
        retained.caret_on = true;
        assert!(retained.set_caret_on(false, false), "the legacy frozen affordance keeps its caret");
    }

    /// The pre-I-5b legacy RENDER, pinned as a fingerprint over the cartoon
    /// (every op's geometry and colour, the glyph runs' advances -- never
    /// an atlas id, which packing order owns) for a history tile in normal
    /// mode with a mark, and in alt-screen. Read off 69f71541 with the
    /// constants at 0; the I-5b paddings, inks and grounds must leave it.
    fn fp_cart(h: &mut u64, c: &Cartoon) {
        use crate::layout::tests::{fnv, fp_i32, fp_u32};
        fp_u32(h, c.ops.len() as u32);
        for op in c.ops.iter() {
            match *op {
                Op::Clear { color } => {
                    fnv(h, b"C");
                    fp_u32(h, color);
                }
                Op::Rect { x, y, w, h: rh, color } => {
                    fnv(h, b"R");
                    fp_i32(h, x);
                    fp_i32(h, y);
                    fp_u32(h, w);
                    fp_u32(h, rh);
                    fp_u32(h, color);
                }
                Op::Glyphs { baseline_x, baseline_y, color, start, count, .. } => {
                    fnv(h, b"G");
                    fp_i32(h, baseline_x);
                    fp_i32(h, baseline_y);
                    fp_u32(h, color);
                    for r in c.runs[start as usize..(start + count) as usize].iter() {
                        fp_i32(h, r.advance);
                    }
                }
                _ => fnv(h, b"?"),
            }
        }
    }

    fn cart_fingerprint(c: &Cartoon) -> u64 {
        let mut h: u64 = 0xcbf29ce484222325;
        fp_cart(&mut h, c);
        h
    }

    fn render_fingerprint(sheet: &Sheet) -> u64 {
        use crate::layout::tests::fp_i32;
        let mut gs = GlyphSource::new_vendored(512);
        let mut h: u64 = 0xcbf29ce484222325;
        let mut hash_cart = |c: &Cartoon, content: i32, su: i32| {
            fp_i32(&mut h, content);
            fp_i32(&mut h, su);
            fp_cart(&mut h, c);
        };
        let (cw, ch, _) = gs.mono_cell();
        let mut t = history_tile(24, 8, 64);
        push_history(&mut t, 12, 3, 'h');
        t.apply(Record::CellDiff {
            changed: vec![(0, 0, cell('l')), (0, 1, cell('i')), (0, 2, cell('v')), (0, 3, cell('e')), (2, 0, cell('x'))],
            cursor: (2, 1, true),
            wrapped: vec![],
            top_continues: false,
        });
        let (w, hh) = ((24 * cw) as usize, (8 * ch) as usize);
        for (su0, mark) in [
            (0, None),
            (37, None),
            (0, Some(Mark { block: GRID_KEY, item: 0, row: usize::MAX, obj: None })),
        ] {
            let mut cart = Cartoon::new();
            let mut su = su0;
            let content = t.render(&mut cart, w, hh, &mut gs, sheet, &mut su, mark);
            hash_cart(&cart, content, su);
        }
        t.apply(Record::Mode(ScreenMode::AltScreen));
        repaint_unchanged(&mut t);
        let mut cart = Cartoon::new();
        let content = t.render(&mut cart, w, hh, &mut gs, sheet, &mut 0, None);
        hash_cart(&cart, content, 0);
        h
    }

    // The producer follows every mode flip with its repaint of the whole
    // grid; this one repaints nothing, so a test keeps the cells it set up.
    fn repaint_unchanged(t: &mut Tile) {
        let (r, c, v) = t.grid.cursor();
        t.apply(Record::CellDiff {
            changed: vec![],
            cursor: (r as u16, c as u16, v),
            wrapped: vec![],
            top_continues: false,
        });
    }

    #[test]
    fn legacy_render_is_byte_identical_to_the_pre_i5b_tree() {
        let fp = render_fingerprint(&crate::layout::daylight_sheet(100));
        assert_eq!(fp, LEGACY_RENDER_FP, "legacy render drifted: got {fp:#018x}");
    }

    const LEGACY_RENDER_FP: u64 = 0xcc2ecb507103e963;
    // ---- I-5b: the document in the tile (7.5 / 7.7 / 14.7) ----

    fn inst_sheet() -> Sheet {
        crate::layout::sheet_for(
            &libhalcyon::instrument::Bundle::builtin(libhalcyon::instrument::Profile::Instrument),
            100,
            crate::layout::TEST_DISPLAY_W,
        )
    }

    /// 7.5 / 7.7: the Instrument tile pads the flow (28 / 38 / 50 at the
    /// reference display) and, once the content overflows, reserves the 8
    /// px lane -- the blocks re-lay narrower -- and paints the `dim` thumb
    /// ending 4 above the view's bottom while following the tail, higher
    /// while in history. Nothing of it under legacy.
    #[test]
    fn the_instrument_tile_pads_the_document_and_indicates_its_position_on_overflow() {
        let mut gs = GlyphSource::new_vendored(512);
        let s = inst_sheet();
        let (cw, ch, _) = gs.mono_cell();
        let (w, h) = ((60 * cw) as usize, (30 * ch) as usize);
        let thumb = |c: &Cartoon| -> Option<(i32, i32, i32)> {
            c.ops.iter().find_map(|op| match op {
                Op::Rect { x, y, w: 3, h, color } if *color == s.inst.dim => Some((*x, *y, *h as i32)),
                _ => None,
            })
        };
        // A short history: the flow is padded, nothing overflows.
        let mut t = history_tile(60, 30, 64);
        push_history(&mut t, 2, 1, 'h');
        let mut cart = Cartoon::new();
        let content = t.render(&mut cart, w, h, &mut gs, &s, &mut 0, None);
        assert!(content < h as i32);
        assert!(!t.lane);
        assert_eq!(t.heights_width, w as i32, "the full width");
        assert_eq!(thumb(&cart), None, "no indicator while the content fits");
        // pad_top + 2 blocks (24 + gap 15 each) + the tail + pad_bottom.
        assert!(content >= 28 + 50, "the paddings are in the content height ({content})");
        let first_glyph_y = cart.ops.iter().find_map(|op| match op {
            Op::Glyphs { baseline_y, .. } => Some(*baseline_y),
            _ => None,
        }).expect("a glyph");
        // The history is un-annotated: raw output, the terminal view -- 14
        // in, the row's 3, the ascent 11 -- under the top padding.
        assert_eq!(first_glyph_y, 28 + 14 + 3 + 11, "the first row sits under the top padding");
        // A long history: overflow.
        push_history(&mut t, 60, 1, 'h');
        let mut cart = Cartoon::new();
        let mut su = 0;
        let content = t.render(&mut cart, w, h, &mut gs, &s, &mut su, None);
        assert!(content > h as i32);
        assert!(t.lane, "the lane is reserved");
        assert_eq!(t.heights_width, w as i32 - 8, "the blocks re-laid inside the lane");
        let (x, y, th) = thumb(&cart).expect("the thumb");
        assert_eq!(x, w as i32 - 6);
        assert_eq!(y + th, h as i32 - 4, "following the tail: the end edge at V - 4");
        assert!(th >= 24);
        // In history: the thumb rises with the view.
        let mut cart = Cartoon::new();
        let mut su = 100;
        t.render(&mut cart, w, h, &mut gs, &s, &mut su, None);
        let (_, y2, th2) = thumb(&cart).expect("the thumb");
        assert!(y2 + th2 < h as i32 - 4, "not at the end while in history");
        assert_eq!(th2, th);
        // Legacy: neither the paddings nor the lane.
        let l = crate::layout::daylight_sheet(100);
        let mut t = history_tile(60, 30, 64);
        push_history(&mut t, 62, 1, 'h');
        let mut cart = Cartoon::new();
        t.render(&mut cart, w, h, &mut gs, &l, &mut 0, None);
        assert!(!t.lane);
        assert_eq!(t.heights_width, w as i32);
        assert!(!cart.ops.iter().any(|op| matches!(op, Op::Rect { w: 3, color, .. } if *color == l.inst.dim)));
    }

    /// r2 B-F1: the lane decision is BOUNDED (three passes at most, the
    /// lane winning a disagreement) and stable across frames -- the loop
    /// once rested on "narrowing never shortens", which a layout rule broke,
    /// and spun forever on ordinary content. A test that fails by hanging
    /// is not runnable, so the bound is pinned through the pass counter
    /// over a sweep of widths, and the next frame's single pass pins the
    /// stability.
    #[test]
    fn the_lane_decision_takes_at_most_three_passes_and_holds_the_next_frame() {
        let mut gs = GlyphSource::new_vendored(512);
        let s = inst_sheet();
        let (cw, ch, _) = gs.mono_cell();
        let h = (30 * ch) as usize;
        for cols in [20usize, 27, 33, 41, 48, 60, 77] {
            let w = (cols * cw as usize) + 5;
            let mut t = history_tile(60, 30, 64);
            push_history(&mut t, 60, 1, 'h');
            let mut cart = Cartoon::new();
            let content = t.render(&mut cart, w, h, &mut gs, &s, &mut 0, None);
            assert!(content > h as i32, "w={w}: the premise, an overflow");
            assert!(t.lane_passes >= 1 && t.lane_passes <= 3, "w={w}: {} passes", t.lane_passes);
            assert!(t.lane, "w={w}: an overflow reserves the lane");
            let lane = t.lane;
            let mut cart = Cartoon::new();
            t.render(&mut cart, w, h, &mut gs, &s, &mut 0, None);
            assert_eq!(t.lane_passes, 1, "w={w}: the next frame decides in one pass");
            assert_eq!(t.lane, lane, "w={w}: and keeps the decision");
        }
    }

    /// 7.7's stability: the lane decision, once taken, holds across frames
    /// (one re-lay at the flip, then a cache hit), and clears when the
    /// content fits again (a taller view).
    #[test]
    fn the_lane_decision_is_stable_across_frames_and_clears_when_the_content_fits() {
        let mut gs = GlyphSource::new_vendored(512);
        let s = inst_sheet();
        let (cw, ch, _) = gs.mono_cell();
        let (w, h) = ((60 * cw) as usize, (30 * ch) as usize);
        let mut t = history_tile(60, 30, 64);
        push_history(&mut t, 40, 1, 'h');
        let mut cart = Cartoon::new();
        t.render(&mut cart, w, h, &mut gs, &s, &mut 0, None);
        assert!(t.lane);
        let laid_first = t.laid_last;
        t.render(&mut cart, w, h, &mut gs, &s, &mut 0, None);
        assert!(t.lane);
        assert!(t.laid_last < laid_first, "the second frame lays no frozen block again ({} < {})", t.laid_last, laid_first);
        // A view tall enough for everything: the lane clears and the
        // blocks re-lay at the full width.
        t.render(&mut cart, w, h * 40, &mut gs, &s, &mut 0, None);
        assert!(!t.lane);
        assert_eq!(t.heights_width, w as i32);
    }

    /// 14.7: the raw application grid fills its rect in `terminal_bg` under
    /// Instrument (the remainder included; no rect for a cell on that
    /// ground) and an SGR italic cell is the Italic face; legacy keeps the
    /// pane surface and the Regular.
    #[test]
    fn the_alt_screen_is_terminal_bg_with_the_italic_cell_under_instrument() {
        let mut gs = GlyphSource::new_vendored(512);
        let s = inst_sheet();
        let (cw, ch, _) = gs.mono_cell();
        let mut t = Tile::new(20, 4, s.theme.terminal);
        t.apply(Record::Mode(ScreenMode::AltScreen));
        let ital = Cell { ch: 'x', fg: s.theme.terminal.fg, bg: s.theme.terminal.bg, attrs: ATTR_ITALIC, span: 0 };
        let mut roman = ital;
        roman.attrs = 0;
        roman.ch = 'y';
        t.apply(Record::CellDiff {
            changed: vec![(0, 0, ital), (0, 1, roman)],
            cursor: (0, 2, true),
            wrapped: vec![],
            top_continues: false,
        });
        let id_i = gs.glyph(crate::raster::FACE_MONO_ITALIC, s.mono_grid_px, 'x').unwrap().glyph;
        let id_r = gs.glyph(FACE_MONO, s.mono_grid_px, 'y').unwrap().glyph;
        let id_x_roman = gs.glyph(FACE_MONO, s.mono_grid_px, 'x').unwrap().glyph;
        assert_ne!(id_i, id_x_roman, "the Italic cell is a distinct raster");
        let mut cart = Cartoon::new();
        let (w, h) = ((20 * cw) as usize, (4 * ch) as usize);
        t.render(&mut cart, w, h, &mut gs, &s, &mut 0, None);
        assert!(matches!(cart.ops.first(), Some(Op::Clear { color }) if *color == s.theme.terminal.bg), "terminal_bg");
        assert!(!cart.ops.iter().any(|op| matches!(op, Op::Rect { color, .. } if *color == s.theme.terminal.bg)), "no per-cell rect on the ground");
        let ids: Vec<u32> = cart.runs.iter().map(|r| r.glyph).collect();
        assert!(ids.contains(&id_i) && ids.contains(&id_r), "the italic cell paints the Italic face, the roman the Regular ({ids:?})");
        assert!(!ids.contains(&id_x_roman));
        // Legacy: the surface, and the Regular for both.
        let l = crate::layout::daylight_sheet(100);
        let mut cart = Cartoon::new();
        t.render(&mut cart, w, h, &mut gs, &l, &mut 0, None);
        assert!(matches!(cart.ops.first(), Some(Op::Clear { color }) if *color == l.ground));
        let ids: Vec<u32> = cart.runs.iter().map(|r| r.glyph).collect();
        assert!(ids.contains(&id_x_roman) && !ids.contains(&id_i));
    }

    // --- TC-1b: the history chord (HALCYON 14.13) ---------------------------

    fn osc(serial: u32, body: &str) -> Record {
        Record::Control(Control::Osc1936Raw {
            serial,
            frame: alloc::format!("\x1b]1936;v1;{}\x1b\\", body).into_bytes(),
        })
    }

    fn grid_text(t: &Tile, r: usize) -> alloc::string::String {
        t.grid.row(r).iter().map(|c| c.ch).collect()
    }

    #[test]
    fn forget_history_keeps_the_live_screen_its_links_and_its_look() {
        let cs = |ch: char, span: u32| Cell {
            ch,
            fg: 0xFFFFFF,
            bg: 0,
            attrs: 0,
            span,
        };
        let mut t = daylight_tile(8, 2);
        t.apply(Record::ScrollOff {
            rows: vec![vec![cs('p', 0), cs('q', 0)]],
            wrapped: vec![false],
        });
        t.apply(osc(1, "obj;type=path;ref=/bin"));
        t.apply(Record::CellDiff {
            changed: vec![(0, 0, cs('b', 1)), (0, 1, cs('i', 1)), (0, 2, cs('n', 1))],
            cursor: (0, 3, true),
            wrapped: vec![],
            top_continues: false,
        });
        t.apply(osc(2, "/obj"));
        // The zone cut freezes the path's block while the path is on screen.
        t.apply(osc(3, "zone;k=prompt"));
        assert_eq!(t.scrollback.frozen_blocks().len(), 1);
        let screen = grid_text(&t, 0);
        t.forget_history();
        assert!(
            t.scrollback.frozen_blocks().is_empty(),
            "the history is gone"
        );
        assert!(t.scrollback.open_block().items.is_empty());
        assert_eq!(grid_text(&t, 0), screen, "the live screen is untouched");
        assert_eq!(
            t.grid_run_obj(0, 1),
            Some(("path", "/bin")),
            "and its path still resolves"
        );
        // Leaving the grid later, the row brings its object along.
        t.apply(Record::ScrollOff {
            rows: vec![vec![cs('b', 1), cs('i', 1), cs('n', 1)]],
            wrapped: vec![false],
        });
        let fr = crate::select::FlatRow {
            block: usize::MAX,
            item: 0,
            row: usize::MAX,
        };
        let runs = crate::menu::runs_on_row(&t.scrollback, fr);
        assert_eq!(runs.len(), 1, "{:?}", runs);
        assert_eq!(
            crate::menu::obj_of(&t.scrollback, usize::MAX, runs[0].obj),
            Some(("path", "/bin"))
        );
    }

    #[test]
    fn forget_history_releases_the_images_only_forgotten_lines_named() {
        use crate::transcript::SPAN_MAP_ENTRIES;
        let cs = |ch: char, span: u32| Cell {
            ch,
            fg: 0xFFFFFF,
            bg: 0,
            attrs: 0,
            span,
        };
        let caption = |n: u128| alloc::format!("obj;type=inline-image;ref={:032x}", n);
        let mut t = daylight_tile(40, 2);
        // Images A and C each captioned in a zone that holds a row, so each
        // freezes at the next cut (an empty block is dropped, not frozen);
        // image B's zone stays open.
        let mut serial = 0u32;
        for (n, ch) in [(0xa, 'a'), (0xc, 'c')] {
            t.apply(osc(serial + 1, "zone;k=output"));
            t.apply(osc(serial + 2, &caption(n)));
            t.apply(osc(serial + 3, "/obj"));
            t.apply(Record::ScrollOff {
                rows: vec![vec![cs(ch, serial + 2)]],
                wrapped: vec![false],
            });
            serial += 3;
        }
        t.apply(osc(7, "zone;k=output"));
        t.apply(osc(8, &caption(0xb)));
        t.apply(osc(9, "/obj"));
        assert_eq!(
            t.scrollback.frozen_blocks().len(),
            2,
            "premise: A's and C's zones are history"
        );
        // The ring turns over A's zone's slots: no live cell can name it now,
        // while C's zone is still named.
        for s in 1..=3u32 {
            t.apply(osc(s + SPAN_MAP_ENTRIES as u32, "em;class=dim"));
        }
        for n in [0xa, 0xb, 0xc] {
            assert!(t.place_image(n, 1, 1, vec![1]));
        }
        t.forget_history();
        assert!(!t.media.contains(0xa), "A: only forgotten history named it");
        assert!(
            t.media.contains(0xc),
            "C: the span ring still names its caption, so its husk keeps it"
        );
        assert!(
            t.media.contains(0xb),
            "B: the ring names its caption, in the open zone"
        );
    }

    #[test]
    fn a_clear_then_the_history_chord_leaves_an_empty_unpinned_tile() {
        let cs = |ch: char| Cell {
            ch,
            fg: 0xFFFFFF,
            bg: 0,
            attrs: 0,
            span: 0,
        };
        let mut t = daylight_tile(8, 3);
        t.apply(Record::ScrollOff {
            rows: vec![vec![cs('h'), cs('i')]],
            wrapped: vec![false],
        });
        t.apply(Record::Control(Control::ScreenErased));
        assert!(t.pinned, "the clear pinned the view against its history");
        t.forget_history();
        assert!(!t.pinned, "the pin goes with the history it pinned against");
        assert!(t.scrollback.frozen_blocks().is_empty());
        assert!(t.scrollback.open_block().items.is_empty());
    }

    #[test]
    fn a_normal_mode_selection_follows_a_scroll_off_through_the_tile() {
        // The tile path end to end: the producer's ScrollOff, then the repaint
        // that shows the rows gone, through Tile::apply, the flat list re-read
        // the way the session reads it.
        use crate::select::{refresh, FlatRow, Sel, Stamp, GRID_BLOCK};
        let cs = |ch: char| Cell {
            ch,
            fg: 0xFFFFFF,
            bg: 0,
            attrs: 0,
            span: 0,
        };
        let row = |item: usize| FlatRow {
            block: GRID_BLOCK,
            item,
            row: usize::MAX,
        };
        let mut t = daylight_tile(8, 4);
        // a wraps into b; then c and d.
        t.apply(Record::CellDiff {
            changed: vec![(0, 0, cs('a')), (1, 0, cs('b')), (2, 0, cs('c')), (3, 0, cs('d'))],
            cursor: (3, 1, true),
            wrapped: vec![true, false, false, false],
            top_continues: false,
        });
        let (mut flat, mut seq) = (Vec::new(), u64::MAX);
        refresh(&t.scrollback, Some(4), &mut flat, &mut seq, None);
        let mut sel = Sel::at(3, Stamp::of(&t.scrollback, &flat));
        sel.anchor = Some(1);
        // Three rows leave; the first two are one wrapped line, so they make
        // two history rows -- a list only clamped, or shifted by what the
        // history gained, lands on other rows.
        t.apply(Record::ScrollOff {
            rows: vec![vec![cs('a')], vec![cs('b')], vec![cs('c')]],
            wrapped: vec![true, false, false],
        });
        refresh(&t.scrollback, Some(4), &mut flat, &mut seq, Some(&mut sel));
        assert_eq!(flat.len(), 2 + 4, "two history rows, then the grid");
        assert_eq!(
            flat[sel.cursor],
            row(3),
            "until the repaint the grid shows d on row 3, and the cursor stays on it"
        );
        assert_eq!(sel.anchor.map(|a| flat[a]), Some(row(1)), "and the anchor on b");
        t.apply(Record::CellDiff {
            changed: vec![(0, 0, cs('d')), (1, 0, cs(' ')), (2, 0, cs(' ')), (3, 0, cs(' '))],
            cursor: (0, 1, true),
            wrapped: vec![false; 4],
            top_continues: false,
        });
        refresh(&t.scrollback, Some(4), &mut flat, &mut seq, Some(&mut sel));
        assert_eq!(flat[sel.cursor], row(0), "the repaint moved grid row 3 up three");
        assert_eq!(
            sel.anchor,
            Some(0),
            "grid row 1, the wrapped line's second half, is in the history row its line joined"
        );
    }

    #[test]
    fn a_resize_restarts_grid_ends_at_the_prompt_and_its_shed_rows_arrive_once() {
        // The session's resize path, Tile::resize_selected: the reflow drops
        // rows off the top that the producer's ScrollOff delivers after it;
        // they left the grid once.
        use crate::select::{refresh, row_text, FlatRow, Sel, Stamp, GRID_BLOCK};
        let cs = |ch: char| Cell {
            ch,
            fg: 0xFFFFFF,
            bg: 0,
            attrs: 0,
            span: 0,
        };
        let mut t = daylight_tile(8, 4);
        t.apply(Record::CellDiff {
            changed: vec![(0, 0, cs('y')), (1, 0, cs('z')), (2, 0, cs('a')), (3, 0, cs('b'))],
            cursor: (3, 1, true),
            wrapped: vec![false; 4],
            top_continues: false,
        });
        let (mut flat, mut seq) = (Vec::new(), u64::MAX);
        refresh(&t.scrollback, Some(4), &mut flat, &mut seq, None);
        let mut sel = Sel::at(1, Stamp::of(&t.scrollback, &flat));
        sel.anchor = Some(3);
        // Two lines scroll y and z off, and the repaint shows it, before the
        // list is re-read: the cursor's row is history row 1 now, no grid row.
        t.apply(Record::ScrollOff {
            rows: vec![vec![cs('y')], vec![cs('z')]],
            wrapped: vec![false, false],
        });
        t.apply(Record::CellDiff {
            changed: vec![(0, 0, cs('a')), (1, 0, cs('b')), (2, 0, cs('c')), (3, 0, cs('d'))],
            cursor: (3, 1, true),
            wrapped: vec![false; 4],
            top_continues: false,
        });
        t.resize_selected(8, 2, &mut flat, &mut seq, Some(&mut sel));
        assert_eq!(t.grid.cursor().0, 1, "premise: the prompt is grid row 1");
        assert_eq!(
            t.scrollback.rows_scrolled(),
            4,
            "premise: the reflow dropped two rows"
        );
        let prompt = FlatRow {
            block: GRID_BLOCK,
            item: 1,
            row: usize::MAX,
        };
        // The producer's reply: the two rows the reflow dropped, the ack, the
        // repaint.
        t.apply(Record::ScrollOff {
            rows: vec![vec![cs('a')], vec![cs('b')]],
            wrapped: vec![false, false],
        });
        refresh(&t.scrollback, Some(2), &mut flat, &mut seq, Some(&mut sel));
        assert_eq!(flat.len(), 4 + 2, "four history rows, then the grid");
        assert_eq!(sel.cursor, 1, "the cursor was on history row 1 at the resize and stays there");
        assert_eq!(row_text(&t.scrollback, flat[sel.cursor]).trim_end(), "z");
        assert_eq!(
            sel.anchor.map(|a| flat[a]),
            Some(prompt),
            "the anchor was on the grid: it starts again at the prompt, and the rows arriving after leave it there"
        );
        t.apply(Record::Control(Control::WinsizeAck));
        t.apply(whole_grid(8, &[("c", false), ("d", false)], (1, 1), false));
        refresh(&t.scrollback, Some(2), &mut flat, &mut seq, Some(&mut sel));
        assert_eq!(t.scrollback.rows_scrolled(), 4, "premise: the repaint settled nothing");
        assert_eq!(sel.cursor, 1, "and moved nothing");
        assert_eq!(sel.anchor.map(|a| flat[a]), Some(prompt));
    }

    #[test]
    fn a_narrowing_resize_restamps_the_selection_before_its_shed_rows_arrive() {
        // A width change alone keeps the grid's height, so only the shed's
        // seq bump makes the list re-read at the resize; the rows the reflow
        // dropped then arrive without moving the selection off the prompt.
        use crate::select::{refresh, FlatRow, Sel, Stamp, GRID_BLOCK};
        let cs = |ch: char| Cell {
            ch,
            fg: 0xFFFFFF,
            bg: 0,
            attrs: 0,
            span: 0,
        };
        let mut t = daylight_tile(8, 4);
        let mut changed = Vec::new();
        for (r, ch, n) in [(0u16, 'a', 8u16), (1, 'b', 8), (2, 'c', 2), (3, 'd', 2)] {
            changed.extend((0..n).map(|c| (r, c, cs(ch))));
        }
        t.apply(Record::CellDiff {
            changed,
            cursor: (3, 2, true),
            wrapped: vec![false; 4],
            top_continues: false,
        });
        let (mut flat, mut seq) = (Vec::new(), u64::MAX);
        refresh(&t.scrollback, Some(4), &mut flat, &mut seq, None);
        let mut sel = Sel::at(2, Stamp::of(&t.scrollback, &flat));
        sel.anchor = Some(0);
        sel.obj = Some(1);
        t.resize_selected(4, 4, &mut flat, &mut seq, Some(&mut sel));
        assert_eq!(t.grid.dims(), (4, 4));
        assert_eq!(sel.obj, None, "a restarted cursor carries no run of the row it left");
        assert_eq!(t.grid.cursor().0, 3, "premise: the prompt is still the last row");
        assert_eq!(
            t.scrollback.rows_scrolled(),
            2,
            "premise: the reflow dropped the a line's two rows"
        );
        t.apply(Record::ScrollOff {
            rows: vec![vec![cs('a'); 4], vec![cs('a'); 4]],
            wrapped: vec![true, false],
        });
        refresh(&t.scrollback, Some(4), &mut flat, &mut seq, Some(&mut sel));
        assert_eq!(flat.len(), 1 + 4, "the a line is one history row");
        let prompt = FlatRow {
            block: GRID_BLOCK,
            item: 3,
            row: usize::MAX,
        };
        assert_eq!(flat[sel.cursor], prompt, "the cursor stays on the prompt");
        assert_eq!(
            sel.anchor.map(|a| flat[a]),
            Some(prompt),
            "so does the anchor that was on grid row 0"
        );
    }

    #[test]
    fn a_height_only_resize_keeps_a_grid_selection_on_its_text() {
        // Only a width change re-cuts the lines: a taller or shorter tile
        // keeps each row's text, the window sliding to keep the cursor row,
        // so an end on a row the window still shows stays on its text.
        use crate::select::{refresh, FlatRow, Sel, Stamp, GRID_BLOCK};
        let cs = |ch: char| Cell {
            ch,
            fg: 0xFFFFFF,
            bg: 0,
            attrs: 0,
            span: 0,
        };
        let row = |item: usize| FlatRow {
            block: GRID_BLOCK,
            item,
            row: usize::MAX,
        };
        let mut t = daylight_tile(8, 4);
        t.apply(Record::CellDiff {
            changed: vec![(0, 0, cs('a')), (1, 0, cs('b')), (2, 0, cs('c')), (3, 0, cs('d'))],
            cursor: (3, 1, true),
            wrapped: vec![false; 4],
            top_continues: false,
        });
        let (mut flat, mut seq) = (Vec::new(), u64::MAX);
        refresh(&t.scrollback, Some(4), &mut flat, &mut seq, None);
        let mut sel = Sel::at(1, Stamp::of(&t.scrollback, &flat));
        t.resize_selected(8, 6, &mut flat, &mut seq, Some(&mut sel));
        assert_eq!(t.grid.cursor().0, 3, "premise: a taller grid keeps its rows where they were");
        assert_eq!(flat[sel.cursor], row(1), "a taller tile keeps the cursor on b");
        sel.anchor = Some(0);
        assert_eq!(sel.anchor.map(|a| flat[a]), Some(row(0)), "premise: the anchor is on a");
        t.resize_selected(8, 3, &mut flat, &mut seq, Some(&mut sel));
        assert_eq!(t.grid.cursor().0, 2, "premise: a shorter one slides a off the top");
        assert_eq!(flat[sel.cursor], row(0), "and b, now its first row, keeps the cursor");
        assert_eq!(
            sel.anchor.map(|a| flat[a]),
            Some(row(2)),
            "the anchor on a, slid past, starts again at the prompt"
        );
        // With the cursor at the top, a shorter window cuts rows off below:
        // an end on one of them has lost its text and starts at the prompt.
        let mut t = daylight_tile(8, 4);
        t.apply(Record::CellDiff {
            changed: vec![(0, 0, cs('a')), (1, 0, cs('b')), (2, 0, cs('c')), (3, 0, cs('d'))],
            cursor: (0, 1, true),
            wrapped: vec![false; 4],
            top_continues: false,
        });
        let (mut flat, mut seq) = (Vec::new(), u64::MAX);
        refresh(&t.scrollback, Some(4), &mut flat, &mut seq, None);
        let mut sel = Sel::at(3, Stamp::of(&t.scrollback, &flat));
        sel.anchor = Some(2);
        t.resize_selected(8, 2, &mut flat, &mut seq, Some(&mut sel));
        assert_eq!(t.scrollback.rows_scrolled(), 0, "premise: nothing slid off the top");
        assert_eq!(flat[sel.cursor], row(0), "d was cut off below: the cursor is on the prompt");
        assert_eq!(
            sel.anchor.map(|a| flat[a]),
            Some(row(0)),
            "so was c, the first row past the window"
        );
    }

    #[test]
    fn a_resize_the_producer_coalesced_away_is_settled_by_its_repaint() {
        // The mirror reflows every resize; the producer applies only the last
        // one it finds pending. Shrunk and grown back before it looked, it
        // sheds nothing, and its repaint of the whole grid -- announced by
        // the ack -- settles the rows the mirror dropped: each end moves to
        // where the repaint puts its row, and the next real scroll counts.
        use crate::select::{FlatRow, Sel, Stamp, GRID_BLOCK};
        let cs = |ch: char| Cell {
            ch,
            fg: 0xFFFFFF,
            bg: 0,
            attrs: 0,
            span: 0,
        };
        // `lines` one character each down column 0 of an 8x6 grid, the rest
        // blank, and the cursor on row `crow`.
        let full = |lines: &str, crow: u16| -> Record {
            let chars: Vec<char> = lines.chars().collect();
            let changed = (0..6u16)
                .flat_map(|r| (0..8u16).map(move |c| (r, c)))
                .map(|(r, c)| {
                    let ch = if c == 0 { chars.get(r as usize).copied().unwrap_or(' ') } else { ' ' };
                    (r, c, cs(ch))
                })
                .collect();
            Record::CellDiff {
                changed,
                cursor: (crow, 1, true),
                wrapped: vec![false; 6],
                top_continues: false,
            }
        };
        let row = |item: usize| FlatRow {
            block: GRID_BLOCK,
            item,
            row: usize::MAX,
        };
        // a, b, c, the prompt d on row 3, and two blank rows below it: the
        // prompt is not the grid's last row, so no clamp can land on it.
        let mut t = daylight_tile(8, 6);
        t.apply(full("abcd", 3));
        let (mut flat, mut seq) = (Vec::new(), u64::MAX);
        t.refresh_selected(&mut flat, &mut seq, None);
        let mut sel = Sel::at(3, Stamp::of(&t.scrollback, &flat));
        sel.anchor = Some(2);
        t.resize_selected(8, 2, &mut flat, &mut seq, Some(&mut sel));
        t.resize_selected(8, 6, &mut flat, &mut seq, Some(&mut sel));
        assert_eq!(t.scrollback.rows_scrolled(), 2, "premise: the mirror dropped two rows");
        assert_eq!(t.grid.cursor().0, 1, "premise: after the grow the mirror's prompt is row 1");
        assert_eq!(flat[sel.cursor], row(1), "premise: the selection slid with d");
        assert_eq!(sel.anchor.map(|a| flat[a]), Some(row(0)), "premise: and its anchor with c");
        // An incremental diff in flight is no repaint: nothing settles.
        t.apply(Record::CellDiff {
            changed: vec![(0, 7, cs('x'))],
            cursor: (3, 1, true),
            wrapped: vec![false; 6],
            top_continues: false,
        });
        t.refresh_selected(&mut flat, &mut seq, Some(&mut sel));
        assert_eq!(t.scrollback.rows_scrolled(), 2, "an incremental diff settles nothing");
        assert_eq!(flat[sel.cursor], row(1), "and moves nothing");
        // The producer applied only the last resize -- no change, no rows --
        // acks it and repaints the whole grid.
        t.apply(Record::Control(Control::WinsizeAck));
        t.apply(full("abcd", 3));
        t.refresh_selected(&mut flat, &mut seq, Some(&mut sel));
        assert_eq!(t.scrollback.rows_scrolled(), 0, "the repaint settled the two");
        assert_eq!(flat[sel.cursor], row(3), "the selection is on d, where the repaint shows it");
        assert_eq!(
            sel.anchor.map(|a| flat[a]),
            Some(row(2)),
            "and its anchor on c: moved down with its row, not started again at the prompt"
        );
        // The next real scroll: the row, then the repaint that shows it gone.
        t.apply(Record::ScrollOff {
            rows: vec![vec![cs('a')]],
            wrapped: vec![false],
        });
        t.apply(full("bcd", 2));
        t.refresh_selected(&mut flat, &mut seq, Some(&mut sel));
        assert_eq!(flat.len(), 1 + 6);
        assert_eq!(flat[sel.cursor], row(2), "and the next real scroll moves it up one");
        assert_eq!(sel.anchor.map(|a| flat[a]), Some(row(1)));
    }

    // A grid of `lines` (row-major, each padded to `cols`) as one CellDiff
    // covering every cell -- the producer's repaint -- with its wrap flags
    // and its top flag (`top`: the row above row 0 continues into it). In
    // the daylight tile's default ink, as a producer's blanks are: a blank
    // in any other ink is content to the reflow, and re-cuts as a character.
    fn whole_grid(cols: usize, lines: &[(&str, bool)], cursor: (u16, u16), top: bool) -> Record {
        let pal = libhalcyon::theme::daylight_palette();
        let cell = |ch: char| Cell {
            ch,
            fg: pal.fg,
            bg: pal.bg,
            attrs: 0,
            span: 0,
        };
        let mut changed = Vec::new();
        for (r, (text, _)) in lines.iter().enumerate() {
            let chars: Vec<char> = text.chars().collect();
            for c in 0..cols {
                changed.push((r as u16, c as u16, cell(chars.get(c).copied().unwrap_or(' '))));
            }
        }
        Record::CellDiff {
            changed,
            cursor: (cursor.0, cursor.1, true),
            wrapped: lines.iter().map(|&(_, w)| w).collect(),
            top_continues: top,
        }
    }

    // The records a producer emitted, through the wire both ways, into the
    // tile (seam_step's second half).
    fn seam_send(t: &mut Tile, out: Vec<Record>) {
        for rec in out {
            let mut buf = Vec::new();
            kaua_term::wire::encode_record(&rec, &mut buf);
            let back = kaua_term::wire::parse_record(buf[0], &buf[5..])
                .expect("the producer's own record must parse");
            assert_eq!(back, rec, "the wire round-trip is lossless");
            t.apply(back);
        }
    }

    // As seam_send, with the session's paint after every record: a record
    // stream spans reads, and the selection is brought current between them.
    fn seam_send_each(
        t: &mut Tile,
        out: Vec<Record>,
        flat: &mut Vec<crate::select::FlatRow>,
        seq: &mut u64,
        sel: &mut crate::select::Sel,
    ) {
        for rec in out {
            seam_send(t, vec![rec]);
            t.refresh_selected(flat, seq, Some(&mut *sel));
        }
    }

    // The producer's side of a resize, as kaua-term's apply_resize does it:
    // the vt re-cut, the rows it pushed off the top, the ack, the repaint.
    fn seam_resized(p: &mut kaua_term::Producer, v: &mut vt::Vt, cols: usize, rows: usize) -> Vec<Record> {
        v.resize(cols, rows);
        let mut out = Vec::new();
        p.drain_pending(v, &mut out);
        p.resized(v, &mut out);
        out
    }

    // A seam tile of `cols` x `rows` showing `bytes`, and a Normal-mode
    // selection on the grid's cursor row. The vt is born in the tile's
    // palette, as a kaua-term is in its host's: the reflow's padding is the
    // default ink, so both sides must mean the same ink by it.
    fn seam_selected(
        cols: usize,
        rows: usize,
        bytes: &[u8],
    ) -> (Tile, kaua_term::Producer, vt::Vt, Vec<crate::select::FlatRow>, u64, crate::select::Sel) {
        let mut v = vt::Vt::with_palette(cols, rows, libhalcyon::theme::daylight_palette());
        v.set_capture_events(true);
        let mut p = kaua_term::Producer::new(&v);
        let mut t = Tile::new(cols, rows, libhalcyon::theme::daylight_palette());
        seam_step(&mut t, &mut p, &mut v, bytes);
        let (mut flat, mut seq) = (Vec::new(), u64::MAX);
        t.refresh_selected(&mut flat, &mut seq, None);
        let at = crate::select::Stamp::of(&t.scrollback, &flat);
        let sel = crate::select::Sel::at(flat.len() - rows + t.grid.cursor().0, at);
        (t, p, v, flat, seq, sel)
    }

    fn grid_row(item: usize) -> crate::select::FlatRow {
        crate::select::FlatRow {
            block: crate::select::GRID_BLOCK,
            item,
            row: usize::MAX,
        }
    }

    #[test]
    fn a_narrowing_drag_the_producer_coalesced_ends_on_the_prompt() {
        // Rows cut at other widths are no distance: the mirror counts six rows
        // at four and two columns, the producer's single re-cut from eight
        // columns sheds nine at two, and the grids come out the same. A count
        // delta of three would put the selection on output three rows above
        // the prompt; the re-cut holds the grid rows until the producer's
        // repaint, which starts them on its prompt.
        let (mut t, mut p, mut v, mut flat, mut seq, mut sel) =
            seam_selected(8, 4, b"aaaaaaaa\r\nbbbbbbbb\r\ncccccccc\r\n$");
        assert_eq!(flat[sel.cursor], grid_row(3), "premise: the selection is on the prompt");
        t.resize_selected(4, 4, &mut flat, &mut seq, Some(&mut sel));
        t.resize_selected(2, 4, &mut flat, &mut seq, Some(&mut sel));
        assert_eq!(
            t.scrollback.rows_scrolled(),
            6,
            "premise: the mirror dropped three rows at four columns and three at two"
        );
        assert_eq!(flat[sel.cursor], grid_row(3), "premise: each re-cut started the cursor on the prompt");
        // The producer finds only the last resize pending and re-cuts once.
        seam_send(&mut t, seam_resized(&mut p, &mut v, 2, 4));
        t.refresh_selected(&mut flat, &mut seq, Some(&mut sel));
        assert_eq!(t.scrollback.rows_scrolled(), 9, "premise: the producer's cut shed nine rows");
        assert_eq!(t.scrollback.rewraps() % 2, 0, "premise: its repaint settled the re-cut");
        assert_eq!(t.grid.cursor().0, 3, "premise: the repaint's prompt is row 3");
        assert_eq!(flat[sel.cursor], grid_row(3), "the cursor is on the prompt the repaint shows");
    }

    #[test]
    fn a_reply_to_a_resize_the_grid_moved_past_reopens_the_re_cut() {
        // Narrowed, widened and narrowed again before any reply: the reply to
        // the first narrow comes at the grid's dims and settles, but the
        // producer still has two resizes to apply. Its reply at the wide
        // width reopens the re-cut, so what it scrolls off at eight columns
        // (the c line, two rows here, counts one) and what its last narrow
        // sheds move no grid end; its last reply starts them on its prompt.
        let (mut t, mut p, mut v, mut flat, mut seq, mut sel) =
            seam_selected(8, 4, b"aaaaaaaa\r\nbbbbbbbb\r\ncccccccc\r\n$");
        t.resize_selected(4, 4, &mut flat, &mut seq, Some(&mut sel));
        t.resize_selected(8, 4, &mut flat, &mut seq, Some(&mut sel));
        t.resize_selected(4, 4, &mut flat, &mut seq, Some(&mut sel));
        // The producer applies each resize as it comes, and prints two lines
        // while it is at eight columns.
        let narrow = seam_resized(&mut p, &mut v, 4, 4);
        let wide = seam_resized(&mut p, &mut v, 8, 4);
        let mut output = Vec::new();
        p.feed(&mut v, b"\r\nyyyyyyyy\r\nzzzzzzzz\r\n$", &mut output);
        let narrow_again = seam_resized(&mut p, &mut v, 4, 4);
        seam_send(&mut t, narrow);
        t.refresh_selected(&mut flat, &mut seq, Some(&mut sel));
        assert_eq!(t.scrollback.rewraps() % 2, 0, "premise: the first reply, at the grid's dims, settled");
        assert_eq!(flat[sel.cursor], grid_row(3), "premise: the cursor is on the prompt that reply shows");
        seam_send(&mut t, wide);
        t.refresh_selected(&mut flat, &mut seq, Some(&mut sel));
        assert_eq!(t.scrollback.rewraps() % 2, 1, "the reply at the wide width reopened the re-cut");
        seam_send(&mut t, output);
        seam_send(&mut t, narrow_again);
        t.refresh_selected(&mut flat, &mut seq, Some(&mut sel));
        assert_eq!(
            t.scrollback.rows_scrolled(),
            7,
            "premise: three rows from the first narrow, two scrolled at eight columns, two shed by the last"
        );
        assert_eq!(t.scrollback.rewraps() % 2, 0, "premise: the last reply settled the re-cut");
        assert_eq!(t.grid.cursor().0, 3, "premise: the last reply's prompt is row 3");
        assert_eq!(flat[sel.cursor], grid_row(3), "the cursor is on the prompt, not on a row counted at eight columns");
    }

    #[test]
    fn narrowed_and_widened_back_the_selection_ends_on_the_prompt() {
        // Back at the width it started from, the producer applies nothing and
        // repaints the line the mirror's two re-cuts shortened by a row: the
        // row the mirror dropped is no row of the repaint's, and moving the
        // selection down by it would leave it on a blank row below the prompt.
        let (mut t, mut p, mut v, mut flat, mut seq, mut sel) = seam_selected(8, 4, b"aaaaaaaa\r\n$");
        assert_eq!(flat[sel.cursor], grid_row(1), "premise: the selection is on the prompt");
        t.resize_selected(2, 4, &mut flat, &mut seq, Some(&mut sel));
        assert_eq!(t.scrollback.rows_scrolled(), 1, "premise: the narrow re-cut dropped a row");
        t.resize_selected(8, 4, &mut flat, &mut seq, Some(&mut sel));
        seam_send_each(&mut t, seam_resized(&mut p, &mut v, 8, 4), &mut flat, &mut seq, &mut sel);
        assert_eq!(t.scrollback.rows_scrolled(), 0, "premise: the repaint settled the row");
        assert_eq!(t.grid.cursor().0, 1, "premise: the repaint's prompt is row 1");
        assert_eq!(flat[sel.cursor], grid_row(1), "the cursor is on the prompt, not the blank row below it");
    }

    #[test]
    fn output_in_flight_across_a_width_change_ends_on_the_new_prompt() {
        // No coalescing: the producer printed four lines before the resize
        // reached it. They left its grid at eight columns, the mirror's guess
        // at four: inside the re-cut the grid rows keep their places whatever
        // the count does, and the repaint starts them on its prompt.
        let (mut t, mut p, mut v, mut flat, mut seq, mut sel) =
            seam_selected(8, 4, b"xxxxxxxx\r\nyyyyyyyy\r\nzzzzzzzz\r\n$");
        let mut in_flight = Vec::new();
        p.feed(&mut v, b"\r\nwwwwwwww\r\nvvvvvvvv\r\nuuuuuuuu\r\n$", &mut in_flight);
        t.resize_selected(4, 4, &mut flat, &mut seq, Some(&mut sel));
        assert_eq!(t.scrollback.rows_scrolled(), 3, "premise: the mirror's re-cut dropped three rows");
        seam_send_each(&mut t, in_flight, &mut flat, &mut seq, &mut sel);
        assert_eq!(
            t.scrollback.rows_scrolled(),
            4,
            "premise: the four rows in flight count one past the mirror's three"
        );
        assert_eq!(flat[sel.cursor], grid_row(3), "inside the re-cut the cursor keeps its grid row");
        seam_send_each(&mut t, seam_resized(&mut p, &mut v, 4, 4), &mut flat, &mut seq, &mut sel);
        assert_eq!(t.grid.cursor().0, 3, "premise: the repaint's prompt is row 3");
        assert_eq!(flat[sel.cursor], grid_row(3), "the cursor is on the new prompt");
    }

    #[test]
    fn a_width_change_while_the_mirror_is_behind_ends_on_the_repaints_cursor_row() {
        // The producer had moved its cursor up (the diff still in flight) when
        // the resize reached it: its window keeps the top rows and cuts the rest
        // off below, where the mirror's, its cursor still on the prompt, slid
        // three rows past. Held in place until the repaint, the cursor then
        // starts on the repaint's cursor row -- not on the row it held, nor on
        // that row moved down by the three rows the producer kept.
        let (mut t, mut p, mut v, mut flat, mut seq, mut sel) =
            seam_selected(8, 4, b"xxxxxxxx\r\nyyyyyyyy\r\nzzzzzzzz\r\n$");
        let mut in_flight = Vec::new();
        p.feed(&mut v, b"\x1b[2;1H", &mut in_flight);
        t.resize_selected(4, 4, &mut flat, &mut seq, Some(&mut sel));
        assert_eq!(t.scrollback.rows_scrolled(), 3, "premise: the mirror's re-cut slid three rows past");
        seam_send_each(&mut t, in_flight, &mut flat, &mut seq, &mut sel);
        seam_send_each(&mut t, seam_resized(&mut p, &mut v, 4, 4), &mut flat, &mut seq, &mut sel);
        assert_eq!(t.scrollback.rows_scrolled(), 0, "premise: the producer's window slid past nothing");
        assert_eq!(t.grid.cursor().0, 2, "premise: the repaint's cursor is on y's first row");
        assert_eq!(flat[sel.cursor], grid_row(2), "the cursor is on the repaint's cursor row");
    }

    #[test]
    fn a_row_the_producer_cut_off_below_restarts_at_the_prompt() {
        // The mirror was behind: the producer's cursor had moved to the top, so
        // its shorter window kept a and b and cut c and d off below, where the
        // mirror's (its cursor still on d) slid past a and b. The repaint moves
        // the grid rows down two; d's row is past the grid's last row, so the
        // end on it starts again at the prompt, not on b.
        let (mut t, mut p, mut v, mut flat, mut seq, mut sel) = seam_selected(8, 4, b"a\r\nb\r\nc\r\nd");
        let mut in_flight = Vec::new();
        p.feed(&mut v, b"\x1b[H", &mut in_flight);
        t.resize_selected(8, 2, &mut flat, &mut seq, Some(&mut sel));
        assert_eq!(t.scrollback.rows_scrolled(), 2, "premise: the mirror slid past a and b");
        assert_eq!(flat[sel.cursor], grid_row(1), "premise: the cursor slid with d");
        seam_send_each(&mut t, in_flight, &mut flat, &mut seq, &mut sel);
        // The prompt the repaint restarts the cursor on is row 0, where wrong
        // paths land too: the cursor must still be on d before the repaint.
        assert_eq!(flat[sel.cursor], grid_row(1), "the diff in flight moved no row: the cursor is still on d");
        seam_send_each(&mut t, seam_resized(&mut p, &mut v, 8, 2), &mut flat, &mut seq, &mut sel);
        assert_eq!(t.scrollback.rows_scrolled(), 0, "premise: the producer dropped nothing");
        assert_eq!(t.grid.cursor().0, 0, "premise: the repaint's cursor is on a");
        assert_eq!(flat[sel.cursor], grid_row(0), "d is gone: the cursor is on the prompt");
    }

    #[test]
    fn a_height_change_inside_an_open_re_cut_slides_the_grid_ends_with_their_rows() {
        // The re-cut holds the grid rows against the count, not against the
        // mirror's own reflow: a shorter tile before the repaint still slides
        // an end with its row, and starts one on a row it slid past again at
        // the prompt.
        let mut t = daylight_tile(8, 6);
        t.apply(whole_grid(
            8,
            &[("a", false), ("b", false), ("c", false), ("d", false), ("e", false), ("$", false)],
            (5, 1),
            false,
        ));
        let (mut flat, mut seq) = (Vec::new(), u64::MAX);
        t.refresh_selected(&mut flat, &mut seq, None);
        let mut sel = crate::select::Sel::at(5, crate::select::Stamp::of(&t.scrollback, &flat));
        t.resize_selected(4, 6, &mut flat, &mut seq, Some(&mut sel));
        assert_eq!(t.scrollback.rewraps() % 2, 1, "premise: the re-cut is unsettled");
        sel.cursor = 4;
        sel.anchor = Some(0);
        assert_eq!(flat[sel.cursor], grid_row(4), "premise: the cursor is on e");
        assert_eq!(flat[0], grid_row(0), "premise: the anchor is on a");
        t.resize_selected(4, 3, &mut flat, &mut seq, Some(&mut sel));
        assert_eq!(t.scrollback.rows_scrolled(), 3, "premise: the shorter window slid past a, b and c");
        assert_eq!(flat[sel.cursor], grid_row(1), "the cursor slid with e");
        assert_eq!(t.grid.cursor().0, 2, "premise: the prompt is row 2");
        assert_eq!(flat[sel.anchor.expect("the anchor stays")], grid_row(2), "the anchor on a starts again at the prompt");
    }

    // What a selection end is on: its grid row's text, or its history row's.
    fn end_text(t: &Tile, flat: &[crate::select::FlatRow], i: usize) -> String {
        let fr = flat[i];
        if fr.block == crate::select::GRID_BLOCK {
            String::from(grid_text(t, fr.item).trim_end())
        } else {
            String::from(crate::select::row_text(&t.scrollback, fr).trim_end())
        }
    }

    // A seam tile of eight columns showing `bytes`, the cursor on grid row
    // `on`; the producer prints `more`, and only the rows it scrolled off
    // reach the tile (the repaint is in the next read, returned).
    fn rows_ahead_of_their_repaint(
        rows: usize,
        bytes: &[u8],
        more: &[u8],
        on: usize,
    ) -> (Tile, kaua_term::Producer, vt::Vt, Vec<crate::select::FlatRow>, u64, crate::select::Sel, Vec<Record>) {
        let (mut t, mut p, mut v, mut flat, mut seq, mut sel) = seam_selected(8, rows, bytes);
        sel.cursor = flat.len() - rows + on;
        let mut out = Vec::new();
        p.feed(&mut v, more, &mut out);
        let at = out
            .iter()
            .position(|r| matches!(r, Record::CellDiff { .. }))
            .expect("premise: the producer repaints");
        let repaint = out.split_off(at);
        assert!(
            out.iter().any(|r| matches!(r, Record::ScrollOff { .. })),
            "premise: the rows come ahead of the repaint"
        );
        seam_send_each(&mut t, out, &mut flat, &mut seq, &mut sel);
        (t, p, v, flat, seq, sel, repaint)
    }

    #[test]
    fn a_shrink_before_the_repaint_counts_the_rows_that_arrived_once() {
        // a and b arrived; their repaint had not when the tile shrank, so the
        // reflow dropped a, which is here already. The producer's repaint at
        // its old height lands and is painted, then its reply, which scrolls
        // c off. Counted again, a would stand in for c: nothing would move at
        // the reply and the cursor would sit on d.
        let (mut t, mut p, mut v, mut flat, mut seq, mut sel, repaint) =
            rows_ahead_of_their_repaint(4, b"a\r\nb\r\nc\r\nd", b"\r\ne\r\nf", 2);
        assert_eq!(end_text(&t, &flat, sel.cursor), "c", "premise: before the repaint the cursor is on c");
        t.resize_selected(8, 3, &mut flat, &mut seq, Some(&mut sel));
        assert_eq!(grid_text(&t, 0).trim_end(), "b", "premise: the reflow dropped a");
        assert_eq!(end_text(&t, &flat, sel.cursor), "c", "the cursor slid with c");
        seam_send_each(&mut t, repaint, &mut flat, &mut seq, &mut sel);
        assert_eq!(grid_text(&t, 0).trim_end(), "c", "premise: the old repaint shows c on top");
        assert_eq!(end_text(&t, &flat, sel.cursor), "c", "the old repaint: the cursor is on c");
        seam_send_each(&mut t, seam_resized(&mut p, &mut v, 8, 3), &mut flat, &mut seq, &mut sel);
        assert_eq!(grid_text(&t, 0).trim_end(), "d", "premise: the producer's window slid past c");
        assert_eq!(end_text(&t, &flat, sel.cursor), "c", "the cursor followed c into history");
    }

    #[test]
    fn a_shrink_starts_an_end_on_a_dropped_row_that_arrived_again_at_the_prompt() {
        // a and b arrived; their repaint had not when the tile shrank, and the
        // reflow dropped a. Its text is in history already, but an end on a
        // row the reflow slid past starts again at the prompt, as any does.
        let (mut t, _p, _v, mut flat, mut seq, mut sel, _repaint) =
            rows_ahead_of_their_repaint(4, b"a\r\nb\r\nc\r\nd", b"\r\ne\r\nf", 0);
        assert_eq!(end_text(&t, &flat, sel.cursor), "a", "premise: before the repaint the cursor is on a");
        t.resize_selected(8, 3, &mut flat, &mut seq, Some(&mut sel));
        assert_eq!(grid_text(&t, 0).trim_end(), "b", "premise: the reflow dropped a");
        assert_eq!(t.grid.cursor().0, 2, "premise: the prompt is row 2");
        assert_eq!(flat[sel.cursor], grid_row(2), "the cursor starts again at the prompt");
    }

    // A drag shrinks a seam tile from six rows to three, the cursor on the
    // prompt; the producer's reply to the first shrink (five rows) lands
    // between the second and the third, or (`late`) after the third.
    fn shrinks_around_an_older_taller_reply(late: bool) -> (String, u64) {
        let (mut t, mut p, mut v, mut flat, mut seq, mut sel) =
            seam_selected(8, 6, b"m0\r\nm1\r\nm2\r\nm3\r\nm4\r\n$");
        assert_eq!(end_text(&t, &flat, sel.cursor), "$", "premise: the cursor is on the prompt");
        t.resize_selected(8, 5, &mut flat, &mut seq, Some(&mut sel));
        let taller = seam_resized(&mut p, &mut v, 8, 5);
        t.resize_selected(8, 4, &mut flat, &mut seq, Some(&mut sel));
        let reply = seam_resized(&mut p, &mut v, 8, 4);
        let taller = if late {
            Some(taller)
        } else {
            seam_send_each(&mut t, taller, &mut flat, &mut seq, &mut sel);
            None
        };
        t.resize_selected(8, 3, &mut flat, &mut seq, Some(&mut sel));
        let last = seam_resized(&mut p, &mut v, 8, 3);
        if let Some(r) = taller {
            seam_send_each(&mut t, r, &mut flat, &mut seq, &mut sel);
        }
        seam_send_each(&mut t, reply, &mut flat, &mut seq, &mut sel);
        seam_send_each(&mut t, last, &mut flat, &mut seq, &mut sel);
        assert_eq!(grid_text(&t, 2).trim_end(), "$", "premise: the producer keeps the prompt on the last row");
        (end_text(&t, &flat, sel.cursor), t.scrollback.rows_left())
    }

    #[test]
    fn a_shrink_after_an_older_taller_repaint_keeps_the_prompt_end() {
        // That reply names a cursor row below the shorter grid, which reads it
        // on its last row -- the prompt's, as the count places it while the
        // rows the second shrink dropped are on their way -- so the third
        // shrink slides the window from the prompt. Anchored on another row it
        // would cut the prompt off below and restart the cursor above it.
        assert_eq!(
            shrinks_around_an_older_taller_reply(true),
            (String::from("$"), 3),
            "control: the replies trail the drag"
        );
        assert_eq!(
            shrinks_around_an_older_taller_reply(false),
            (String::from("$"), 3),
            "the older reply lands mid-drag"
        );
    }

    #[test]
    fn a_shrink_before_the_repaint_settles_exactly_in_one_read() {
        // The same, with the old repaint and the reply in one read: no paint
        // between them, so a wrong count at the old repaint reaches nothing.
        let (mut t, mut p, mut v, mut flat, mut seq, mut sel, mut repaint) =
            rows_ahead_of_their_repaint(4, b"a\r\nb\r\nc\r\nd", b"\r\ne\r\nf", 2);
        t.resize_selected(8, 3, &mut flat, &mut seq, Some(&mut sel));
        assert_eq!(end_text(&t, &flat, sel.cursor), "c", "premise: the cursor slid with c");
        repaint.extend(seam_resized(&mut p, &mut v, 8, 3));
        seam_send(&mut t, repaint);
        t.refresh_selected(&mut flat, &mut seq, Some(&mut sel));
        assert_eq!(grid_text(&t, 0).trim_end(), "d", "premise: the producer's window slid past c");
        assert_eq!(end_text(&t, &flat, sel.cursor), "c", "the cursor followed c into history");
    }

    #[test]
    fn an_old_repaint_after_a_shrink_moves_the_selection_by_the_rows_it_shows_gone() {
        // The old repaint shows one row gone past what the reflow dropped
        // (b), not two: a was both dropped and among the rows that arrived.
        let (mut t, mut p, mut v, mut flat, mut seq, mut sel, repaint) =
            rows_ahead_of_their_repaint(6, b"a\r\nb\r\nc\r\nd\r\ne\r\nf", b"\r\ng\r\nh", 4);
        assert_eq!(end_text(&t, &flat, sel.cursor), "e", "premise: the cursor is on e");
        t.resize_selected(8, 5, &mut flat, &mut seq, Some(&mut sel));
        assert_eq!(grid_text(&t, 0).trim_end(), "b", "premise: the reflow dropped a");
        assert_eq!(end_text(&t, &flat, sel.cursor), "e", "the cursor slid with e");
        seam_send_each(&mut t, repaint, &mut flat, &mut seq, &mut sel);
        assert_eq!(grid_text(&t, 2).trim_end(), "e", "premise: the old repaint shows e on row 2");
        assert_eq!(end_text(&t, &flat, sel.cursor), "e", "the old repaint: the cursor is still on e");
        seam_send_each(&mut t, seam_resized(&mut p, &mut v, 8, 5), &mut flat, &mut seq, &mut sel);
        assert_eq!(grid_text(&t, 1).trim_end(), "e", "premise: the reply slid past c");
        assert_eq!(end_text(&t, &flat, sel.cursor), "e", "and after the reply");
    }

    #[test]
    fn a_shrink_and_a_grow_back_before_the_repaint_keep_the_selection_on_its_rows() {
        // The drag comes back before the producer sees it: it applies only
        // the last size, its own, and settles nothing it never shed. Counted
        // twice, a and b would push both ends to the first row at the old
        // repaint and the settle would bring them down onto e.
        let (mut t, mut p, mut v, mut flat, mut seq, mut sel, repaint) =
            rows_ahead_of_their_repaint(6, b"a\r\nb\r\nc\r\nd\r\ne\r\n$", b"\r\nf\r\n$", 3);
        sel.anchor = Some(flat.len() - 6 + 2);
        let ends = |t: &Tile, flat: &[crate::select::FlatRow], sel: &crate::select::Sel| {
            (end_text(t, flat, sel.anchor.expect("the anchor stays")), end_text(t, flat, sel.cursor))
        };
        assert_eq!(ends(&t, &flat, &sel), (String::from("c"), String::from("d")), "premise: c to d");
        t.resize_selected(8, 4, &mut flat, &mut seq, Some(&mut sel));
        assert_eq!(grid_text(&t, 0).trim_end(), "c", "premise: the reflow dropped a and b");
        t.resize_selected(8, 6, &mut flat, &mut seq, Some(&mut sel));
        assert_eq!(ends(&t, &flat, &sel), (String::from("c"), String::from("d")), "premise: the grow moved nothing");
        seam_send_each(&mut t, repaint, &mut flat, &mut seq, &mut sel);
        assert_eq!(ends(&t, &flat, &sel), (String::from("c"), String::from("d")), "the old repaint: still c to d");
        seam_send_each(&mut t, seam_resized(&mut p, &mut v, 8, 6), &mut flat, &mut seq, &mut sel);
        assert_eq!(ends(&t, &flat, &sel), (String::from("c"), String::from("d")), "and after the reply");
    }

    #[test]
    fn a_selection_moves_with_the_grid_at_the_repaint_not_at_the_rows() {
        // A scroll is two records -- the rows that left, then the repaint that
        // shows them gone -- and the session paints between reads. Until the
        // repaint the grid still shows every row where it was: an end moved at
        // the rows would sit on another row's text, and the band and Enter
        // would act on that row.
        let (mut t, mut p, mut v, mut flat, mut seq, mut sel) = seam_selected(8, 4, b"a\r\nb\r\nc\r\n$");
        assert_eq!(flat[sel.cursor], grid_row(3), "premise: the selection is on the prompt");
        sel.anchor = Some(flat.len() - 4 + 1);
        let mut out = Vec::new();
        p.feed(&mut v, b"\r\nd\r\n$", &mut out);
        let at = out
            .iter()
            .position(|r| matches!(r, Record::CellDiff { .. }))
            .expect("premise: the producer repaints");
        let repaint = out.split_off(at);
        assert!(
            out.iter().any(|r| matches!(r, Record::ScrollOff { .. })),
            "premise: the rows come ahead of the repaint"
        );
        seam_send_each(&mut t, out, &mut flat, &mut seq, &mut sel);
        assert_eq!(flat[sel.cursor], grid_row(3), "before the repaint the cursor stays on its row");
        assert_eq!(grid_text(&t, 3).trim_end(), "$", "which still shows the prompt");
        assert_eq!(sel.anchor.map(|a| flat[a]), Some(grid_row(1)), "and the anchor on b's row");
        assert_eq!(grid_text(&t, 1).trim_end(), "b");
        seam_send_each(&mut t, repaint, &mut flat, &mut seq, &mut sel);
        assert_eq!(grid_text(&t, 1).trim_end(), "$", "premise: the repaint moved the prompt up two");
        assert_eq!(flat[sel.cursor], grid_row(1), "and the cursor with it");
        let anchor = sel.anchor.expect("the anchor stays");
        assert_eq!(
            crate::select::row_text(&t.scrollback, flat[anchor]).trim_end(),
            "b",
            "the anchor followed b into history"
        );
    }

    #[test]
    fn a_resize_on_the_alt_screen_holds_a_selection_made_before_its_reply() {
        // The mirror resized while a TUI ran, and the TUI exited before the
        // resize reached the producer: the main screen came back cut at the
        // old width, and a selection was made on it. The producer then re-cut
        // its main screen, and rows cut at the new width scroll off: counted
        // as a distance they would put the cursor on output above the prompt.
        // The width change opened the re-cut on the alt screen, so the grid
        // ends hold until the reply, which starts them on its prompt.
        let (mut t, mut p, mut v, _, _, _) = seam_selected(8, 4, b"aaaaaaaa\r\nbbbbbbbb\r\ncccccccc\r\n$");
        seam_step(&mut t, &mut p, &mut v, b"\x1b[?1049h");
        assert_eq!(t.mode, ScreenMode::AltScreen, "premise: the TUI's screen is up");
        t.resize(4, 4);
        seam_step(&mut t, &mut p, &mut v, b"\x1b[?1049l");
        assert_eq!(t.mode, ScreenMode::Normal, "premise: the main screen is back");
        assert_eq!(t.scrollback.rewraps() % 2, 1, "the width change on the alt screen opened the re-cut");
        // Normal mode, the cursor moved up off the prompt: a selection that
        // never moved would stay on this row.
        let (mut flat, mut seq) = (Vec::new(), u64::MAX);
        t.refresh_selected(&mut flat, &mut seq, None);
        let at = crate::select::Stamp::of(&t.scrollback, &flat);
        let mut sel = crate::select::Sel::at(flat.len() - 4 + 1, at);
        assert_eq!(flat[sel.cursor], grid_row(1), "premise: the selection is on grid row 1");
        seam_send_each(&mut t, seam_resized(&mut p, &mut v, 4, 4), &mut flat, &mut seq, &mut sel);
        assert_eq!(t.scrollback.rows_scrolled(), 3, "premise: the producer's re-cut shed three rows");
        assert_eq!(t.scrollback.rewraps() % 2, 0, "premise: its repaint settled the re-cut");
        assert_eq!(t.grid.cursor().0, 3, "premise: the repaint's prompt is row 3");
        assert_eq!(
            flat[sel.cursor],
            grid_row(3),
            "the cursor is on the prompt, not on a row counted at the new width"
        );
    }

    #[test]
    fn inside_an_open_re_cut_a_run_goes_when_the_grid_moves_under_it() {
        // A held end keeps its grid row, but its run key names columns of the
        // line the grid showed there: once the grid has moved, the row under
        // the key is another line, cut at another width.
        let mut t = daylight_tile(8, 6);
        t.apply(whole_grid(
            8,
            &[("a", false), ("b", false), ("c", false), ("d", false), ("e", false), ("$", false)],
            (5, 1),
            false,
        ));
        let (mut flat, mut seq) = (Vec::new(), u64::MAX);
        t.refresh_selected(&mut flat, &mut seq, None);
        let mut sel = crate::select::Sel::at(5, crate::select::Stamp::of(&t.scrollback, &flat));
        t.resize_selected(4, 6, &mut flat, &mut seq, Some(&mut sel));
        assert_eq!(t.scrollback.rewraps() % 2, 1, "premise: the re-cut is unsettled");
        sel.cursor = 4;
        sel.obj = Some(1);
        // Output in flight at the old width: its row arrives, then its repaint.
        t.apply(Record::ScrollOff {
            rows: vec![vec![Cell {
                ch: 'a',
                fg: 0,
                bg: 0,
                attrs: 0,
                span: 0,
            }]],
            wrapped: vec![false],
        });
        t.refresh_selected(&mut flat, &mut seq, Some(&mut sel));
        assert_eq!(flat[sel.cursor], grid_row(4), "premise: the held cursor keeps its row");
        assert_eq!(sel.obj, Some(1), "the grid has not moved: the run is still under the key");
        t.apply(whole_grid(
            8,
            &[("b", false), ("c", false), ("d", false), ("e", false), ("$", false), ("", false)],
            (4, 1),
            false,
        ));
        t.refresh_selected(&mut flat, &mut seq, Some(&mut sel));
        assert_eq!(flat[sel.cursor], grid_row(4), "the held cursor still keeps its row");
        assert_eq!(sel.obj, None, "but the grid moved under it, and its run went");
    }

    #[test]
    fn inside_an_open_re_cut_a_run_goes_at_a_repaint_that_moves_no_count() {
        // The rows a narrowing dropped arrive cut at the old width and take
        // back the shed, so no count moves; the repaint after them still
        // writes the old width's cells over the rows the re-cut holds.
        let mut t = daylight_tile(8, 4);
        t.apply(whole_grid(
            8,
            &[("aaaaaaaa", false), ("bbbbbbbb", false), ("cccccccc", false), ("$", false)],
            (3, 1),
            false,
        ));
        let (mut flat, mut seq) = (Vec::new(), u64::MAX);
        t.refresh_selected(&mut flat, &mut seq, None);
        let mut sel = crate::select::Sel::at(3, crate::select::Stamp::of(&t.scrollback, &flat));
        t.resize_selected(4, 4, &mut flat, &mut seq, Some(&mut sel));
        assert_eq!(t.scrollback.rewraps() % 2, 1, "premise: the re-cut is unsettled");
        let left = t.scrollback.rows_left();
        assert_eq!(left, 3, "premise: the re-cut dropped three rows off the top");
        sel.cursor = flat.len() - 4;
        sel.obj = Some(1);
        let row8 = |s: &str| {
            s.chars()
                .map(|ch| Cell {
                    ch,
                    fg: 0,
                    bg: 0,
                    attrs: 0,
                    span: 0,
                })
                .collect::<Vec<_>>()
        };
        t.apply(Record::ScrollOff {
            rows: vec![row8("aaaaaaaa"), row8("bbbbbbbb")],
            wrapped: vec![false, false],
        });
        t.refresh_selected(&mut flat, &mut seq, Some(&mut sel));
        assert_eq!(sel.obj, Some(1), "no repaint yet: the run is still under the key");
        t.apply(whole_grid(
            8,
            &[("cccccccc", false), ("$", false), ("", false), ("", false)],
            (1, 1),
            false,
        ));
        assert_eq!(t.scrollback.rows_left(), left, "premise: the repaint moved no count");
        t.refresh_selected(&mut flat, &mut seq, Some(&mut sel));
        assert_eq!(flat[sel.cursor], grid_row(0), "the held cursor keeps its row");
        assert_eq!(sel.obj, None, "but the repaint wrote other cells under it, and its run went");
    }

    #[test]
    fn inside_an_open_re_cut_a_run_goes_at_a_repaint_with_no_rows() {
        // A program rewrites a row in place at the old width: nothing
        // arrives, and only the repaint says the grid moved under the key.
        let mut t = daylight_tile(8, 6);
        t.apply(whole_grid(
            8,
            &[("a", false), ("b", false), ("c", false), ("d", false), ("e", false), ("$", false)],
            (5, 1),
            false,
        ));
        let (mut flat, mut seq) = (Vec::new(), u64::MAX);
        t.refresh_selected(&mut flat, &mut seq, None);
        let mut sel = crate::select::Sel::at(5, crate::select::Stamp::of(&t.scrollback, &flat));
        t.resize_selected(4, 6, &mut flat, &mut seq, Some(&mut sel));
        assert_eq!(t.scrollback.rewraps() % 2, 1, "premise: the re-cut is unsettled");
        sel.cursor = 3;
        sel.obj = Some(1);
        t.apply(whole_grid(
            8,
            &[("a", false), ("b", false), ("c", false), ("x", false), ("e", false), ("$", false)],
            (5, 1),
            false,
        ));
        t.refresh_selected(&mut flat, &mut seq, Some(&mut sel));
        assert_eq!(flat[sel.cursor], grid_row(3), "the held cursor keeps its row");
        assert_eq!(sel.obj, None, "the repaint moved no count, and the run went");
    }

    #[test]
    fn esc_is_the_apps_until_the_normal_screen_is_repainted() {
        // A full-screen app exits: the mode flip is a record of its own, and
        // the main screen's repaint can be reads behind it. Until it lands
        // the grid shows the app's last frame, and Esc is still the app's.
        let (mut t, mut p, mut v, _, _, _) = seam_selected(8, 4, b"a\r\n$");
        assert!(t.normal_screen_shown(), "premise: the normal screen shows");
        assert!(t.modal_key(false, 0x1b, 1), "premise: Esc enters Normal mode");
        seam_step(&mut t, &mut p, &mut v, b"\x1b[?1049h");
        assert!(!t.normal_screen_shown(), "premise: the app's screen is up");
        assert!(!t.modal_key(false, 0x1b, 1), "the app's screen: Esc is the app's");
        let mut out = Vec::new();
        p.feed(&mut v, b"\x1b[?1049l", &mut out);
        let at = out
            .iter()
            .position(|r| matches!(r, Record::Mode(ScreenMode::Normal)))
            .expect("premise: the producer flips the mode");
        let repaint = out.split_off(at + 1);
        seam_send(&mut t, out);
        assert_eq!(t.mode, ScreenMode::Normal, "premise: the tile's mode is normal");
        assert!(!t.normal_screen_shown(), "the grid still shows the app's frame");
        assert!(!t.modal_key(false, 0x1b, 1), "Esc is still the app's");
        seam_send(&mut t, repaint);
        assert!(t.normal_screen_shown(), "the repaint landed: the normal screen shows");
        assert!(t.modal_key(false, 0x1b, 1), "Esc enters Normal mode again");
    }

    #[test]
    fn the_modal_gate_takes_a_press_in_normal_mode_and_the_esc_that_enters_it() {
        // On the normal screen Normal mode keeps every key, pressed or
        // repeated; in Insert only Esc's press goes to the transcript, and a
        // release never does.
        let t = daylight_tile(8, 4);
        assert!(t.normal_screen_shown(), "premise: the normal screen shows");
        let k = u32::from('k');
        assert!(t.modal_key(true, k, 1), "Normal mode keeps a key");
        assert!(t.modal_key(true, k, 2), "and its repeat");
        assert!(t.modal_key(true, 0x1b, 1), "and Esc");
        assert!(t.modal_key(false, 0x1b, 1), "Esc enters Normal mode");
        assert!(!t.modal_key(false, 0x1b, 2), "Insert: an Esc repeat is the program's");
        assert!(!t.modal_key(false, k, 1), "Insert: a key is the program's");
        assert!(!t.modal_key(true, k, 0), "a release never reaches Normal mode");
        assert!(!t.modal_key(false, 0x1b, 0), "nor does Esc's");
    }

    // A seam tile of eight columns and six rows showing the shell's `bytes`,
    // its prompt on the last row; a full-screen app ran with its cursor on
    // its own last row, then exited. The mode flip is applied; the main
    // screen's repaint, the next read, is returned.
    fn app_exit_before_its_repaint(bytes: &[u8]) -> (Tile, kaua_term::Producer, vt::Vt, Vec<Record>) {
        let (mut t, mut p, mut v, _, _, _) = seam_selected(8, 6, bytes);
        seam_step(&mut t, &mut p, &mut v, b"\x1b[?1049h\x1b[6;1Hstatus");
        assert_eq!(t.grid.cursor().0, 5, "premise: the app's cursor is on its last row");
        let mut out = Vec::new();
        p.feed(&mut v, b"\x1b[?1049l", &mut out);
        let at = out
            .iter()
            .position(|r| matches!(r, Record::Mode(ScreenMode::Normal)))
            .expect("premise: the producer flips the mode");
        let repaint = out.split_off(at + 1);
        seam_send(&mut t, out);
        (t, p, v, repaint)
    }

    #[test]
    fn a_resize_before_the_normal_screens_repaint_counts_no_row_of_the_apps_frame() {
        // The app's last frame is still on the grid when a drag shrinks the
        // tile. Reflowed, it would count the three rows the app's cursor
        // slides past as rows that left; the producer's reply carries three
        // real ones, and a selection made between the repaint and the reply
        // would stay three rows off its text. Cropped, as the alt screen is,
        // nothing is counted until the reply's rows arrive.
        let (mut shown, _, _, repaint) = app_exit_before_its_repaint(b"m0\r\nm1\r\nm2\r\nm3\r\nm4\r\n$");
        seam_send(&mut shown, repaint);
        let left = shown.scrollback.rows_left();
        shown.resize(8, 3);
        assert_eq!(shown.scrollback.rows_left(), left + 3, "control: the normal screen's reflow drops three rows");
        let (mut t, mut p, mut v, repaint) = app_exit_before_its_repaint(b"m0\r\nm1\r\nm2\r\nm3\r\nm4\r\n$");
        assert!(!t.normal_screen_shown(), "premise: the app's frame is on the grid");
        let counts = (t.scrollback.rows_left(), t.scrollback.rows_scrolled());
        t.resize(8, 3);
        assert_eq!(
            (t.scrollback.rows_left(), t.scrollback.rows_scrolled()),
            counts,
            "no row of the app's frame left the grid"
        );
        seam_send(&mut t, repaint);
        assert_eq!(grid_text(&t, 0).trim_end(), "m0", "premise: the repaint's top rows show");
        let (mut flat, mut seq) = (Vec::new(), u64::MAX);
        t.refresh_selected(&mut flat, &mut seq, None);
        let mut sel = crate::select::Sel::at(flat.len() - 3, crate::select::Stamp::of(&t.scrollback, &flat));
        assert_eq!(end_text(&t, &flat, sel.cursor), "m0", "premise: the selection is on m0");
        seam_send_each(&mut t, seam_resized(&mut p, &mut v, 8, 3), &mut flat, &mut seq, &mut sel);
        assert_eq!(grid_text(&t, 0).trim_end(), "m3", "premise: the producer's window slid past m0, m1 and m2");
        assert_eq!(end_text(&t, &flat, sel.cursor), "m0", "the cursor followed m0 into history");
    }

    #[test]
    fn the_apps_last_frame_paints_as_its_grid_until_the_normal_screens_repaint() {
        // Until the main screen's repaint lands the grid holds the app's last
        // frame: it paints as the app painted it, the mono grid alone, not
        // as normal-screen rows laid out beneath the history.
        let mut gs = GlyphSource::new_vendored(512);
        let sheet = crate::layout::daylight_sheet(100);
        let (cw, ch, _) = gs.mono_cell();
        let (w, h) = ((8 * cw) as usize, (6 * ch) as usize);
        let mut cart = Cartoon::new();
        let (mut t, _, _, repaint) =
            app_exit_before_its_repaint(b"h0\r\nh1\r\nm0\r\nm1\r\nm2\r\nm3\r\nm4\r\n$");
        let pending = t.render(&mut cart, w, h, &mut gs, &sheet, &mut 0, None);
        assert_eq!(pending, 6 * ch, "the app's frame paints as the mono grid alone");
        seam_send(&mut t, repaint);
        let shown = t.render(&mut cart, w, h, &mut gs, &sheet, &mut 0, None);
        assert!(shown > pending, "control: the normal screen lays its rows beneath the history ({shown} > {pending})");
    }

    #[test]
    fn the_apps_last_frame_keeps_the_terminal_ground_until_the_repaint() {
        // HALCYON-INSTRUMENT 14.7: the raw application grid sits on
        // terminal_bg -- the app's last frame too, while the normal screen's
        // repaint is on its way.
        let mut gs = GlyphSource::new_vendored(512);
        let s = inst_sheet();
        assert_ne!(s.ground, s.theme.terminal.bg, "premise: the two grounds differ");
        let (cw, ch, _) = gs.mono_cell();
        let (w, h) = ((8 * cw) as usize, (4 * ch) as usize);
        let mut t = Tile::new(8, 4, s.theme.terminal);
        let diff = |c: char| Record::CellDiff {
            changed: vec![(3, 0, cell(c))],
            cursor: (3, 1, true),
            wrapped: vec![],
            top_continues: false,
        };
        t.apply(Record::Mode(ScreenMode::AltScreen));
        t.apply(diff('x'));
        t.apply(Record::Mode(ScreenMode::Normal));
        let mut cart = Cartoon::new();
        t.render(&mut cart, w, h, &mut gs, &s, &mut 0, None);
        assert!(
            matches!(cart.ops.first(), Some(Op::Clear { color }) if *color == s.theme.terminal.bg),
            "the app's frame keeps terminal_bg"
        );
        t.apply(diff('$'));
        t.render(&mut cart, w, h, &mut gs, &s, &mut 0, None);
        assert!(
            matches!(cart.ops.first(), Some(Op::Clear { color }) if *color == s.ground),
            "control: the normal screen sits on the sheet's ground"
        );
    }

    // A seam tile of eight columns and six rows showing the shell's `bytes`;
    // the shell prints `typed` and a full-screen app starts in the same read.
    // The mode flip is applied; the app's first paint, the next read, is
    // returned.
    fn app_start_before_its_paint(cols: usize, bytes: &[u8], typed: &[u8]) -> (Tile, Vec<Record>) {
        let (mut t, mut p, mut v, _, _, _) = seam_selected(cols, 6, bytes);
        let mut feed = Vec::from(typed);
        feed.extend_from_slice(b"\x1b[?1049h");
        let mut out = Vec::new();
        p.feed(&mut v, &feed, &mut out);
        let at = out
            .iter()
            .position(|r| matches!(r, Record::Mode(ScreenMode::AltScreen)))
            .expect("premise: the producer flips the mode");
        let paint = out.split_off(at + 1);
        seam_send(&mut t, out);
        (t, paint)
    }

    #[test]
    fn the_shells_frame_paints_as_before_until_the_apps_first_paint() {
        // A full-screen app starts: its first paint (the blank alt screen, a
        // whole grid) can be reads behind the mode flip. Until it lands the
        // grid holds the shell's screen, which paints as it did -- beneath the
        // history, its soft-wrapped line joined -- not as a raw cell grid.
        let mut gs = GlyphSource::new_vendored(512);
        let sheet = crate::layout::daylight_sheet(100);
        let (cw, ch, _) = gs.mono_cell();
        let (w, h) = ((16 * cw) as usize, (6 * ch) as usize);
        let mut cart = Cartoon::new();
        // What the tile paints, every op, not only its height.
        let mut paint_of = |t: &mut Tile| {
            let content = t.render(&mut cart, w, h, &mut gs, &sheet, &mut 0, None);
            (content, cart_fingerprint(&cart))
        };
        // Narrow letters, and wide enough that joining the rows changes the paint (asserted below).
        let bytes = b"h0\r\nh1\r\nh2\r\niiiiiiiiiiiiiiiiiiii\r\nm1\r\n$ ";
        let typed = b"h0\r\nh1\r\nh2\r\niiiiiiiiiiiiiiiiiiii\r\nm1\r\n$ vi";
        let (mut before, _, _, _, _, _) = seam_selected(16, 6, typed);
        assert!(before.grid.wrapped().iter().any(|&w| w), "premise: the shell's screen holds a soft-wrapped row");
        let shown = paint_of(&mut before);
        // The same frame repainted without its wrap flags, as the alt screen's would say.
        let (mut split, _, _, _, _, _) = seam_selected(16, 6, typed);
        repaint_unchanged(&mut split);
        assert_ne!(paint_of(&mut split), shown, "premise: the paint tells the joined line from its rows split");
        let (mut t, paint) = app_start_before_its_paint(16, bytes, b"vi");
        assert_eq!(t.grid.wrapped(), before.grid.wrapped(), "the shell's frame keeps its wrap flags");
        assert_eq!(paint_of(&mut t), shown, "the shell's frame paints as it did before the flip");
        seam_send(&mut t, paint);
        assert_eq!(paint_of(&mut t).0, 6 * ch, "control: the app's paint is the mono grid alone");
    }

    #[test]
    fn the_shells_frame_keeps_the_sheets_ground_until_the_apps_first_paint() {
        // HALCYON-INSTRUMENT 14.7: the rich document sits on the sheet's
        // ground -- the shell's last frame too, while the app's first paint
        // (the raw grid, on terminal_bg) is on its way.
        let mut gs = GlyphSource::new_vendored(512);
        let s = inst_sheet();
        assert_ne!(s.ground, s.theme.terminal.bg, "premise: the two grounds differ");
        let (cw, ch, _) = gs.mono_cell();
        let (w, h) = ((8 * cw) as usize, (4 * ch) as usize);
        let mut t = Tile::new(8, 4, s.theme.terminal);
        let diff = |c: char| Record::CellDiff {
            changed: vec![(3, 0, cell(c))],
            cursor: (3, 1, true),
            wrapped: vec![],
            top_continues: false,
        };
        t.apply(diff('$'));
        t.apply(Record::Mode(ScreenMode::AltScreen));
        let mut cart = Cartoon::new();
        t.render(&mut cart, w, h, &mut gs, &s, &mut 0, None);
        assert!(
            matches!(cart.ops.first(), Some(Op::Clear { color }) if *color == s.ground),
            "the shell's frame keeps the sheet's ground"
        );
        t.apply(diff(' '));
        t.render(&mut cart, w, h, &mut gs, &s, &mut 0, None);
        assert!(
            matches!(cart.ops.first(), Some(Op::Clear { color }) if *color == s.theme.terminal.bg),
            "control: the app's paint sits on terminal_bg"
        );
    }

    #[test]
    fn a_settle_that_moves_no_row_still_restarts_the_grid_ends() {
        // A widening sheds nothing, so its reply carries no rows, and its
        // settle moves no count: only the re-cut's own change makes the list
        // re-read, and a cursor moved inside the open re-cut starts on the
        // repaint's prompt.
        let (mut t, mut p, mut v, mut flat, mut seq, mut sel) = seam_selected(4, 4, b"aaaaaaaa\r\n$");
        assert_eq!(t.grid.cursor().0, 2, "premise: the a line takes two rows at four columns");
        t.resize_selected(8, 4, &mut flat, &mut seq, Some(&mut sel));
        assert_eq!(t.scrollback.rewraps() % 2, 1, "premise: the widening opened a re-cut");
        sel.cursor = flat.len() - 4;
        let reply = seam_resized(&mut p, &mut v, 8, 4);
        assert!(
            !reply.iter().any(|r| matches!(r, Record::ScrollOff { .. })),
            "premise: the widening sheds nothing"
        );
        seam_send_each(&mut t, reply, &mut flat, &mut seq, &mut sel);
        assert_eq!(t.scrollback.rows_left(), 0, "premise: no row left the grid");
        assert_eq!(t.scrollback.rewraps() % 2, 0, "premise: the repaint settled the re-cut");
        assert_eq!(t.grid.cursor().0, 1, "premise: the repaint's prompt is row 1");
        assert_eq!(
            flat[sel.cursor],
            grid_row(1),
            "the cursor starts on the prompt, not on the row it was moved to"
        );
    }

    #[test]
    fn rows_of_a_reply_the_grid_moved_past_move_nothing_before_its_repaint() {
        // Narrowed, widened, narrowed and widened again before any reply; the
        // producer applies each. Its reply to the first widen settles at the
        // grid's dims, early. Its next narrow sheds rows four columns wide,
        // and a paint can fall between those rows and the repaint behind them
        // (a whole-grid diff spans many reads). The rows alone move no end --
        // the grid has not moved -- so none is counted into history, on the c
        // line, where no report restarts it; the repaint at four columns
        // reopens the re-cut, and the last reply starts the cursor on its
        // prompt.
        let (mut t, mut p, mut v, mut flat, mut seq, mut sel) =
            seam_selected(8, 4, b"aaaaaaaa\r\nbbbbbbbb\r\ncccccccc\r\n$");
        for (cols, rows) in [(4, 4), (8, 4), (4, 4), (8, 4)] {
            t.resize_selected(cols, rows, &mut flat, &mut seq, Some(&mut sel));
        }
        let first_narrow = seam_resized(&mut p, &mut v, 4, 4);
        let first_widen = seam_resized(&mut p, &mut v, 8, 4);
        let mut output = Vec::new();
        p.feed(&mut v, b"\r\nyyyyyyyy\r\n$", &mut output);
        let mut narrow = seam_resized(&mut p, &mut v, 4, 4);
        let widen = seam_resized(&mut p, &mut v, 8, 4);
        seam_send_each(&mut t, first_narrow, &mut flat, &mut seq, &mut sel);
        seam_send_each(&mut t, first_widen, &mut flat, &mut seq, &mut sel);
        assert_eq!(t.scrollback.rewraps() % 2, 0, "premise: the reply to the first widen settled, early");
        seam_send_each(&mut t, output, &mut flat, &mut seq, &mut sel);
        // The narrow's rows arrive; its ack and repaint are still unread.
        let ack = narrow
            .iter()
            .position(|r| matches!(r, Record::Control(Control::WinsizeAck)))
            .expect("premise: the producer acks the narrow");
        let reply = narrow.split_off(ack);
        assert!(!narrow.is_empty(), "premise: the narrow shed rows ahead of its ack");
        let held = flat[sel.cursor];
        assert_eq!(held.block, crate::select::GRID_BLOCK, "premise: the cursor is on the grid");
        seam_send_each(&mut t, narrow, &mut flat, &mut seq, &mut sel);
        assert_eq!(flat[sel.cursor], held, "the rows alone move no end: the grid has not moved");
        assert_eq!(t.scrollback.rewraps() % 2, 0, "nor do they open a re-cut");
        seam_send_each(&mut t, reply, &mut flat, &mut seq, &mut sel);
        assert_eq!(t.scrollback.rewraps() % 2, 1, "the repaint at four columns reopened the re-cut");
        seam_send_each(&mut t, widen, &mut flat, &mut seq, &mut sel);
        assert_eq!(t.scrollback.rewraps() % 2, 0, "premise: the last reply settled the re-cut");
        assert_eq!(t.grid.cursor().0, 2, "premise: the last reply's prompt is row 2");
        assert_eq!(
            flat[sel.cursor],
            grid_row(2),
            "the cursor is on the prompt, not in history on the c line"
        );
    }

    #[test]
    fn only_the_acked_repaint_at_the_grids_dims_settles() {
        // A whole-grid diff with no ack before it (an alt-screen switch, a
        // palette re-emit) is no resize's reply, and an acked repaint at
        // other dims answers an earlier resize: neither settles.
        let mut t = daylight_tile(8, 4);
        t.apply(whole_grid(8, &[("a", false), ("b", false), ("c", false), ("d", false)], (3, 1), false));
        t.resize(8, 2);
        assert_eq!(t.scrollback.rows_scrolled(), 2, "premise: the mirror slid past a and b");
        t.apply(whole_grid(8, &[("c", false), ("d", false)], (1, 1), false));
        assert_eq!(t.scrollback.rows_scrolled(), 2, "no ack: no settle");
        t.apply(Record::Control(Control::WinsizeAck));
        t.apply(whole_grid(8, &[("a", false), ("b", false), ("c", false), ("d", false)], (3, 1), false));
        assert_eq!(t.scrollback.rows_scrolled(), 2, "an acked repaint at other dims settles nothing");
        assert_eq!(t.scrollback.rewraps() % 2, 0, "nor, at the grid's width, opens a re-cut");
        t.apply(whole_grid(8, &[("c", false), ("d", false)], (1, 1), false));
        assert_eq!(t.scrollback.rows_scrolled(), 2, "and its ack is spent");
        t.apply(Record::Control(Control::WinsizeAck));
        t.apply(whole_grid(8, &[("c", false), ("d", false)], (1, 1), false));
        assert_eq!(t.scrollback.rows_scrolled(), 0, "the acked repaint at the grid's dims settles");
        // A new width re-cuts the grid until its repaint.
        t.resize(4, 2);
        assert_eq!(t.scrollback.rewraps() % 2, 1, "a new width opens a re-cut");
        t.apply(Record::Control(Control::WinsizeAck));
        t.apply(whole_grid(4, &[("c", false), ("d", false)], (1, 1), false));
        assert_eq!(t.scrollback.rewraps() % 2, 0, "its repaint settles it");
        // The ack names no resize: a reply at another width answers one the
        // grid moved past, and the producer's lines are cut at that width.
        t.apply(Record::Control(Control::WinsizeAck));
        t.apply(whole_grid(8, &[("c", false), ("d", false)], (1, 1), false));
        assert_eq!(t.scrollback.rewraps() % 2, 1, "a reply at another width reopens the re-cut");
        t.apply(Record::Control(Control::WinsizeAck));
        t.apply(whole_grid(4, &[("c", false), ("d", false)], (1, 1), false));
        assert_eq!(t.scrollback.rewraps() % 2, 0, "and the reply at the grid's dims closes it");
        // On the alt screen too: the producer re-cuts its main screen
        // beneath it.
        t.apply(Record::Mode(ScreenMode::AltScreen));
        t.resize(6, 2);
        assert_eq!(t.scrollback.rewraps() % 2, 1, "a new width on the alt screen opens a re-cut");
        t.apply(Record::Control(Control::WinsizeAck));
        t.apply(whole_grid(6, &[("c", false), ("d", false)], (1, 1), false));
        assert_eq!(t.scrollback.rewraps() % 2, 0, "its repaint settles it");
        t.apply(Record::Control(Control::WinsizeAck));
        t.apply(whole_grid(4, &[("c", false), ("d", false)], (1, 1), false));
        assert_eq!(t.scrollback.rewraps() % 2, 1, "and a reply at another width reopens it");
    }

    #[test]
    fn forget_history_keeps_an_image_not_yet_captioned() {
        // `view` uploads first and captions after: an image no object names
        // yet is not history.
        let mut t = daylight_tile(40, 2);
        t.apply(osc(1, "zone;k=output"));
        assert!(t.place_image(0xd, 1, 1, vec![1]));
        t.forget_history();
        assert!(t.media.contains(0xd));
    }

    #[test]
    fn forget_history_keeps_the_image_an_open_obj_names() {
        // An obj span still open at the chord survives it (what is written
        // next is the running command's), and so does the image it names --
        // even when no ring slot names it any more: a nested obj's frames
        // turned over the slot of the frame that opened it.
        use crate::transcript::SPAN_MAP_ENTRIES;
        let mut t = daylight_tile(40, 2);
        t.apply(osc(1, "zone;k=output"));
        t.apply(osc(2, &alloc::format!("obj;type=inline-image;ref={:032x}", 0xeu128)));
        t.apply(osc(3, "obj;type=path;ref=/inner"));
        t.apply(osc(2 + SPAN_MAP_ENTRIES as u32, "em;class=dim"));
        let open = t.scrollback.open_block().id;
        assert!(
            !t.spans.named().contains(&(open, 1)),
            "premise: no ring slot names the image's obj"
        );
        assert!(t.place_image(0xe, 1, 1, vec![1]));
        t.forget_history();
        assert!(t.media.contains(0xe), "the open obj keeps its image");
    }

    const T_NS: u64 = 5_000_000_000;

    #[test]
    fn the_frame_records_open_and_close_the_hold_and_an_exit_cuts_it() {
        let mut t = tile();
        assert!(!t.hold.holds(T_NS), "no frame, no hold");
        t.apply(Record::Control(Control::SyncBegin));
        assert!(t.hold.holds(T_NS));
        t.apply(Record::Control(Control::SyncEnd));
        assert!(!t.hold.holds(T_NS + 1));
        assert_eq!(t.hold.painted(), vt::Held::UntilClose(1));
        t.apply(Record::Control(Control::SyncBegin));
        assert!(t.hold.holds(T_NS + 2));
        t.apply(Record::Control(Control::Exit(0)));
        assert!(!t.hold.holds(T_NS + 3), "the program's exit ends its frame");
        assert_eq!(
            t.hold.painted(),
            vt::Held::Cut(1),
            "cut short, never shown whole"
        );
    }

    /// FL-1 across every link, in lantern's shape: the slide's frame reaches
    /// the tile in two reads of the pipe, as the session reads it. The blank
    /// the first read leaves is held, never painted; the close lets the
    /// slide through.
    #[test]
    fn a_slide_split_across_two_reads_is_held_until_its_close() {
        let mut v = vt::Vt::new(20, 4);
        v.set_capture_events(true);
        let mut p = kaua_term::Producer::new(&v);
        let mut t = Tile::new(20, 4, libhalcyon::theme::daylight_palette());
        seam_step(&mut t, &mut p, &mut v, b"% lantern deck\r\n");
        let mut recs = Vec::new();
        p.feed(
            &mut v,
            b"\x1b[?2026h\x1b[0m\x1b[H\x1b[2Jslide one\x1b[?2026l",
            &mut recs,
        );
        let mut wire = Vec::new();
        let mut cut = 0;
        for r in &recs {
            kaua_term::wire::encode_record(r, &mut wire);
            if *r == Record::Control(Control::ScreenErased) {
                cut = wire.len();
            }
        }
        assert!(
            cut > 0 && cut < wire.len(),
            "the read ends inside the frame"
        );
        let mut dec = kaua_term::wire::FrameDecoder::new();
        let mut read = |t: &mut Tile, bytes: &[u8]| {
            dec.push(bytes);
            while let Some(f) = dec.next_frame() {
                let (tag, payload) = f.expect("the producer's frames decode");
                t.apply(kaua_term::wire::parse_record(tag, &payload).expect("and parse"));
            }
        };
        read(&mut t, &wire[..cut]);
        assert_eq!(
            grid_text(&t, 0).trim_end(),
            "",
            "the first read leaves the blank"
        );
        assert!(t.hold.holds(T_NS), "and the paint waits");
        read(&mut t, &wire[cut..]);
        assert_eq!(grid_text(&t, 0).trim_end(), "slide one");
        assert!(!t.hold.holds(T_NS + 1), "the close lets the slide through");
        assert_eq!(t.hold.painted(), vt::Held::UntilClose(1));
    }
}
