//! The seam wire codec (KT-1.4): the ordered record stream serialized for the
//! halcyond-owned per-tile Loom ring. Transport-agnostic byte framing so it
//! rides any byte channel (the Loom ring's registered buffer, a pipe, a 9P
//! read); the frame is `[tag:u8][len:u32 LE][payload:len]`.
//!
//! The DECODE side is a trust boundary: halcyond ingests this stream from the
//! kaua-term, which is the crash-isolated HOSTILE-INPUT parser process, so the
//! decoder must be bounds-safe on any byte sequence (the "bounds-check like the
//! 9P wire" the seam requires). Every read is checked against the buffer; a
//! frame longer than MAX_FRAME, or a payload that under-runs its declared
//! counts, is a hard error -- the caller tears the tile down (a misbehaving
//! kaua-term loses its own tile, nothing else). No decode path panics or
//! pre-allocates on an untrusted count.

use alloc::string::String;
use alloc::vec::Vec;
use kaua::{KeyCode, KeyEvent, Mods};
use vt::Cell;

use crate::{Control, Record, ScreenMode};

/// The largest single frame payload the decoder will assemble. A frame beyond
/// this is unrecoverable desync -> tear down the tile. Sized to hold a
/// full-screen CellDiff for a generous tile (a 17-byte entry per cell); the
/// ring transport (KT-1.4b) may pin it to the registered buffer size.
/// The longest title a record may carry (matches the vt's OSC 0/2 cap).
pub const MAX_TITLE: usize = 256;

pub const MAX_FRAME: usize = 4 * 1024 * 1024;

// One serialized CellDiff/ScrollOff entry is >= this many bytes (row u16 + col
// u16 + one Cell = 2 + 2 + 13). Used only to cap a decode pre-allocation so a
// hostile count cannot force a huge Vec before the byte under-run is caught.
const MIN_CELL_ENTRY: usize = 2 + 2 + CELL_BYTES;
const CELL_BYTES: usize = 4 + 4 + 4 + 1; // ch:u32 fg:u32 bg:u32 attrs:u8

// Up-record tags.
const T_CELLDIFF: u8 = 0;
const T_SCROLLOFF: u8 = 1;
const T_CONTROL: u8 = 2;
const T_MODE: u8 = 3;
// Down-input tags.
const T_KEY: u8 = 0;
const T_RESIZE: u8 = 1;
// Control subtags.
const C_OSC1936: u8 = 0;
const C_BELL: u8 = 1;
const C_TITLE: u8 = 2;
const C_EXIT: u8 = 3;
const C_WINSIZE_ACK: u8 = 4;

/// A down-channel input record (halcyond -> kaua-term).
#[derive(Clone, Debug, PartialEq)]
pub enum Input {
    Key(KeyEvent),
    Resize { cols: u16, rows: u16 },
}

/// A wire decode failure.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum WireError {
    /// A frame's payload under-ran its declared structure, or an unknown tag.
    Malformed,
    /// A frame length exceeded MAX_FRAME -- unrecoverable stream desync.
    TooLarge,
}

// ---- encode ----

fn put_u16(o: &mut Vec<u8>, v: u16) {
    o.extend_from_slice(&v.to_le_bytes());
}
fn put_u32(o: &mut Vec<u8>, v: u32) {
    o.extend_from_slice(&v.to_le_bytes());
}
fn put_cell(o: &mut Vec<u8>, c: &Cell) {
    put_u32(o, c.ch as u32);
    put_u32(o, c.fg);
    put_u32(o, c.bg);
    o.push(c.attrs);
}
fn frame(tag: u8, payload: &[u8], out: &mut Vec<u8>) {
    out.push(tag);
    put_u32(out, payload.len() as u32);
    out.extend_from_slice(payload);
}

/// Serialize one up-record onto `out` (framed).
pub fn encode_record(rec: &Record, out: &mut Vec<u8>) {
    let mut p = Vec::new();
    let tag = match rec {
        Record::CellDiff { changed, cursor } => {
            put_u16(&mut p, cursor.0);
            put_u16(&mut p, cursor.1);
            p.push(cursor.2 as u8);
            put_u32(&mut p, changed.len() as u32);
            for (r, c, cell) in changed {
                put_u16(&mut p, *r);
                put_u16(&mut p, *c);
                put_cell(&mut p, cell);
            }
            T_CELLDIFF
        }
        Record::ScrollOff { rows } => {
            put_u32(&mut p, rows.len() as u32);
            for row in rows {
                put_u32(&mut p, row.len() as u32);
                for c in row {
                    put_cell(&mut p, c);
                }
            }
            T_SCROLLOFF
        }
        Record::Control(c) => {
            match c {
                Control::Osc1936Raw(b) => {
                    p.push(C_OSC1936);
                    put_u32(&mut p, b.len() as u32);
                    p.extend_from_slice(b);
                }
                Control::Bell => p.push(C_BELL),
                Control::Title(s) => {
                    p.push(C_TITLE);
                    put_u32(&mut p, s.len() as u32);
                    p.extend_from_slice(s.as_bytes());
                }
                Control::Exit(code) => {
                    p.push(C_EXIT);
                    put_u32(&mut p, *code as u32);
                }
                Control::WinsizeAck => p.push(C_WINSIZE_ACK),
            }
            T_CONTROL
        }
        Record::Mode(m) => {
            p.push(match m {
                ScreenMode::Normal => 0,
                ScreenMode::AltScreen => 1,
            });
            T_MODE
        }
    };
    frame(tag, &p, out);
}

/// Serialize one down-record onto `out` (framed).
pub fn encode_input(inp: &Input, out: &mut Vec<u8>) {
    let mut p = Vec::new();
    let tag = match inp {
        Input::Key(ev) => {
            match ev.code {
                KeyCode::Char(c) => {
                    p.push(0);
                    put_u32(&mut p, c as u32);
                }
                KeyCode::Enter => p.push(1),
                KeyCode::Esc => p.push(2),
                KeyCode::Backspace => p.push(3),
                KeyCode::Tab => p.push(4),
                KeyCode::BackTab => p.push(5),
                KeyCode::Left => p.push(6),
                KeyCode::Right => p.push(7),
                KeyCode::Up => p.push(8),
                KeyCode::Down => p.push(9),
                KeyCode::Home => p.push(10),
                KeyCode::End => p.push(11),
                KeyCode::PageUp => p.push(12),
                KeyCode::PageDown => p.push(13),
                KeyCode::Delete => p.push(14),
                KeyCode::Insert => p.push(15),
                KeyCode::F(n) => {
                    p.push(16);
                    p.push(n);
                }
            }
            p.push(ev.mods.bits());
            T_KEY
        }
        Input::Resize { cols, rows } => {
            put_u16(&mut p, *cols);
            put_u16(&mut p, *rows);
            T_RESIZE
        }
    };
    frame(tag, &p, out);
}

// ---- decode ----

// A bounds-checked cursor over one frame payload. Every accessor returns
// Malformed rather than panicking on an under-run.
struct Reader<'a> {
    b: &'a [u8],
    pos: usize,
}

impl<'a> Reader<'a> {
    fn new(b: &'a [u8]) -> Reader<'a> {
        Reader { b, pos: 0 }
    }
    fn take(&mut self, n: usize) -> Result<&'a [u8], WireError> {
        let end = self.pos.checked_add(n).ok_or(WireError::Malformed)?;
        if end > self.b.len() {
            return Err(WireError::Malformed);
        }
        let s = &self.b[self.pos..end];
        self.pos = end;
        Ok(s)
    }
    fn u8(&mut self) -> Result<u8, WireError> {
        Ok(self.take(1)?[0])
    }
    fn u16(&mut self) -> Result<u16, WireError> {
        let s = self.take(2)?;
        Ok(u16::from_le_bytes([s[0], s[1]]))
    }
    fn u32(&mut self) -> Result<u32, WireError> {
        let s = self.take(4)?;
        Ok(u32::from_le_bytes([s[0], s[1], s[2], s[3]]))
    }
    fn cell(&mut self) -> Result<Cell, WireError> {
        let ch = char::from_u32(self.u32()?).unwrap_or('\u{FFFD}');
        let fg = self.u32()?;
        let bg = self.u32()?;
        let attrs = self.u8()?;
        Ok(Cell { ch, fg, bg, attrs })
    }
    // A count capped for pre-allocation: never trust it beyond what the
    // remaining bytes could hold (min `unit` bytes each).
    fn capped(&self, n: u32, unit: usize) -> usize {
        let remaining = self.b.len().saturating_sub(self.pos);
        (n as usize).min(remaining / unit.max(1) + 1)
    }
    fn done(&self) -> bool {
        self.pos == self.b.len()
    }
}

/// Parse one up-record from a complete frame `(tag, payload)`.
pub fn parse_record(tag: u8, payload: &[u8]) -> Result<Record, WireError> {
    let mut r = Reader::new(payload);
    let rec = match tag {
        T_CELLDIFF => {
            let cr = r.u16()?;
            let cc = r.u16()?;
            let cv = r.u8()? != 0;
            let n = r.u32()?;
            let mut changed = Vec::with_capacity(r.capped(n, MIN_CELL_ENTRY));
            for _ in 0..n {
                let rr = r.u16()?;
                let cx = r.u16()?;
                changed.push((rr, cx, r.cell()?));
            }
            Record::CellDiff {
                changed,
                cursor: (cr, cc, cv),
            }
        }
        T_SCROLLOFF => {
            let nrows = r.u32()?;
            let mut rows = Vec::with_capacity(r.capped(nrows, 4));
            for _ in 0..nrows {
                let ncells = r.u32()?;
                let mut row = Vec::with_capacity(r.capped(ncells, CELL_BYTES));
                for _ in 0..ncells {
                    row.push(r.cell()?);
                }
                rows.push(row);
            }
            Record::ScrollOff { rows }
        }
        T_CONTROL => {
            let sub = r.u8()?;
            let c = match sub {
                C_OSC1936 => {
                    let n = r.u32()? as usize;
                    Control::Osc1936Raw(r.take(n)?.to_vec())
                }
                C_BELL => Control::Bell,
                C_TITLE => {
                    // The vt caps a title at 256 bytes on the untrusted side;
                    // the consumer retains it for the tile's life, so the cap
                    // is enforced HERE too.
                    let n = r.u32()? as usize;
                    if n > MAX_TITLE {
                        return Err(WireError::Malformed);
                    }
                    let s = core::str::from_utf8(r.take(n)?).map_err(|_| WireError::Malformed)?;
                    Control::Title(String::from(s))
                }
                C_EXIT => Control::Exit(r.u32()? as i32),
                C_WINSIZE_ACK => Control::WinsizeAck,
                _ => return Err(WireError::Malformed),
            };
            Record::Control(c)
        }
        T_MODE => {
            let m = match r.u8()? {
                0 => ScreenMode::Normal,
                1 => ScreenMode::AltScreen,
                _ => return Err(WireError::Malformed),
            };
            Record::Mode(m)
        }
        _ => return Err(WireError::Malformed),
    };
    if !r.done() {
        return Err(WireError::Malformed); // trailing bytes -> reject (no silent slack)
    }
    Ok(rec)
}

/// Parse one down-record from a complete frame `(tag, payload)`.
pub fn parse_input(tag: u8, payload: &[u8]) -> Result<Input, WireError> {
    let mut r = Reader::new(payload);
    let inp = match tag {
        T_KEY => {
            let disc = r.u8()?;
            let code = match disc {
                0 => KeyCode::Char(char::from_u32(r.u32()?).unwrap_or('\u{FFFD}')),
                1 => KeyCode::Enter,
                2 => KeyCode::Esc,
                3 => KeyCode::Backspace,
                4 => KeyCode::Tab,
                5 => KeyCode::BackTab,
                6 => KeyCode::Left,
                7 => KeyCode::Right,
                8 => KeyCode::Up,
                9 => KeyCode::Down,
                10 => KeyCode::Home,
                11 => KeyCode::End,
                12 => KeyCode::PageUp,
                13 => KeyCode::PageDown,
                14 => KeyCode::Delete,
                15 => KeyCode::Insert,
                16 => KeyCode::F(r.u8()?),
                _ => return Err(WireError::Malformed),
            };
            let mods = decode_mods(r.u8()?);
            Input::Key(KeyEvent::with(code, mods))
        }
        T_RESIZE => Input::Resize {
            cols: r.u16()?,
            rows: r.u16()?,
        },
        _ => return Err(WireError::Malformed),
    };
    if !r.done() {
        return Err(WireError::Malformed);
    }
    Ok(inp)
}

fn decode_mods(bits: u8) -> Mods {
    let mut m = Mods::NONE;
    if bits & Mods::SHIFT.bits() != 0 {
        m |= Mods::SHIFT;
    }
    if bits & Mods::ALT.bits() != 0 {
        m |= Mods::ALT;
    }
    if bits & Mods::CTRL.bits() != 0 {
        m |= Mods::CTRL;
    }
    m
}

/// Reassembles frames from a byte stream that may deliver partial frames. Feed
/// bytes with `push`, then drain complete frames with `next_frame`. A frame whose
/// declared length exceeds MAX_FRAME yields `Err(TooLarge)` (the stream is then
/// unrecoverable). The internal buffer is compacted lazily so draining N frames
/// stays linear, not quadratic.
#[derive(Default)]
pub struct FrameDecoder {
    buf: Vec<u8>,
    pos: usize,
}

impl FrameDecoder {
    pub fn new() -> FrameDecoder {
        FrameDecoder {
            buf: Vec::new(),
            pos: 0,
        }
    }

    pub fn push(&mut self, bytes: &[u8]) {
        self.buf.extend_from_slice(bytes);
    }

    /// The next complete frame as `(tag, payload)`, `Err` on an oversize frame,
    /// or `None` when more bytes are needed.
    pub fn next_frame(&mut self) -> Option<Result<(u8, Vec<u8>), WireError>> {
        let avail = self.buf.len() - self.pos;
        if avail < 5 {
            self.compact();
            return None;
        }
        let h = &self.buf[self.pos..self.pos + 5];
        let len = u32::from_le_bytes([h[1], h[2], h[3], h[4]]) as usize;
        if len > MAX_FRAME {
            return Some(Err(WireError::TooLarge));
        }
        if avail < 5 + len {
            self.compact();
            return None;
        }
        let tag = self.buf[self.pos];
        let start = self.pos + 5;
        let payload = self.buf[start..start + len].to_vec();
        self.pos = start + len;
        Some(Ok((tag, payload)))
    }

    // Drop the consumed prefix once it dominates the buffer (amortized O(1)).
    fn compact(&mut self) {
        if self.pos > 0 && self.pos * 2 >= self.buf.len() {
            self.buf.drain(0..self.pos);
            self.pos = 0;
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use alloc::vec;

    fn rt_record(rec: Record) {
        let mut buf = Vec::new();
        encode_record(&rec, &mut buf);
        let mut d = FrameDecoder::new();
        d.push(&buf);
        let (tag, payload) = d.next_frame().unwrap().unwrap();
        assert_eq!(parse_record(tag, &payload).unwrap(), rec);
        assert!(d.next_frame().is_none()); // exactly one frame consumed
    }

    fn cell(ch: char) -> Cell {
        Cell {
            ch,
            fg: 0x11,
            bg: 0x22,
            attrs: vt::ATTR_BOLD,
        }
    }

    #[test]
    fn record_round_trips() {
        rt_record(Record::CellDiff {
            changed: vec![(0, 0, cell('a')), (3, 7, cell('Z'))],
            cursor: (2, 5, true),
        });
        rt_record(Record::ScrollOff {
            rows: vec![vec![cell('x'), cell('y')], vec![cell('z')]],
        });
        rt_record(Record::Control(Control::Osc1936Raw(
            b"\x1b]1936;v1;zone\x1b\\".to_vec(),
        )));
        rt_record(Record::Control(Control::Bell));
        rt_record(Record::Control(Control::Title(String::from("hi there"))));
        rt_record(Record::Control(Control::Exit(-7)));
        rt_record(Record::Control(Control::WinsizeAck));
        rt_record(Record::Mode(ScreenMode::AltScreen));
        rt_record(Record::Mode(ScreenMode::Normal));
    }

    fn rt_input(inp: Input) {
        let mut buf = Vec::new();
        encode_input(&inp, &mut buf);
        let mut d = FrameDecoder::new();
        d.push(&buf);
        let (tag, payload) = d.next_frame().unwrap().unwrap();
        assert_eq!(parse_input(tag, &payload).unwrap(), inp);
    }

    #[test]
    fn input_round_trips() {
        rt_input(Input::Key(KeyEvent::char('q')));
        rt_input(Input::Key(KeyEvent::with(
            KeyCode::Up,
            Mods::CTRL | Mods::SHIFT,
        )));
        rt_input(Input::Key(KeyEvent::new(KeyCode::F(9))));
        rt_input(Input::Key(KeyEvent::with(KeyCode::Char('c'), Mods::ALT)));
        rt_input(Input::Resize {
            cols: 132,
            rows: 43,
        });
    }

    #[test]
    fn frame_decoder_reassembles_partial_and_multiple() {
        let mut buf = Vec::new();
        encode_record(&Record::Control(Control::Bell), &mut buf);
        encode_record(&Record::Mode(ScreenMode::AltScreen), &mut buf);
        let mut d = FrameDecoder::new();
        // Feed one byte at a time: frames surface only when complete.
        let mut got = Vec::new();
        for &b in &buf {
            d.push(&[b]);
            while let Some(res) = d.next_frame() {
                let (tag, payload) = res.unwrap();
                got.push(parse_record(tag, &payload).unwrap());
            }
        }
        assert_eq!(
            got,
            vec![
                Record::Control(Control::Bell),
                Record::Mode(ScreenMode::AltScreen)
            ]
        );
    }

    #[test]
    fn oversize_frame_is_rejected() {
        // A header claiming a huge length must error, not allocate.
        let mut d = FrameDecoder::new();
        let mut hdr = vec![T_CELLDIFF];
        hdr.extend_from_slice(&((MAX_FRAME as u32) + 1).to_le_bytes());
        d.push(&hdr);
        assert_eq!(d.next_frame(), Some(Err(WireError::TooLarge)));
    }

    #[test]
    fn truncated_payload_is_malformed_not_a_panic() {
        // CellDiff claiming 100 cells but no cell bytes -> Malformed, no OOB.
        let mut p = Vec::new();
        put_u16(&mut p, 0);
        put_u16(&mut p, 0);
        p.push(1);
        put_u32(&mut p, 100); // 100 changed cells...
                              // ...but nothing follows.
        assert_eq!(parse_record(T_CELLDIFF, &p), Err(WireError::Malformed));
    }

    #[test]
    fn unknown_tag_and_trailing_bytes_rejected() {
        assert_eq!(parse_record(99, &[]), Err(WireError::Malformed));
        // A valid Bell control frame with an extra trailing byte -> reject.
        let mut p = vec![C_BELL, 0xAB];
        assert_eq!(parse_record(T_CONTROL, &p), Err(WireError::Malformed));
        p.clear();
    }

    #[test]
    fn non_utf8_title_is_malformed() {
        let mut p = vec![C_TITLE];
        put_u32(&mut p, 2);
        p.extend_from_slice(&[0xff, 0xfe]); // invalid UTF-8
        assert_eq!(parse_record(T_CONTROL, &p), Err(WireError::Malformed));
    }
}
