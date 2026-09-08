// The transcript model (HALCYON.md section 13.3): store semantics, derive
// pixels. A bounded deque of BLOCKS -- one per shell zone cycle (prompt /
// output; un-zoned foreign bytes coalesce into anonymous blocks) -- each
// holding line-discipline-resolved CELLS styled by the SGR pen (the vt
// crate's SgrPen: one SGR machinery, two consumers -- 13.4b) plus the
// Beacon span state (em / obj / hdr) and captured TABLES. Pixels are never
// stored; layout is a later, pure pass over frozen blocks.
//
// The feed is a byte STREAM: chunk boundaries fall anywhere, so feed()
// holds back an incomplete trailing escape (bounded by the wire caps) and
// carries partial UTF-8 across calls -- feeding a stream byte-by-byte
// yields the identical structure to feeding it whole (the determinism
// property the tests pin).
//
// Row-addressed control (CUP/CUU/CUD/ED/scroll/DECSTBM/alt-screen) is
// foreign-FULLSCREEN intent in a flowed transcript (13.4b): it paints
// nothing here and latches `raw_vt_intent` -- the pane-class flip that
// consumes it lands with the raw-VT pane (H-3); the alt-screen switch is
// the primary trigger.
//
// Deviations from the 13.3 sketch, deliberate (recorded at the chunk):
//   - Selection addressing is (block, line, col) over CELLS, not
//     (block, run, byte): the line discipline is column-based (\r
//     overwrite, tabs, EL), so cells are the honest unit; runs derive at
//     layout by grouping adjacent same-style cells.
//   - Beacon spans auto-close at a block boundary (a program dying with
//     an open `em` must not restyle the next prompt); the SGR pen
//     PERSISTS across blocks (terminal semantics).

use alloc::collections::BTreeMap;
use alloc::collections::VecDeque;
use alloc::string::String;
use alloc::vec::Vec;

use beacon::wire::{self, Event, Op};
use vt::{Palette, SgrPen};

pub const EM_NONE: u8 = 0;
pub const EM_EMPH: u8 = 1;
pub const EM_STRONG: u8 = 2;
pub const EM_DIM: u8 = 3;
pub const EM_CODE: u8 = 4;

/// The `hdr` byte packs the level (bits 0-1, 0-3) with the heading's ROLE
/// (BEACON.md 12.2 `class=title`, bit 2): a title page's heading -- the
/// herald that opens a splash/welcome -- which the stylesheet sets apart
/// from a section heading (HALCYON-COMPOSITION 3, the title-page pattern).
/// One byte because the tag rides the 16-byte span slot, which has no spare
/// byte; a `Style` keeps only these three bits (`HDR_MASK`).
pub const HDR_TITLE: u8 = 0x04;
pub const HDR_MASK: u8 = 0x07;

#[inline]
pub fn hdr_level(hdr: u8) -> u8 {
    hdr & 0x03
}

#[inline]
pub fn hdr_is_title(hdr: u8) -> bool {
    hdr & HDR_TITLE != 0
}

// The span-tag packing for a frame-fed tile (KT-1: the tile's transcript
// sees the Beacon FRAMES but never the text -- the text arrives as grid
// cells, tagged with the serial of the last frame before them). The console's
// byte-fed parser builds tables, pre blocks and rules from the stream; a
// tile has to REBUILD them from the cells, so the tag carries the structure
// each cell was written inside. The `em` byte: bits 0-2 the EM_* class, then
// the structure bits; the `hdr` byte: the HDR_MASK bits, then the table
// column (bits 3-6, 0-15). A `Style` keeps only the class / the HDR_MASK
// bits (`tag_style_em` / `tag_style_hdr`).
pub const TAG_EM_MASK: u8 = 0x07;
/// Written inside a `pre` block.
pub const TAG_PRE: u8 = 0x08;
/// Written inside a table cell (the column rides the hdr byte).
pub const TAG_CELL: u8 = 0x10;
/// The frame this cell follows was a `rule`: a rule precedes the line.
pub const TAG_RULE: u8 = 0x20;
/// The cell belongs to a table's header row.
pub const TAG_ROW_HDR: u8 = 0x40;
/// Written inside a PROMPT zone. Carried on the tag rather than looked up
/// through the tag's block: an empty zone-less block is dropped at the zone
/// cut and its id REUSED by the prompt block that follows (`freeze_open`),
/// so a block lookup would class a zone-less document (the welcome) as the
/// prompt that came after it.
pub const TAG_PROMPT: u8 = 0x80;

#[inline]
pub fn tag_style_em(em: u8) -> u8 {
    em & TAG_EM_MASK
}

#[inline]
pub fn tag_style_hdr(hdr: u8) -> u8 {
    hdr & HDR_MASK
}

#[inline]
pub fn tag_col(hdr: u8) -> usize {
    ((hdr >> 3) & 0x0F) as usize
}

/// One resolved cell style. `obj` is 0 = none, else index+1 into the
/// block's obj table; `em` is an EM_* class; `hdr` a heading level (0-3)
/// packed with the title flag (`hdr_level` / `hdr_is_title`).
#[derive(Clone, Copy, PartialEq)]
pub struct Style {
    pub fg: u32,
    pub bg: u32,
    pub attrs: u8,
    pub em: u8,
    pub obj: u16,
    pub hdr: u8,
}

impl Style {
    /// A Beacon annotation of any kind: the mark of DOCUMENT content (the
    /// producer spoke Beacon), as opposed to raw terminal bytes.
    #[inline]
    pub fn annotated(&self) -> bool {
        self.em != 0 || self.obj != 0 || self.hdr != 0
    }
}

/// How a line renders (HALCYON 14.13 + HALCYON-VISUAL 7/9, the operator's
/// sc3 ruling): PROMPT lines are the shell's own (proportional, the prompt
/// size); DOC lines are Beacon-structured content (proportional prose with
/// mono islands); RAW lines are a program's un-annotated terminal bytes --
/// "preformatted output, terminal content" -- set in the mono island. A
/// frozen block's lines INHERIT the block's class (`Block::class`); the live
/// grid block, which straddles zones, stamps each line from its source zone.
#[derive(Clone, Copy, PartialEq, Eq, Debug, Default)]
pub enum LineClass {
    #[default]
    Inherit,
    Prompt,
    Doc,
    Raw,
}

/// A presented object (BEACON.md 12.2 `obj`): `ty` is the type token,
/// `refv` the canonical ref (`ref` is a keyword).
pub struct Obj {
    pub ty: String,
    pub refv: String,
}

#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum BlockKind {
    Prompt,
    Output,
    Foreign,
}

#[derive(Clone, Copy)]
pub struct TCell {
    pub ch: char,
    pub style: u16,
}

pub struct Line {
    pub cells: Vec<TCell>,
    pub class: LineClass,
}

impl Line {
    pub fn plain(cells: Vec<TCell>) -> Line {
        Line {
            cells,
            class: LineClass::Inherit,
        }
    }
}

/// A captured Beacon table: `cols` holds the alignment spec bytes
/// (b'l'/b'r'/b'c'), `rows` -> cells -> styled content. Inter-cell padding
/// (the plain realization) is dropped -- rich layout re-derives geometry.
pub struct TableModel {
    pub cols: Vec<u8>,
    pub hdr: bool,
    pub rows: Vec<Vec<Vec<TCell>>>,
    /// Per row, each cell's START COLUMN in the row's source line: the
    /// grid column of a rebuilt row (the padding between cells is dropped
    /// from `rows`, so the cells alone cannot say where they sat), the
    /// plain-realization offset (cells joined by two spaces) of a byte-fed
    /// row. A laid cell run carries it as its `src_col`, so a click on a
    /// table row inverts to the grid cell it came from.
    pub starts: Vec<Vec<usize>>,
    /// The frame serial of the `table` open that produced this model in a
    /// frame-fed tile (0 for the console's byte-fed parser): the identity a
    /// later row of the SAME table joins on when its rows arrive one at a
    /// time (scroll-off) or all at once (the live grid).
    pub src: u32,
}

/// The plain realization's cell offsets for a byte-fed table row (cells
/// joined by two spaces -- `select::row_text`'s currency).
fn plain_starts(row: &[Vec<TCell>]) -> Vec<usize> {
    let mut out = Vec::with_capacity(row.len());
    let mut off = 0usize;
    for c in row.iter() {
        out.push(off);
        off += c.len() + 2;
    }
    out
}

pub enum Item {
    Line(Line),
    Table(TableModel),
    Rule,
    /// PL-1b: a Beacon `pre` block (BEACON.md 100/370) -- preformatted lines
    /// laid MONO + verbatim (no word-wrap, no space-collapse), set apart by
    /// its own ground + a leading gutter rule (HALCYON.md 110-113). Each line
    /// is a `Line` so an inline `em`/`obj` run inside the block keeps its span;
    /// the block forces mono regardless of a run's annotation.
    Pre(Vec<Line>),
}

pub struct Block {
    /// Stable identity for layout caching (survives freeze; never reused).
    pub id: u64,
    pub kind: BlockKind,
    /// True when this block continues an over-long predecessor (the
    /// per-block line cap froze it mid-zone).
    pub continuation: bool,
    pub exit: Option<i64>,
    /// H-3d: the command this OUTPUT block ran -- ut's `mark k=cmd`, the
    /// zone's first child (BEACON.md 12.2); None for a prompt / foreign
    /// block, or an output zone from a shell that does not mark.
    pub cmd: Option<String>,
    pub items: Vec<Item>,
    pub styles: Vec<Style>,
    pub objs: Vec<Obj>,
    /// Approximate stored size, for the content budget.
    pub cost: usize,
}

impl Block {
    fn new(id: u64, kind: BlockKind) -> Block {
        Block {
            id,
            kind,
            continuation: false,
            exit: None,
            cmd: None,
            items: Vec::new(),
            styles: Vec::new(),
            objs: Vec::new(),
            cost: 0,
        }
    }

    fn has_content(&self) -> bool {
        !self.items.is_empty() || self.exit.is_some()
    }

    /// Any Beacon structure at all: an annotated style, or a table / rule /
    /// pre item. A block with none is a program's raw terminal output.
    pub fn annotated(&self) -> bool {
        self.styles.iter().any(|s| s.annotated())
            || self.items.iter().any(|it| !matches!(it, Item::Line(_)))
    }

    /// The class every `Inherit` line of this block renders as: a prompt
    /// zone is the shell's prompt; a block that carries any Beacon structure
    /// is a document; a block with none is raw terminal content. Decided per
    /// ZONE (not per line) so a Beacon program's un-annotated prose lines
    /// stay prose; the one transient is a live block whose first annotation
    /// has not arrived yet, which re-lays as a document when it does.
    pub fn class(&self) -> LineClass {
        if self.kind == BlockKind::Prompt {
            LineClass::Prompt
        } else if self.annotated() {
            LineClass::Doc
        } else {
            LineClass::Raw
        }
    }
}

/// H-4d: the span state a tile's cell was written under -- the block that
/// was open (its obj table is the one `obj` indexes), the obj (idx+1; 0 =
/// none), em, hdr. A cell reaches it through its `vt::Cell.span` serial and
/// the tile's `SpanMap`.
#[derive(Clone, Copy, PartialEq, Eq, Debug, Default)]
pub struct SpanTag {
    pub block: u64,
    pub obj: u16,
    pub em: u8,
    pub hdr: u8,
}

/// The ring holds the last `SPAN_MAP_ENTRIES` frames' tags, validated by
/// the full serial (a serial that fell off resolves to no span). A cell
/// keeps its serial while it stays on the grid, so the bound is reached
/// only by more frames written OVER a still-visible cell than the ring
/// holds without it scrolling off -- a repainting TUI, which lives on the
/// alt screen where no span is read.
pub const SPAN_MAP_ENTRIES: usize = 8192;

/// One ring slot: the serial and its tag packed into 16 bytes (block 8,
/// serial 4, obj 2, em 1, hdr 1) -- the tuple `(u32, SpanTag)` padded to
/// 24. Pinned by `span_slot_is_16_bytes`.
#[derive(Clone, Copy, Default)]
struct SpanSlot {
    block: u64,
    serial: u32,
    obj: u16,
    em: u8,
    hdr: u8,
}

/// The ring's live footprint per rich tile (the H-arc round-1 audit, B-F4:
/// a fixed cost OUTSIDE `SESSION_SCROLLBACK_BUDGET`, bounded by the pane
/// count -- `MAX_PANES` x 128 KiB -- and recorded in the I-32 accounting
/// rather than charged to the history it would otherwise evict).
pub const SPAN_MAP_BYTES: usize = SPAN_MAP_ENTRIES * core::mem::size_of::<SpanSlot>();

/// serial (`vt::Cell.span`) -> the span state after feeding that frame.
/// The producer stamps cells with the serial of the last Beacon frame it
/// forwarded (parser-free, R5); the consumer, feeding the same frames in
/// order, notes the state after each -- so a cell knows its presentation
/// however late it scrolls off, and across the grid's zone straddle.
/// Allocated on the FIRST note: a plain tile (no Beacon frame ever) costs
/// nothing; a rich one `SPAN_MAP_BYTES` once.
pub struct SpanMap {
    ring: Vec<SpanSlot>,
}

impl Default for SpanMap {
    fn default() -> SpanMap {
        SpanMap::new()
    }
}

impl SpanMap {
    pub fn new() -> SpanMap {
        SpanMap { ring: Vec::new() }
    }

    /// The ring's heap footprint: 0 until the first frame, then `SPAN_MAP_BYTES`.
    pub fn bytes(&self) -> usize {
        self.ring.len() * core::mem::size_of::<SpanSlot>()
    }

    /// Record the state after frame `serial` (0 = no frame; ignored).
    pub fn note(&mut self, serial: u32, tag: SpanTag) {
        if serial == 0 {
            return;
        }
        if self.ring.is_empty() {
            self.ring = alloc::vec![SpanSlot::default(); SPAN_MAP_ENTRIES];
        }
        let i = serial as usize % SPAN_MAP_ENTRIES;
        self.ring[i] = SpanSlot {
            block: tag.block,
            serial,
            obj: tag.obj,
            em: tag.em,
            hdr: tag.hdr,
        };
    }

    /// The state a cell stamped `serial` was written under.
    pub fn get(&self, serial: u32) -> Option<SpanTag> {
        if serial == 0 || self.ring.is_empty() {
            return None;
        }
        let e = self.ring[serial as usize % SPAN_MAP_ENTRIES];
        if e.serial == serial {
            Some(SpanTag {
                block: e.block,
                obj: e.obj,
                em: e.em,
                hdr: e.hdr,
            })
        } else {
            None
        }
    }
}

// --- the feed-side scanners ------------------------------------------------

/// The OSC body bound (aurora's `osc_buf` size): an oversize body is dropped
/// whole at its terminator.
const OSC_MAX: usize = 256;

/// Percent-decode a `file:` URL path (H-3d, OSC 7): `%XX` pairs decode; a
/// malformed escape, a control byte (raw or decoded), or invalid UTF-8
/// rejects the whole report -- a path is never half-decoded.
fn pct_decode_path(raw: &[u8]) -> Option<String> {
    let mut out: Vec<u8> = Vec::with_capacity(raw.len());
    let mut i = 0;
    while i < raw.len() {
        let b = raw[i];
        let v = if b == b'%' {
            let hex = |c: u8| -> Option<u8> {
                match c {
                    b'0'..=b'9' => Some(c - b'0'),
                    b'a'..=b'f' => Some(c - b'a' + 10),
                    b'A'..=b'F' => Some(c - b'A' + 10),
                    _ => None,
                }
            };
            if i + 2 >= raw.len() {
                return None;
            }
            let v = (hex(raw[i + 1])? << 4) | hex(raw[i + 2])?;
            i += 3;
            v
        } else {
            i += 1;
            b
        };
        if v < 0x20 || v == 0x7f {
            return None;
        }
        out.push(v);
    }
    if out.first() != Some(&b'/') {
        return None;
    }
    String::from_utf8(out).ok()
}

enum ScanState {
    Ground,
    Esc,
    EscCharset,
    Csi,
    Osc,
    OscEsc,
}

const MAX_PARAMS: usize = 16;

// Hard per-block accumulation ceilings (the format-fuzz bounds). The budget
// machinery (`enforce_budget`) evicts only FROZEN blocks; the OPEN block, an
// in-progress table, and the nesting stacks all accumulate BETWEEN
// producer-chosen boundaries (newline / zone close / table close), and a
// hostile producer simply never emits one. Each of those therefore needs an
// incremental ceiling that is checked as bytes/frames ARRIVE, not at a
// boundary. All are fail-safe: at the cap, content is soft-wrapped or
// dropped, never grown -- halcyond IS the console, and its own OOM is a
// silent `t_exits(1)` (the fixed-heap no_std OOM), i.e. the machine's face
// vanishing.
const MAX_LINE_CELLS: usize = 4096; // == the CUF/CHA col clamps; a longer line soft-wraps
const MAX_OBJS_PER_BLOCK: usize = 4096; // also keeps the idx+1 encoding inside u16
const MAX_STYLES_PER_BLOCK: usize = 4096; // also bounds the style_idx scan (no O(n^2))
const MAX_SPAN_NEST: usize = 64; // em/obj nesting (wire caps 8/parse; this bounds the cross-feed leak)
const MAX_TABLE_ROWS: usize = 100_000;
const MAX_TABLE_COLS: usize = 256; // cells per row
const MAX_CELL_CHARS: usize = 4096;
// The pre/table in-progress accumulators (each uncharged to the block budget
// until close) are bounded by `transient_cap()` -- HALF the tile's scrollback
// share, floored -- NOT a fixed constant. A fixed 16 MiB, unscaled by tile
// count, let N tiles hold N x 16 MiB and OOM the 64 MiB heap (pre XOR table, so
// 16 MiB per tile). Since a tile's share is SESSION_SCROLLBACK_BUDGET/N, the
// per-tile half-share sums to SESSION_SCROLLBACK_BUDGET/2 regardless of N.

struct TableCap {
    cols: Vec<u8>,
    hdr: bool,
    rows: Vec<Vec<Vec<TCell>>>,
    row: Vec<Vec<TCell>>,
    cell: Vec<TCell>,
    in_row: bool,
    in_cell: bool,
    /// Running memory estimate (content + Vec overhead) of the capture; the
    /// incremental bound on a table the producer never closes.
    bytes: usize,
}

/// KT-1 cells mode: the spec of a table a tile's frames opened, kept for the
/// rows rebuilt from its tagged cells. A cell's serial lies in
/// [open_serial, close_serial]; the most recent spec whose open precedes the
/// cell is the cell's table.
struct TableSpec {
    open_serial: u32,
    close_serial: u32,
    cols: Vec<u8>,
    hdr: bool,
}

/// The bound on remembered table specs (a tile shows a handful of tables at
/// once; a spec older than this has scrolled out of every rebuild).
const MAX_TABLE_SPECS: usize = 32;

/// What a rebuilt line's tags say about its structure (cells mode).
#[derive(Clone, Copy, Default)]
struct RowShape {
    pre: bool,
    cell: bool,
    hdr_row: bool,
    /// Written inside a prompt zone (TAG_PROMPT on the first tagged cell).
    prompt: bool,
    /// The serial of the rule frame this line follows, if any.
    rule: Option<u32>,
    /// The first tagged cell's serial (the table-spec lookup key).
    serial: u32,
}

/// Trim a rebuilt line's tail of never-written cells (span 0, blank): the
/// grid's unused columns, not content. A typed or printed space carries its
/// frame's serial and stays.
fn trim_untagged_tail(cells: &mut Vec<vt::Cell>) {
    while let Some(c) = cells.last() {
        if c.span == 0 && (c.ch == ' ' || c.ch == '\0') {
            cells.pop();
        } else {
            break;
        }
    }
}

fn row_shape(cells: &[vt::Cell], spans: &SpanMap) -> RowShape {
    let mut shape = RowShape::default();
    let mut first = true;
    for c in cells {
        let Some(tag) = spans.get(c.span) else {
            continue;
        };
        if first {
            first = false;
            shape.serial = c.span;
            shape.pre = tag.em & TAG_PRE != 0;
            shape.prompt = tag.em & TAG_PROMPT != 0;
            if tag.em & TAG_RULE != 0 {
                shape.rule = Some(c.span);
            }
        }
        if tag.em & TAG_CELL != 0 {
            shape.cell = true;
        }
        if tag.em & TAG_ROW_HDR != 0 {
            shape.hdr_row = true;
        }
    }
    shape
}

/// Place one rebuilt logical line into `items` per its tags: a rule before
/// it (once per rule frame), a pre line joining the open pre block, a table
/// row joining the table of the same spec (the padding between cells --
/// untagged-as-cell -- dropped; a cell's text is its TAG_CELL run per
/// column, its start column kept in `starts`), else a plain line.
/// `interned` holds the line's cells with their styles already interned;
/// `raw` the same cells with their tags. Returns the item the line landed
/// in and its ROW within it (`usize::MAX` for a plain line; the pre line /
/// table row index otherwise) -- the address the hit-test inverts through,
/// since every row of one rebuilt table shares the item.
#[allow(clippy::too_many_arguments)]
fn place_tagged_line(
    items: &mut Vec<Item>,
    interned: Vec<TCell>,
    raw: &[vt::Cell],
    shape: RowShape,
    spec: Option<(u32, &[u8], bool)>,
    class: LineClass,
    spans: &SpanMap,
    last_rule: &mut u32,
) -> (usize, usize) {
    if let Some(s) = shape.rule {
        if s != *last_rule {
            *last_rule = s;
            items.push(Item::Rule);
        }
    }
    if shape.pre {
        if let Some(Item::Pre(lines)) = items.last_mut() {
            lines.push(Line::plain(interned));
            let row = lines.len() - 1;
            return (items.len() - 1, row);
        }
        items.push(Item::Pre(alloc::vec![Line::plain(interned)]));
        return (items.len() - 1, 0);
    }
    if shape.cell {
        let (src, cols) = match spec {
            Some((s, c, _)) => (s, c.to_vec()),
            None => (shape.serial, Vec::new()),
        };
        // Split into cells by column: consecutive TAG_CELL cells of one
        // column form the cell; anything else between them is padding.
        let mut row: Vec<Vec<TCell>> = Vec::new();
        let mut starts: Vec<usize> = Vec::new();
        let mut cur_col: Option<usize> = None;
        for (i, c) in raw.iter().enumerate() {
            let Some(t) = interned.get(i).copied() else {
                break;
            };
            let tag = spans.get(c.span);
            match tag {
                Some(tag) if tag.em & TAG_CELL != 0 => {
                    // A column CHANGE starts the next cell (a run of one
                    // column is one cell); past the column cap the text joins
                    // the last cell rather than growing the row.
                    let col = tag_col(tag.hdr);
                    if cur_col != Some(col) {
                        cur_col = Some(col);
                        if row.len() < MAX_TABLE_COLS {
                            row.push(Vec::new());
                            starts.push(i);
                        }
                    }
                    if let Some(cell) = row.last_mut() {
                        cell.push(t);
                    }
                }
                _ => {
                    cur_col = None;
                }
            }
        }
        // Empty trailing cells are real (a blank value column); a row with
        // NO cell content at all (a table's padding-only line) is dropped.
        if row.iter().all(|c| c.is_empty()) {
            return (items.len().saturating_sub(1), usize::MAX);
        }
        if let Some(Item::Table(t)) = items.last_mut() {
            if t.src == src && t.rows.len() < MAX_TABLE_ROWS {
                t.rows.push(row);
                t.starts.push(starts);
                let ri = t.rows.len() - 1;
                return (items.len() - 1, ri);
            }
        }
        // The header row is the one the tag names (TAG_ROW_HDR), never the
        // spec's flag alone: a table whose header row scrolled off earlier
        // restarts here with its first BODY row, which is not a header.
        items.push(Item::Table(TableModel {
            cols,
            hdr: shape.hdr_row,
            rows: alloc::vec![row],
            starts: alloc::vec![starts],
            src,
        }));
        return (items.len() - 1, 0);
    }
    items.push(Item::Line(Line {
        cells: interned,
        class,
    }));
    (items.len() - 1, usize::MAX)
}

/// The transcript: feed bytes in, read frozen blocks + the open tail out.
pub struct Transcript {
    frozen: VecDeque<Block>,
    open: Block,
    /// The line being built (column-addressed; the line discipline).
    line: Vec<TCell>,
    col: usize,
    /// PL-3: raw cells of a soft-wrapped logical line being rejoined from
    /// ScrollOff rows -- held uninterned (self-contained style, so a block
    /// change mid-line interns cleanly at finalize) until a non-wrapped row
    /// ends the line. Bounded by MAX_LINE_CELLS (hard-split), the only window
    /// in which it is off-budget.
    scroll_pending: Vec<vt::Cell>,
    pal: Palette,
    pen: SgrPen,
    em_stack: Vec<u8>,
    obj_stack: Vec<u16>,
    // Suppressed-open counters (mirroring the wire layer): opens beyond
    // MAX_SPAN_NEST are counted, not pushed, so the matching close skips a
    // pop -- LIFO balance is preserved exactly while the stacks stay bounded.
    em_suppressed: u32,
    obj_suppressed: u32,
    hdr: u8,
    table: Option<TableCap>,
    /// KT-1 cells mode (`set_cells_mode`): frames only; structure is rebuilt
    /// from tagged cells. `cur_serial` is the frame being fed; `rule_pending`
    /// the rule EPISODE -- from a `rule` frame (its serial in `rule_serial`)
    /// until the tile reports cells written after it (`end_rule`), every tag
    /// noted meanwhile carries TAG_RULE, so the first line written after the
    /// rule places it wherever the rule's frames took the transcript (an
    /// inline open, a zone close, the next prompt); `last_rule_serial` dedupes
    /// the rebuilt rule across the two placement paths; `table_specs` the
    /// specs of the tables opened, keyed by open serial, for the rebuilt rows.
    cells_mode: bool,
    cur_serial: u32,
    rule_pending: bool,
    rule_serial: u32,
    last_rule_serial: u32,
    table_specs: VecDeque<TableSpec>,
    /// PL-1b: the open `pre` block's lines, accumulated between open_op(Pre)
    /// and close_op(Pre). Built through the SAME line discipline (put_char /
    /// newline / flush_line) so tabs, spacing and `\r` behave verbatim; the
    /// flushed lines are redirected HERE instead of into the block's items.
    /// None = not in a pre. Bounded by the per-block line cap AND `pre_bytes`
    /// (`transient_cap()`, the tile's half-share); charged + cap-enforced at
    /// close (a bounded transient like `scroll_pending`).
    pre: Option<Vec<Line>>,
    /// PL-1b: bytes accumulated in the open `pre` (content only). The pre is
    /// uncharged to the block budget until close, so this bounds the transient
    /// independently -- an unclosed / hostile pre cannot exceed `transient_cap()`
    /// (the format-fuzz DoS floor, mirroring TableCap.bytes). Reset at open.
    pre_bytes: usize,
    // Escape-scanner state (persists across feeds via `carry`, but the
    // scanner itself also survives a split mid-sequence).
    state: ScanState,
    params: [u32; MAX_PARAMS],
    nparams: usize,
    cur_param: u32,
    csi_private: bool,
    // Partial trailing escape held back between feeds.
    carry: Vec<u8>,
    /// H-3d: the session's working directory -- ut's latest OSC 7 report
    /// (BEACON.md 12.11); empty until one arrives.
    cwd: String,
    /// The OSC body being scanned (bounded: an oversize body is dropped
    /// whole at its terminator, never truncated into a different value).
    osc_buf: Vec<u8>,
    osc_over: bool,
    // Partial UTF-8 held across feeds/events.
    utf8: [u8; 4],
    utf8_len: u8,
    utf8_need: u8,
    /// Latched on row-addressed / alt-screen control (13.4b's class
    /// boundary); the consumer clears it when it acts.
    pub raw_vt_intent: bool,
    next_id: u64,
    stored_cost: usize,
    max_blocks: usize,
    max_cost: usize,
    max_lines_per_block: usize,
    /// The OPEN block's byte cap: the budget evicts only FROZEN blocks, so
    /// an open block that never freezes escapes it entirely (the H-3b round
    /// F1: 10 000 soft-wrapped 4096-cell lines = 320 MiB before the line
    /// cap fires, against a 64 MiB heap). Crossing it freezes the block as
    /// a continuation, exactly as the line cap does, so eviction can reach
    /// the bytes. `max_cost / 8` by default.
    max_open_cost: usize,
    /// Bumps on every structural change (a consumer's cheap dirty check).
    pub seq: u64,
    /// The exit code of the most recently completed command, latched by
    /// its exit mark until the consumer takes it (`take_exit`): the tile
    /// status feed (H-3b-4). A latch, not a queue -- only the LAST exit is
    /// the tile's status.
    last_exit: Option<i64>,
}

/// Default caps: sized against the 13.3 budget (a content budget, not a
/// pixel budget; the layout cache is bounded separately).
pub const DEFAULT_MAX_BLOCKS: usize = 1000;
pub const DEFAULT_MAX_COST: usize = 32 << 20;
pub const DEFAULT_MAX_LINES_PER_BLOCK: usize = 10_000;
/// The open block freezes at the smaller of `max_cost / 8` and this: the
/// open block is laid out WHOLE on every render (it is the one block no
/// height cache can position), and a block straddling the view is laid out
/// whole too, so the render transient is O(view + 2 x this), not O(share /
/// 8) -- 4 MiB of cells at a 32 MiB share. It also bounds what a re-budget
/// cannot evict (the newest frozen block, sized by the cap in force when it
/// froze). 64 such blocks fill the default budget; the block cap holds 1000.
pub const OPEN_BLOCK_MAX_COST: usize = 512 << 10;

/// What one retained line costs beyond its cells: the `Item` slot, the
/// `Line`'s vector header, and the allocator's per-block overhead. A cost
/// model that charged only cells let an empty line be free, and a count cap
/// alone is a budget the item count can spend past.
const ITEM_OVERHEAD: usize = core::mem::size_of::<Item>() + core::mem::size_of::<Line>() + 16;

impl Transcript {
    pub fn new(pal: Palette) -> Transcript {
        Transcript::with_caps(
            pal,
            DEFAULT_MAX_BLOCKS,
            DEFAULT_MAX_COST,
            DEFAULT_MAX_LINES_PER_BLOCK,
        )
    }

    pub fn with_caps(
        pal: Palette,
        max_blocks: usize,
        max_cost: usize,
        max_lines: usize,
    ) -> Transcript {
        Transcript {
            frozen: VecDeque::new(),
            open: Block::new(0, BlockKind::Foreign),
            last_exit: None,
            line: Vec::new(),
            col: 0,
            pen: SgrPen::new(&pal),
            pal,
            em_stack: Vec::new(),
            obj_stack: Vec::new(),
            em_suppressed: 0,
            obj_suppressed: 0,
            hdr: 0,
            table: None,
            cells_mode: false,
            cur_serial: 0,
            rule_pending: false,
            rule_serial: 0,
            last_rule_serial: 0,
            table_specs: VecDeque::new(),
            pre: None,
            pre_bytes: 0,
            state: ScanState::Ground,
            cwd: String::new(),
            osc_buf: Vec::new(),
            osc_over: false,
            params: [0; MAX_PARAMS],
            nparams: 0,
            cur_param: 0,
            csi_private: false,
            carry: Vec::new(),
            scroll_pending: Vec::new(),
            utf8: [0; 4],
            utf8_len: 0,
            utf8_need: 0,
            raw_vt_intent: false,
            next_id: 1,
            stored_cost: 0,
            max_blocks,
            max_cost,
            max_lines_per_block: max_lines.max(1),
            max_open_cost: open_cap(max_cost),
            seq: 0,
        }
    }

    pub fn frozen_blocks(&self) -> &VecDeque<Block> {
        &self.frozen
    }

    pub fn open_block(&self) -> &Block {
        &self.open
    }

    /// The un-frozen line under construction (the cursor's line).
    pub fn pending_line(&self) -> &[TCell] {
        &self.line
    }

    /// Take the latched exit of the most recently completed command, if
    /// one landed since the last take.
    pub fn take_exit(&mut self) -> Option<i64> {
        self.last_exit.take()
    }

    /// H-3d: the session's working directory as last reported by the shell
    /// (OSC 7); empty before the first report.
    pub fn cwd(&self) -> &str {
        &self.cwd
    }

    /// H-3d: the command running now (the open output block's mark) or,
    /// between commands, the last one that ran.
    pub fn last_command(&self) -> Option<&str> {
        self.open
            .cmd
            .as_deref()
            .or_else(|| self.frozen.iter().rev().find_map(|b| b.cmd.as_deref()))
    }

    pub fn pending_col(&self) -> usize {
        self.col
    }

    /// (em_stack, obj_stack) depths -- the nesting bound witness (F4).
    #[cfg(test)]
    pub(crate) fn nest_depths(&self) -> (usize, usize) {
        (self.em_stack.len(), self.obj_stack.len())
    }

    // --- the stream entry ---------------------------------------------------

    pub fn feed(&mut self, input: &[u8]) {
        self.seq = self.seq.wrapping_add(1);
        // Join the held-back tail with the new bytes (allocation-free when
        // nothing was held).
        let buf: Vec<u8>;
        let joined: &[u8] = if self.carry.is_empty() {
            input
        } else {
            let mut b = core::mem::take(&mut self.carry);
            b.extend_from_slice(input);
            buf = b;
            &buf
        };
        let cut = safe_cut(joined);
        let (head, tail) = joined.split_at(cut);
        if !tail.is_empty() {
            let mut c = Vec::with_capacity(tail.len());
            c.extend_from_slice(tail);
            self.carry = c;
        }
        for ev in wire::parse(head) {
            match ev {
                Event::Text(bytes) => self.scan_text(&bytes),
                Event::Open(op, args) => self.open_op(op, &args),
                Event::Close(op) => self.close_op(op),
                Event::Point(op, args) => self.point_op(op, &args),
            }
        }
    }

    // --- beacon events ------------------------------------------------------

    fn arg<'a>(args: &'a [wire::Arg], key: &str) -> Option<&'a str> {
        args.iter().find(|a| a.key == key).map(|a| a.value.as_str())
    }

    fn open_op(&mut self, op: Op, args: &[wire::Arg]) {
        // A `pre` block nests no block op -- only inline `em`/`obj` (BEACON.md
        // 351-355). While one is open, ignore any other open (malformed
        // nesting, incl. a nested `pre`); em/obj still color its cells via
        // style_idx. The format-fuzz containment guard.
        if self.pre.is_some() && !matches!(op, Op::Em | Op::Obj) {
            return;
        }
        match op {
            Op::Zone => {
                let kind = match Self::arg(args, "k") {
                    Some("prompt") => BlockKind::Prompt,
                    Some("output") => BlockKind::Output,
                    // `command` is RESERVED in v1; unknown k tolerated.
                    _ => BlockKind::Foreign,
                };
                self.freeze_open(kind, false);
            }
            Op::Table => {
                self.flush_line();
                let mut cols = Vec::new();
                if let Some(spec) = Self::arg(args, "cols") {
                    for b in spec.bytes().take(16) {
                        cols.push(match b {
                            b'r' => b'r',
                            b'c' => b'c',
                            _ => b'l',
                        });
                    }
                }
                let hdr = Self::arg(args, "hdr") == Some("1");
                if self.cells_mode {
                    // The spec a tile's rebuilt rows will need, keyed by this
                    // frame's serial (a cell's serial >= the open's); bounded.
                    if self.table_specs.len() >= MAX_TABLE_SPECS {
                        self.table_specs.pop_front();
                    }
                    self.table_specs.push_back(TableSpec {
                        open_serial: self.cur_serial,
                        close_serial: u32::MAX,
                        cols: cols.clone(),
                        hdr,
                    });
                }
                self.table = Some(TableCap {
                    cols,
                    hdr,
                    rows: Vec::new(),
                    row: Vec::new(),
                    cell: Vec::new(),
                    in_row: false,
                    in_cell: false,
                    bytes: 0,
                });
            }
            Op::Row => {
                if let Some(t) = self.table.as_mut() {
                    if !t.in_row {
                        t.in_row = true;
                        t.row = Vec::new();
                    }
                }
            }
            Op::Cell => {
                if let Some(t) = self.table.as_mut() {
                    if t.in_row && !t.in_cell {
                        t.in_cell = true;
                        t.cell = Vec::new();
                    }
                }
            }
            Op::Em => {
                let class = match Self::arg(args, "class") {
                    Some("emph") => EM_EMPH,
                    Some("strong") => EM_STRONG,
                    Some("dim") => EM_DIM,
                    Some("code") => EM_CODE,
                    _ => EM_NONE,
                };
                self.em_push(class);
            }
            Op::Obj => {
                // At the count cap, degrade to the no-obj sentinel (0): the
                // block's obj table stops growing, the idx+1 encoding stays
                // in u16, and the open/close still balance via obj_push/pop.
                let idx = if self.open.objs.len() >= MAX_OBJS_PER_BLOCK {
                    0
                } else {
                    let ty = Self::arg(args, "type").unwrap_or("");
                    let refv = Self::arg(args, "ref").unwrap_or("");
                    let mut sty = String::new();
                    sty.push_str(ty);
                    let mut srf = String::new();
                    srf.push_str(refv);
                    let bytes = sty.len() + srf.len();
                    self.open.cost += bytes;
                    // Symmetric with cells/tables/styles: charge stored_cost
                    // too, so eviction's `sub(dead.cost)` cannot drift the
                    // budget to zero (else max_cost never enforces).
                    self.stored_cost += bytes;
                    self.open.objs.push(Obj { ty: sty, refv: srf });
                    self.open.objs.len() as u16 // idx+1 encoding, <= u16::MAX
                };
                self.obj_push(idx);
            }
            Op::Hdr => {
                let level = match Self::arg(args, "level") {
                    Some("2") => 2,
                    Some("3") => 3,
                    _ => 1,
                };
                // `class=title` names the heading's ROLE (a title page's
                // herald); any other value is a section heading. Packed with
                // the level (HDR_TITLE) so the span tag stays one byte.
                let title = if Self::arg(args, "class") == Some("title") {
                    HDR_TITLE
                } else {
                    0
                };
                self.hdr = level | title;
            }
            // `pre` opens a preformatted block: flush the pending flow line,
            // then redirect subsequent flushed lines into the pre accumulator
            // (close_op(Pre) finalizes it). Not inside a table (malformed); the
            // top guard already blocks a nested pre.
            Op::Pre => {
                if self.table.is_none() && self.pre.is_none() {
                    self.flush_line();
                    self.pre = Some(Vec::new());
                    self.pre_bytes = 0;
                    self.col = 0;
                }
            }
            Op::Mark | Op::Rule => {} // point ops; a paired open is malformed -- ignore
        }
    }

    fn close_op(&mut self, op: Op) {
        // Mirror of open_op's containment guard: while a `pre` is open, only an
        // inline em/obj close -- or the pre's own close -- is meaningful.
        if self.pre.is_some() && !matches!(op, Op::Em | Op::Obj | Op::Pre) {
            return;
        }
        // The table byte cap for this tile (half its scrollback share); read
        // before any self.table borrow below so the arms can pass/compare it.
        let byte_cap = self.transient_cap();
        match op {
            Op::Zone => {
                self.freeze_open(BlockKind::Foreign, false);
            }
            Op::Table => {
                if let Some(mut t) = self.table.take() {
                    // Tolerate unclosed row/cell at table close (the final
                    // row/cell -- bounded, one each -- still honors the caps).
                    if t.in_cell {
                        Self::table_push_cell(&mut t, byte_cap);
                    }
                    if t.in_row && t.rows.len() < MAX_TABLE_ROWS {
                        t.rows.push(core::mem::take(&mut t.row));
                    }
                    let mut cost = 0usize;
                    for r in t.rows.iter() {
                        for c in r.iter() {
                            cost += c.len() * core::mem::size_of::<TCell>();
                        }
                    }
                    self.open.cost += cost;
                    self.stored_cost += cost;
                    // A frame-fed tile captured no text (the rows are on its
                    // grid): drop the empty shell -- the rows are rebuilt from
                    // the tagged cells at scroll-off / live layout; only the
                    // spec registered at open (`table_specs`) survives.
                    if self.cells_mode {
                        if let Some(spec) = self.table_specs.back_mut() {
                            if spec.close_serial == u32::MAX {
                                spec.close_serial = self.cur_serial;
                            }
                        }
                        return;
                    }
                    let starts = t.rows.iter().map(|r| plain_starts(r)).collect();
                    self.open.items.push(Item::Table(TableModel {
                        cols: t.cols,
                        hdr: t.hdr,
                        rows: t.rows,
                        starts,
                        src: 0,
                    }));
                    self.enforce_block_cap();
                }
            }
            Op::Row => {
                if let Some(t) = self.table.as_mut() {
                    if t.in_cell {
                        Self::table_push_cell(t, byte_cap);
                        t.in_cell = false;
                    }
                    if t.in_row {
                        // At the row cap or byte budget, drop the row (never
                        // grow the Vec-of-Vecs unboundedly on an unclosed
                        // table); else charge its overhead + keep it.
                        if t.rows.len() < MAX_TABLE_ROWS && t.bytes < byte_cap {
                            t.bytes = t.bytes.saturating_add(
                                core::mem::size_of::<Vec<TCell>>() * (t.row.len() + 1),
                            );
                            t.rows.push(core::mem::take(&mut t.row));
                        } else {
                            t.row.clear();
                        }
                        t.in_row = false;
                    }
                }
            }
            Op::Cell => {
                if let Some(t) = self.table.as_mut() {
                    if t.in_cell {
                        Self::table_push_cell(t, byte_cap);
                        t.in_cell = false;
                    }
                }
            }
            Op::Em => {
                self.em_pop();
            }
            Op::Obj => {
                self.obj_pop();
            }
            Op::Hdr => {
                self.hdr = 0;
            }
            Op::Pre => {
                // Flush the last pre line (routes into the accumulator), then
                // finalize the block: charge its cost (like Table) and push
                // the Item::Pre. A well-formed `pre` always balances; a stray
                // close with no open falls through (self.pre is None).
                self.flush_line();
                if self.cells_mode {
                    // The tile's pre lines live on its grid (tagged TAG_PRE);
                    // the accumulator here only carried the tag state.
                    self.pre = None;
                    return;
                }
                if let Some(lines) = self.pre.take() {
                    let mut cost = 0usize;
                    for l in lines.iter() {
                        cost += l.cells.len() * core::mem::size_of::<TCell>() + ITEM_OVERHEAD;
                    }
                    self.open.cost += cost;
                    self.stored_cost += cost;
                    self.open.items.push(Item::Pre(lines));
                    self.enforce_block_cap();
                }
            }
            Op::Mark | Op::Rule => {}
        }
    }

    // Nesting-stack push/pop with the suppressed-open discipline: an open
    // beyond MAX_SPAN_NEST is counted, not pushed; the matching close skips a
    // pop. LIFO balance is exact, memory is bounded, and well-formed input
    // (wire-capped at depth 8/parse) never reaches the ceiling.
    fn em_push(&mut self, class: u8) {
        if self.em_stack.len() >= MAX_SPAN_NEST {
            self.em_suppressed = self.em_suppressed.saturating_add(1);
        } else {
            self.em_stack.push(class);
        }
    }

    fn em_pop(&mut self) {
        if self.em_suppressed > 0 {
            self.em_suppressed -= 1;
        } else {
            self.em_stack.pop();
        }
    }

    fn obj_push(&mut self, idx: u16) {
        if self.obj_stack.len() >= MAX_SPAN_NEST {
            self.obj_suppressed = self.obj_suppressed.saturating_add(1);
        } else {
            self.obj_stack.push(idx);
        }
    }

    fn obj_pop(&mut self) {
        if self.obj_suppressed > 0 {
            self.obj_suppressed -= 1;
        } else {
            self.obj_stack.pop();
        }
    }

    /// Finalize the current cell into the row under the col cap + byte budget;
    /// past either, the cell is dropped (never grow an unclosed table).
    fn table_push_cell(t: &mut TableCap, byte_cap: usize) {
        if t.row.len() < MAX_TABLE_COLS && t.bytes < byte_cap {
            t.bytes = t.bytes.saturating_add(core::mem::size_of::<Vec<TCell>>());
            t.row.push(core::mem::take(&mut t.cell));
        } else {
            t.cell.clear();
        }
    }

    fn point_op(&mut self, op: Op, args: &[wire::Arg]) {
        // PL-1b: a `pre` block contains only inline em/obj + text; a stray
        // point op (mark/rule) inside it is malformed -- ignore it (the
        // containment guard, mirroring open_op/close_op). Prevents a rule from
        // interleaving into the block or freezing it mid-accumulation.
        if self.pre.is_some() {
            return;
        }
        match op {
            Op::Mark => {
                // H-3d: the output zone's command (its first child, ut's
                // `mark k=cmd`): recorded on the block it opens.
                if Self::arg(args, "k") == Some("cmd") {
                    if let Some(t) = Self::arg(args, "text") {
                        if self.open.kind == BlockKind::Output {
                            self.open.cost += t.len();
                            // Symmetric with the obj/cell/table/style sites:
                            // charge stored_cost too, or eviction's
                            // `sub(dead.cost)` drifts the byte budget to zero
                            // and max_cost never enforces again (F1).
                            self.stored_cost += t.len();
                            self.open.cmd = Some(String::from(t));
                        }
                    }
                }
                if Self::arg(args, "k") == Some("exit") {
                    let code = Self::arg(args, "code").and_then(|c| c.parse::<i64>().ok());
                    if code.is_some() {
                        self.last_exit = code;
                        if self.open.kind != BlockKind::Foreign || self.open.has_content() {
                            self.open.exit = code;
                        } else if let Some(last) = self.frozen.back_mut() {
                            // Tolerate the pre-deviation-8 floating order:
                            // a mark right AFTER the output close lands on
                            // the block it completed.
                            if last.kind == BlockKind::Output && last.exit.is_none() {
                                last.exit = code;
                            }
                        }
                    }
                }
            }
            Op::Rule => {
                if self.cells_mode {
                    // Tagged onto the cells that follow (TAG_RULE) until the
                    // tile reports cells written after this frame; the tile
                    // places the rule before the first line carrying it.
                    self.rule_pending = true;
                    self.rule_serial = self.cur_serial;
                    return;
                }
                self.flush_line();
                self.open.items.push(Item::Rule);
                self.enforce_block_cap();
            }
            _ => {}
        }
    }

    // --- block lifecycle ----------------------------------------------------

    /// Freeze the open block (if it earned it) and start the next one.
    /// Beacon spans die at the boundary; the SGR pen persists.
    fn freeze_open(&mut self, next: BlockKind, continuation: bool) {
        self.flush_line();
        // An abandoned table capture at a block boundary flushes as-is
        // (renderer hygiene: content beats loss).
        if self.table.is_some() {
            self.close_op(Op::Table);
        }
        // Symmetric with the table arm: an open `pre` at a block boundary is
        // finalized into THIS block, where its cells' block-relative style
        // indices are valid. Without it the Item::Pre commits to the fresh block
        // below (0 styles) and layout_block's `b.styles[sid]` panics on a stale
        // index -- reachable from an untrusted tile stream interleaving a
        // ScrollOff (or a tile-split's set_max_cost) between pre-open and
        // pre-close. Inline (no re-entrant enforce_block_cap -- the mem::replace
        // below freezes this block); a pre spanning a block-freeze is split, its
        // post-freeze content resuming as ordinary lines.
        if let Some(lines) = self.pre.take() {
            let mut cost = 0usize;
            for l in lines.iter() {
                cost += l.cells.len() * core::mem::size_of::<TCell>() + ITEM_OVERHEAD;
            }
            self.open.cost += cost;
            self.stored_cost += cost;
            self.open.items.push(Item::Pre(lines));
            self.pre_bytes = 0;
        }
        // Cells mode: a zone-less block whose text is still on the grid has
        // no items, but its obj table is what the grid's tags index -- drop
        // it and the grid's objects resolve to nothing. Kept, it lays to 0 px.
        let keep = self.open.has_content()
            || self.open.kind != BlockKind::Foreign
            || (self.cells_mode && !self.open.objs.is_empty());
        // Ids are monotonic and NEVER recycled, dropped block or not: in
        // cells mode the dropped block's cells are still on the grid with
        // tags naming its id, and a successor wearing the same id would
        // inherit their annotations (a zone-less `em` line's Doc class
        // leaking onto the raw output zone after it). A dropped id resolves
        // to no block, which every lookup already tolerates.
        let id = self.next_id;
        self.next_id += 1;
        let mut b = core::mem::replace(&mut self.open, Block::new(id, next));
        self.open.continuation = continuation;
        if keep {
            b.cost += b.styles.len() * core::mem::size_of::<Style>();
            self.stored_cost += b.styles.len() * core::mem::size_of::<Style>();
            self.frozen.push_back(b);
            self.enforce_budget();
        }
        self.em_stack.clear();
        self.obj_stack.clear();
        self.em_suppressed = 0;
        self.obj_suppressed = 0;
        self.hdr = 0;
    }

    fn enforce_budget(&mut self) {
        while self.frozen.len() > self.max_blocks
            || (self.stored_cost > self.max_cost && self.frozen.len() > 1)
        {
            if let Some(dead) = self.frozen.pop_front() {
                self.stored_cost = self.stored_cost.saturating_sub(dead.cost);
            } else {
                break;
            }
        }
    }

    /// The per-block caps: an endless un-zoned stream must not grow one
    /// block unboundedly, by LINE COUNT or by BYTES -- freeze and continue,
    /// same kind, marked. The byte cap is the one that binds first under
    /// the soft-wrap (each wrapped line is MAX_LINE_CELLS cells).
    fn enforce_block_cap(&mut self) {
        if self.open.items.len() >= self.max_lines_per_block || self.open.cost >= self.max_open_cost
        {
            let kind = self.open.kind;
            self.freeze_open(kind, true);
        }
    }

    fn flush_line(&mut self) {
        if self.line.is_empty() {
            return;
        }
        let cells = core::mem::take(&mut self.line);
        self.col = 0;
        // PL-1b: inside a `pre`, the flushed line joins the pre accumulator
        // instead of the block items -- bounded by the per-block line cap (each
        // line is already <= MAX_LINE_CELLS via put_char's soft-wrap). Charged
        // + cap-enforced at close_op(Pre).
        let cap = self.max_lines_per_block;
        let byte_cap = self.transient_cap();
        if let Some(pre) = self.pre.as_mut() {
            // Bounded by BOTH the line cap and the byte budget (the pre is
            // uncharged to the block until close; transient_cap() -- the tile's
            // half-share -- bounds the transient). A line past either is dropped.
            if pre.len() < cap && self.pre_bytes < byte_cap {
                self.pre_bytes += cells.len() * core::mem::size_of::<TCell>();
                pre.push(Line::plain(cells));
            }
            return;
        }
        let cost = cells.len() * core::mem::size_of::<TCell>() + ITEM_OVERHEAD;
        self.open.cost += cost;
        self.stored_cost += cost;
        self.open.items.push(Item::Line(Line::plain(cells)));
        self.enforce_block_cap();
    }

    // --- the VT-subset text scanner -----------------------------------------

    fn scan_text(&mut self, bytes: &[u8]) {
        for &b in bytes {
            self.scan_byte(b);
        }
    }

    fn scan_byte(&mut self, b: u8) {
        match self.state {
            ScanState::Ground => match b {
                0x1b => {
                    self.utf8_len = 0;
                    self.utf8_need = 0;
                    self.state = ScanState::Esc;
                }
                b'\n' => {
                    self.newline();
                }
                b'\r' => {
                    self.col = 0;
                }
                b'\t' => {
                    let next = (self.col / 8 + 1) * 8;
                    while self.col < next {
                        self.put_char(' ');
                    }
                }
                0x08 => {
                    if self.col > 0 {
                        self.col -= 1;
                    }
                }
                0x00..=0x1f | 0x7f => {} // other C0 + DEL: dropped
                _ => self.utf8_byte(b),
            },
            ScanState::Esc => match b {
                b'[' => {
                    self.nparams = 0;
                    self.cur_param = 0;
                    self.csi_private = false;
                    self.state = ScanState::Csi;
                }
                b']' => {
                    self.osc_buf.clear();
                    self.osc_over = false;
                    self.state = ScanState::Osc;
                }
                b'(' | b')' => self.state = ScanState::EscCharset,
                b'D' | b'M' | b'E' | b'7' | b'8' | b'c' => {
                    // Index/reverse-index/save-restore/reset: cursor-motion
                    // era control -- fullscreen intent in a flowed block.
                    self.raw_vt_intent = true;
                    self.state = ScanState::Ground;
                }
                _ => self.state = ScanState::Ground,
            },
            ScanState::EscCharset => self.state = ScanState::Ground,
            ScanState::Csi => match b {
                b'0'..=b'9' => {
                    self.cur_param = self
                        .cur_param
                        .saturating_mul(10)
                        .saturating_add((b - b'0') as u32);
                }
                b';' | b':' => self.push_param(),
                b'?' => self.csi_private = true,
                0x20..=0x2f => {} // intermediates: swallowed
                0x40..=0x7e => {
                    self.push_param();
                    self.dispatch_csi(b);
                    self.state = ScanState::Ground;
                }
                _ => self.state = ScanState::Ground, // malformed: abandon
            },
            ScanState::Osc => match b {
                0x07 => {
                    self.osc_end();
                    self.state = ScanState::Ground;
                }
                0x1b => self.state = ScanState::OscEsc,
                _ => self.osc_push(b), // the body, bounded; OSC 7 is read at the end
            },
            ScanState::OscEsc => {
                if b == b'\\' {
                    self.osc_end();
                    self.state = ScanState::Ground;
                } else {
                    self.osc_push(0x1b);
                    self.osc_push(b);
                    self.state = ScanState::Osc;
                }
            }
        }
    }

    fn osc_push(&mut self, b: u8) {
        if self.osc_over {
            return;
        }
        if self.osc_buf.len() >= OSC_MAX {
            self.osc_over = true;
            self.osc_buf.clear();
            return;
        }
        self.osc_buf.push(b);
    }

    /// The OSC terminated: the one foreign OSC this sink interprets is 7,
    /// the working-directory report (`7;file://<host><path>`, BEACON.md
    /// 12.11). Ours only when the host is empty or `localhost`; the path is
    /// percent-decoded, must be absolute, and may carry no control byte.
    /// Everything else -- another OSC, an oversize body -- is dropped.
    fn osc_end(&mut self) {
        let over = self.osc_over;
        self.osc_over = false;
        if over {
            self.osc_buf.clear();
            return;
        }
        if let Some(rest) = self.osc_buf.strip_prefix(b"7;") {
            if let Some(url) = rest.strip_prefix(b"file://") {
                let slash = url.iter().position(|&b| b == b'/').unwrap_or(url.len());
                let (host, path) = url.split_at(slash);
                if (host.is_empty() || host == b"localhost") && !path.is_empty() {
                    if let Some(p) = pct_decode_path(path) {
                        self.cwd = p;
                    }
                }
            }
        }
        self.osc_buf.clear();
    }

    fn push_param(&mut self) {
        if self.nparams < MAX_PARAMS {
            self.params[self.nparams] = self.cur_param;
            self.nparams += 1;
        }
        self.cur_param = 0;
    }

    fn dispatch_csi(&mut self, fin: u8) {
        let p1 = if self.nparams > 0 { self.params[0] } else { 0 };
        match fin {
            b'm' => {
                // The trailing implicit param: `CSI m` pushed one 0; a bare
                // reset either way. Pass exactly what the grid would.
                let n = self.nparams;
                let mut pen = self.pen;
                pen.apply(&self.pal, &self.params[..n]);
                self.pen = pen;
            }
            b'K' => match p1 {
                0 => self.line.truncate(self.col.min(self.line.len())),
                1 => {
                    let end = self.col.min(self.line.len().saturating_sub(1));
                    let style = self.style_idx();
                    for i in 0..=end {
                        if i < self.line.len() {
                            self.line[i] = TCell { ch: ' ', style };
                        }
                    }
                }
                2 => self.line.clear(),
                _ => {}
            },
            b'C' => {
                self.col = self.col.saturating_add(p1.max(1) as usize).min(4096);
            }
            b'D' => {
                self.col = self.col.saturating_sub(p1.max(1) as usize);
            }
            b'G' => {
                self.col = (p1.max(1) as usize - 1).min(4096);
            }
            b'h' | b'l' => {
                // Only the alt-screen family latches intent; other modes
                // (DECAWM, cursor visibility) are grid concerns, ignored.
                if self.csi_private && matches!(p1, 47 | 1047 | 1049) {
                    self.raw_vt_intent = true;
                }
            }
            b'H' | b'f' | b'A' | b'B' | b'J' | b'S' | b'T' | b'r' | b'd' => {
                // Row addressing / display erase / scroll: fullscreen
                // intent (13.4b) -- paint nothing, latch the boundary.
                self.raw_vt_intent = true;
            }
            _ => {}
        }
    }

    // --- UTF-8 + the line discipline ----------------------------------------

    fn utf8_byte(&mut self, b: u8) {
        if self.utf8_need == 0 {
            if b < 0x80 {
                self.put_char(b as char);
                return;
            }
            let need = if b & 0xe0 == 0xc0 {
                2
            } else if b & 0xf0 == 0xe0 {
                3
            } else if b & 0xf8 == 0xf0 {
                4
            } else {
                self.put_char('\u{fffd}');
                return;
            };
            self.utf8[0] = b;
            self.utf8_len = 1;
            self.utf8_need = need;
            return;
        }
        if b & 0xc0 != 0x80 {
            // Broken continuation: emit one replacement, reprocess `b`.
            self.utf8_len = 0;
            self.utf8_need = 0;
            self.put_char('\u{fffd}');
            self.scan_byte(b);
            return;
        }
        self.utf8[self.utf8_len as usize] = b;
        self.utf8_len += 1;
        if self.utf8_len == self.utf8_need {
            let s = &self.utf8[..self.utf8_len as usize];
            match core::str::from_utf8(s) {
                Ok(st) => {
                    if let Some(ch) = st.chars().next() {
                        self.put_char(ch);
                    }
                }
                Err(_) => self.put_char('\u{fffd}'),
            }
            self.utf8_len = 0;
            self.utf8_need = 0;
        }
    }

    fn put_char(&mut self, ch: char) {
        let style = self.style_idx();
        let byte_cap = self.transient_cap();
        if let Some(t) = self.table.as_mut() {
            // Inside a table: cell content appends (no column discipline);
            // padding between cells is the plain realization -- dropped. The
            // per-cell char cap AND the whole-table byte budget both bound it.
            if t.in_cell && t.cell.len() < MAX_CELL_CHARS && t.bytes < byte_cap {
                t.cell.push(TCell {
                    ch: if ch < ' ' { ' ' } else { ch },
                    style,
                });
                t.bytes = t.bytes.saturating_add(core::mem::size_of::<TCell>());
            }
            return;
        }
        // Soft-wrap a pathological single line (no newline): flushing it
        // charges its cost + advances toward the per-block line cap, so the
        // budget/eviction machinery can bound an endless line (else `self.line`
        // grows until the heap dies). MAX_LINE_CELLS == the CUF/CHA clamps, so
        // a cursor-positioned write never trips this early.
        if self.col >= MAX_LINE_CELLS {
            self.flush_line();
        }
        if self.col < self.line.len() {
            self.line[self.col] = TCell { ch, style };
        } else {
            while self.line.len() < self.col {
                self.line.push(TCell { ch: ' ', style });
            }
            self.line.push(TCell { ch, style });
        }
        self.col += 1;
    }

    fn newline(&mut self) {
        if self.table.is_some() {
            return; // row separation is structural, not textual
        }
        if self.line.is_empty() {
            self.col = 0;
            // PL-1b: a blank line inside a pre is a verbatim blank pre line
            // (the accumulator, cap-bounded); outside, a charged empty Line.
            let cap = self.max_lines_per_block;
            if let Some(pre) = self.pre.as_mut() {
                if pre.len() < cap {
                    pre.push(Line::plain(Vec::new()));
                }
                return;
            }
            // A blank line is content: keep it as an empty Line item -- and
            // charge it: a million empty lines is a million items.
            let cost = ITEM_OVERHEAD;
            self.open.cost += cost;
            self.stored_cost += cost;
            self.open.items.push(Item::Line(Line::plain(Vec::new())));
            self.enforce_block_cap();
            return;
        }
        self.flush_line();
    }

    /// Intern the current pen+span state as a style index in the OPEN block.
    fn style_idx(&mut self) -> u16 {
        let s = Style {
            fg: self.pen.fg,
            bg: self.pen.bg,
            attrs: self.pen.attrs,
            em: self.em_stack.last().copied().unwrap_or(EM_NONE),
            obj: self.obj_stack.last().copied().unwrap_or(0),
            hdr: self.hdr,
        };
        self.intern_style(s)
    }

    /// Intern an explicit style as an index in the OPEN block: dedup on the hot
    /// tail, linear scan under the cap, then degrade-to-last past it (bounds
    /// memory + keeps the index in u16; a truecolor-gradient spam otherwise
    /// scans a growing table per char, and reaching thousands of distinct
    /// styles in one block is hostile). Shared by the pen path (`style_idx`) and
    /// the KT-1.5 ScrollOff ingest (`push_scrolled_rows`, a pre-styled vt::Cell).
    fn intern_style(&mut self, s: Style) -> u16 {
        // Blocks carry few styles; a linear scan with a hot tail wins over a map.
        if let Some(last) = self.open.styles.last() {
            if *last == s {
                return (self.open.styles.len() - 1) as u16;
            }
        }
        if self.open.styles.len() >= MAX_STYLES_PER_BLOCK {
            return (self.open.styles.len() - 1) as u16;
        }
        for (i, st) in self.open.styles.iter().enumerate() {
            if *st == s {
                return i as u16;
            }
        }
        self.open.styles.push(s);
        (self.open.styles.len() - 1) as u16
    }

    /// KT-1.5 (HALCYON 14.11.2): ingest ScrollOff rows -- lines that left the top
    /// of a tile's live grid -- as history in the current (open) block. Each row
    /// is a finished screen line of pre-styled `vt::Cell`s (the kaua-term already
    /// ran the VT); intern each cell's style into the open block and append the
    /// row as a `Line`, mirroring `flush_line`'s cost accounting so the block-cap
    /// / eviction machinery bounds a tile that scrolls forever. No zone logic
    /// here: a zone cut arrives as a separate `Control(Osc1936Raw)` record fed
    /// through `feed`, and stream order (guaranteed by the producer) lands each
    /// scroll-off in the block that was open when it happened.
    ///
    /// H-4d: each cell's span (`vt::Cell.span` -> `spans`) gives the em / obj
    /// / hdr it was WRITTEN under -- the grid's rows straddle zone cuts, so
    /// a row may land in a later block than the one its objs index; the obj
    /// is then COPIED into the landing block (`local_obj`), keeping every
    /// block self-contained (its Line styles index its own obj table, the
    /// console's invariant every run/menu consumer relies on).
    pub fn push_scrolled_rows(&mut self, rows: &[Vec<vt::Cell>], wrapped: &[bool], spans: &SpanMap) {
        for (i, row) in rows.iter().enumerate() {
            // PL-3: a soft-wrapped grid row is half of a logical line the grid
            // broke at `cols` (often mid-word, s5). Accumulate the raw cells
            // until a row that did NOT wrap ends the logical line, then
            // finalize it as ONE Line so the flow layout re-wraps at word
            // boundaries. Raw vt::Cells, not interned TCells: their style is
            // self-contained (no block-relative index), so a block change
            // mid-line -- a frozen open block, or an inline obj / zone frame
            // arriving as a Control record between two ScrollOff records --
            // interns cleanly into whatever block is open at finalize (the
            // straddle case local_obj already copies the obj across).
            self.scroll_pending.extend_from_slice(row);
            // A logical line that soft-wraps forever (no LF) is hard-split at
            // MAX_LINE_CELLS, the same clamp the feed path uses, so a
            // pathological stream cannot grow the held fragment unbounded.
            let ends = !wrapped.get(i).copied().unwrap_or(false);
            if ends || self.scroll_pending.len() >= MAX_LINE_CELLS {
                self.finalize_scroll_pending(spans);
            }
        }
    }

    /// Intern the pending soft-wrapped logical line into the open block as one
    /// Line and clear it, mirroring the old per-row cost accounting so the
    /// block-cap / eviction machinery still bounds a tile that scrolls forever.
    /// No-op when nothing is pending.
    fn finalize_scroll_pending(&mut self, spans: &SpanMap) {
        if self.scroll_pending.is_empty() {
            return;
        }
        let mut raw = core::mem::take(&mut self.scroll_pending);
        trim_untagged_tail(&mut raw);
        // (source block, obj) -> the index in the open block. A map, not a
        // scan: a cell naming a distinct frozen obj must not pay O(n) (the
        // H-arc round-1 audit, B-F3). One push at the end, so the open block
        // cannot change mid-loop and no reset is needed.
        let mut remap: BTreeMap<(u64, u16), u16> = BTreeMap::new();
        let mut cells: Vec<TCell> = Vec::with_capacity(raw.len());
        for c in &raw {
            let tag = spans.get(c.span).unwrap_or_default();
            let obj = self.local_obj(tag, &mut remap);
            let style = self.intern_style(Style {
                fg: c.fg,
                bg: c.bg,
                attrs: c.attrs,
                em: tag_style_em(tag.em),
                obj,
                hdr: tag_style_hdr(tag.hdr),
            });
            cells.push(TCell { ch: c.ch, style });
        }
        let cost = cells.len() * core::mem::size_of::<TCell>() + ITEM_OVERHEAD;
        self.open.cost += cost;
        self.stored_cost += cost;
        // Cells mode rebuilds the structure the frames announced (a rule, a
        // pre line, a table row) from the tags; the byte-fed console already
        // captured its structure from the stream and takes the plain line.
        let shape = row_shape(&raw, spans);
        let spec = self.table_spec_for(shape.serial);
        let mut last_rule = self.last_rule_serial;
        // A prompt line keeps its class wherever it lands (a scrolled-off
        // prompt row lands in the block open at finalize, often its output).
        let class = if shape.prompt {
            LineClass::Prompt
        } else {
            LineClass::Inherit
        };
        let _ = place_tagged_line(
            &mut self.open.items,
            cells,
            &raw,
            shape,
            spec.as_ref().map(|s| (s.0, s.1.as_slice(), s.2)),
            class,
            spans,
            &mut last_rule,
        );
        self.last_rule_serial = last_rule;
        self.enforce_block_cap();
    }

    /// The spec of the table a cell with `serial` was written inside (cells
    /// mode): the most recent open at or before the serial that had not
    /// closed by it. Copied out (the caller then mutates `self`).
    fn table_spec_for(&self, serial: u32) -> Option<(u32, Vec<u8>, bool)> {
        self.table_specs
            .iter()
            .rev()
            .find(|s| s.open_serial <= serial && serial <= s.close_serial)
            .map(|s| (s.open_serial, s.cols.clone(), s.hdr))
    }

    /// PL-3: force any in-flight soft-wrapped ScrollOff line to finalize -- at a
    /// screen-mode change, where the content model has a hard discontinuity and
    /// a fragment must not carry a stale continuation across it. No-op when
    /// nothing is pending.
    pub fn flush_scroll_pending(&mut self, spans: &SpanMap) {
        self.finalize_scroll_pending(spans);
    }

    /// PL-4: build the LIVE grid as a transient block for the normal-mode
    /// proportional render. Joins the grid's soft-wrapped rows into logical
    /// lines (using `wrapped`, the CellDiff snapshot), interns each cell's span
    /// (obj / em / hdr, the obj COPIED from its source block -- frozen or open)
    /// into the transient block's own tables, and returns it plus, per grid row,
    /// the `(logical-line item index, starting column in that line)` so the
    /// caller maps a grid row / cursor / obj-run back to its proportional
    /// position (the caret + the live selection, PL-4b). READ-ONLY -- a
    /// per-frame render, never history, so nothing here mutates the transcript
    /// (the obj-copy and style-intern build fresh transient tables, unlike
    /// `finalize_scroll_pending` which writes into the open block).
    pub fn live_block(
        &self,
        grid_cells: &[vt::Cell],
        cols: usize,
        rows: usize,
        wrapped: &[bool],
        spans: &SpanMap,
    ) -> (Block, Vec<(usize, usize, usize)>) {
        struct Pending {
            raw: Vec<vt::Cell>,
            cells: Vec<TCell>,
            src: Option<u64>,
        }
        // Pass 1: the logical lines -- soft-wrapped grid rows joined -- each
        // with its raw cells and its source zone (the first tagged cell's
        // block). `prov` provisionally maps a grid row to its LINE index and
        // start column; it is patched to the (ITEM, ROW) address once the
        // lines are placed (a pre line or a table row joins an item an
        // earlier line started, so the item alone would name the FIRST row
        // of every rebuilt structure). The live grid straddles zones, so each line is classed
        // from its zone: a prompt zone's line is the prompt; a zone with any
        // Beacon structure -- in the cells still on the grid (the tags) or in
        // its scrolled-off part (the block's own styles/items) -- is a
        // document; a zone with none, or a cell no frame ever tagged (a plain
        // tile), is raw terminal content. One late annotation classes every
        // line of its zone alike.
        let mut lines: Vec<Pending> = Vec::new();
        let mut prov: Vec<(usize, usize, usize)> = Vec::with_capacity(rows);
        let mut cur_raw: Vec<vt::Cell> = Vec::new();
        let mut cur_src: Option<u64> = None;
        let mut zone_tagged: BTreeMap<u64, bool> = BTreeMap::new();
        for r in 0..rows {
            prov.push((lines.len(), usize::MAX, cur_raw.len()));
            let base = r * cols;
            let row = grid_cells.get(base..base + cols).unwrap_or(&[]);
            for c in row {
                if let Some(tag) = spans.get(c.span) {
                    if cur_src.is_none() {
                        cur_src = Some(tag.block);
                    }
                    let e = zone_tagged.entry(tag.block).or_insert(false);
                    *e |= tag.obj != 0 || tag.em != 0 || tag.hdr != 0;
                }
                cur_raw.push(*c);
            }
            // A row that did NOT autowrap ends the logical line.
            if !wrapped.get(r).copied().unwrap_or(false) {
                lines.push(Pending {
                    raw: core::mem::take(&mut cur_raw),
                    cells: Vec::new(),
                    src: cur_src.take(),
                });
            }
        }
        // A trailing soft-wrapped row (the grid ends mid-logical-line) still lays.
        if !cur_raw.is_empty() {
            lines.push(Pending {
                raw: cur_raw,
                cells: Vec::new(),
                src: cur_src.take(),
            });
        }
        // Intern each line's cells (the grid's never-written tail trimmed)
        // into the transient block's own tables: the obj COPIED from its
        // source block (frozen or open), deduped through remap -- an evicted
        // source or a full table yields 0 (a run that lost its object, never
        // a wrong one), the local_obj discipline; the style through the
        // intern_style discipline (hot tail, capped scan, degrade to last).
        let mut styles: Vec<Style> = Vec::new();
        let mut objs: Vec<Obj> = Vec::new();
        let mut remap: BTreeMap<(u64, u16), u16> = BTreeMap::new();
        for l in lines.iter_mut() {
            trim_untagged_tail(&mut l.raw);
            for c in l.raw.iter() {
                let tag = spans.get(c.span).unwrap_or_default();
                let obj = if tag.obj == 0 {
                    0
                } else if let Some(&idx) = remap.get(&(tag.block, tag.obj)) {
                    idx
                } else {
                    let src = self
                        .frozen
                        .iter()
                        .chain(core::iter::once(&self.open))
                        .find(|b| b.id == tag.block)
                        .and_then(|b| b.objs.get((tag.obj as usize).wrapping_sub(1)))
                        .map(|o| (o.ty.clone(), o.refv.clone()));
                    let idx = match src {
                        None => 0,
                        Some(_) if objs.len() >= MAX_OBJS_PER_BLOCK => 0,
                        Some((ty, refv)) => {
                            objs.push(Obj { ty, refv });
                            objs.len() as u16
                        }
                    };
                    remap.insert((tag.block, tag.obj), idx);
                    idx
                };
                let st = Style {
                    fg: c.fg,
                    bg: c.bg,
                    attrs: c.attrs,
                    em: tag_style_em(tag.em),
                    obj,
                    hdr: tag_style_hdr(tag.hdr),
                };
                let style = if styles.last() == Some(&st) || styles.len() >= MAX_STYLES_PER_BLOCK {
                    (styles.len() - 1) as u16
                } else if let Some(i) = styles.iter().position(|s| *s == st) {
                    i as u16
                } else {
                    styles.push(st);
                    (styles.len() - 1) as u16
                };
                l.cells.push(TCell { ch: c.ch, style });
            }
        }
        // Pass 2: class each line from its zone and place it -- a rule before
        // it, a pre line into the open pre, a table row into its table, else
        // a plain line -- patching `prov` to the item each line landed in.
        let mut zone_class: BTreeMap<u64, LineClass> = BTreeMap::new();
        let mut items: Vec<Item> = Vec::new();
        let mut line_item: Vec<(usize, usize)> = Vec::with_capacity(lines.len());
        let mut last_rule = self.last_rule_serial;
        for l in lines.into_iter() {
            let shape = row_shape(&l.raw, spans);
            // The prompt axis rides the tag (TAG_PROMPT); the document/raw
            // axis is per zone: any annotation among the zone's tags, or in
            // its block's scrolled-off part.
            let class = match l.src {
                _ if shape.prompt => LineClass::Prompt,
                None => LineClass::Raw,
                Some(id) => *zone_class.entry(id).or_insert_with(|| {
                    let blk = self
                        .frozen
                        .iter()
                        .chain(core::iter::once(&self.open))
                        .find(|b| b.id == id);
                    match blk {
                        _ if zone_tagged.get(&id).copied().unwrap_or(false) => LineClass::Doc,
                        Some(b) if b.annotated() => LineClass::Doc,
                        _ => LineClass::Raw,
                    }
                }),
            };
            let spec = self.table_spec_for(shape.serial);
            let idx = place_tagged_line(
                &mut items,
                l.cells,
                &l.raw,
                shape,
                spec.as_ref().map(|s| (s.0, s.1.as_slice(), s.2)),
                class,
                spans,
                &mut last_rule,
            );
            line_item.push(idx);
        }
        let last = (items.len().saturating_sub(1), usize::MAX);
        for p in prov.iter_mut() {
            let (item, row) = line_item.get(p.0).copied().unwrap_or(last);
            p.0 = item;
            p.1 = row;
        }
        let b = Block {
            id: u64::MAX,
            kind: BlockKind::Foreign,
            continuation: false,
            exit: None,
            cmd: None,
            items,
            styles,
            objs,
            cost: 0,
        };
        (b, prov)
    }

    /// The open block's index for a tagged obj: its own when the tag's block
    /// IS the open block; else the obj is copied in once (the remap cache)
    /// at the same cost the wire's obj-open charges. An evicted source block
    /// or a full table yields 0 -- a run that lost its object, never a wrong
    /// one.
    fn local_obj(&mut self, tag: SpanTag, remap: &mut BTreeMap<(u64, u16), u16>) -> u16 {
        if tag.obj == 0 {
            return 0;
        }
        if tag.block == self.open.id {
            return tag.obj;
        }
        if let Some(&idx) = remap.get(&(tag.block, tag.obj)) {
            return idx;
        }
        let src = self
            .frozen
            .iter()
            .find(|b| b.id == tag.block)
            .and_then(|b| b.objs.get((tag.obj as usize).wrapping_sub(1)))
            .map(|o| (o.ty.clone(), o.refv.clone()));
        let idx = match src {
            None => 0,
            Some(_) if self.open.objs.len() >= MAX_OBJS_PER_BLOCK => 0,
            Some((ty, refv)) => {
                let bytes = ty.len() + refv.len();
                self.open.cost += bytes;
                self.stored_cost += bytes;
                self.open.objs.push(Obj { ty, refv });
                self.open.objs.len() as u16
            }
        };
        remap.insert((tag.block, tag.obj), idx);
        idx
    }

    /// H-4d: the span state after the last feed, as the tag for the cells
    /// the producer writes next (the tile notes it under the frame's serial).
    pub fn span_tag(&self) -> SpanTag {
        let mut em = self.em_stack.last().copied().unwrap_or(EM_NONE) & TAG_EM_MASK;
        let mut hdr = self.hdr & HDR_MASK;
        // The structure bits a frame-fed tile rebuilds from (the byte-fed
        // console never notes tags, so they cost it nothing).
        if self.pre.is_some() {
            em |= TAG_PRE;
        }
        if let Some(t) = self.table.as_ref() {
            if t.in_cell {
                em |= TAG_CELL;
                hdr |= (t.row.len().min(15) as u8) << 3;
                if t.hdr && t.rows.is_empty() {
                    em |= TAG_ROW_HDR;
                }
            }
        }
        if self.rule_pending {
            em |= TAG_RULE;
        }
        if self.open.kind == BlockKind::Prompt {
            em |= TAG_PROMPT;
        }
        SpanTag {
            block: self.open.id,
            obj: self.obj_stack.last().copied().unwrap_or(0),
            em,
            hdr,
        }
    }

    /// KT-1: feed one Beacon frame from a tile, stamped with the serial its
    /// following cells carry. Sets the frame context the structure tags need
    /// (`cur_serial`).
    pub fn feed_frame(&mut self, frame: &[u8], serial: u32) {
        self.cur_serial = serial;
        self.feed(frame);
    }

    /// The serial of the `rule` frame whose episode is open (cells mode):
    /// the tile ends it when it sees cells written after that frame.
    pub fn rule_open(&self) -> Option<u32> {
        if self.rule_pending {
            Some(self.rule_serial)
        } else {
            None
        }
    }

    /// KT-1: the tile saw cells written after the open rule frame -- the
    /// line the rule precedes now exists, so later frames stop carrying it.
    /// (Ending it on an OP instead loses a rule the frames close over --
    /// `rule` then `/zone` -- and double-places one whose text line is
    /// followed by an inline open: no op can tell whether text was written.)
    pub fn end_rule(&mut self) {
        self.rule_pending = false;
    }

    /// KT-1: this transcript is fed FRAMES only -- the text arrives as grid
    /// cells (ScrollOff rows + the live grid) tagged with the frame serials.
    /// Tables, pre blocks and rules are then REBUILT from the tagged cells
    /// (`place_tagged_line`) instead of captured from a byte stream the tile
    /// never sees.
    pub fn set_cells_mode(&mut self, on: bool) {
        self.cells_mode = on;
    }

    /// The block with id `id` -- the open one or a frozen one; None once
    /// evicted.
    pub fn block_by_id(&self, id: u64) -> Option<&Block> {
        if self.open.id == id {
            Some(&self.open)
        } else {
            self.frozen.iter().find(|b| b.id == id)
        }
    }

    /// The (type, resolved ref) of obj `obj` (idx+1) in block `id`.
    pub fn obj_in_block(&self, id: u64, obj: u16) -> Option<(&str, &str)> {
        let b = self.block_by_id(id)?;
        let o = b.objs.get((obj as usize).checked_sub(1)?)?;
        Some((o.ty.as_str(), o.refv.as_str()))
    }

    /// Re-budget a live transcript (a session shares one scrollback budget
    /// across its tiles, so each tile's share moves as tiles come and go).
    /// Enforced NOW, not at the next push: a QUIET tile never pushes, so a
    /// lazy cap would let it keep its whole old share indefinitely and the
    /// session's retained sum would grow as the budget times the harmonic
    /// number of the tile count. The open block freezes if it alone exceeds
    /// the new open cap, so the eviction loop can reach it.
    pub fn set_max_cost(&mut self, max_cost: usize) {
        self.max_cost = max_cost;
        self.max_open_cost = open_cap(max_cost);
        self.enforce_block_cap();
        self.enforce_budget();
    }

    /// The per-tile cap on the pre/table in-progress accumulators (each
    /// uncharged to the block budget until close). HALF the tile's scrollback
    /// share: N tiles -- each share = SESSION_SCROLLBACK_BUDGET/N -- hold at
    /// most SESSION_SCROLLBACK_BUDGET/2 in transients TOTAL, regardless of tile
    /// count, against a fixed 16 MiB that N tiles multiply into an OOM. Floored
    /// at one open block so an artificially tiny max_cost (a test's
    /// set_max_cost(1)) still admits a small transient; the floor never binds
    /// for a real tile (N <= MAX_PANES keeps max_cost/2 above it).
    fn transient_cap(&self) -> usize {
        (self.max_cost / 2).max(OPEN_BLOCK_MAX_COST)
    }

    /// The retained cost the budget bounds (frozen blocks + the open one).
    pub fn stored_cost(&self) -> usize {
        self.stored_cost
    }
}

// --- the chunk-boundary holdback -------------------------------------------

/// The open block's byte cap for a budget (see `OPEN_BLOCK_MAX_COST`).
fn open_cap(max_cost: usize) -> usize {
    (max_cost / 8).clamp(1, OPEN_BLOCK_MAX_COST)
}

/// Find the safe parse cut: the start of the escape sequence still OPEN at
/// the buffer end (or len when none is). A last-ESC heuristic is wrong
/// here -- an OSC's ST terminator is itself a later ESC, so cutting at the
/// last ESC can strand the OSC's OPENER unterminated in the head, which
/// the wire parser then rightly drops whole (caught by the byte-by-byte
/// determinism test). So: walk the buffer with a tiny state machine,
/// remembering where the current sequence began. Bounded by the wire caps:
/// an over-long partial flushes through (the parser's own drop /
/// passthrough rules then apply).
fn safe_cut(buf: &[u8]) -> usize {
    #[derive(Clone, Copy, PartialEq)]
    enum S {
        Ground,
        Esc,
        Csi,
        Osc,
        OscEsc,
        Charset,
    }
    let mut st = S::Ground;
    let mut start = 0usize;
    for (i, &b) in buf.iter().enumerate() {
        match st {
            S::Ground => {
                if b == 0x1b {
                    start = i;
                    st = S::Esc;
                }
            }
            S::Esc => {
                st = match b {
                    b'[' => S::Csi,
                    b']' => S::Osc,
                    b'(' | b')' => S::Charset,
                    _ => S::Ground,
                };
            }
            S::Charset => st = S::Ground,
            S::Csi => {
                if (0x40..=0x7e).contains(&b) {
                    st = S::Ground;
                }
            }
            S::Osc => {
                if b == 0x07 {
                    st = S::Ground;
                } else if b == 0x1b {
                    st = S::OscEsc;
                }
            }
            S::OscEsc => {
                st = if b == b'\\' { S::Ground } else { S::Osc };
            }
        }
        if st != S::Ground && i - start > wire::FRAME_MAX + 16 {
            // Over-long partial: stop protecting it; flush through.
            st = S::Ground;
        }
    }
    if st == S::Ground {
        buf.len()
    } else {
        start
    }
}

// ---------------------------------------------------------------------------

#[cfg(test)]
mod tests {
    use super::*;
    use alloc::format;

    #[test]
    fn span_slot_is_16_bytes_and_the_ring_allocates_on_the_first_note() {
        // B-F4: a plain tile costs nothing; a rich one one fixed ring.
        assert_eq!(core::mem::size_of::<SpanSlot>(), 16);
        assert_eq!(SPAN_MAP_BYTES, SPAN_MAP_ENTRIES * 16);
        let mut m = SpanMap::new();
        assert_eq!(m.bytes(), 0);
        assert_eq!(m.get(1), None, "an unallocated ring resolves nothing");
        m.note(0, SpanTag { block: 9, obj: 1, em: 0, hdr: 0 });
        assert_eq!(m.bytes(), 0, "serial 0 is ignored and allocates nothing");
        let tag = SpanTag { block: 7, obj: 3, em: 2, hdr: 1 };
        m.note(5, tag);
        assert_eq!(m.bytes(), SPAN_MAP_BYTES);
        assert_eq!(m.get(5), Some(tag), "the full tag round-trips the packed slot");
        // The full serial validates the slot: a colliding serial reads none,
        // and overwriting the slot retires the old serial.
        assert_eq!(m.get(5 + SPAN_MAP_ENTRIES as u32), None);
        m.note(5 + SPAN_MAP_ENTRIES as u32, SpanTag::default());
        assert_eq!(m.get(5), None, "a serial that fell off resolves to no span");
        assert_eq!(m.get(5 + SPAN_MAP_ENTRIES as u32), Some(SpanTag::default()));
    }
    use alloc::vec;
    use beacon::wire::Op;

    fn daylight() -> Palette {
        libhalcyon::theme::daylight_palette()
    }

    fn frames(parts: &[FramePart]) -> Vec<u8> {
        let mut out = Vec::new();
        for p in parts {
            match p {
                FramePart::Open(op, args) => wire::open(&mut out, *op, args),
                FramePart::Close(op) => wire::close(&mut out, *op),
                FramePart::Point(op, args) => wire::point(&mut out, *op, args),
                FramePart::Text(t) => out.extend_from_slice(t.as_bytes()),
            }
        }
        out
    }

    enum FramePart<'a> {
        Open(Op, &'a [(&'a str, &'a str)]),
        Close(Op),
        Point(Op, &'a [(&'a str, &'a str)]),
        Text(&'a str),
    }
    use FramePart as F;

    fn line_str(l: &Line) -> String {
        l.cells.iter().map(|c| c.ch).collect()
    }

    #[test]
    fn osc7_sets_the_session_cwd_and_a_bad_report_changes_nothing() {
        // H-3d: the one foreign OSC the transcript interprets. ST and BEL
        // terminators; percent-decoding; another host is not ours; an
        // oversize body is dropped WHOLE (never truncated into a different
        // path); a control byte rejects the report.
        let mut t = Transcript::new(daylight());
        assert_eq!(t.cwd(), "");
        t.feed(b"\x1b]7;file://localhost/lib/aurora\x1b\\");
        assert_eq!(t.cwd(), "/lib/aurora");
        t.feed(b"before \x1b]7;file:///a%20b\x07 after");
        assert_eq!(t.cwd(), "/a b");
        assert_eq!(
            line_str(&Line::plain(t.pending_line().to_vec())),
            "before  after"
        );
        t.feed(b"\x1b]7;file://otherhost/elsewhere\x1b\\");
        assert_eq!(t.cwd(), "/a b", "another host's report is not ours");
        let mut long: Vec<u8> = Vec::from(&b"\x1b]7;file://localhost/"[..]);
        long.extend(core::iter::repeat(b'x').take(300));
        long.extend_from_slice(b"\x1b\\");
        t.feed(&long);
        assert_eq!(t.cwd(), "/a b", "oversize: dropped whole");
        t.feed(b"\x1b]7;file://localhost/no\x01ctl\x1b\\");
        assert_eq!(t.cwd(), "/a b", "a control byte: rejected");
        t.feed(b"\x1b]7;file://localhost/bad%zz\x1b\\");
        assert_eq!(t.cwd(), "/a b", "a malformed escape: rejected");
        t.feed(b"\x1b]7;file://localhostrelative\x1b\\");
        assert_eq!(t.cwd(), "/a b", "not absolute: rejected");
        // Split across feeds (the byte-at-a-time console): the scanner and
        // its body persist.
        t.feed(b"\x1b]7;file://local");
        t.feed(b"host/split\x1b");
        t.feed(b"\\");
        assert_eq!(t.cwd(), "/split");
    }

    #[test]
    fn the_output_zones_cmd_mark_is_the_running_then_the_last_command() {
        // H-3d: ut marks the accepted line as the output zone's first child;
        // while the zone is open it is the RUNNING command, then the last.
        let mut t = Transcript::new(daylight());
        assert!(t.last_command().is_none());
        t.feed(&frames(&[
            F::Open(Op::Zone, &[("k", "prompt")]),
            F::Text("cora@thyla / $ ls -l\n"),
            F::Close(Op::Zone),
            F::Open(Op::Zone, &[("k", "output")]),
            F::Point(Op::Mark, &[("k", "cmd"), ("text", "ls -l")]),
            F::Text("total 0\n"),
        ]));
        assert_eq!(t.last_command(), Some("ls -l"), "while it runs");
        t.feed(&frames(&[
            F::Point(Op::Mark, &[("k", "exit"), ("code", "0")]),
            F::Close(Op::Zone),
            F::Open(Op::Zone, &[("k", "prompt")]),
            F::Text("cora@thyla / $ "),
        ]));
        assert_eq!(t.last_command(), Some("ls -l"), "after it, until the next");
        t.feed(&frames(&[
            F::Text("make\n"),
            F::Close(Op::Zone),
            F::Open(Op::Zone, &[("k", "output")]),
            F::Point(Op::Mark, &[("k", "cmd"), ("text", "make; echo x%3B")]),
        ]));
        assert_eq!(
            t.last_command(),
            Some("make; echo x%3B"),
            "the wire's escaping is transparent"
        );
        // A cmd mark outside an output zone (a prompt block) is not a command.
        let mut u = Transcript::new(daylight());
        u.feed(&frames(&[
            F::Open(Op::Zone, &[("k", "prompt")]),
            F::Point(Op::Mark, &[("k", "cmd"), ("text", "nope")]),
        ]));
        assert!(u.last_command().is_none());
    }

    fn session_corpus() -> Vec<u8> {
        frames(&[
            F::Text("Thylacine login: cora\n"),
            F::Open(Op::Zone, &[("k", "prompt")]),
            F::Text("cora@thyla / $ ls -l\n"),
            F::Close(Op::Zone),
            F::Open(Op::Zone, &[("k", "output")]),
            F::Open(Op::Table, &[("cols", "lr"), ("hdr", "1")]),
            F::Open(Op::Row, &[]),
            F::Open(Op::Cell, &[]),
            F::Text("NAME"),
            F::Close(Op::Cell),
            F::Text("  "),
            F::Open(Op::Cell, &[]),
            F::Text("SIZE"),
            F::Close(Op::Cell),
            F::Close(Op::Row),
            F::Text("\n"),
            F::Open(Op::Row, &[]),
            F::Open(Op::Cell, &[]),
            F::Open(Op::Obj, &[("type", "path"), ("ref", "/version")]),
            F::Text("version"),
            F::Close(Op::Obj),
            F::Close(Op::Cell),
            F::Text("  "),
            F::Open(Op::Cell, &[]),
            F::Text("42"),
            F::Close(Op::Cell),
            F::Close(Op::Row),
            F::Text("\n"),
            F::Close(Op::Table),
            F::Text("\x1b[31mred error\x1b[0m plain \u{e9}\n"),
            F::Point(Op::Mark, &[("k", "exit"), ("code", "0")]),
            F::Close(Op::Zone),
            F::Open(Op::Zone, &[("k", "prompt")]),
            F::Text("cora@thyla / $ "),
        ])
    }

    fn structure_fingerprint(t: &Transcript) -> String {
        let mut s = String::new();
        for b in t.frozen_blocks().iter() {
            s.push_str(&format!(
                "[{:?} exit={:?} items={} styles={} objs={}]",
                b.kind,
                b.exit,
                b.items.len(),
                b.styles.len(),
                b.objs.len()
            ));
            for it in b.items.iter() {
                match it {
                    Item::Line(l) => {
                        s.push('L');
                        s.push_str(&line_str(l));
                        for c in l.cells.iter() {
                            s.push_str(&format!("{:x}", c.style));
                        }
                    }
                    Item::Table(tb) => {
                        s.push_str(&format!(
                            "T{}r{}h{}",
                            tb.rows.len(),
                            tb.cols.len(),
                            tb.hdr as u8
                        ));
                        for r in tb.rows.iter() {
                            for c in r.iter() {
                                s.push(':');
                                s.extend(c.iter().map(|x| x.ch));
                            }
                        }
                    }
                    Item::Rule => s.push('R'),
                    Item::Pre(lines) => {
                        s.push('P');
                        for l in lines.iter() {
                            s.push('|');
                            s.push_str(&line_str(l));
                        }
                    }
                }
            }
            s.push('|');
        }
        s.push_str(&format!(
            "open[{:?} items={}]",
            t.open_block().kind,
            t.open_block().items.len()
        ));
        s.push_str(&line_str(&Line::plain(t.pending_line().to_vec())));
        s
    }

    #[test]
    fn zones_become_blocks() {
        let mut t = Transcript::new(daylight());
        t.feed(&session_corpus());
        let blocks = t.frozen_blocks();
        assert_eq!(blocks.len(), 3, "foreign login + prompt + output");
        assert_eq!(blocks[0].kind, BlockKind::Foreign);
        assert_eq!(blocks[1].kind, BlockKind::Prompt);
        assert_eq!(blocks[2].kind, BlockKind::Output);
        assert_eq!(
            blocks[2].exit,
            Some(0),
            "the exit mark landed inside the output zone"
        );
        assert_eq!(
            t.open_block().kind,
            BlockKind::Prompt,
            "the next prompt is open"
        );
        assert_eq!(
            line_str(&Line::plain(t.pending_line().to_vec())),
            "cora@thyla / $ "
        );
    }

    #[test]
    fn table_captures_cells_and_drops_padding() {
        let mut t = Transcript::new(daylight());
        t.feed(&session_corpus());
        let out = &t.frozen_blocks()[2];
        let Some(Item::Table(tb)) = out.items.first() else {
            panic!("first output item is the table");
        };
        assert_eq!(tb.cols, vec![b'l', b'r']);
        assert!(tb.hdr);
        assert_eq!(tb.rows.len(), 2);
        assert_eq!(tb.rows[0].len(), 2);
        let name: String = tb.rows[1][0].iter().map(|c| c.ch).collect();
        assert_eq!(name, "version");
        // The obj span covered the name cell's cells.
        let st = out.styles[tb.rows[1][0][0].style as usize];
        assert!(st.obj > 0, "name cell is an obj span");
        let o = &out.objs[(st.obj - 1) as usize];
        assert_eq!(o.ty, "path");
        assert_eq!(o.refv, "/version");
        // The padding between cells never became content.
        for r in tb.rows.iter() {
            for c in r.iter() {
                let s: String = c.iter().map(|x| x.ch).collect();
                assert!(!s.contains("  "), "no inter-cell padding captured: {:?}", s);
            }
        }
    }

    #[test]
    fn sgr_styles_and_utf8() {
        let mut t = Transcript::new(daylight());
        t.feed(&session_corpus());
        let out = &t.frozen_blocks()[2];
        let Some(Item::Line(l)) = out.items.get(1) else {
            panic!("the red-error line follows the table");
        };
        let s = line_str(l);
        assert_eq!(s, "red error plain \u{e9}");
        let red_style = out.styles[l.cells[0].style as usize];
        let pal = daylight();
        assert_eq!(
            red_style.fg, pal.ansi[1],
            "SGR 31 resolved against parchment"
        );
        let plain_style = out.styles[l.cells[10].style as usize];
        assert_eq!(plain_style.fg, pal.fg, "SGR 0 reset");
        assert_eq!(l.cells[s.chars().count() - 1].ch, '\u{e9}', "UTF-8 decoded");
    }

    fn pre_of(items: &[Item]) -> &[Line] {
        items
            .iter()
            .find_map(|i| match i {
                Item::Pre(l) => Some(l.as_slice()),
                _ => None,
            })
            .expect("an Item::Pre in the block")
    }

    #[test]
    fn pre_block_captures_verbatim_lines() {
        // PL-1b: a `pre` block builds ONE Item::Pre holding its lines VERBATIM
        // -- internal spacing preserved (no collapse), line breaks significant.
        let mut t = Transcript::new(daylight());
        t.feed(&frames(&[
            F::Open(Op::Pre, &[]),
            F::Text("a  b\n"), // two spaces
            F::Text("cd\n"),
            F::Close(Op::Pre),
        ]));
        let items = &t.open_block().items;
        let lines = pre_of(items);
        assert_eq!(lines.len(), 2, "two verbatim pre lines");
        assert_eq!(line_str(&lines[0]), "a  b", "spacing preserved (no collapse)");
        assert_eq!(line_str(&lines[1]), "cd");
        assert_eq!(items.len(), 1, "the pre content did not leak into Line items");
    }

    #[test]
    fn pre_inline_obj_keeps_its_span() {
        // An inline `obj` inside a pre keeps its span (a path in a `la` listing
        // stays a resolvable object even though the block is mono).
        let mut t = Transcript::new(daylight());
        t.feed(&frames(&[
            F::Open(Op::Pre, &[]),
            F::Text("see "),
            F::Open(Op::Obj, &[("type", "path"), ("ref", "/bin")]),
            F::Text("bin"),
            F::Close(Op::Obj),
            F::Text("\n"),
            F::Close(Op::Pre),
        ]));
        let b = t.open_block();
        let lines = pre_of(&b.items);
        assert_eq!(line_str(&lines[0]), "see bin");
        let st = b.styles[lines[0].cells[4].style as usize]; // the 'b' of "bin"
        assert_ne!(st.obj, 0, "the obj run's cell carries an obj index");
        let obj = &b.objs[(st.obj - 1) as usize];
        assert_eq!((obj.ty.as_str(), obj.refv.as_str()), ("path", "/bin"));
    }

    #[test]
    fn pre_is_bounded_under_a_line_flood() {
        // A hostile `pre` (a newline storm) stays bounded: the accumulator caps
        // at the per-block line limit -- the format-fuzz DoS floor (KT-1).
        let mut t = Transcript::with_caps(daylight(), DEFAULT_MAX_BLOCKS, DEFAULT_MAX_COST, 8);
        let mut parts = vec![F::Open(Op::Pre, &[])];
        for _ in 0..100 {
            parts.push(F::Text("\n"));
        }
        parts.push(F::Close(Op::Pre));
        t.feed(&frames(&parts));
        let lines = pre_of(&t.open_block().items);
        assert!(lines.len() <= 8, "the pre line count is capped: {}", lines.len());
    }

    #[test]
    fn pre_is_bounded_under_a_byte_flood() {
        // Wide lines are bounded by the byte budget (transient_cap()), not only
        // the line cap. A ~16 MiB pre also charges enough to FREEZE the block
        // on close, so the Item::Pre may land in a frozen block -- look across
        // both. The invariant: fewer lines than fed (bytes dropped some) AND
        // the content stays within one line of the cap.
        let mut t = Transcript::new(daylight());
        let wide: String = core::iter::repeat('x').take(4000).collect();
        let framed: Vec<String> = (0..700).map(|_| format!("{wide}\n")).collect();
        let mut parts = vec![F::Open(Op::Pre, &[])];
        for l in framed.iter() {
            parts.push(F::Text(l.as_str()));
        }
        parts.push(F::Close(Op::Pre));
        t.feed(&frames(&parts));
        let mut lines_len = 0usize;
        let mut cells = 0usize;
        let measure = |ls: &[Line], ll: &mut usize, cc: &mut usize| {
            *ll = ls.len();
            *cc = ls.iter().map(|l| l.cells.len()).sum();
        };
        for b in t.frozen_blocks().iter() {
            for it in b.items.iter() {
                if let Item::Pre(ls) = it {
                    measure(ls, &mut lines_len, &mut cells);
                }
            }
        }
        for it in t.open_block().items.iter() {
            if let Item::Pre(ls) = it {
                measure(ls, &mut lines_len, &mut cells);
            }
        }
        assert!(lines_len > 0, "the pre landed (open or frozen)");
        assert!(
            lines_len < 700,
            "the byte budget dropped lines: {lines_len} of 700 fed"
        );
        assert!(
            cells * 8 <= t.transient_cap() + 4096 * 8,
            "pre content bounded within one line of the cap: {} MiB",
            cells * 8 / (1 << 20)
        );
    }

    #[test]
    fn transient_cap_is_half_the_share_floored_at_one_open_block() {
        // F2: the pre/table transient cap = HALF the tile's scrollback share,
        // so N tiles (each share = SESSION_SCROLLBACK_BUDGET/N) sum to at most
        // SESSION_SCROLLBACK_BUDGET/2 regardless of N -- vs a fixed 16 MiB that
        // N tiles multiply into an OOM.
        let mut t = Transcript::new(daylight());
        // The default 32 MiB single-tile share -> 16 MiB (unchanged behavior).
        assert_eq!(t.transient_cap(), 16 << 20);
        // A 4-tile share (8 MiB) -> 4 MiB; aggregate over 4 tiles = 16 MiB.
        t.set_max_cost(8 << 20);
        assert_eq!(t.transient_cap(), 4 << 20);
        // An artificially tiny share floors at one open block, never 0, so a
        // test's set_max_cost(1) still admits a small transient (the floor never
        // binds for a real tile: N <= MAX_PANES keeps share/2 above it).
        t.set_max_cost(1);
        assert_eq!(t.transient_cap(), OPEN_BLOCK_MAX_COST);
    }

    #[test]
    fn the_pre_transient_scales_with_the_tile_share_not_a_fixed_16_mib() {
        // F2 runtime: a small-share tile caps the pre at its half-share, well
        // below the old fixed 16 MiB. Pre-F2 this tile would accumulate 16 MiB.
        // (max_lines high so the BYTE cap binds first, not the line cap.)
        let mut t = Transcript::with_caps(daylight(), 1000, 4 << 20, 1_000_000);
        assert_eq!(t.transient_cap(), 2 << 20, "half the 4 MiB share");
        let wide: String = core::iter::repeat('x').take(4000).collect();
        let framed: Vec<String> = (0..500).map(|_| format!("{wide}\n")).collect();
        let mut parts = vec![F::Open(Op::Pre, &[])];
        for l in framed.iter() {
            parts.push(F::Text(l.as_str()));
        }
        parts.push(F::Close(Op::Pre));
        t.feed(&frames(&parts));
        let mut cells = 0usize;
        for b in t
            .frozen_blocks()
            .iter()
            .chain(core::iter::once(t.open_block()))
        {
            for it in b.items.iter() {
                if let Item::Pre(ls) = it {
                    cells += ls.iter().map(|l| l.cells.len()).sum::<usize>();
                }
            }
        }
        // Bounded within one wide line of the 2 MiB half-share -- NOT 16 MiB.
        assert!(
            cells * 8 <= (2 << 20) + 4000 * 8,
            "pre bounded by the 2 MiB half-share, not 16 MiB: {} KiB",
            cells * 8 / 1024
        );
        assert!(
            cells * 8 > (2 << 20) / 2,
            "the pre accumulated up to its cap (not dropped early): {} KiB",
            cells * 8 / 1024
        );
    }

    #[test]
    fn pre_ignores_a_malformed_nested_block_and_still_closes() {
        // A block op inside a pre is malformed (pre nests only inline): ignored,
        // the pre keeps accumulating and closes cleanly -- no table leaks, no
        // stuck-open pre. The containment guard.
        let mut t = Transcript::new(daylight());
        t.feed(&frames(&[
            F::Open(Op::Pre, &[]),
            F::Text("x\n"),
            F::Open(Op::Table, &[("cols", "l")]), // malformed -> ignored
            F::Text("y\n"),
            F::Close(Op::Table), // ignored
            F::Close(Op::Pre),
            F::Text("after\n"), // an ordinary Line AFTER the pre
        ]));
        let items = &t.open_block().items;
        let lines = pre_of(items);
        assert_eq!(lines.len(), 2, "both x and y are pre lines (the table was ignored)");
        assert_eq!(line_str(&lines[0]), "x");
        assert_eq!(line_str(&lines[1]), "y");
        assert!(
            items
                .iter()
                .any(|i| matches!(i, Item::Line(l) if line_str(l) == "after")),
            "text after the pre is a normal Line (the pre really closed)"
        );
        assert!(
            !items.iter().any(|i| matches!(i, Item::Table(_))),
            "the malformed nested table did not leak"
        );
    }

    #[test]
    fn pre_lays_mono_with_ground_and_gutter() {
        // PL-1b render: a pre lays MONO (every seg FACE_MONO, even an annotated
        // obj run -- a Line would lay that proportional) and emits its two
        // chrome rects (the code-fence ground + the leading 2px gutter rule).
        let mut t = Transcript::new(daylight());
        t.feed(&frames(&[
            F::Open(Op::Pre, &[]),
            F::Open(Op::Obj, &[("type", "path"), ("ref", "/bin")]),
            F::Text("/bin"),
            F::Close(Op::Obj),
            F::Text("\n"),
            F::Close(Op::Pre),
        ]));
        let b = t.open_block();
        let mut gs = crate::raster::GlyphSource::new_vendored(512);
        let sheet = crate::layout::daylight_sheet();
        let lb = crate::layout::layout_block(b, 400, &sheet, &mut gs);
        let segs: Vec<_> = lb.lines.iter().flat_map(|l| l.segs.iter()).collect();
        assert!(!segs.is_empty(), "the pre laid glyphs");
        assert!(
            segs.iter().all(|s| s.face == crate::raster::FACE_MONO),
            "every pre seg is mono (the annotation is overridden)"
        );
        assert!(
            lb.rects.iter().any(|r| r.color == sheet.island_ground),
            "the code-fence ground rect (the island ground, `.hal-out`)"
        );
        assert!(
            lb.rects.iter().any(|r| r.color == sheet.island_rule && r.w == 2),
            "the 2px leading gutter rule"
        );
    }

    #[test]
    fn pre_spanning_a_block_freeze_lays_out_without_panic() {
        // F1 (P0): a `pre` open when its block freezes must be finalized into
        // THAT block -- else its Item::Pre commits to the fresh block (0 styles)
        // carrying the old block's style indices, and layout_block's
        // `b.styles[sid]` panics on the stale index. Reachable from an untrusted
        // tile stream: a ScrollOff, or a tile-split's set_max_cost, between
        // pre-open and pre-close. Pre-fix this panics in layout_block.
        let mut t = Transcript::new(daylight());
        // Content before the pre gives the open block an item + cost to freeze on.
        t.feed(&frames(&[F::Text("before\n")]));
        // Open a pre + a styled line: the pre cell interns a style index into the
        // CURRENT open block; the pre line rides self.pre, not the block items.
        t.feed(&frames(&[
            F::Open(Op::Pre, &[]),
            F::Open(Op::Obj, &[("type", "path"), ("ref", "/bin")]),
            F::Text("/bin"),
            F::Close(Op::Obj),
            F::Text("\n"),
        ]));
        // Freeze the open block WHILE the pre is open (the tile-split trigger);
        // the fresh open block has zero styles.
        t.set_max_cost(1);
        t.feed(&frames(&[F::Close(Op::Pre)]));
        // Lay out every block -- pre-fix one panics on the stale style index.
        let mut gs = crate::raster::GlyphSource::new_vendored(512);
        let sheet = crate::layout::daylight_sheet();
        for b in t.frozen_blocks() {
            let _ = crate::layout::layout_block(b, 400, &sheet, &mut gs);
        }
        let _ = crate::layout::layout_block(t.open_block(), 400, &sheet, &mut gs);
        // No panic reached here; the pre was finalized into a block, not lost.
        let has_pre = t
            .frozen_blocks()
            .iter()
            .chain(core::iter::once(t.open_block()))
            .any(|b| b.items.iter().any(|it| matches!(it, Item::Pre(_))));
        assert!(has_pre, "the pre was finalized into a block, not lost");
    }

    #[test]
    fn pre_finalized_at_a_scrolloff_triggered_freeze_lays_out_without_panic() {
        // F1's OTHER trigger (the re-round's F6): a freeze mid-pre driven by a
        // ScrollOff (push_scrolled_rows), not a tile-split (set_max_cost). The
        // set_max_cost test above lowers the cap; here finalize_scroll_pending
        // pushes an Item::Line into the open block and, over the open cap,
        // freezes it WHILE a pre is open -- the identical fix arm, reached from
        // the second of the two uncovered callers. It also interleaves a scroll
        // Line before the finalized Item::Pre in one block. Both the scroll
        // line's and the pre's obj-styled cells interned their indices into THIS
        // block; the fix finalizes the pre into it, so layout_block's
        // b.styles[sid] stays in bounds. Pre-fix: the pre carries to a fresh
        // block and layout panics on the stale index.
        let mut t = Transcript::with_caps(daylight(), 1000, 256, 10_000); // open cap 32
        // A pre with an obj-styled cell: the index interns into the open block;
        // the pre line rides self.pre uncharged, so it never freezes its own
        // block (open.cost stays the obj's few bytes, under the cap).
        t.feed(&frames(&[
            F::Open(Op::Pre, &[]),
            F::Open(Op::Obj, &[("type", "path"), ("ref", "/bin")]),
            F::Text("/bin"),
            F::Close(Op::Obj),
            F::Text("\n"),
        ]));
        // One ScrollOff line lands in the open block and pushes it over the open
        // cap: finalize_scroll_pending -> enforce_block_cap -> freeze_open with
        // pre=Some. This is the caller the set_max_cost test does not exercise.
        t.push_scrolled_rows(&[wrow("a scrolled grid line over the tiny cap")], &[false], &SpanMap::new());
        // Close the pre after the freeze: with the fix it is a no-op (already
        // finalized); pre-fix the still-open pre finalizes into the FRESH block
        // (its stale indices name the frozen block), and layout OOB-panics below.
        t.feed(&frames(&[F::Close(Op::Pre)]));
        // Lay out every block -- pre-fix the pre's stale index panics here.
        let mut gs = crate::raster::GlyphSource::new_vendored(512);
        let sheet = crate::layout::daylight_sheet();
        for b in t.frozen_blocks() {
            let _ = crate::layout::layout_block(b, 400, &sheet, &mut gs);
        }
        let _ = crate::layout::layout_block(t.open_block(), 400, &sheet, &mut gs);
        // The scroll-triggered freeze finalized the pre into a block (not lost),
        // alongside the scroll Line that triggered it (the interleave).
        let has_pre = t
            .frozen_blocks()
            .iter()
            .chain(core::iter::once(t.open_block()))
            .any(|b| b.items.iter().any(|it| matches!(it, Item::Pre(_))));
        let has_scroll_line = t
            .frozen_blocks()
            .iter()
            .chain(core::iter::once(t.open_block()))
            .any(|b| b.items.iter().any(|it| matches!(it, Item::Line(_))));
        assert!(has_pre, "the pre was finalized by the scroll-triggered freeze");
        assert!(
            has_scroll_line,
            "the ScrollOff line that triggered the freeze is present"
        );
    }

    #[test]
    fn byte_by_byte_equals_whole() {
        let corpus = session_corpus();
        let mut whole = Transcript::new(daylight());
        whole.feed(&corpus);
        let mut split = Transcript::new(daylight());
        for &b in corpus.iter() {
            split.feed(&[b]);
        }
        assert_eq!(
            structure_fingerprint(&whole),
            structure_fingerprint(&split),
            "chunk boundaries are invisible (the streaming property)"
        );
    }

    #[test]
    fn line_discipline_overwrite_tab_bs_el() {
        let mut t = Transcript::new(daylight());
        t.feed(b"abc\rXY\n");
        t.feed(b"a\tb\n");
        t.feed(b"abcd\x08\x08Z\n");
        t.feed(b"wipe me\x1b[2Kk\n");
        let b = t.open_block();
        let l0 = match &b.items[0] {
            Item::Line(l) => line_str(l),
            _ => panic!(),
        };
        assert_eq!(l0, "XYc", "\\r overwrites in place");
        let l1 = match &b.items[1] {
            Item::Line(l) => line_str(l),
            _ => panic!(),
        };
        assert_eq!(l1, "a       b", "tab to the 8-col stop");
        let l2 = match &b.items[2] {
            Item::Line(l) => line_str(l),
            _ => panic!(),
        };
        assert_eq!(l2, "abZd", "backspace repositions, write overwrites");
        let l3 = match &b.items[3] {
            Item::Line(l) => line_str(l),
            _ => panic!(),
        };
        // EL never moves the cursor (VT semantics): the wipe cleared the
        // line, the cursor stayed at col 7, and `k` landed there.
        assert_eq!(l3, "       k", "EL2 wipes without moving the cursor");
    }

    #[test]
    fn fullscreen_intent_latches() {
        let mut t = Transcript::new(daylight());
        t.feed(b"hello\n");
        assert!(!t.raw_vt_intent);
        t.feed(b"\x1b[?1049h");
        assert!(t.raw_vt_intent, "alt-screen enter is the primary trigger");
        t.raw_vt_intent = false;
        t.feed(b"\x1b[5;10H");
        assert!(t.raw_vt_intent, "CUP is fullscreen intent");
        t.raw_vt_intent = false;
        t.feed(b"\x1b[31mstill styled\x1b[0m\n");
        assert!(!t.raw_vt_intent, "SGR alone never latches");
    }

    #[test]
    fn spans_die_at_block_edge_pen_survives() {
        let mut t = Transcript::new(daylight());
        let mut buf = Vec::new();
        wire::open(&mut buf, Op::Zone, &[("k", "output")]);
        wire::open(&mut buf, Op::Em, &[("class", "strong")]);
        buf.extend_from_slice(b"\x1b[31membolden");
        // The zone closes with the em still open + red still set.
        wire::close(&mut buf, Op::Zone);
        t.feed(&buf);
        t.feed(b"after\n");
        let pal = daylight();
        let open = t.open_block();
        let Item::Line(l) = &open.items[0] else {
            panic!()
        };
        let st = open.styles[l.cells[0].style as usize];
        assert_eq!(st.em, EM_NONE, "the em span died at the boundary");
        assert_eq!(
            st.fg, pal.ansi[1],
            "the SGR pen persisted (terminal semantics)"
        );
    }

    #[test]
    fn budget_evicts_oldest() {
        let mut t = Transcript::with_caps(daylight(), 3, usize::MAX, 100);
        for i in 0..6 {
            let mut buf = Vec::new();
            wire::open(&mut buf, Op::Zone, &[("k", "output")]);
            buf.extend_from_slice(format!("cmd {}\n", i).as_bytes());
            wire::close(&mut buf, Op::Zone);
            t.feed(&buf);
        }
        assert_eq!(t.frozen_blocks().len(), 3);
        let Item::Line(l) = &t.frozen_blocks()[0].items[0] else {
            panic!()
        };
        assert_eq!(line_str(l), "cmd 3", "the oldest blocks evicted");
    }

    #[test]
    fn line_cap_freezes_a_continuation() {
        let mut t = Transcript::with_caps(daylight(), 100, usize::MAX, 4);
        let mut buf = Vec::new();
        wire::open(&mut buf, Op::Zone, &[("k", "output")]);
        for i in 0..6 {
            buf.extend_from_slice(format!("l{}\n", i).as_bytes());
        }
        wire::close(&mut buf, Op::Zone);
        t.feed(&buf);
        let blocks = t.frozen_blocks();
        assert_eq!(blocks.len(), 2, "the cap split the monster block");
        assert_eq!(blocks[0].kind, BlockKind::Output);
        assert_eq!(blocks[1].kind, BlockKind::Output);
        assert!(
            blocks[1].continuation,
            "the second is marked a continuation"
        );
    }

    #[test]
    fn floating_exit_mark_attaches_backward() {
        let mut t = Transcript::new(daylight());
        let mut buf = Vec::new();
        wire::open(&mut buf, Op::Zone, &[("k", "output")]);
        buf.extend_from_slice(b"out\n");
        wire::close(&mut buf, Op::Zone);
        wire::point(&mut buf, Op::Mark, &[("k", "exit"), ("code", "7")]);
        t.feed(&buf);
        assert_eq!(
            t.frozen_blocks()[0].exit,
            Some(7),
            "the pre-deviation-8 floating order still lands"
        );
    }

    #[test]
    fn split_frame_and_split_utf8_survive_the_cut() {
        // An OSC 1936 frame + a two-byte char, each split mid-sequence
        // across feeds, must parse exactly as when whole.
        let corpus = session_corpus();
        let mut a = Transcript::new(daylight());
        a.feed(&corpus);
        let mut b = Transcript::new(daylight());
        let mid = corpus.len() / 3;
        let mid2 = 2 * corpus.len() / 3;
        b.feed(&corpus[..mid]);
        b.feed(&corpus[mid..mid2]);
        b.feed(&corpus[mid2..]);
        assert_eq!(structure_fingerprint(&a), structure_fingerprint(&b));
    }

    // --- the format-fuzz bounds (the H-2 audit F1..F5) ---------------------

    #[test]
    fn csi_param_overflow_does_not_panic() {
        // A ~10-digit CSI numeric parameter: `saturating_mul(10)` caps the
        // multiply, but a plain `+ digit` add overflows u32 -> panic under
        // the shipped overflow-checks profile -> the console dies (F1). One
        // untrusted escape sequence; must be absorbed.
        let mut t = Transcript::new(daylight());
        t.feed(b"\x1b[9999999999mX\x1b[0m\n");
        // The huge param is a no-op SGR; the text after it survives (the line
        // is flushed into the still-open block -- no zone boundary froze it).
        let Some(Item::Line(l)) = t.open_block().items.first() else {
            panic!("no line")
        };
        assert_eq!(line_str(l), "X");
    }

    #[test]
    fn unbounded_line_soft_wraps_and_stays_bounded() {
        // An endless no-newline stream must not grow one line unboundedly
        // (F3): the soft-wrap flushes it into MAX_LINE_CELLS-wide lines that
        // the block cap + budget then bound.
        let mut t = Transcript::new(daylight());
        let blob = vec![b'a'; MAX_LINE_CELLS * 4 + 17];
        t.feed(&blob);
        assert!(
            t.pending_line().len() <= MAX_LINE_CELLS + 1,
            "the open line is bounded"
        );
        for b in t.frozen_blocks().iter() {
            for it in b.items.iter() {
                if let Item::Line(l) = it {
                    assert!(
                        l.cells.len() <= MAX_LINE_CELLS + 1,
                        "each flushed line is bounded"
                    );
                }
            }
        }
    }

    // The H-3b round F1 (the H-2 F3 re-prosecution): the budget evicts only
    // FROZEN blocks, so an open block that soft-wraps forever must FREEZE on
    // bytes, not only on its 10 000-line count -- else 320 MiB accrue before
    // eviction can reach any of it. Small caps make the test cheap: the open
    // block must never exceed its byte cap, and the whole transcript must
    // stay within one open-cap of the budget.
    // PL-3: the soft-wrap rejoin. A row helper for these tests: chars -> a row
    // of unstyled vt::Cells (span 0 -> the default tag).
    fn wrow(s: &str) -> Vec<vt::Cell> {
        s.chars()
            .map(|ch| vt::Cell {
                ch,
                fg: 0xFFFFFF,
                bg: 0,
                attrs: 0,
                span: 0,
            })
            .collect()
    }

    #[test]
    fn soft_wrapped_scroll_rows_rejoin_into_one_logical_line() {
        // The grid broke "hello world" at col 5 into "hello"(wrapped) +
        // " worl"(wrapped) + "d"(not). The three ScrollOff rows rejoin into ONE
        // Line -- so the flow layout re-wraps at the space, not mid-word (s5) --
        // not three Lines.
        let mut t = Transcript::with_caps(daylight(), 1000, 1 << 20, 10_000);
        let rows = alloc::vec![wrow("hello"), wrow(" worl"), wrow("d")];
        t.push_scrolled_rows(&rows, &[true, true, false], &SpanMap::new());
        let items = &t.open_block().items;
        assert_eq!(items.len(), 1, "three soft-wrapped rows -> one Line");
        let Item::Line(l) = &items[0] else {
            panic!("a Line")
        };
        let s: String = l.cells.iter().map(|c| c.ch).collect();
        assert_eq!(s, "hello world");
    }

    #[test]
    fn a_hard_wrapped_batch_stays_one_line_per_row() {
        // The all-false case (a listing of distinct short lines) is unchanged:
        // one Line per row.
        let mut t = Transcript::with_caps(daylight(), 1000, 1 << 20, 10_000);
        let rows = alloc::vec![wrow("dev"), wrow("dl-symlink")];
        t.push_scrolled_rows(&rows, &[false, false], &SpanMap::new());
        assert_eq!(t.open_block().items.len(), 2, "two rows -> two Lines");
    }

    #[test]
    fn a_pending_soft_wrapped_line_spans_push_calls() {
        // The last row of a batch may soft-wrap (its continuation is still on
        // the live grid); the fragment carries to the next call and rejoins,
        // not finalizes early.
        let mut t = Transcript::with_caps(daylight(), 1000, 1 << 20, 10_000);
        t.push_scrolled_rows(&[wrow("abc")], &[true], &SpanMap::new());
        assert_eq!(
            t.open_block().items.len(),
            0,
            "nothing finalizes while a line is pending"
        );
        t.push_scrolled_rows(&[wrow("def")], &[false], &SpanMap::new());
        let items = &t.open_block().items;
        assert_eq!(items.len(), 1, "the completed line finalizes as one Line");
        let Item::Line(l) = &items[0] else {
            panic!("a Line")
        };
        let s: String = l.cells.iter().map(|c| c.ch).collect();
        assert_eq!(s, "abcdef");
    }

    #[test]
    fn flush_scroll_pending_finalizes_an_in_flight_fragment() {
        // A screen-mode change forces a held fragment out as its own Line.
        let mut t = Transcript::with_caps(daylight(), 1000, 1 << 20, 10_000);
        t.push_scrolled_rows(&[wrow("ab")], &[true], &SpanMap::new());
        assert_eq!(t.open_block().items.len(), 0);
        t.flush_scroll_pending(&SpanMap::new());
        assert_eq!(t.open_block().items.len(), 1, "the fragment flushed");
    }

    #[test]
    fn an_endless_soft_wrap_hard_splits_at_max_line_cells() {
        // A never-ending soft-wrapped line (every row wrapped) must not grow the
        // held fragment unbounded: it hard-splits at MAX_LINE_CELLS.
        let mut t = Transcript::with_caps(daylight(), 1000, 8 << 20, 10_000);
        let wide = wrow(&"a".repeat(256));
        let n = (MAX_LINE_CELLS / 256) + 4;
        let rows = alloc::vec![wide; n];
        let wrapped = alloc::vec![true; n]; // never ends
        t.push_scrolled_rows(&rows, &wrapped, &SpanMap::new());
        assert!(
            !t.open_block().items.is_empty(),
            "the endless line hard-split at least once"
        );
        for it in &t.open_block().items {
            if let Item::Line(l) = it {
                assert!(
                    l.cells.len() <= MAX_LINE_CELLS + 256,
                    "a finalized line stayed bounded, got {}",
                    l.cells.len()
                );
            }
        }
    }

    #[test]
    fn cells_mode_rebuilds_table_rule_and_pre_from_tagged_cells() {
        // KT-1 cells mode: the tile's transcript sees only FRAMES; the text is
        // grid cells tagged with the serial of the frame before them. The
        // structure the frames announced -- a table (its rows and cells), a
        // rule, a pre block -- must come back from the tags on BOTH paths:
        // the live grid (`live_block`) and the scroll-off ingest
        // (`push_scrolled_rows`). Pre-fix the transcript captured a text-less
        // table shell + an orphaned rule (phantom blank rows + a rule at the
        // top of the tile) and the grid laid the table's plain realization as
        // misaligned prose.
        let mut t = Transcript::with_caps(daylight(), 1000, 1 << 20, 10_000);
        t.set_cells_mode(true);
        let mut spans = SpanMap::new();
        let mut serial = 0u32;
        let mut frame = |t: &mut Transcript, spans: &mut SpanMap, bytes: Vec<u8>| -> u32 {
            serial += 1;
            t.feed_frame(&bytes, serial);
            spans.note(serial, t.span_tag());
            serial
        };
        let open = |op: Op, args: &[(&str, &str)]| {
            let mut b = Vec::new();
            wire::open(&mut b, op, args);
            b
        };
        let close = |op: Op| {
            let mut b = Vec::new();
            wire::close(&mut b, op);
            b
        };
        frame(&mut t, &mut spans, open(Op::Zone, &[("k", "output")]));
        let s_table = frame(&mut t, &mut spans, open(Op::Table, &[("cols", "lr"), ("hdr", "0")]));
        frame(&mut t, &mut spans, open(Op::Row, &[]));
        let s_k = frame(&mut t, &mut spans, open(Op::Cell, &[]));
        let s_pad1 = frame(&mut t, &mut spans, close(Op::Cell));
        let s_1 = frame(&mut t, &mut spans, open(Op::Cell, &[]));
        frame(&mut t, &mut spans, close(Op::Cell));
        frame(&mut t, &mut spans, close(Op::Row));
        frame(&mut t, &mut spans, open(Op::Row, &[]));
        let s_l = frame(&mut t, &mut spans, open(Op::Cell, &[]));
        let s_pad2 = frame(&mut t, &mut spans, close(Op::Cell));
        let s_3 = frame(&mut t, &mut spans, open(Op::Cell, &[]));
        frame(&mut t, &mut spans, close(Op::Cell));
        frame(&mut t, &mut spans, close(Op::Row));
        frame(&mut t, &mut spans, close(Op::Table));
        let s_rule = {
            let mut b = Vec::new();
            wire::point(&mut b, Op::Rule, &[]);
            frame(&mut t, &mut spans, b)
        };
        // The "after" line's cells are written here (the tile ends the rule
        // episode on them), before the pre opens.
        t.end_rule();
        let s_pre = frame(&mut t, &mut spans, open(Op::Pre, &[]));
        frame(&mut t, &mut spans, close(Op::Pre));
        assert!(
            t.open_block().items.is_empty(),
            "cells mode captures no text-less shells from the frames alone"
        );
        let gc = |ch: char, span: u32| vt::Cell {
            ch,
            fg: 0,
            bg: 0,
            attrs: 0,
            span,
        };
        let row = |parts: &[(&str, u32)]| -> Vec<vt::Cell> {
            let mut r: Vec<vt::Cell> = Vec::new();
            for (s, sp) in parts {
                r.extend(s.chars().map(|c| gc(c, *sp)));
            }
            while r.len() < 12 {
                r.push(gc(' ', 0)); // the grid's never-written tail
            }
            r
        };
        let rows = alloc::vec![
            row(&[("kernel", s_k), ("  ", s_pad1), ("1", s_1)]),
            row(&[("loom", s_l), ("  ", s_pad2), ("333", s_3)]),
            row(&[("after", s_rule)]),
            row(&[("mono", s_pre)]),
        ];
        let grid: Vec<vt::Cell> = rows.iter().flatten().copied().collect();
        // The live grid.
        let (b, prov) = t.live_block(&grid, 12, 4, &[false, false, false, false], &spans);
        assert_eq!(b.items.len(), 4, "table, rule, line, pre");
        let Item::Table(tm) = &b.items[0] else {
            panic!("item 0 is the rebuilt table");
        };
        assert_eq!(tm.cols, b"lr".to_vec());
        assert_eq!(tm.src, s_table, "the table joins on its open serial");
        assert_eq!(tm.rows.len(), 2);
        let text = |cells: &[TCell]| cells.iter().map(|c| c.ch).collect::<String>();
        assert_eq!(text(&tm.rows[0][0]), "kernel");
        assert_eq!(text(&tm.rows[0][1]), "1", "the padding between cells is dropped");
        assert_eq!(text(&tm.rows[1][0]), "loom");
        assert_eq!(text(&tm.rows[1][1]), "333");
        assert!(matches!(b.items[1], Item::Rule), "the rule precedes the first line after it");
        let Item::Line(l) = &b.items[2] else {
            panic!("item 2 is the line after the rule");
        };
        assert_eq!(text(&l.cells), "after", "the unused tail is trimmed");
        assert_eq!(l.class, LineClass::Doc, "a zone with structure is a document");
        let Item::Pre(pl) = &b.items[3] else {
            panic!("item 3 is the pre block");
        };
        assert_eq!(text(&pl[0].cells), "mono");
        assert_eq!(prov[0].0, 0, "row 0 maps to the table item");
        assert_eq!(prov[1].0, 0, "row 1 too (the same table)");
        assert_eq!((prov[0].1, prov[1].1), (0, 1), "each grid row names ITS table row");
        assert_eq!(prov[2].0, 2, "row 2 maps past the rule to its line");
        assert_eq!(prov[2].1, usize::MAX, "a plain line has no row");
        assert_eq!((prov[3].0, prov[3].1), (3, 0), "the pre's first line");
        assert_eq!(tm.starts, alloc::vec![alloc::vec![0, 8], alloc::vec![0, 6]], "each cell's grid start column");
        // The scroll-off path rebuilds the same structure into the open block,
        // one row at a time.
        t.push_scrolled_rows(&rows, &[false, false, false, false], &spans);
        let items = &t.open_block().items;
        assert_eq!(items.len(), 4, "scroll-off: table, rule, line, pre");
        let Item::Table(tm2) = &items[0] else {
            panic!("scroll-off item 0 is the table");
        };
        assert_eq!(tm2.rows.len(), 2, "both rows joined one table");
        assert!(matches!(items[1], Item::Rule));
        assert!(matches!(&items[3], Item::Pre(p) if p.len() == 1));
        // The rule is emitted ONCE per rule frame across both paths' lines.
        t.push_scrolled_rows(&[row(&[("more", s_rule)])], &[false], &spans);
        let n_rules = t.open_block().items.iter().filter(|i| matches!(i, Item::Rule)).count();
        assert_eq!(n_rules, 1, "a second line after the same rule adds no rule");
        // A rule directly followed by an inline OPEN (no cells between): the
        // rule rides that open's tag -- its cells ARE the line after the rule.
        // The episode ends when the tile reports cells written after the
        // rule frame (`end_rule`), not at an op: the close noted BEFORE any
        // cells still carries it, the one noted after does not.
        let s_rule2 = {
            let mut b = Vec::new();
            wire::point(&mut b, Op::Rule, &[]);
            frame(&mut t, &mut spans, b)
        };
        assert_eq!(t.rule_open(), Some(s_rule2));
        let s_dim = frame(&mut t, &mut spans, open(Op::Em, &[("class", "dim")]));
        assert!(spans.get(s_dim).unwrap().em & TAG_RULE != 0, "the open after a rule carries it");
        t.end_rule();
        let s_after = frame(&mut t, &mut spans, close(Op::Em));
        assert!(spans.get(s_after).unwrap().em & TAG_RULE == 0, "cells written end the episode");
        assert_eq!(t.rule_open(), None);
        t.push_scrolled_rows(&[row(&[("lineage", s_dim)])], &[false], &spans);
        let n_rules = t.open_block().items.iter().filter(|i| matches!(i, Item::Rule)).count();
        assert_eq!(n_rules, 2, "the rule before the dim line landed via the open's tag");
        // Audit F3: a rule, a TEXT line (its cells carry the rule serial),
        // then an em OPEN starting the next line -- the text line ended the
        // episode, so the open's tag carries no rule and one frame places
        // ONE rule (pre-fix the open still carried it: two rules).
        let s_rule3 = {
            let mut b = Vec::new();
            wire::point(&mut b, Op::Rule, &[]);
            frame(&mut t, &mut spans, b)
        };
        t.end_rule();
        let s_em3 = frame(&mut t, &mut spans, open(Op::Em, &[("class", "dim")]));
        assert!(spans.get(s_em3).unwrap().em & TAG_RULE == 0);
        t.push_scrolled_rows(&[row(&[("abc", s_rule3)]), row(&[("xyz", s_em3)])], &[false, false], &spans);
        let n_rules = t.open_block().items.iter().filter(|i| matches!(i, Item::Rule)).count();
        assert_eq!(n_rules, 3, "one rule frame, one rule");
        frame(&mut t, &mut spans, close(Op::Em));
        // Audit F8: a rule the frames close over (`rule` then `/zone`) is
        // not lost -- the episode survives the close and the next zone's
        // first line (the prompt) carries it.
        let s_rule4 = {
            let mut b = Vec::new();
            wire::point(&mut b, Op::Rule, &[]);
            frame(&mut t, &mut spans, b)
        };
        frame(&mut t, &mut spans, close(Op::Zone));
        assert_eq!(t.rule_open(), Some(s_rule4), "a close does not end the episode");
        // A prompt zone's cells carry TAG_PROMPT and class as the prompt even
        // though the zone-less block before it was dropped (its id is never
        // reused).
        let s_prompt = frame(&mut t, &mut spans, open(Op::Zone, &[("k", "prompt")]));
        assert!(spans.get(s_prompt).unwrap().em & TAG_RULE != 0, "the trailing rule rides into the next zone");
        assert!(spans.get(s_prompt).unwrap().em & TAG_PROMPT != 0);
        let grid2 = row(&[("~ > ", s_prompt)]);
        let (lb, _) = t.live_block(&grid2, 12, 1, &[false], &spans);
        assert!(matches!(lb.items[0], Item::Rule), "the rule precedes the prompt line");
        let Item::Line(pl) = &lb.items[1] else {
            panic!("the prompt line");
        };
        assert_eq!(pl.class, LineClass::Prompt);
    }

    #[test]
    fn cells_mode_rebuilt_body_rows_without_the_header_row_are_not_a_header() {
        // Audit F4: a headed table whose header row scrolled off earlier --
        // the live grid holds only BODY rows. The rebuilt table's header
        // flag comes from the row tag (TAG_ROW_HDR), never from the spec
        // alone, so the first body row is not bolded + ruled as a header.
        let mut t = Transcript::with_caps(daylight(), 1000, 1 << 20, 10_000);
        t.set_cells_mode(true);
        let mut spans = SpanMap::new();
        let mut s = 0u32;
        let mut f = |t: &mut Transcript, spans: &mut SpanMap, bytes: Vec<u8>| -> u32 {
            s += 1;
            t.feed_frame(&bytes, s);
            spans.note(s, t.span_tag());
            s
        };
        let open = |op: Op, args: &[(&str, &str)]| {
            let mut b = Vec::new();
            wire::open(&mut b, op, args);
            b
        };
        let close = |op: Op| {
            let mut b = Vec::new();
            wire::close(&mut b, op);
            b
        };
        f(&mut t, &mut spans, open(Op::Table, &[("cols", "lr"), ("hdr", "1")]));
        f(&mut t, &mut spans, open(Op::Row, &[]));
        let s_h = f(&mut t, &mut spans, open(Op::Cell, &[]));
        f(&mut t, &mut spans, close(Op::Cell));
        f(&mut t, &mut spans, close(Op::Row));
        f(&mut t, &mut spans, open(Op::Row, &[]));
        let s_b = f(&mut t, &mut spans, open(Op::Cell, &[]));
        f(&mut t, &mut spans, close(Op::Cell));
        f(&mut t, &mut spans, close(Op::Row));
        f(&mut t, &mut spans, close(Op::Table));
        assert!(spans.get(s_h).unwrap().em & TAG_ROW_HDR != 0);
        assert!(spans.get(s_b).unwrap().em & TAG_ROW_HDR == 0);
        let gc = |ch: char, span: u32| vt::Cell { ch, fg: 0, bg: 0, attrs: 0, span };
        let body: Vec<vt::Cell> = "body".chars().map(|c| gc(c, s_b)).collect();
        let (b, _) = t.live_block(&body, 4, 1, &[false], &spans);
        let Item::Table(tm) = &b.items[0] else {
            panic!("a table")
        };
        assert!(!tm.hdr, "only body rows: no header row");
        // With the header row on the grid the table IS headed.
        let mut both: Vec<vt::Cell> = "hdr ".chars().map(|c| gc(c, s_h)).collect();
        both.extend("body".chars().map(|c| gc(c, s_b)));
        let (b, _) = t.live_block(&both, 4, 2, &[false, false], &spans);
        let Item::Table(tm) = &b.items[0] else {
            panic!("a table")
        };
        assert!(tm.hdr, "the header row on the grid heads the table");
        assert_eq!(tm.rows.len(), 2);
    }

    #[test]
    fn a_dropped_zone_less_block_never_lends_its_id_to_the_next_zone() {
        // Audit F5: a zone-less block holding only `em` frames (no obj, no
        // items) is dropped at the zone cut. Its cells on the grid carry tags
        // naming its id; had the next zone reused that id, its raw lines
        // would inherit the dim line's annotation and class as a document.
        let mut t = Transcript::with_caps(daylight(), 1000, 1 << 20, 10_000);
        t.set_cells_mode(true);
        let mut spans = SpanMap::new();
        let mut buf = Vec::new();
        wire::open(&mut buf, Op::Em, &[("class", "dim")]);
        t.feed_frame(&buf, 1);
        spans.note(1, t.span_tag());
        let dropped_id = spans.get(1).unwrap().block;
        buf.clear();
        wire::close(&mut buf, Op::Em);
        t.feed_frame(&buf, 2);
        spans.note(2, t.span_tag());
        buf.clear();
        wire::open(&mut buf, Op::Zone, &[("k", "output")]);
        t.feed_frame(&buf, 3);
        spans.note(3, t.span_tag());
        let zone_id = spans.get(3).unwrap().block;
        assert_ne!(zone_id, dropped_id, "the dropped block's id is not recycled");
        assert!(t.block_by_id(dropped_id).is_none(), "the dropped block is gone");
        let gc = |ch: char, span: u32| vt::Cell { ch, fg: 0, bg: 0, attrs: 0, span };
        let mut grid: Vec<vt::Cell> = "dim".chars().map(|c| gc(c, 1)).collect();
        grid.push(gc(' ', 0));
        grid.extend("raw!".chars().map(|c| gc(c, 3)));
        let (b, _) = t.live_block(&grid, 4, 2, &[false, false], &spans);
        let Item::Line(l0) = &b.items[0] else {
            panic!("the dim line")
        };
        let Item::Line(l1) = &b.items[1] else {
            panic!("the raw line")
        };
        assert_eq!(l0.class, LineClass::Doc, "the annotated zone-less line is a document");
        assert_eq!(l1.class, LineClass::Raw, "the output zone's plain line stays raw");
    }

    #[test]
    fn cells_mode_title_tag_survives_to_the_laid_herald() {
        // The herald through the TILE path: `hdr class=title` arrives as a
        // frame, its text as tagged cells; the title bit must survive the
        // tag packing and centre the laid line (the screendump round found
        // the title left-aligned after the bit moved to 0x04).
        let mut t = Transcript::with_caps(daylight(), 1000, 1 << 20, 10_000);
        t.set_cells_mode(true);
        let mut spans = SpanMap::new();
        let mut b = Vec::new();
        wire::open(&mut b, Op::Hdr, &[("level", "1"), ("class", "title")]);
        t.feed_frame(&b, 1);
        spans.note(1, t.span_tag());
        assert_eq!(t.span_tag().hdr & HDR_MASK, 1 | HDR_TITLE, "the tag carries level 1 + title");
        let mut c = Vec::new();
        wire::close(&mut c, Op::Hdr);
        t.feed_frame(&c, 2);
        spans.note(2, t.span_tag());
        let mut d = Vec::new();
        wire::open(&mut d, Op::Em, &[("class", "dim")]);
        t.feed_frame(&d, 3);
        spans.note(3, t.span_tag());
        let gc = |ch: char, span: u32| vt::Cell { ch, fg: 0, bg: 0, attrs: 0, span };
        let mut grid: Vec<vt::Cell> = "Title".chars().map(|ch| gc(ch, 1)).collect();
        while grid.len() < 20 {
            grid.push(gc(' ', 0));
        }
        grid.extend("deck".chars().map(|ch| gc(ch, 3)));
        while grid.len() < 40 {
            grid.push(gc(' ', 0));
        }
        let (blk, _) = t.live_block(&grid, 20, 2, &[false, false], &spans);
        let Item::Line(l) = &blk.items[0] else {
            panic!("the title line");
        };
        let st = blk.styles[l.cells[0].style as usize];
        assert!(hdr_is_title(st.hdr) && hdr_level(st.hdr) == 1, "hdr byte {:#x}", st.hdr);
        assert_eq!(l.cells.len(), 5, "the unused tail is trimmed");
        let sheet = crate::layout::daylight_sheet();
        let mut gs = crate::raster::GlyphSource::new_vendored(512);
        let laid = crate::layout::layout_block(&blk, 600, &sheet, &mut gs);
        let x0 = laid.lines[0].segs[0].x;
        assert!(x0 > sheet.pad_x + 100, "the title is centred (x0 = {x0})");
        let x1 = laid.lines[1].segs[0].x;
        assert!(x1 > sheet.pad_x + 100, "the deck is centred (x1 = {x1})");
    }

    #[test]
    fn live_block_joins_soft_wrapped_grid_rows() {
        // PL-4b: "abc"(wrapped) + "def"(not) on a 3x2 grid -> ONE logical line
        // "abcdef"; the provenance maps grid row 1 to column 3 of line 0, the
        // key the caret + live selection need.
        let t = Transcript::with_caps(daylight(), 1000, 1 << 20, 10_000);
        let grid = wrow("abcdef");
        let (b, prov) = t.live_block(&grid, 3, 2, &[true, false], &SpanMap::new());
        assert_eq!(b.items.len(), 1, "one logical line");
        let Item::Line(l) = &b.items[0] else {
            panic!("a Line")
        };
        let s: String = l.cells.iter().map(|c| c.ch).collect();
        assert_eq!(s, "abcdef");
        assert_eq!(prov, alloc::vec![(0, usize::MAX, 0), (0, usize::MAX, 3)]);
    }

    #[test]
    fn live_block_unwrapped_rows_are_separate_lines() {
        // Two hard-terminated rows -> two logical lines; provenance keeps them
        // apart (each starts at column 0 of its own line).
        let t = Transcript::with_caps(daylight(), 1000, 1 << 20, 10_000);
        let grid = wrow("abcdef");
        let (b, prov) = t.live_block(&grid, 3, 2, &[false, false], &SpanMap::new());
        assert_eq!(b.items.len(), 2, "two logical lines");
        assert_eq!(prov, alloc::vec![(0, usize::MAX, 0), (1, usize::MAX, 0)]);
    }

    #[test]
    fn a_re_budget_residue_is_bounded_by_the_constant_open_cap() {
        // Round-3 F5: the eviction floor keeps the newest frozen block, sized
        // by the open cap in force when it froze -- at a 32 MiB share that
        // was 4 MiB, and across N re-budgeted tiles it summed to 4 MiB x H(N)
        // over the budget. With a CONSTANT open cap the residue is <= that
        // constant (+ the styles charged at freeze) whatever the old share.
        let share = 32 << 20;
        let mut t = Transcript::with_caps(daylight(), 1000, share, 10_000);
        let row: Vec<vt::Cell> = (0..128)
            .map(|_| vt::Cell {
                ch: 'y',
                fg: 0xFFFFFF,
                bg: 0,
                attrs: 0,
                span: 0,
            })
            .collect();
        let rows: Vec<Vec<vt::Cell>> = alloc::vec![row; 64];
        // several cap-sized continuation blocks
        while t.frozen_blocks().len() < 6 {
            t.push_scrolled_rows(&rows, &alloc::vec![false; rows.len()], &SpanMap::new());
        }
        let last = t.frozen_blocks().back().map_or(0, |b| b.cost);
        assert!(
            last <= OPEN_BLOCK_MAX_COST + 128 * 1024 + 64 * 1024,
            "a frozen block is bounded by the constant cap (+ one row + styles), got {last}"
        );
        let small = 1 << 20;
        t.set_max_cost(small);
        assert!(
            t.stored_cost()
                <= small + OPEN_BLOCK_MAX_COST + 128 * 1024 + 64 * 1024 + open_cap(small),
            "residue {} exceeds the new share {} plus the constant cap",
            t.stored_cost(),
            small
        );
    }

    #[test]
    fn set_max_cost_evicts_a_quiet_transcript_at_once() {
        // B2-F2: lowering the share of a tile that receives no more output
        // must shrink its retained set NOW -- nothing else will ever push.
        let big = 1 << 20;
        let mut t = Transcript::with_caps(daylight(), 1000, big, 10_000);
        let row: Vec<vt::Cell> = (0..128)
            .map(|_| vt::Cell {
                ch: 'x',
                fg: 0xFFFFFF,
                bg: 0,
                attrs: 0,
                span: 0,
            })
            .collect();
        let rows: Vec<Vec<vt::Cell>> = alloc::vec![row; 64];
        while t.stored_cost() < big - (big / 8) {
            t.push_scrolled_rows(&rows, &alloc::vec![false; rows.len()], &SpanMap::new());
        }
        let before = t.stored_cost();
        assert!(
            before > big / 2,
            "the fill reached the old share ({before})"
        );
        assert!(
            t.frozen_blocks().len() > 1,
            "several frozen blocks to evict"
        );

        let small = big / 4;
        t.set_max_cost(small);
        // At most the new cap plus one un-evictable block (the loop keeps the
        // newest frozen block) plus the new open cap.
        let slack = t.frozen_blocks().back().map_or(0, |b| b.cost) + open_cap(small);
        assert!(
            t.stored_cost() <= small + slack,
            "stored_cost {} still above the new share {} (+{} slack) after set_max_cost",
            t.stored_cost(),
            small,
            slack
        );
        assert!(t.stored_cost() < before, "the re-budget evicted something");
    }

    #[test]
    fn open_block_freezes_on_bytes_so_the_budget_can_evict_it() {
        // 1 MiB budget -> a 128 KiB open cap (4 soft-wrapped lines per block);
        // 4 MiB of newline-free bytes = 32 blocks' worth, of which the budget
        // retains ~7. Without the byte cap the single open block would hold
        // all 4 MiB (128 lines, far below the 10 000-line cap).
        let max_cost = 1 << 20;
        let mut t = Transcript::with_caps(daylight(), 1000, max_cost, 10_000);
        let open_cap = max_cost / 8;
        let line_bytes = MAX_LINE_CELLS * core::mem::size_of::<TCell>();
        let blob = vec![b'z'; 4 << 20];
        for chunk in blob.chunks(4096) {
            t.feed(chunk);
            assert!(
                t.open_block().cost < open_cap + line_bytes,
                "the open block crossed its byte cap without freezing ({})",
                t.open_block().cost
            );
            assert!(
                t.stored_cost <= max_cost + open_cap + line_bytes,
                "stored_cost {} escaped the budget {}",
                t.stored_cost,
                max_cost
            );
        }
        assert!(
            t.frozen_blocks().len() > 1,
            "the retained set holds several frozen blocks (frozen {} stored_cost {})",
            t.frozen_blocks().len(),
            t.stored_cost
        );
        assert!(
            t.frozen_blocks()
                .iter()
                .all(|b| b.cost <= open_cap + line_bytes),
            "every frozen block is bounded by the open cap"
        );
        assert!(
            t.frozen_blocks().iter().filter(|b| b.continuation).count() >= 1,
            "the byte cap froze the stream as continuation blocks"
        );
    }

    #[test]
    fn the_cmd_mark_charges_the_shared_byte_budget_symmetrically() {
        // F1: `mark k=cmd` bumped the output block's `cost` but NOT
        // `stored_cost`. Eviction does `stored_cost -= dead.cost`, so each
        // evicted cmd-marked block subtracted a charge that was never added,
        // drifting the byte budget toward zero until max_cost stopped
        // enforcing (the sibling obj/cell/table/style comment names exactly
        // this hazard). The invariant every content site upholds:
        // stored_cost == the sum of every live block's cost.
        let mut t = Transcript::new(daylight());
        t.feed(&frames(&[
            F::Open(Op::Zone, &[("k", "prompt")]),
            F::Text("$ "),
            F::Close(Op::Zone),
            F::Open(Op::Zone, &[("k", "output")]),
            F::Point(Op::Mark, &[("k", "cmd"), ("text", "make -j8 all")]),
            F::Text("building\n"),
        ]));
        let live: usize =
            t.frozen_blocks().iter().map(|b| b.cost).sum::<usize>() + t.open_block().cost;
        assert_eq!(
            t.stored_cost, live,
            "stored_cost {} != the sum of live block costs {}: the cmd mark's \
             t.len() must charge stored_cost too (else the byte budget drifts)",
            t.stored_cost, live
        );
        assert_eq!(t.last_command(), Some("make -j8 all"));

        // And the drift is fatal at scale: many cmd-marked blocks under a
        // tight budget must keep stored_cost tracking the retained set (never
        // saturating to zero, which would disable max_cost enforcement).
        let mut u = Transcript::with_caps(daylight(), 1000, 1 << 16, 10_000);
        for i in 0..400 {
            let cmd = format!("command-number-{}-with-some-length-to-charge", i);
            u.feed(&frames(&[
                F::Open(Op::Zone, &[("k", "output")]),
                F::Point(Op::Mark, &[("k", "cmd"), ("text", cmd.as_str())]),
                F::Text("out\n"),
                F::Close(Op::Zone),
            ]));
        }
        let live2: usize =
            u.frozen_blocks().iter().map(|b| b.cost).sum::<usize>() + u.open_block().cost;
        assert_eq!(
            u.stored_cost, live2,
            "after eviction stored_cost {} drifted from the retained cost {}",
            u.stored_cost, live2
        );
    }

    #[test]
    fn balanced_obj_frames_stay_bounded() {
        // open Obj / close Obj repeated grows `open.objs` (close only pops the
        // stack) -- capped at MAX_OBJS_PER_BLOCK, degrading to no-obj (F3),
        // which also keeps the idx+1 encoding inside u16 (P3).
        let mut t = Transcript::new(daylight());
        let mut buf = Vec::new();
        for _ in 0..(MAX_OBJS_PER_BLOCK + 500) {
            wire::open(&mut buf, Op::Obj, &[("type", "path"), ("ref", "/x")]);
            wire::close(&mut buf, Op::Obj);
        }
        t.feed(&buf);
        assert!(t.open_block().objs.len() <= MAX_OBJS_PER_BLOCK);
        let (_, obj_depth) = t.nest_depths();
        assert!(obj_depth <= MAX_SPAN_NEST);
    }

    #[test]
    fn distinct_style_spam_stays_bounded() {
        // A truecolor gradient (2^24 distinct fg) with a distinct style per
        // char grows `open.styles` unboundedly and turns style_idx O(n^2)
        // (F3) -- capped at MAX_STYLES_PER_BLOCK.
        let mut t = Transcript::new(daylight());
        let mut buf = Vec::new();
        for i in 0..(MAX_STYLES_PER_BLOCK + 500) {
            let (r, g, b) = (
                (i & 0xff) as u32,
                ((i >> 8) & 0xff) as u32,
                ((i >> 4) & 0xff) as u32,
            );
            buf.extend_from_slice(format!("\x1b[38;2;{};{};{}mZ", r, g, b).as_bytes());
        }
        t.feed(&buf);
        assert!(t.open_block().styles.len() <= MAX_STYLES_PER_BLOCK);
    }

    #[test]
    fn unbounded_table_rows_stay_bounded() {
        // An unclosed table with endless empty rows grows the Vec-of-Vecs
        // (F3) -- rows cap at MAX_TABLE_ROWS; the realized model proves it.
        let mut t = Transcript::new(daylight());
        let mut buf = Vec::new();
        wire::open(&mut buf, Op::Table, &[("cols", "l"), ("hdr", "0")]);
        for _ in 0..(MAX_TABLE_ROWS + 200) {
            wire::open(&mut buf, Op::Row, &[]);
            wire::open(&mut buf, Op::Cell, &[]);
            buf.extend_from_slice(b"c");
            wire::close(&mut buf, Op::Cell);
            wire::close(&mut buf, Op::Row);
        }
        wire::close(&mut buf, Op::Table);
        wire::open(&mut buf, Op::Zone, &[("k", "prompt")]); // force the table's block to freeze
        t.feed(&buf);
        let mut saw = false;
        for b in t.frozen_blocks().iter() {
            for it in b.items.iter() {
                if let Item::Table(tb) = it {
                    saw = true;
                    assert!(tb.rows.len() <= MAX_TABLE_ROWS + 1, "table rows bounded");
                }
            }
        }
        assert!(saw, "the table realized");
    }

    #[test]
    fn deep_nesting_across_feeds_stays_bounded() {
        // The wire depth cap resets per feed() (F4): unbalanced opens paced
        // across drains would grow the nesting stacks without bound. The
        // transcript-side cap holds regardless of chunking.
        let mut t = Transcript::new(daylight());
        for _ in 0..64 {
            let mut buf = Vec::new();
            for _ in 0..8 {
                wire::open(&mut buf, Op::Em, &[("class", "strong")]);
            }
            t.feed(&buf); // 8 opens per feed, never closed
        }
        let (em_depth, _) = t.nest_depths();
        assert!(
            em_depth <= MAX_SPAN_NEST,
            "em nesting bounded across feeds: {}",
            em_depth
        );
    }
}
