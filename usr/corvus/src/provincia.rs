// corvus::provincia -- the trusted-path COMPOSER (TRUSTED-PATH.md 7;
// IMPERIUM-DESIGN.md 3 + 11.5).
//
// corvus lays out what the operator must read before authenticating -- the
// provincia: exactly the cap-set a pending request would confer, the axe, the
// term, the requester -- as a medium-INDEPENDENT cell grid, and (ratified fork
// F1) rasterizes it in userspace for the serial medium through its console
// handle. The v1.x kernel framebuffer sink consumes the same grid, so nothing
// here knows the medium: a cell is one glyph byte + one attribute byte.
//
// 8-bit ASCII only, no Unicode box drawing: the harness decodes the serial
// stream as iso8859-1 (tools/interactive/lib.exp), and a byte grid is what a
// baked-font blit wants. Non-printable bytes are rendered as '?' so an
// attacker-influenced name can never carry an escape sequence onto the
// unforgeable channel (the requester's user name is corvus's own record, but
// the composer is total regardless).

use alloc::vec::Vec;

pub const ATTR_NONE: u8 = 0;
pub const ATTR_BOLD: u8 = 1 << 0;

pub const PROVINCIA_COLS: usize = 64;

pub struct Grid {
    cols: usize,
    rows: usize,
    glyph: Vec<u8>,
    attr: Vec<u8>,
}

impl Grid {
    pub fn new(cols: usize, rows: usize) -> Grid {
        let n = cols * rows;
        let mut glyph = Vec::with_capacity(n);
        glyph.resize(n, b' ');
        let mut attr = Vec::with_capacity(n);
        attr.resize(n, ATTR_NONE);
        Grid { cols, rows, glyph, attr }
    }

    // Place `s` at (x, y), clipped to the grid; non-printable -> '?'.
    pub fn put(&mut self, x: usize, y: usize, s: &[u8], attr: u8) {
        if y >= self.rows {
            return;
        }
        for (k, &b) in s.iter().enumerate() {
            let cx = x + k;
            if cx >= self.cols {
                break;
            }
            let g = if (0x20..=0x7e).contains(&b) { b } else { b'?' };
            let i = y * self.cols + cx;
            self.glyph[i] = g;
            self.attr[i] = attr;
        }
    }

    // An ASCII frame around the grid's border with `title` in the top rule.
    pub fn frame(&mut self, title: &[u8]) {
        if self.cols < 4 || self.rows < 2 {
            return;
        }
        let last_row = self.rows - 1;
        let last_col = self.cols - 1;
        for x in 0..self.cols {
            self.put(x, 0, b"-", ATTR_NONE);
            self.put(x, last_row, b"-", ATTR_NONE);
        }
        for y in 0..self.rows {
            self.put(0, y, b"|", ATTR_NONE);
            self.put(last_col, y, b"|", ATTR_NONE);
        }
        self.put(0, 0, b"+", ATTR_NONE);
        self.put(last_col, 0, b"+", ATTR_NONE);
        self.put(0, last_row, b"+", ATTR_NONE);
        self.put(last_col, last_row, b"+", ATTR_NONE);
        if !title.is_empty() {
            self.put(2, 0, b" ", ATTR_NONE);
            self.put(3, 0, title, ATTR_BOLD);
            let end = (3 + title.len()).min(last_col.saturating_sub(1));
            self.put(end, 0, b" ", ATTR_NONE);
        }
    }

    // Serial rasterization: one CR LF-terminated line per row, trailing blanks
    // trimmed, bold toggled with the minimal SGR pair. No cursor addressing --
    // the trusted output is a transcript the operator reads top to bottom, and
    // it must stay legible on any terminal (or in a captured log).
    pub fn rasterize(&self, out: &mut Vec<u8>) {
        for y in 0..self.rows {
            let row = &self.glyph[y * self.cols..(y + 1) * self.cols];
            let arow = &self.attr[y * self.cols..(y + 1) * self.cols];
            let mut end = self.cols;
            while end > 0 && row[end - 1] == b' ' {
                end -= 1;
            }
            let mut bold = false;
            for x in 0..end {
                let want = arow[x] & ATTR_BOLD != 0;
                if want != bold {
                    out.extend_from_slice(if want { b"\x1b[1m" } else { b"\x1b[0m" });
                    bold = want;
                }
                out.push(row[x]);
            }
            if bold {
                out.extend_from_slice(b"\x1b[0m");
            }
            out.extend_from_slice(b"\r\n");
        }
    }
}

// What the provincia states. `caps_names` are the capability names in the
// effective (self-restricted) set, in bit order; `term_ns` 0 = unbounded.
pub struct Provincia<'a> {
    pub user: &'a [u8],
    pub pid: u32,
    pub level: &'a [u8],
    pub caps_names: &'a [&'a [u8]],
    pub axe: bool,
    pub term_ns: u64,
    pub propagating: bool,
}

// Render a duration as the operator reads it: "4h", "90m", "45s", "unbounded".
fn put_term(g: &mut Grid, x: usize, y: usize, term_ns: u64) {
    if term_ns == 0 {
        g.put(x, y, b"unbounded", ATTR_NONE);
        return;
    }
    let secs = term_ns / 1_000_000_000;
    let (n, unit) = if secs >= 3600 && secs.is_multiple_of(3600) {
        (secs / 3600, b'h')
    } else if secs >= 60 && secs.is_multiple_of(60) {
        (secs / 60, b'm')
    } else {
        (secs.max(1), b's')
    };
    let mut buf = [0u8; 21];
    let s = dec(n, &mut buf);
    g.put(x, y, s, ATTR_NONE);
    g.put(x + s.len(), y, &[unit], ATTR_NONE);
}

pub fn dec(mut n: u64, buf: &mut [u8; 21]) -> &[u8] {
    if n == 0 {
        buf[0] = b'0';
        return &buf[..1];
    }
    let mut tmp = [0u8; 21];
    let mut i = 0;
    while n > 0 && i < tmp.len() {
        tmp[i] = b'0' + (n % 10) as u8;
        n /= 10;
        i += 1;
    }
    for j in 0..i {
        buf[j] = tmp[i - 1 - j];
    }
    &buf[..i]
}

// The conferral surface. Everything the grant will contain is on it BEFORE the
// key prompt (section 3's load-bearing property: no program can trick the
// operator into a wider imperium than the one displayed).
pub fn compose_provincia(p: &Provincia, out: &mut Vec<u8>) {
    let mut g = Grid::new(PROVINCIA_COLS, 11);
    g.frame(b"CONFERRING IMPERIUM -- provincia");
    g.put(3, 2, b"to:      ", ATTR_NONE);
    g.put(12, 2, p.user, ATTR_BOLD);
    let mut pidbuf = [0u8; 21];
    let pid = dec(p.pid as u64, &mut pidbuf);
    let mut x = 12 + p.user.len() + 2;
    g.put(x, 2, b"(pid ", ATTR_NONE);
    x += 5;
    g.put(x, 2, pid, ATTR_NONE);
    x += pid.len();
    g.put(x, 2, b")", ATTR_NONE);
    g.put(3, 3, b"level:   ", ATTR_NONE);
    g.put(12, 3, p.level, ATTR_NONE);
    g.put(3, 4, b"caps:    ", ATTR_NONE);
    let mut cx = 12;
    for name in p.caps_names {
        g.put(cx, 4, name, ATTR_BOLD);
        cx += name.len() + 2;
    }
    g.put(3, 5, b"axe:     ", ATTR_NONE);
    if p.axe {
        g.put(12, 5, b"YES (CAP_KILL -- power of life and death)", ATTR_BOLD);
    } else {
        g.put(12, 5, b"no", ATTR_NONE);
    }
    g.put(3, 6, b"term:    ", ATTR_NONE);
    put_term(&mut g, 12, 6, p.term_ns);
    g.put(30, 6, b"propagating: ", ATTR_NONE);
    g.put(43, 6, if p.propagating { b"yes" } else { b"no" }, ATTR_NONE);
    g.put(3, 8, b"authenticate against the imperium key for ", ATTR_NONE);
    g.put(3 + 42, 8, p.user, ATTR_BOLD);
    g.put(3, 9, b"(Enter with no key, or Ctrl-C, declines)", ATTR_NONE);
    out.extend_from_slice(b"\r\n");
    g.rasterize(out);
}

// The prompt itself, on its own line, no newline: the key is typed after it.
pub fn compose_key_prompt(out: &mut Vec<u8>) {
    out.extend_from_slice(b"imperium key: ");
}

// A SAK that found no pending request (section 4: "when in doubt, hit the
// SAK -- corvus tells you your real provincia"): the operator learns they ARE
// on the trusted path and that nothing is waiting to be conferred.
pub fn compose_nothing_pending(out: &mut Vec<u8>) {
    let mut g = Grid::new(PROVINCIA_COLS, 5);
    g.frame(b"TRUSTED PATH");
    g.put(3, 2, b"nothing pending -- press any key", ATTR_BOLD);
    out.extend_from_slice(b"\r\n");
    g.rasterize(out);
}

// The rate-limited surface: the key is not even asked for.
pub fn compose_locked(user: &[u8], level: &[u8], out: &mut Vec<u8>) {
    let mut g = Grid::new(PROVINCIA_COLS, 6);
    g.frame(b"IMPERIUM LOCKED");
    g.put(3, 2, b"too many wrong keys for ", ATTR_NONE);
    let mut x = 3 + 24;
    g.put(x, 2, user, ATTR_BOLD);
    x += user.len();
    g.put(x, 2, b" / ", ATTR_NONE);
    g.put(x + 3, 2, level, ATTR_BOLD);
    g.put(3, 3, b"locked until corvus restarts -- press any key", ATTR_NONE);
    out.extend_from_slice(b"\r\n");
    g.rasterize(out);
}

// The one-line verdict after the prompt, then a blank line so the shell's
// next output starts clean after END.
pub fn compose_verdict(line: &[u8], out: &mut Vec<u8>) {
    out.extend_from_slice(b"\r\n");
    let mut g = Grid::new(PROVINCIA_COLS, 1);
    g.put(0, 0, line, ATTR_BOLD);
    g.rasterize(out);
    out.extend_from_slice(b"\r\n");
}
