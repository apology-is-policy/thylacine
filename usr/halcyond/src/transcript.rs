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

/// One resolved cell style. `obj` is 0 = none, else index+1 into the
/// block's obj table; `em` is an EM_* class; `hdr` a heading level (0-3).
#[derive(Clone, Copy, PartialEq)]
pub struct Style {
    pub fg: u32,
    pub bg: u32,
    pub attrs: u8,
    pub em: u8,
    pub obj: u16,
    pub hdr: u8,
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
}

/// A captured Beacon table: `cols` holds the alignment spec bytes
/// (b'l'/b'r'/b'c'), `rows` -> cells -> styled content. Inter-cell padding
/// (the plain realization) is dropped -- rich layout re-derives geometry.
pub struct TableModel {
    pub cols: Vec<u8>,
    pub hdr: bool,
    pub rows: Vec<Vec<Vec<TCell>>>,
}

pub enum Item {
    Line(Line),
    Table(TableModel),
    Rule,
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
const MAX_TABLE_BYTES: usize = 16 << 20; // total in-progress table memory (content + Vec overhead)

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

/// The transcript: feed bytes in, read frozen blocks + the open tail out.
pub struct Transcript {
    frozen: VecDeque<Block>,
    open: Block,
    /// The line being built (column-addressed; the line discipline).
    line: Vec<TCell>,
    col: usize,
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
            state: ScanState::Ground,
            cwd: String::new(),
            osc_buf: Vec::new(),
            osc_over: false,
            params: [0; MAX_PARAMS],
            nparams: 0,
            cur_param: 0,
            csi_private: false,
            carry: Vec::new(),
            utf8: [0; 4],
            utf8_len: 0,
            utf8_need: 0,
            raw_vt_intent: false,
            next_id: 1,
            stored_cost: 0,
            max_blocks,
            max_cost,
            max_lines_per_block: max_lines.max(1),
            max_open_cost: (max_cost / 8).max(1),
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
                self.hdr = level;
            }
            Op::Mark | Op::Rule => {} // point ops; a paired open is malformed -- ignore
        }
    }

    fn close_op(&mut self, op: Op) {
        match op {
            Op::Zone => {
                self.freeze_open(BlockKind::Foreign, false);
            }
            Op::Table => {
                if let Some(mut t) = self.table.take() {
                    // Tolerate unclosed row/cell at table close (the final
                    // row/cell -- bounded, one each -- still honors the caps).
                    if t.in_cell {
                        Self::table_push_cell(&mut t);
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
                    self.open.items.push(Item::Table(TableModel {
                        cols: t.cols,
                        hdr: t.hdr,
                        rows: t.rows,
                    }));
                    self.enforce_block_cap();
                }
            }
            Op::Row => {
                if let Some(t) = self.table.as_mut() {
                    if t.in_cell {
                        Self::table_push_cell(t);
                        t.in_cell = false;
                    }
                    if t.in_row {
                        // At the row cap or byte budget, drop the row (never
                        // grow the Vec-of-Vecs unboundedly on an unclosed
                        // table); else charge its overhead + keep it.
                        if t.rows.len() < MAX_TABLE_ROWS && t.bytes < MAX_TABLE_BYTES {
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
                        Self::table_push_cell(t);
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
    fn table_push_cell(t: &mut TableCap) {
        if t.row.len() < MAX_TABLE_COLS && t.bytes < MAX_TABLE_BYTES {
            t.bytes = t.bytes.saturating_add(core::mem::size_of::<Vec<TCell>>());
            t.row.push(core::mem::take(&mut t.cell));
        } else {
            t.cell.clear();
        }
    }

    fn point_op(&mut self, op: Op, args: &[wire::Arg]) {
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
        let keep = self.open.has_content() || self.open.kind != BlockKind::Foreign;
        let id = self.next_id;
        self.next_id += 1;
        let mut b = core::mem::replace(&mut self.open, Block::new(id, next));
        self.open.continuation = continuation;
        if keep {
            b.cost += b.styles.len() * core::mem::size_of::<Style>();
            self.stored_cost += b.styles.len() * core::mem::size_of::<Style>();
            self.frozen.push_back(b);
            self.enforce_budget();
        } else {
            self.next_id -= 1;
            self.open.id = id - 1;
            // (an empty Foreign block leaves no trace and its id is reused
            // for the next open -- ids stay dense and monotonic)
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
        let cost = cells.len() * core::mem::size_of::<TCell>() + ITEM_OVERHEAD;
        self.open.cost += cost;
        self.stored_cost += cost;
        self.open.items.push(Item::Line(Line { cells }));
        self.col = 0;
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
        if let Some(t) = self.table.as_mut() {
            // Inside a table: cell content appends (no column discipline);
            // padding between cells is the plain realization -- dropped. The
            // per-cell char cap AND the whole-table byte budget both bound it.
            if t.in_cell && t.cell.len() < MAX_CELL_CHARS && t.bytes < MAX_TABLE_BYTES {
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
            // A blank line is content: keep it as an empty Line item -- and
            // charge it: a million empty lines is a million items.
            let cost = ITEM_OVERHEAD;
            self.open.cost += cost;
            self.stored_cost += cost;
            self.open.items.push(Item::Line(Line { cells: Vec::new() }));
            self.col = 0;
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
    pub fn push_scrolled_rows(&mut self, rows: &[Vec<vt::Cell>]) {
        for row in rows {
            let mut cells: Vec<TCell> = Vec::with_capacity(row.len());
            for c in row {
                let style = self.intern_style(Style {
                    fg: c.fg,
                    bg: c.bg,
                    attrs: c.attrs,
                    em: EM_NONE,
                    obj: 0,
                    hdr: 0,
                });
                cells.push(TCell { ch: c.ch, style });
            }
            let cost = cells.len() * core::mem::size_of::<TCell>() + ITEM_OVERHEAD;
            self.open.cost += cost;
            self.stored_cost += cost;
            self.open.items.push(Item::Line(Line { cells }));
            self.enforce_block_cap();
        }
    }

    /// Re-budget a live transcript (a session shares one scrollback budget
    /// across its tiles, so each tile's share moves as tiles come and go).
    /// Enforced lazily at the next push, like the constructor's value.
    pub fn set_max_cost(&mut self, max_cost: usize) {
        self.max_cost = max_cost;
        self.max_open_cost = (max_cost / 8).max(1);
    }
}

// --- the chunk-boundary holdback -------------------------------------------

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
            line_str(&Line {
                cells: t.pending_line().to_vec()
            }),
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
                }
            }
            s.push('|');
        }
        s.push_str(&format!(
            "open[{:?} items={}]",
            t.open_block().kind,
            t.open_block().items.len()
        ));
        s.push_str(&line_str(&Line {
            cells: t.pending_line().to_vec(),
        }));
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
            line_str(&Line {
                cells: t.pending_line().to_vec()
            }),
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
