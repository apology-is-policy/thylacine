// kaua::input -- the VT/ANSI input parser: raw fd-0 bytes -> KeyEvent.
//
// A stateful, byte-at-a-time state machine. The Terminal (kaua::term) reads a
// chunk from the pollable console and feeds each byte to `Parser::feed`; after a
// chunk it calls `Parser::flush` to resolve a dangling lone ESC. This is pure
// logic -- no I/O -- so the whole truth table is host-testable.
//
// AUDIT INVARIANT (the load-bearing property of this file): the parser holds
// O(1) state -- a fixed `PARAM_CAP`-byte CSI buffer plus a 4-byte UTF-8 scratch.
// NO input, however long, malformed, or adversarial, grows its memory or makes
// it loop: a flood of CSI parameter bytes overflows the fixed buffer (the
// `csi_overflow` flag latches) and the whole sequence is consumed to its final
// byte and yields NO event; an over-long / invalid UTF-8 lead consumes a bounded
// run then resets. `feed` is total and never panics. A garbage byte stream
// produces at most one event per byte and bounded memory.
//
// LOCAL-CONSOLE ASSUMPTION: the kernel console (LS-8a) delivers each logical
// input -- a keypress or a pasted escape sequence -- within one ring drain, so a
// complete escape sequence arrives inside one `feed` chunk and a lone Escape
// keypress arrives as a single 0x1b with nothing after it in that chunk. That is
// why `flush` (end of chunk) resolves a pending ESC to `KeyCode::Esc`. A
// sequence split across two reads (theoretically possible, not produced by a
// local console) would mis-resolve its leading ESC to `Esc` -- a documented
// benign edge, never a crash.
//
// MALFORMED-INPUT POLICY: exotic/malformed sequences are *consumed safely*, not
// recovered byte-perfectly. Specifically a byte may be dropped in three rare
// cases that a keyboard never produces (ESC immediately followed by a C0
// control; a CSI aborted by a mid-sequence control; an invalid UTF-8
// continuation). Each is bounded and never panics or emits a garbage key -- the
// property the audit cares about.

use crate::event::{KeyCode, KeyEvent, Mods};

/// CSI parameter+intermediate buffer cap. A real sequence is tiny (`1;5` = 3
/// bytes; a cursor-position report `999;999` = 7); 24 is generous headroom, and
/// anything past it is malformed -> consumed without an event.
const PARAM_CAP: usize = 24;

#[derive(Clone, Copy, PartialEq, Eq, Debug)]
enum State {
    /// Between sequences: a byte is text, a C0 control, or the start of one.
    Ground,
    /// Saw `ESC`; the next byte selects CSI / SS3 / Alt-char / lone-Esc.
    Esc,
    /// In a CSI (`ESC[`): accumulating params/intermediates until a final byte.
    Csi,
    /// In an SS3 (`ESC O`): the next byte is the single final.
    Ss3,
    /// Collecting UTF-8 continuation bytes after a multibyte lead.
    Utf8,
}

/// The byte-at-a-time VT/ANSI parser. Fixed-size state (see the file header's
/// audit invariant).
pub struct Parser {
    state: State,
    // CSI accumulation.
    csi: [u8; PARAM_CAP],
    csi_len: usize,
    csi_overflow: bool,
    // UTF-8 accumulation.
    utf8: [u8; 4],
    utf8_have: usize,
    utf8_need: usize,
    // A recognized cursor-position report (CPR) `ESC[<rows>;<cols>R` -- the
    // launch size handshake's reply, or a LATE one that the slow/dribbled HVF
    // serial leaked past the probe into the steady state. Surfaced as an
    // `Event::Resize` by the source (NEVER a key), so a late reply RESIZES the
    // app instead of mis-keying as `<digits>;<digits>R` (bug_nora_hvf_cpr_
    // handshake). `(cols, rows)`; taken via `take_resize`.
    resize: Option<(u16, u16)>,
}

impl Default for Parser {
    fn default() -> Self {
        Self::new()
    }
}

impl Parser {
    pub const fn new() -> Self {
        Parser {
            state: State::Ground,
            csi: [0; PARAM_CAP],
            csi_len: 0,
            csi_overflow: false,
            utf8: [0; 4],
            utf8_have: 0,
            utf8_need: 0,
            resize: None,
        }
    }

    /// Take a recognized cursor-position report (a resize), if `feed` just
    /// completed one. `(cols, rows)`. The source calls this after each `feed`
    /// and emits an `Event::Resize`; a CPR never surfaces as a key.
    pub fn take_resize(&mut self) -> Option<(u16, u16)> {
        self.resize.take()
    }

    /// Feed one byte. Returns `Some(event)` when a byte completes a key.
    pub fn feed(&mut self, b: u8) -> Option<KeyEvent> {
        match self.state {
            State::Ground => self.feed_ground(b),
            State::Esc => self.feed_esc(b),
            State::Csi => self.feed_csi(b),
            State::Ss3 => self.feed_ss3(b),
            State::Utf8 => self.feed_utf8(b),
        }
    }

    /// End-of-drain: resolve a dangling lone `ESC` to `KeyCode::Esc`. A
    /// half-collected CSI/SS3/UTF-8 is RETAINED for the next poll, NOT dropped:
    /// under a slow/dribbled console (HVF) a sequence can straddle a poll
    /// boundary, and dropping the partial here would mis-key the tail as literal
    /// chars (bug_nora_hvf_cpr_handshake -- the launch CPR reply split across
    /// poll-drains). The local console delivers each sequence whole, so a
    /// dangling CSI/SS3/UTF-8 never occurs there at drain end -- retention is
    /// inert for it and only assembles the HVF-split case. The parser holds O(1)
    /// state, so retaining is bounded; a real sequence completes on the next
    /// poll's bytes. Call once per poll, after the drain's bytes.
    pub fn flush(&mut self) -> Option<KeyEvent> {
        match self.state {
            // A lone ESC with nothing after it at the true end of the drain is a
            // real Escape keypress (the local-console disambiguation; a split
            // ESC-led sequence whose head arrives alone is the documented
            // residual -- the ESC-timeout fix is a separate, larger change).
            State::Esc => {
                self.reset_ground();
                Some(KeyEvent::new(KeyCode::Esc))
            }
            // Ground: nothing pending. Csi/Ss3/Utf8: retain across the poll.
            State::Ground | State::Csi | State::Ss3 | State::Utf8 => None,
        }
    }

    fn reset_ground(&mut self) {
        self.state = State::Ground;
        self.csi_len = 0;
        self.csi_overflow = false;
        self.utf8_have = 0;
        self.utf8_need = 0;
    }

    // ---- Ground: text, C0 controls, or the start of a sequence/UTF-8. -------

    fn feed_ground(&mut self, b: u8) -> Option<KeyEvent> {
        if b == 0x1b {
            self.state = State::Esc;
            return None;
        }
        if b < 0x80 {
            return ascii_key(b);
        }
        // UTF-8 lead. Determine the total length; a stray continuation or an
        // invalid lead is dropped.
        let need = match b {
            0xc0..=0xdf => 2,
            0xe0..=0xef => 3,
            0xf0..=0xf7 => 4,
            _ => return None, // stray continuation / invalid lead -> drop
        };
        self.utf8[0] = b;
        self.utf8_have = 1;
        self.utf8_need = need;
        self.state = State::Utf8;
        None
    }

    // ---- Esc: the byte after ESC. -------------------------------------------

    fn feed_esc(&mut self, b: u8) -> Option<KeyEvent> {
        match b {
            b'[' => {
                self.state = State::Csi;
                self.csi_len = 0;
                self.csi_overflow = false;
                None
            }
            b'O' => {
                self.state = State::Ss3;
                None
            }
            // ESC ESC: the first was a lone Escape; emit it and stay armed for
            // the second.
            0x1b => Some(KeyEvent::new(KeyCode::Esc)),
            // ESC + printable -> Alt+char (Meta).
            0x20..=0x7e => {
                self.state = State::Ground;
                Some(KeyEvent::with(KeyCode::Char(b as char), Mods::ALT))
            }
            // ESC + a C0 control: treat the ESC as a lone Escape and drop the
            // control (a keyboard never produces this; bounded + safe).
            _ => {
                self.state = State::Ground;
                Some(KeyEvent::new(KeyCode::Esc))
            }
        }
    }

    // ---- CSI: ESC[ ... final. -----------------------------------------------

    fn feed_csi(&mut self, b: u8) -> Option<KeyEvent> {
        match b {
            // Parameter bytes (0-9 ; : < = > ?) and intermediates (space..'/').
            0x20..=0x3f => {
                if self.csi_len < PARAM_CAP {
                    self.csi[self.csi_len] = b;
                    self.csi_len += 1;
                } else {
                    self.csi_overflow = true; // bounded: stop appending
                }
                None
            }
            // Final byte.
            0x40..=0x7e => {
                let ev = if self.csi_overflow {
                    None // malformed (too long) -> consumed, no event
                } else if b == b'R' {
                    // A cursor-position report `ESC[<rows>;<cols>R` -- the launch
                    // size handshake's reply (or a late one leaked into the
                    // steady state). Recognize it as a RESIZE, never a key: the
                    // params are <rows>;<cols>, so (cols, rows) = (n2, n1). A
                    // non-CPR `R` (no two non-zero params) is just consumed.
                    let (n1, n2) = parse_two(&self.csi[..self.csi_len]);
                    if let (Some(rows), Some(cols)) = (n1, n2) {
                        if rows > 0 && cols > 0 {
                            self.resize = Some((cols, rows));
                        }
                    }
                    None
                } else {
                    dispatch_csi(&self.csi[..self.csi_len], b)
                };
                self.reset_ground();
                ev
            }
            // ESC mid-CSI: abort this sequence, start fresh on the new ESC.
            0x1b => {
                self.reset_ground();
                self.state = State::Esc;
                None
            }
            // Any other control mid-CSI: malformed -> abort + drop.
            _ => {
                self.reset_ground();
                None
            }
        }
    }

    // ---- SS3: ESC O <final>. ------------------------------------------------

    fn feed_ss3(&mut self, b: u8) -> Option<KeyEvent> {
        self.reset_ground();
        let code = match b {
            b'A' => KeyCode::Up,
            b'B' => KeyCode::Down,
            b'C' => KeyCode::Right,
            b'D' => KeyCode::Left,
            b'H' => KeyCode::Home,
            b'F' => KeyCode::End,
            b'P' => KeyCode::F(1),
            b'Q' => KeyCode::F(2),
            b'R' => KeyCode::F(3),
            b'S' => KeyCode::F(4),
            _ => return None, // unknown SS3 final -> consumed, no event
        };
        Some(KeyEvent::new(code))
    }

    // ---- UTF-8 continuation. ------------------------------------------------

    fn feed_utf8(&mut self, b: u8) -> Option<KeyEvent> {
        if (0x80..=0xbf).contains(&b) {
            // Valid continuation. utf8_have < utf8_need <= 4 always holds here.
            self.utf8[self.utf8_have] = b;
            self.utf8_have += 1;
            if self.utf8_have == self.utf8_need {
                let bytes = &self.utf8[..self.utf8_have];
                let ev = core::str::from_utf8(bytes)
                    .ok()
                    .and_then(|s| s.chars().next())
                    .map(KeyEvent::char);
                self.reset_ground();
                return ev;
            }
            None
        } else {
            // Invalid continuation -> the multibyte run is malformed. Reset and
            // re-handle `b` in Ground (a single bounded re-dispatch -- `b` may
            // be a legitimate next key). Depth is 1: reset_ground sets Ground,
            // and feed_ground never re-enters Utf8 except on a fresh lead, which
            // does not recurse.
            self.reset_ground();
            self.feed_ground(b)
        }
    }
}

/// An ASCII (`b < 0x80`) Ground byte -> a key. C0 controls map to named keys or
/// Ctrl-letter; printables to `Char`.
fn ascii_key(b: u8) -> Option<KeyEvent> {
    match b {
        0x0d | 0x0a => Some(KeyEvent::new(KeyCode::Enter)), // CR / LF
        0x09 => Some(KeyEvent::new(KeyCode::Tab)),
        0x7f | 0x08 => Some(KeyEvent::new(KeyCode::Backspace)), // DEL / BS
        0x00 => None,                                           // NUL -> ignore
        // Ctrl-letter (0x01..=0x1a), excluding the C0s with dedicated keys
        // (BS/Tab/LF/CR handled above). 0x01 -> Ctrl-A, ... 0x1a -> Ctrl-Z.
        0x01..=0x1a => {
            let letter = (b - 1 + b'a') as char;
            Some(KeyEvent::with(KeyCode::Char(letter), Mods::CTRL))
        }
        // 0x1c..=0x1f (Ctrl-\ ] ^ _) and other low controls -> ignored at v1.0.
        0x1b => None, // ESC handled by the caller before this
        0x1c..=0x1f => None,
        // Printable ASCII.
        _ => Some(KeyEvent::char(b as char)),
    }
}

/// Decode a complete CSI: `params` is the bytes between `ESC[` and the final
/// `b` (a final in 0x40..=0x7e). Recognizes arrows, navigation, `~`-keys,
/// BackTab, and modifier params; consumes-without-event for anything unknown.
/// The cursor-position report (`R`) is intercepted in `feed_csi` BEFORE this
/// (it becomes a resize, not a key), so `R` never reaches here; the trailing
/// `_ => None` would consume it harmlessly even if it did.
fn dispatch_csi(params: &[u8], final_byte: u8) -> Option<KeyEvent> {
    let (n1, n2) = parse_two(params);

    // Letter finals: arrows + Home/End. A `;mod` second param carries the
    // modifiers (e.g. `ESC[1;5C` = Ctrl-Right).
    let letter_code = match final_byte {
        b'A' => Some(KeyCode::Up),
        b'B' => Some(KeyCode::Down),
        b'C' => Some(KeyCode::Right),
        b'D' => Some(KeyCode::Left),
        b'H' => Some(KeyCode::Home),
        b'F' => Some(KeyCode::End),
        b'Z' => return Some(KeyEvent::with(KeyCode::BackTab, Mods::SHIFT)),
        _ => None,
    };
    if let Some(code) = letter_code {
        let mods = decode_mods(n2);
        return Some(KeyEvent::with(code, mods));
    }

    // The `~` family: `ESC[<n>~` or `ESC[<n>;<mod>~`.
    if final_byte == b'~' {
        let code = match n1 {
            Some(1) | Some(7) => KeyCode::Home,
            Some(2) => KeyCode::Insert,
            Some(3) => KeyCode::Delete,
            Some(4) | Some(8) => KeyCode::End,
            Some(5) => KeyCode::PageUp,
            Some(6) => KeyCode::PageDown,
            // Function keys: 11-15 -> F1-F5, 17-21 -> F6-F10, 23-24 -> F11-F12.
            Some(11) => KeyCode::F(1),
            Some(12) => KeyCode::F(2),
            Some(13) => KeyCode::F(3),
            Some(14) => KeyCode::F(4),
            Some(15) => KeyCode::F(5),
            Some(17) => KeyCode::F(6),
            Some(18) => KeyCode::F(7),
            Some(19) => KeyCode::F(8),
            Some(20) => KeyCode::F(9),
            Some(21) => KeyCode::F(10),
            Some(23) => KeyCode::F(11),
            Some(24) => KeyCode::F(12),
            _ => return None, // unknown ~-code -> consumed, no event
        };
        return Some(KeyEvent::with(code, decode_mods(n2)));
    }

    // Everything else (including `R` = cursor-position report) -> no key.
    None
}

/// Decode an xterm modifier param (`1`=none, `2`=Shift, `3`=Alt, `5`=Ctrl, and
/// combinations: value-1 is a bitfield 1=Shift,2=Alt,4=Ctrl). `None`/`Some(1)`
/// -> no modifiers.
fn decode_mods(n: Option<u16>) -> Mods {
    let v = match n {
        Some(v) if v >= 2 => v - 1,
        _ => return Mods::NONE,
    };
    let mut m = Mods::NONE;
    if v & 0x1 != 0 {
        m |= Mods::SHIFT;
    }
    if v & 0x2 != 0 {
        m |= Mods::ALT;
    }
    if v & 0x4 != 0 {
        m |= Mods::CTRL;
    }
    m
}

/// Parse up to two `;`-separated decimal params from a CSI parameter buffer.
/// Leading private-marker bytes (`?`, `<`, `=`, `>`) and any non-digit are
/// skipped within a field; a field with no digits is `None`. Saturating (a
/// pathological digit run clamps at u16::MAX, never overflows).
fn parse_two(params: &[u8]) -> (Option<u16>, Option<u16>) {
    let mut iter = params.split(|&c| c == b';');
    let a = iter.next().map(parse_u16).unwrap_or(None);
    let b = iter.next().map(parse_u16).unwrap_or(None);
    (a, b)
}

fn parse_u16(field: &[u8]) -> Option<u16> {
    let mut acc: u32 = 0;
    let mut saw_digit = false;
    for &c in field {
        if c.is_ascii_digit() {
            saw_digit = true;
            acc = acc.saturating_mul(10).saturating_add((c - b'0') as u32);
            if acc > u16::MAX as u32 {
                acc = u16::MAX as u32;
            }
        }
        // Non-digit bytes within a field (e.g. a leading '?') are skipped.
    }
    if saw_digit {
        Some(acc as u16)
    } else {
        None
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use alloc::vec::Vec;

    /// Feed a whole chunk + flush, collecting every event. Mirrors the
    /// Terminal's read loop.
    fn run(bytes: &[u8]) -> Vec<KeyEvent> {
        let mut p = Parser::new();
        let mut out = Vec::new();
        for &b in bytes {
            if let Some(ev) = p.feed(b) {
                out.push(ev);
            }
        }
        if let Some(ev) = p.flush() {
            out.push(ev);
        }
        out
    }

    fn one(bytes: &[u8]) -> KeyEvent {
        let v = run(bytes);
        assert_eq!(
            v.len(),
            1,
            "expected exactly one event from {:?}, got {:?}",
            bytes,
            v
        );
        v[0]
    }

    #[test]
    fn plain_ascii_chars() {
        assert_eq!(one(b"a").code, KeyCode::Char('a'));
        assert_eq!(one(b"Z").code, KeyCode::Char('Z'));
        assert_eq!(one(b" ").code, KeyCode::Char(' '));
        let typed = run(b"hi!");
        assert_eq!(typed.len(), 3);
        assert_eq!(typed[0].code, KeyCode::Char('h'));
        assert_eq!(typed[2].code, KeyCode::Char('!'));
    }

    #[test]
    fn c0_named_keys() {
        assert_eq!(one(b"\r").code, KeyCode::Enter);
        assert_eq!(one(b"\n").code, KeyCode::Enter);
        assert_eq!(one(b"\t").code, KeyCode::Tab);
        assert_eq!(one(b"\x7f").code, KeyCode::Backspace); // DEL
        assert_eq!(one(b"\x08").code, KeyCode::Backspace); // BS
    }

    #[test]
    fn ctrl_letters() {
        let c = one(b"\x03"); // Ctrl-C
        assert_eq!(c.code, KeyCode::Char('c'));
        assert_eq!(c.mods, Mods::CTRL);
        assert!(c.is_ctrl('c'));
        let l = one(b"\x0c"); // Ctrl-L
        assert!(l.is_ctrl('l'));
        // NUL is ignored; the C0s with dedicated keys are NOT ctrl-letters.
        assert!(run(b"\x00").is_empty());
    }

    #[test]
    fn lone_esc_resolves_at_flush() {
        assert_eq!(one(b"\x1b").code, KeyCode::Esc);
    }

    #[test]
    fn esc_esc_is_two_escapes() {
        let v = run(b"\x1b\x1b");
        assert_eq!(v.len(), 2);
        assert_eq!(v[0].code, KeyCode::Esc);
        assert_eq!(v[1].code, KeyCode::Esc);
    }

    #[test]
    fn alt_char() {
        let a = one(b"\x1ba"); // ESC a -> Alt-a
        assert_eq!(a.code, KeyCode::Char('a'));
        assert_eq!(a.mods, Mods::ALT);
    }

    #[test]
    fn csi_arrows() {
        assert_eq!(one(b"\x1b[A").code, KeyCode::Up);
        assert_eq!(one(b"\x1b[B").code, KeyCode::Down);
        assert_eq!(one(b"\x1b[C").code, KeyCode::Right);
        assert_eq!(one(b"\x1b[D").code, KeyCode::Left);
        assert_eq!(one(b"\x1b[H").code, KeyCode::Home);
        assert_eq!(one(b"\x1b[F").code, KeyCode::End);
    }

    #[test]
    fn ss3_arrows_and_fkeys() {
        assert_eq!(one(b"\x1bOA").code, KeyCode::Up);
        assert_eq!(one(b"\x1bOH").code, KeyCode::Home);
        assert_eq!(one(b"\x1bOP").code, KeyCode::F(1));
        assert_eq!(one(b"\x1bOS").code, KeyCode::F(4));
    }

    #[test]
    fn csi_tilde_navigation() {
        assert_eq!(one(b"\x1b[3~").code, KeyCode::Delete);
        assert_eq!(one(b"\x1b[2~").code, KeyCode::Insert);
        assert_eq!(one(b"\x1b[5~").code, KeyCode::PageUp);
        assert_eq!(one(b"\x1b[6~").code, KeyCode::PageDown);
        assert_eq!(one(b"\x1b[1~").code, KeyCode::Home);
        assert_eq!(one(b"\x1b[4~").code, KeyCode::End);
        assert_eq!(one(b"\x1b[15~").code, KeyCode::F(5));
        assert_eq!(one(b"\x1b[24~").code, KeyCode::F(12));
    }

    #[test]
    fn csi_modifiers() {
        let cr = one(b"\x1b[1;5C"); // Ctrl-Right
        assert_eq!(cr.code, KeyCode::Right);
        assert_eq!(cr.mods, Mods::CTRL);
        let sd = one(b"\x1b[1;2B"); // Shift-Down
        assert_eq!(sd.code, KeyCode::Down);
        assert_eq!(sd.mods, Mods::SHIFT);
        let csd = one(b"\x1b[3;6~"); // Shift-Ctrl-Delete
        assert_eq!(csd.code, KeyCode::Delete);
        assert_eq!(csd.mods, Mods::SHIFT | Mods::CTRL);
    }

    #[test]
    fn backtab() {
        let z = one(b"\x1b[Z");
        assert_eq!(z.code, KeyCode::BackTab);
        assert_eq!(z.mods, Mods::SHIFT);
    }

    #[test]
    fn cursor_position_report_is_a_resize_not_a_key() {
        // ESC[24;80R (a CPR) must not surface as a bogus key -- it is recognized
        // as a resize (cols, rows) = (80, 24) and taken via take_resize.
        let mut p = Parser::new();
        for &b in b"\x1b[24;80R" {
            assert_eq!(p.feed(b), None, "no CPR byte is a key");
        }
        assert_eq!(p.take_resize(), Some((80, 24)));
        assert_eq!(p.take_resize(), None, "taken once, then cleared");
        // The keys-only run() helper sees nothing.
        assert!(run(b"\x1b[24;80R").is_empty());
    }

    #[test]
    fn split_cpr_across_flush_assembles_as_resize() {
        // The launch CPR reply split across poll-drains by the slow/dribbled HVF
        // serial: `ESC[` drains, the poll goes dry (flush), then `24;80R` drains.
        // With flush-RETENTION the partial CSI survives and completes as a
        // resize -- NO phantom keys (bug_nora_hvf_cpr_handshake).
        let mut p = Parser::new();
        assert_eq!(p.feed(0x1b), None);
        assert_eq!(p.feed(b'['), None);
        assert_eq!(p.flush(), None); // dry mid-sequence: retain, do not drop
        for &b in b"24;80R" {
            assert_eq!(p.feed(b), None, "no tail byte is a phantom key");
        }
        assert_eq!(p.take_resize(), Some((80, 24)));
    }

    #[test]
    fn non_cpr_r_final_is_consumed_not_a_resize() {
        // An `R` with no two non-zero params is a plain unknown CSI final --
        // consumed, no key AND no resize.
        let mut p = Parser::new();
        for &b in b"\x1b[R" {
            let _ = p.feed(b);
        }
        assert_eq!(p.take_resize(), None);
    }

    #[test]
    fn unknown_csi_final_consumed() {
        assert!(run(b"\x1b[99X").is_empty());
    }

    #[test]
    fn utf8_multibyte_char() {
        // é = U+00E9 = 0xc3 0xa9.
        assert_eq!(one(&[0xc3, 0xa9]).code, KeyCode::Char('é'));
        // € = U+20AC = 0xe2 0x82 0xac.
        assert_eq!(one(&[0xe2, 0x82, 0xac]).code, KeyCode::Char('€'));
        // 😀 = U+1F600 = 0xf0 0x9f 0x98 0x80.
        assert_eq!(one(&[0xf0, 0x9f, 0x98, 0x80]).code, KeyCode::Char('😀'));
    }

    #[test]
    fn utf8_then_ascii_in_one_chunk() {
        let v = run(&[0xc3, 0xa9, b'x']); // é then x
        assert_eq!(v.len(), 2);
        assert_eq!(v[0].code, KeyCode::Char('é'));
        assert_eq!(v[1].code, KeyCode::Char('x'));
    }

    #[test]
    fn invalid_utf8_continuation_recovers_the_next_byte() {
        // 0xc3 lead then 'a' (not a continuation): the run aborts and 'a' is
        // re-dispatched as a normal char.
        let v = run(&[0xc3, b'a']);
        assert_eq!(v.len(), 1);
        assert_eq!(v[0].code, KeyCode::Char('a'));
    }

    #[test]
    fn stray_continuation_byte_dropped() {
        assert!(run(&[0x80]).is_empty());
        assert!(run(&[0xff]).is_empty());
    }

    #[test]
    fn csi_param_flood_is_bounded_and_yields_no_event() {
        // A pathological parameter flood must not grow state or emit garbage:
        // it overflows the fixed buffer and is consumed at the final byte.
        let mut bytes = Vec::new();
        bytes.extend_from_slice(b"\x1b[");
        bytes.resize(bytes.len() + 10_000, b'9'); // a pathological param run
        bytes.push(b'C'); // final
        let v = run(&bytes);
        assert!(
            v.is_empty(),
            "overflowed CSI must yield no event, got {:?}",
            v
        );
        // And the parser is back in Ground: a following key parses normally.
        let mut p = Parser::new();
        for &b in &bytes {
            let _ = p.feed(b);
        }
        assert_eq!(p.feed(b'x'), Some(KeyEvent::char('x')));
    }

    #[test]
    fn esc_mid_csi_starts_fresh() {
        // ESC[ then ESC[A : the first CSI aborts on the inner ESC, the second
        // parses to Up.
        let v = run(b"\x1b[\x1b[A");
        assert_eq!(v.len(), 1);
        assert_eq!(v[0].code, KeyCode::Up);
    }

    #[test]
    fn a_typed_line_mixes_text_and_controls() {
        // "ab\x08c\r" = a, b, Backspace, c, Enter.
        let v = run(b"ab\x08c\r");
        let codes: Vec<KeyCode> = v.iter().map(|e| e.code).collect();
        assert_eq!(
            codes,
            [
                KeyCode::Char('a'),
                KeyCode::Char('b'),
                KeyCode::Backspace,
                KeyCode::Char('c'),
                KeyCode::Enter
            ]
        );
    }

    #[test]
    fn csi_split_across_reads_assembles_when_flush_is_deferred() {
        // The invariant the PollSource drain relies on (#106-F2): a CSI split
        // across two reads is one key IF flush() is deferred to the true end.
        // `ESC [` lands in read 1 (parser mid-CSI, no key), `A` in read 2.
        let mut p = Parser::new();
        assert_eq!(p.feed(0x1b), None);
        assert_eq!(p.feed(b'['), None); // end of read 1: parser holds a partial CSI
        let k = p.feed(b'A'); // read 2 completes it
        assert_eq!(k, Some(KeyEvent::new(KeyCode::Up)));
        assert_eq!(p.flush(), None); // dry: nothing dangling

        // Flush-RETENTION now also survives a PREMATURE flush mid-CSI (the
        // HVF-split case the drain's deferral cannot cover when the poll goes dry
        // mid-sequence -- bug_nora_hvf_cpr_handshake):
        // (a) split after `ESC[` -- the premature flush KEEPS the partial CSI, so
        //     the tail `A` completes the arrow instead of mis-keying as a char.
        let mut q = Parser::new();
        assert_eq!(q.feed(0x1b), None);
        assert_eq!(q.feed(b'['), None);
        assert_eq!(q.flush(), None); // retains the partial CSI (NOT dropped)
        assert_eq!(q.feed(b'A'), Some(KeyEvent::new(KeyCode::Up))); // completes it
        // (b) split after the lone `ESC` is the DOCUMENTED RESIDUAL: a dangling
        //     lone ESC still resolves to a (possibly spurious) Escape, because it
        //     is indistinguishable from a real Escape keypress without an
        //     ESC-timeout (a separate, larger change). The CPR/arrow leak this
        //     bug is about splits AFTER `ESC[`, which (a) covers; a split AT the
        //     ESC is rare (ESC and `[` are adjacent in the reply burst).
        let mut r = Parser::new();
        assert_eq!(r.feed(0x1b), None);
        assert_eq!(r.flush(), Some(KeyEvent::new(KeyCode::Esc))); // residual
        assert_eq!(r.feed(b'['), Some(KeyEvent::char('[')));
        assert_eq!(r.feed(b'A'), Some(KeyEvent::char('A')));
    }
}
