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

enum ScanState {
    Ground,
    Esc,
    EscCharset,
    Csi,
    Osc,
    OscEsc,
}

const MAX_PARAMS: usize = 16;

struct TableCap {
    cols: Vec<u8>,
    hdr: bool,
    rows: Vec<Vec<Vec<TCell>>>,
    row: Vec<Vec<TCell>>,
    cell: Vec<TCell>,
    in_row: bool,
    in_cell: bool,
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
    /// Bumps on every structural change (a consumer's cheap dirty check).
    pub seq: u64,
}

/// Default caps: sized against the 13.3 budget (a content budget, not a
/// pixel budget; the layout cache is bounded separately).
pub const DEFAULT_MAX_BLOCKS: usize = 1000;
pub const DEFAULT_MAX_COST: usize = 32 << 20;
pub const DEFAULT_MAX_LINES_PER_BLOCK: usize = 10_000;

impl Transcript {
    pub fn new(pal: Palette) -> Transcript {
        Transcript::with_caps(pal, DEFAULT_MAX_BLOCKS, DEFAULT_MAX_COST, DEFAULT_MAX_LINES_PER_BLOCK)
    }

    pub fn with_caps(pal: Palette, max_blocks: usize, max_cost: usize, max_lines: usize) -> Transcript {
        Transcript {
            frozen: VecDeque::new(),
            open: Block::new(0, BlockKind::Foreign),
            line: Vec::new(),
            col: 0,
            pen: SgrPen::new(&pal),
            pal,
            em_stack: Vec::new(),
            obj_stack: Vec::new(),
            hdr: 0,
            table: None,
            state: ScanState::Ground,
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

    pub fn pending_col(&self) -> usize {
        self.col
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
                self.em_stack.push(class);
            }
            Op::Obj => {
                let ty = Self::arg(args, "type").unwrap_or("");
                let refv = Self::arg(args, "ref").unwrap_or("");
                let mut sty = String::new();
                sty.push_str(ty);
                let mut srf = String::new();
                srf.push_str(refv);
                self.open.cost += sty.len() + srf.len();
                self.open.objs.push(Obj { ty: sty, refv: srf });
                let idx = self.open.objs.len() as u16; // idx+1 encoding
                self.obj_stack.push(idx);
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
                    // Tolerate unclosed row/cell at table close.
                    if t.in_cell {
                        t.row.push(core::mem::take(&mut t.cell));
                    }
                    if t.in_row {
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
                        t.row.push(core::mem::take(&mut t.cell));
                        t.in_cell = false;
                    }
                    if t.in_row {
                        t.rows.push(core::mem::take(&mut t.row));
                        t.in_row = false;
                    }
                }
            }
            Op::Cell => {
                if let Some(t) = self.table.as_mut() {
                    if t.in_cell {
                        t.row.push(core::mem::take(&mut t.cell));
                        t.in_cell = false;
                    }
                }
            }
            Op::Em => {
                self.em_stack.pop();
            }
            Op::Obj => {
                self.obj_stack.pop();
            }
            Op::Hdr => {
                self.hdr = 0;
            }
            Op::Mark | Op::Rule => {}
        }
    }

    fn point_op(&mut self, op: Op, args: &[wire::Arg]) {
        match op {
            Op::Mark => {
                if Self::arg(args, "k") == Some("exit") {
                    let code = Self::arg(args, "code").and_then(|c| c.parse::<i64>().ok());
                    if code.is_some() {
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

    /// The per-block line cap: an endless un-zoned stream must not grow one
    /// block unboundedly -- freeze and continue, same kind, marked.
    fn enforce_block_cap(&mut self) {
        if self.open.items.len() >= self.max_lines_per_block {
            let kind = self.open.kind;
            self.freeze_open(kind, true);
        }
    }

    fn flush_line(&mut self) {
        if self.line.is_empty() {
            return;
        }
        let cells = core::mem::take(&mut self.line);
        let cost = cells.len() * core::mem::size_of::<TCell>();
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
                b']' => self.state = ScanState::Osc,
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
                    self.cur_param = self.cur_param.saturating_mul(10) + (b - b'0') as u32;
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
                0x07 => self.state = ScanState::Ground,
                0x1b => self.state = ScanState::OscEsc,
                _ => {} // foreign OSC body: swallowed (termination-detect only)
            },
            ScanState::OscEsc => {
                self.state = if b == b'\\' { ScanState::Ground } else { ScanState::Osc };
            }
        }
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
            // padding between cells is the plain realization -- dropped.
            if t.in_cell && t.cell.len() < 4096 {
                t.cell.push(TCell { ch: if ch < ' ' { ' ' } else { ch }, style });
            }
            return;
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
            // A blank line is content: keep it as an empty Line item.
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
        // Blocks carry few styles; a linear scan with a hot tail wins over
        // a map here.
        if let Some(last) = self.open.styles.last() {
            if *last == s {
                return (self.open.styles.len() - 1) as u16;
            }
        }
        for (i, st) in self.open.styles.iter().enumerate() {
            if *st == s {
                return i as u16;
            }
        }
        self.open.styles.push(s);
        (self.open.styles.len() - 1) as u16
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
    use vt::THEMES;

    fn parchment() -> Palette {
        THEMES[1].1
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
            s.push_str(&format!("[{:?} exit={:?} items={} styles={} objs={}]",
                b.kind, b.exit, b.items.len(), b.styles.len(), b.objs.len()));
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
                        s.push_str(&format!("T{}r{}h{}", tb.rows.len(), tb.cols.len(), tb.hdr as u8));
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
        s.push_str(&format!("open[{:?} items={}]", t.open_block().kind, t.open_block().items.len()));
        s.push_str(&line_str(&Line { cells: t.pending_line().to_vec() }));
        s
    }

    #[test]
    fn zones_become_blocks() {
        let mut t = Transcript::new(parchment());
        t.feed(&session_corpus());
        let blocks = t.frozen_blocks();
        assert_eq!(blocks.len(), 3, "foreign login + prompt + output");
        assert_eq!(blocks[0].kind, BlockKind::Foreign);
        assert_eq!(blocks[1].kind, BlockKind::Prompt);
        assert_eq!(blocks[2].kind, BlockKind::Output);
        assert_eq!(blocks[2].exit, Some(0), "the exit mark landed inside the output zone");
        assert_eq!(t.open_block().kind, BlockKind::Prompt, "the next prompt is open");
        assert_eq!(line_str(&Line { cells: t.pending_line().to_vec() }), "cora@thyla / $ ");
    }

    #[test]
    fn table_captures_cells_and_drops_padding() {
        let mut t = Transcript::new(parchment());
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
        let mut t = Transcript::new(parchment());
        t.feed(&session_corpus());
        let out = &t.frozen_blocks()[2];
        let Some(Item::Line(l)) = out.items.get(1) else {
            panic!("the red-error line follows the table");
        };
        let s = line_str(l);
        assert_eq!(s, "red error plain \u{e9}");
        let red_style = out.styles[l.cells[0].style as usize];
        let pal = parchment();
        assert_eq!(red_style.fg, pal.ansi[1], "SGR 31 resolved against parchment");
        let plain_style = out.styles[l.cells[10].style as usize];
        assert_eq!(plain_style.fg, pal.fg, "SGR 0 reset");
        assert_eq!(l.cells[s.chars().count() - 1].ch, '\u{e9}', "UTF-8 decoded");
    }

    #[test]
    fn byte_by_byte_equals_whole() {
        let corpus = session_corpus();
        let mut whole = Transcript::new(parchment());
        whole.feed(&corpus);
        let mut split = Transcript::new(parchment());
        for &b in corpus.iter() {
            split.feed(&[b]);
        }
        assert_eq!(structure_fingerprint(&whole), structure_fingerprint(&split),
            "chunk boundaries are invisible (the streaming property)");
    }

    #[test]
    fn line_discipline_overwrite_tab_bs_el() {
        let mut t = Transcript::new(parchment());
        t.feed(b"abc\rXY\n");
        t.feed(b"a\tb\n");
        t.feed(b"abcd\x08\x08Z\n");
        t.feed(b"wipe me\x1b[2Kk\n");
        let b = t.open_block();
        let l0 = match &b.items[0] { Item::Line(l) => line_str(l), _ => panic!() };
        assert_eq!(l0, "XYc", "\\r overwrites in place");
        let l1 = match &b.items[1] { Item::Line(l) => line_str(l), _ => panic!() };
        assert_eq!(l1, "a       b", "tab to the 8-col stop");
        let l2 = match &b.items[2] { Item::Line(l) => line_str(l), _ => panic!() };
        assert_eq!(l2, "abZd", "backspace repositions, write overwrites");
        let l3 = match &b.items[3] { Item::Line(l) => line_str(l), _ => panic!() };
        // EL never moves the cursor (VT semantics): the wipe cleared the
        // line, the cursor stayed at col 7, and `k` landed there.
        assert_eq!(l3, "       k", "EL2 wipes without moving the cursor");
    }

    #[test]
    fn fullscreen_intent_latches() {
        let mut t = Transcript::new(parchment());
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
        let mut t = Transcript::new(parchment());
        let mut buf = Vec::new();
        wire::open(&mut buf, Op::Zone, &[("k", "output")]);
        wire::open(&mut buf, Op::Em, &[("class", "strong")]);
        buf.extend_from_slice(b"\x1b[31membolden");
        // The zone closes with the em still open + red still set.
        wire::close(&mut buf, Op::Zone);
        t.feed(&buf);
        t.feed(b"after\n");
        let pal = parchment();
        let open = t.open_block();
        let Item::Line(l) = &open.items[0] else { panic!() };
        let st = open.styles[l.cells[0].style as usize];
        assert_eq!(st.em, EM_NONE, "the em span died at the boundary");
        assert_eq!(st.fg, pal.ansi[1], "the SGR pen persisted (terminal semantics)");
    }

    #[test]
    fn budget_evicts_oldest() {
        let mut t = Transcript::with_caps(parchment(), 3, usize::MAX, 100);
        for i in 0..6 {
            let mut buf = Vec::new();
            wire::open(&mut buf, Op::Zone, &[("k", "output")]);
            buf.extend_from_slice(format!("cmd {}\n", i).as_bytes());
            wire::close(&mut buf, Op::Zone);
            t.feed(&buf);
        }
        assert_eq!(t.frozen_blocks().len(), 3);
        let Item::Line(l) = &t.frozen_blocks()[0].items[0] else { panic!() };
        assert_eq!(line_str(l), "cmd 3", "the oldest blocks evicted");
    }

    #[test]
    fn line_cap_freezes_a_continuation() {
        let mut t = Transcript::with_caps(parchment(), 100, usize::MAX, 4);
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
        assert!(blocks[1].continuation, "the second is marked a continuation");
    }

    #[test]
    fn floating_exit_mark_attaches_backward() {
        let mut t = Transcript::new(parchment());
        let mut buf = Vec::new();
        wire::open(&mut buf, Op::Zone, &[("k", "output")]);
        buf.extend_from_slice(b"out\n");
        wire::close(&mut buf, Op::Zone);
        wire::point(&mut buf, Op::Mark, &[("k", "exit"), ("code", "7")]);
        t.feed(&buf);
        assert_eq!(t.frozen_blocks()[0].exit, Some(7),
            "the pre-deviation-8 floating order still lands");
    }

    #[test]
    fn split_frame_and_split_utf8_survive_the_cut() {
        // An OSC 1936 frame + a two-byte char, each split mid-sequence
        // across feeds, must parse exactly as when whole.
        let corpus = session_corpus();
        let mut a = Transcript::new(parchment());
        a.feed(&corpus);
        let mut b = Transcript::new(parchment());
        let mid = corpus.len() / 3;
        let mid2 = 2 * corpus.len() / 3;
        b.feed(&corpus[..mid]);
        b.feed(&corpus[mid..mid2]);
        b.feed(&corpus[mid2..]);
        assert_eq!(structure_fingerprint(&a), structure_fingerprint(&b));
    }
}
