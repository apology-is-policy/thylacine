// The VT interpreter: a byte stream in, a cell grid out (AURORA.md section 3
// -- Aurora is the SCREEN-side of the terminal protocol; Kaua is the
// app-side emitter). The subset covers what the tree's emitters produce
// (libutopia's ANSI + truecolor SGR, Kaua's cursor/erase/alt-screen, login's
// plain lines) plus the classic VT100 core; unknown sequences are parsed and
// dropped (never desync). DECSTBM scroll regions are accepted-and-ignored
// (full-screen scroll only) -- the recorded MVP seam.
//
// Colors are the Bonfire palette (docs/UTOPIA-VISUAL.md section 1): default
// bg/fg + the role-derived ANSI-16 map below. SGR 38;2 truecolor passes
// through exactly (libutopia emits it), 38;5 maps via the xterm-256 cube.

use alloc::vec;
use alloc::vec::Vec;

// Bonfire (UTOPIA-VISUAL.md section 1). The 16-color map derives from the
// section-1.4 role table (slate=blue, sage=cyan, sand=yellow, moss/fen=green,
// dusk=magenta, cinnabar=red); the bright tier lifts each role toward fg
// (brights are not pinned by scripture -- these are Aurora's derivation,
// documented here).
pub const BG: u32 = 0xFF0E_0C0C;
pub const FG: u32 = 0xFFE4_DDD8;
const ANSI: [u32; 16] = [
    0xFF2A_1F1C, // 0 black   (gutter -- visible against bg)
    0xFFC0_6050, // 1 red     (cinnabar)
    0xFF6A_9A6A, // 2 green   (fen)
    0xFFC8_A882, // 3 yellow  (sand)
    0xFF8A_9AC8, // 4 blue    (slate)
    0xFFA8_98C8, // 5 magenta (dusk)
    0xFF8A_B8A8, // 6 cyan    (sage)
    0xFFC8_BDB8, // 7 white   (fg_dim)
    0xFF5A_4E48, // 8 bright black (fg_subtle)
    0xFFE0_7840, // 9 bright red   (ember -- the fire)
    0xFFB8_D098, // 10 bright green (moss)
    0xFFE0_C090, // 11 bright yellow
    0xFFA0_B0E0, // 12 bright blue
    0xFFC0_B0E0, // 13 bright magenta
    0xFFA0_D0C0, // 14 bright cyan
    0xFFE4_DDD8, // 15 bright white (fg)
];

pub const ATTR_REVERSE: u8 = 1 << 0;
pub const ATTR_UNDERLINE: u8 = 1 << 1;
pub const ATTR_BOLD: u8 = 1 << 2;

#[derive(Clone, Copy)]
pub struct Cell {
    pub ch: char,
    pub fg: u32,
    pub bg: u32,
    pub attrs: u8,
}

impl Cell {
    fn blank(bg: u32) -> Cell {
        Cell { ch: ' ', fg: FG, bg, attrs: 0 }
    }
}

enum State {
    Ground,
    Esc,
    EscCharset, // ESC ( / ESC ) -- swallow one designator byte
    Csi,
    Osc,
    OscEsc, // saw ESC inside OSC (ST = ESC \)
}

const MAX_PARAMS: usize = 16;

pub struct Vt {
    pub cols: usize,
    pub rows: usize,
    pub cells: Vec<Cell>,
    alt_cells: Vec<Cell>, // the inactive buffer (alt-screen swap)
    pub on_alt: bool,
    pub cx: usize,
    pub cy: usize,
    pub cursor_visible: bool,
    fg: u32,
    bg: u32,
    attrs: u8,
    state: State,
    params: [u32; MAX_PARAMS],
    nparams: usize,
    cur_param: u32,
    param_seen: bool,
    csi_priv: bool,
    saved: (usize, usize),
    // UTF-8 assembly (the prompt glyphs are multi-byte).
    utf_acc: u32,
    utf_rem: u8,
    pub dirty: Vec<bool>, // per-row damage
}

impl Vt {
    pub fn new(cols: usize, rows: usize) -> Vt {
        Vt {
            cols,
            rows,
            cells: vec![Cell::blank(BG); cols * rows],
            alt_cells: vec![Cell::blank(BG); cols * rows],
            on_alt: false,
            cx: 0,
            cy: 0,
            cursor_visible: true,
            fg: FG,
            bg: BG,
            attrs: 0,
            state: State::Ground,
            params: [0; MAX_PARAMS],
            nparams: 0,
            cur_param: 0,
            param_seen: false,
            csi_priv: false,
            saved: (0, 0),
            utf_acc: 0,
            utf_rem: 0,
            dirty: vec![true; rows],
        }
    }

    #[inline]
    fn mark(&mut self, row: usize) {
        if row < self.rows {
            self.dirty[row] = true;
        }
    }

    fn mark_all(&mut self) {
        for d in self.dirty.iter_mut() {
            *d = true;
        }
    }

    pub fn feed(&mut self, bytes: &[u8]) {
        for &b in bytes {
            self.byte(b);
        }
    }

    fn byte(&mut self, b: u8) {
        match self.state {
            State::Ground => self.ground(b),
            State::Esc => self.esc(b),
            State::EscCharset => self.state = State::Ground,
            State::Csi => self.csi(b),
            State::Osc => {
                if b == 0x07 {
                    self.state = State::Ground;
                } else if b == 0x1B {
                    self.state = State::OscEsc;
                }
            }
            State::OscEsc => {
                // ST is ESC \; anything else stays in the OSC swallow.
                self.state = if b == b'\\' { State::Ground } else { State::Osc };
            }
        }
    }

    fn ground(&mut self, b: u8) {
        // UTF-8 continuation assembly first.
        if self.utf_rem > 0 {
            if b & 0xC0 == 0x80 {
                self.utf_acc = (self.utf_acc << 6) | (b & 0x3F) as u32;
                self.utf_rem -= 1;
                if self.utf_rem == 0 {
                    let ch = char::from_u32(self.utf_acc).unwrap_or('\u{FFFD}');
                    self.put_char(ch);
                }
                return;
            }
            // Malformed sequence: drop the accumulator, reprocess this byte.
            self.utf_rem = 0;
        }
        match b {
            0x1B => self.state = State::Esc,
            b'\n' => self.line_feed(),
            b'\r' => {
                self.cx = 0;
            }
            0x08 => {
                if self.cx > 0 {
                    self.cx -= 1;
                }
            }
            b'\t' => {
                let next = (self.cx / 8 + 1) * 8;
                self.cx = next.min(self.cols - 1);
            }
            0x07 => {} // BEL
            0x00..=0x06 | 0x0B..=0x0C | 0x0E..=0x1F | 0x7F => {} // other C0 + DEL: drop
            0x20..=0x7E => self.put_char(b as char),
            0xC0..=0xDF => {
                self.utf_acc = (b & 0x1F) as u32;
                self.utf_rem = 1;
            }
            0xE0..=0xEF => {
                self.utf_acc = (b & 0x0F) as u32;
                self.utf_rem = 2;
            }
            0xF0..=0xF7 => {
                self.utf_acc = (b & 0x07) as u32;
                self.utf_rem = 3;
            }
            _ => {} // stray continuation byte
        }
    }

    fn esc(&mut self, b: u8) {
        self.state = State::Ground;
        match b {
            b'[' => {
                self.nparams = 0;
                self.cur_param = 0;
                self.param_seen = false;
                self.csi_priv = false;
                self.state = State::Csi;
            }
            b']' => self.state = State::Osc,
            b'(' | b')' => self.state = State::EscCharset,
            b'7' => self.saved = (self.cx, self.cy),
            b'8' => {
                self.cx = self.saved.0.min(self.cols - 1);
                self.cy = self.saved.1.min(self.rows - 1);
            }
            b'D' => self.line_feed(),
            b'M' => {
                // Reverse index: up, scrolling down at the top.
                if self.cy > 0 {
                    self.cy -= 1;
                } else {
                    self.scroll_down();
                }
            }
            b'c' => {
                // RIS full reset.
                self.fg = FG;
                self.bg = BG;
                self.attrs = 0;
                self.cx = 0;
                self.cy = 0;
                self.cursor_visible = true;
                let bg = self.bg;
                for c in self.cells.iter_mut() {
                    *c = Cell::blank(bg);
                }
                self.mark_all();
            }
            _ => {}
        }
    }

    fn csi(&mut self, b: u8) {
        match b {
            b'0'..=b'9' => {
                self.cur_param = self.cur_param.saturating_mul(10) + (b - b'0') as u32;
                self.param_seen = true;
            }
            b';' => {
                self.push_param();
            }
            b'?' => self.csi_priv = true,
            b' '..=b'/' => {} // intermediates: swallow
            b':' => {
                // Sub-parameter separator (SGR 38:2:: form) -- treat like ';'
                // (adequate for the tree's emitters, which use ';').
                self.push_param();
            }
            0x40..=0x7E => {
                self.push_param();
                self.dispatch_csi(b);
                self.state = State::Ground;
            }
            _ => self.state = State::Ground, // malformed: abort the sequence
        }
    }

    fn push_param(&mut self) {
        if self.nparams < MAX_PARAMS {
            self.params[self.nparams] = if self.param_seen { self.cur_param } else { 0 };
            self.nparams += 1;
        }
        self.cur_param = 0;
        self.param_seen = false;
    }

    fn p(&self, i: usize, default: u32) -> u32 {
        if i < self.nparams && self.params[i] != 0 {
            self.params[i]
        } else {
            default
        }
    }

    fn dispatch_csi(&mut self, fin: u8) {
        match fin {
            b'A' => self.cy = self.cy.saturating_sub(self.p(0, 1) as usize),
            b'B' => self.cy = (self.cy + self.p(0, 1) as usize).min(self.rows - 1),
            b'C' => self.cx = (self.cx + self.p(0, 1) as usize).min(self.cols - 1),
            b'D' => self.cx = self.cx.saturating_sub(self.p(0, 1) as usize),
            b'G' => self.cx = (self.p(0, 1) as usize - 1).min(self.cols - 1),
            b'd' => self.cy = (self.p(0, 1) as usize - 1).min(self.rows - 1),
            b'H' | b'f' => {
                self.cy = (self.p(0, 1) as usize - 1).min(self.rows - 1);
                self.cx = (self.p(1, 1) as usize - 1).min(self.cols - 1);
            }
            b'J' => self.erase_display(self.p(0, 0)),
            b'K' => self.erase_line(self.p(0, 0)),
            b'L' => self.insert_lines(self.p(0, 1) as usize),
            b'M' => self.delete_lines(self.p(0, 1) as usize),
            b'@' => self.insert_chars(self.p(0, 1) as usize),
            b'P' => self.delete_chars(self.p(0, 1) as usize),
            b'X' => self.erase_chars(self.p(0, 1) as usize),
            b'm' => self.sgr(),
            b'h' | b'l' => self.mode_set(fin == b'h'),
            b's' => self.saved = (self.cx, self.cy),
            b'u' => {
                self.cx = self.saved.0.min(self.cols - 1);
                self.cy = self.saved.1.min(self.rows - 1);
            }
            b'r' => {} // DECSTBM: accepted, ignored (full-screen scroll; MVP seam)
            _ => {}
        }
    }

    fn mode_set(&mut self, set: bool) {
        if !self.csi_priv {
            return; // ANSI modes (4 IRM etc.): not implemented
        }
        for i in 0..self.nparams {
            match self.params[i] {
                25 => self.cursor_visible = set,
                47 | 1047 | 1049 => self.alt_screen(set),
                _ => {}
            }
        }
    }

    fn alt_screen(&mut self, enter: bool) {
        if enter == self.on_alt {
            return;
        }
        core::mem::swap(&mut self.cells, &mut self.alt_cells);
        self.on_alt = enter;
        if enter {
            // A fresh alt screen (1049 semantics): clear + home.
            let bg = self.bg;
            for c in self.cells.iter_mut() {
                *c = Cell::blank(bg);
            }
            self.saved = (self.cx, self.cy);
            self.cx = 0;
            self.cy = 0;
        } else {
            self.cx = self.saved.0.min(self.cols - 1);
            self.cy = self.saved.1.min(self.rows - 1);
        }
        self.mark_all();
    }

    fn sgr(&mut self) {
        if self.nparams == 0 {
            self.fg = FG;
            self.bg = BG;
            self.attrs = 0;
            return;
        }
        let mut i = 0;
        while i < self.nparams {
            let v = self.params[i];
            match v {
                0 => {
                    self.fg = FG;
                    self.bg = BG;
                    self.attrs = 0;
                }
                1 => self.attrs |= ATTR_BOLD,
                4 => self.attrs |= ATTR_UNDERLINE,
                7 => self.attrs |= ATTR_REVERSE,
                22 => self.attrs &= !ATTR_BOLD,
                24 => self.attrs &= !ATTR_UNDERLINE,
                27 => self.attrs &= !ATTR_REVERSE,
                30..=37 => self.fg = self.ansi_fg((v - 30) as usize),
                39 => self.fg = FG,
                40..=47 => self.bg = ANSI[(v - 40) as usize],
                49 => self.bg = BG,
                90..=97 => self.fg = ANSI[(v - 90 + 8) as usize],
                100..=107 => self.bg = ANSI[(v - 100 + 8) as usize],
                38 | 48 => {
                    let (color, used) = self.extended_color(i);
                    if let Some(c) = color {
                        if v == 38 {
                            self.fg = c;
                        } else {
                            self.bg = c;
                        }
                    }
                    i += used;
                }
                _ => {}
            }
            i += 1;
        }
    }

    /// SGR 38/48 extended color at params[i]: `;2;r;g;b` or `;5;n`.
    /// Returns (color, extra params consumed).
    fn extended_color(&self, i: usize) -> (Option<u32>, usize) {
        if i + 1 >= self.nparams {
            return (None, 0);
        }
        match self.params[i + 1] {
            2 => {
                if i + 4 < self.nparams {
                    let r = self.params[i + 2].min(255);
                    let g = self.params[i + 3].min(255);
                    let b = self.params[i + 4].min(255);
                    (Some(0xFF00_0000 | (r << 16) | (g << 8) | b), 4)
                } else {
                    (None, self.nparams - i - 1)
                }
            }
            5 => {
                if i + 2 < self.nparams {
                    (Some(xterm256(self.params[i + 2] as u8)), 2)
                } else {
                    (None, 1)
                }
            }
            _ => (None, 1),
        }
    }

    fn ansi_fg(&self, idx: usize) -> u32 {
        // BOLD promotes the base tier to the bright tier (the classic
        // bold-as-bright terminal convention; we bake one weight).
        if self.attrs & ATTR_BOLD != 0 {
            ANSI[idx + 8]
        } else {
            ANSI[idx]
        }
    }

    fn put_char(&mut self, ch: char) {
        if self.cx >= self.cols {
            // Deferred wrap (last-column semantics simplified: wrap before
            // the write once past the edge).
            self.cx = 0;
            self.line_feed();
        }
        let (cx, cy) = (self.cx, self.cy);
        let idx = cy * self.cols + cx;
        self.cells[idx] = Cell {
            ch,
            fg: if self.attrs & ATTR_BOLD != 0 && self.fg == FG {
                // Bold default-fg stays fg (no brighter tier exists).
                FG
            } else {
                self.fg
            },
            bg: self.bg,
            attrs: self.attrs,
        };
        self.mark(cy);
        self.cx += 1;
    }

    fn line_feed(&mut self) {
        if self.cy + 1 < self.rows {
            self.cy += 1;
        } else {
            self.scroll_up();
        }
    }

    fn scroll_up(&mut self) {
        let cols = self.cols;
        self.cells.copy_within(cols.., 0);
        let bg = self.bg;
        let last = (self.rows - 1) * cols;
        for c in self.cells[last..].iter_mut() {
            *c = Cell::blank(bg);
        }
        self.mark_all();
    }

    fn scroll_down(&mut self) {
        let cols = self.cols;
        let total = self.cols * self.rows;
        self.cells.copy_within(0..total - cols, cols);
        let bg = self.bg;
        for c in self.cells[..cols].iter_mut() {
            *c = Cell::blank(bg);
        }
        self.mark_all();
    }

    fn erase_display(&mut self, mode: u32) {
        let bg = self.bg;
        // Clamp cx: put_char leaves the cursor in the deferred-wrap state
        // cx == cols (past the last column) after a line that exactly fills
        // the width. Mode 1's inclusive `..=cur` bound would then form
        // cur == cells.len() -> `[..=len]` is out of range (a panic ->
        // renderer abort, reachable from any console writer). The clamp
        // erases through the last column of the current row (the cursor is
        // logically ON the last column at the wrap point). Holotype G-4 F1.
        let cx = self.cx.min(self.cols - 1);
        let cur = self.cy * self.cols + cx;
        match mode {
            0 => {
                for c in self.cells[cur..].iter_mut() {
                    *c = Cell::blank(bg);
                }
                for r in self.cy..self.rows {
                    self.mark(r);
                }
            }
            1 => {
                for c in self.cells[..=cur].iter_mut() {
                    *c = Cell::blank(bg);
                }
                for r in 0..=self.cy {
                    self.mark(r);
                }
            }
            _ => {
                for c in self.cells.iter_mut() {
                    *c = Cell::blank(bg);
                }
                self.mark_all();
            }
        }
    }

    fn erase_line(&mut self, mode: u32) {
        let bg = self.bg;
        let row = self.cy * self.cols;
        // Clamp cx (the deferred-wrap cx == cols state): mode 1's `cx + 1`
        // would form b == cols + 1 -> `cells[row .. row + cols + 1]` overruns
        // the row by one (the same F1 OOB as erase_display). Mode 0's `a = cx`
        // is already safe (an empty slice when cx == cols), but clamp it too
        // for consistency. Holotype G-4 F1.
        let cx = self.cx.min(self.cols - 1);
        let (a, b) = match mode {
            0 => (cx, self.cols),
            1 => (0, cx + 1),
            _ => (0, self.cols),
        };
        for c in self.cells[row + a..row + b].iter_mut() {
            *c = Cell::blank(bg);
        }
        self.mark(self.cy);
    }

    fn insert_lines(&mut self, n: usize) {
        let n = n.min(self.rows - self.cy);
        if n == 0 {
            return;
        }
        let cols = self.cols;
        let start = self.cy * cols;
        let end = self.rows * cols;
        self.cells.copy_within(start..end - n * cols, start + n * cols);
        let bg = self.bg;
        for c in self.cells[start..start + n * cols].iter_mut() {
            *c = Cell::blank(bg);
        }
        for r in self.cy..self.rows {
            self.mark(r);
        }
    }

    fn delete_lines(&mut self, n: usize) {
        let n = n.min(self.rows - self.cy);
        if n == 0 {
            return;
        }
        let cols = self.cols;
        let start = self.cy * cols;
        let end = self.rows * cols;
        self.cells.copy_within(start + n * cols..end, start);
        let bg = self.bg;
        for c in self.cells[end - n * cols..].iter_mut() {
            *c = Cell::blank(bg);
        }
        for r in self.cy..self.rows {
            self.mark(r);
        }
    }

    fn insert_chars(&mut self, n: usize) {
        let n = n.min(self.cols - self.cx);
        if n == 0 {
            return;
        }
        let row = self.cy * self.cols;
        let (a, b) = (row + self.cx, row + self.cols);
        self.cells.copy_within(a..b - n, a + n);
        let bg = self.bg;
        for c in self.cells[a..a + n].iter_mut() {
            *c = Cell::blank(bg);
        }
        self.mark(self.cy);
    }

    fn delete_chars(&mut self, n: usize) {
        let n = n.min(self.cols - self.cx);
        if n == 0 {
            return;
        }
        let row = self.cy * self.cols;
        let (a, b) = (row + self.cx, row + self.cols);
        self.cells.copy_within(a + n..b, a);
        let bg = self.bg;
        for c in self.cells[b - n..b].iter_mut() {
            *c = Cell::blank(bg);
        }
        self.mark(self.cy);
    }

    fn erase_chars(&mut self, n: usize) {
        let n = n.min(self.cols - self.cx);
        let row = self.cy * self.cols;
        let bg = self.bg;
        for c in self.cells[row + self.cx..row + self.cx + n].iter_mut() {
            *c = Cell::blank(bg);
        }
        self.mark(self.cy);
    }
}

/// xterm-256 palette: 0-15 the ANSI map, 16-231 the 6x6x6 cube, 232-255 the
/// grayscale ramp.
fn xterm256(n: u8) -> u32 {
    match n {
        0..=15 => ANSI[n as usize],
        16..=231 => {
            let v = n - 16;
            let steps = [0u32, 95, 135, 175, 215, 255];
            let r = steps[(v / 36) as usize];
            let g = steps[((v / 6) % 6) as usize];
            let b = steps[(v % 6) as usize];
            0xFF00_0000 | (r << 16) | (g << 8) | b
        }
        _ => {
            let g = 8 + 10 * (n - 232) as u32;
            0xFF00_0000 | (g << 16) | (g << 8) | g
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    // The VT parser is `no_std`+alloc but pure logic -- these host tests
    // (cargo test, std available) drive the byte stream directly. The atlas
    // blit (render.rs) needs a framebuffer + the baked atlas, so it stays
    // proven by the in-guest ls-gfx E2E; the parser is proven here.

    fn feed(vt: &mut Vt, s: &[u8]) {
        vt.feed(s);
    }

    // Holotype G-4 F1: the deferred-wrap-at-last-row + ESC[1J / ESC[1K case
    // that overran the cell grid (cx == cols -> the mode-1 inclusive bound
    // formed cells[..=len]). A pre-fix build PANICS here; post-fix it erases
    // cleanly. Reachable from any console writer, so this is the regression.
    #[test]
    fn erase_mode1_at_deferred_wrap_last_row_no_oob() {
        let mut vt = Vt::new(8, 4); // cols=8, rows=4
        // Move to the last row, then fill the row exactly to the width so the
        // cursor lands in the deferred-wrap state cx == cols.
        feed(&mut vt, b"\x1b[4;1H"); // CUP row 4 col 1 -> cy=3, cx=0
        feed(&mut vt, b"abcdefgh"); // 8 chars fill the row; cx now == cols (8)
        assert_eq!(vt.cy, 3);
        assert_eq!(vt.cx, vt.cols); // the deferred-wrap state
        // ESC[1J (erase from start of display to cursor inclusive) MUST NOT
        // panic. Pre-fix: cur = 3*8 + 8 = 32 == cells.len() -> cells[..=32] OOB.
        feed(&mut vt, b"\x1b[1J");
        // ESC[1K (erase from start of line to cursor inclusive) -- the sibling.
        feed(&mut vt, b"\x1b[4;1H");
        feed(&mut vt, b"abcdefgh");
        feed(&mut vt, b"\x1b[1K");
        // Survived without a panic; the grid is intact (the row was cleared).
        assert_eq!(vt.cells.len(), 32);
    }

    // The classic full-width type-past-the-edge deferred wrap: writing cols+1
    // printables wraps to the next row rather than OOB-ing.
    #[test]
    fn deferred_wrap_writes_next_row() {
        let mut vt = Vt::new(4, 3);
        feed(&mut vt, b"abcd"); // exactly fills row 0; cx == 4 (== cols)
        assert_eq!(vt.cy, 0);
        assert_eq!(vt.cx, vt.cols);
        feed(&mut vt, b"e"); // the wrap: cy -> 1, then write 'e' at (1,0)
        assert_eq!(vt.cy, 1);
        assert_eq!(vt.cx, 1);
        assert_eq!(vt.cells[0].ch, 'a');
        assert_eq!(vt.cells[4].ch, 'e'); // row 1, col 0
    }

    // A malformed / truncated escape sequence must never panic or desync
    // the ground state.
    #[test]
    fn malformed_escapes_do_not_panic() {
        let mut vt = Vt::new(10, 4);
        feed(&mut vt, b"\x1b[999;999H"); // way-out-of-range CUP -> clamped
        assert!(vt.cx < vt.cols && vt.cy < vt.rows);
        feed(&mut vt, b"\x1b["); // truncated CSI
        feed(&mut vt, b"\x1b[38;2;300;400;500m"); // out-of-range truecolor
        feed(&mut vt, b"\x1b]0;title-with-no-terminator"); // unterminated OSC
        feed(&mut vt, b"\xff\xfe"); // stray UTF-8 continuation bytes
        feed(&mut vt, b"x"); // ground state still works
        // No panic reaching here is the assertion.
    }

    // Alt-screen enter/leave swaps a fresh buffer and restores dims + cursor.
    #[test]
    fn alt_screen_swap_preserves_dims() {
        let mut vt = Vt::new(6, 3);
        feed(&mut vt, b"main");
        feed(&mut vt, b"\x1b[?1049h"); // enter alt: clear + home
        assert_eq!(vt.cx, 0);
        assert_eq!(vt.cells.len(), 18);
        feed(&mut vt, b"alt");
        feed(&mut vt, b"\x1b[?1049l"); // leave: restore
        assert_eq!(vt.cells.len(), 18);
        assert_eq!(vt.cells[0].ch, 'm'); // the main buffer is back
    }
}
