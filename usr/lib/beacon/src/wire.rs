//! The OSC 1936 frame grammar (BEACON.md 12.1-12.2, normative).
//!
//! Framing: `ESC ] 1936 ; v1 ; op [; k=v]* ST` opens (or, for a point op,
//! stands alone); `ESC ] 1936 ; v1 ; /op ST` closes. ST is `ESC \`; the
//! parser also accepts BEL, per VT convention. The load-bearing rule: the
//! annotated text is ordinary stream bytes BETWEEN frames -- never inside
//! them -- so `strip()` yields byte-exactly the `none`-tier emission (the
//! 12.8 P1 property, tested here).
//!
//! The parser is deliberately structural: it recognizes frames, enforces the
//! byte caps, decodes args, and passes EVERYTHING else through as payload --
//! foreign OSC (aurora's 7770 config channel), SGR, arbitrary escapes are
//! text to this layer. Nesting legality and depth (12.1 rules 3 + 5) are the
//! consumer's flattening concern, not the wire's.

use alloc::string::String;
use alloc::vec::Vec;

pub const ESC: u8 = 0x1b;
pub const BEL: u8 = 0x07;

/// Whole frame, ESC through ST inclusive (12.1 rule 3). An oversized frame
/// is consumed to its terminator and dropped -- the aurora `osc_over` idiom.
pub const FRAME_MAX: usize = 2048;
/// One decoded value.
pub const VALUE_MAX: usize = 1024;
/// Args per frame.
pub const ARGS_MAX: usize = 8;

/// The v1 op registry (BEACON.md 12.2). `Mark` and `Rule` are point ops (no
/// close); everything else pairs.
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum Op {
    Zone,
    Mark,
    Table,
    Row,
    Cell,
    Hdr,
    Rule,
    Em,
    Obj,
}

impl Op {
    pub fn as_str(self) -> &'static str {
        match self {
            Op::Zone => "zone",
            Op::Mark => "mark",
            Op::Table => "table",
            Op::Row => "row",
            Op::Cell => "cell",
            Op::Hdr => "hdr",
            Op::Rule => "rule",
            Op::Em => "em",
            Op::Obj => "obj",
        }
    }

    pub fn parse(s: &str) -> Option<Op> {
        match s {
            "zone" => Some(Op::Zone),
            "mark" => Some(Op::Mark),
            "table" => Some(Op::Table),
            "row" => Some(Op::Row),
            "cell" => Some(Op::Cell),
            "hdr" => Some(Op::Hdr),
            "rule" => Some(Op::Rule),
            "em" => Some(Op::Em),
            "obj" => Some(Op::Obj),
        _ => None,
        }
    }

    /// A point op stands alone; a paired op opens and must close.
    pub fn is_point(self) -> bool {
        matches!(self, Op::Mark | Op::Rule)
    }
}

/// A decoded key=value argument.
#[derive(Clone, PartialEq, Eq, Debug)]
pub struct Arg {
    pub key: String,
    pub value: String,
}

/// One parsed stream element. Adjacent payload coalesces into one `Text`.
#[derive(Clone, PartialEq, Eq, Debug)]
pub enum Event {
    Text(Vec<u8>),
    Open(Op, Vec<Arg>),
    Close(Op),
    Point(Op, Vec<Arg>),
}

// ---------------------------------------------------------------------------
// Emit
// ---------------------------------------------------------------------------

fn push_prefix(out: &mut Vec<u8>) {
    out.extend_from_slice(b"\x1b]1936;v1;");
}

fn push_st(out: &mut Vec<u8>) {
    out.push(ESC);
    out.push(b'\\');
}

/// Percent-escape a value byte-wise: `%`, `;`, and anything outside
/// 0x20-0x7E escape as %XX (12.1 rule 2). UTF-8 content escapes its high
/// bytes; the decode side reassembles them.
fn push_escaped(out: &mut Vec<u8>, v: &str) {
    const HEX: &[u8; 16] = b"0123456789ABCDEF";
    for &b in v.as_bytes() {
        if b == b'%' || b == b';' || !(0x20..=0x7e).contains(&b) {
            out.push(b'%');
            out.push(HEX[(b >> 4) as usize]);
            out.push(HEX[(b & 0xf) as usize]);
        } else {
            out.push(b);
        }
    }
}

fn push_args(out: &mut Vec<u8>, args: &[(&str, &str)]) {
    debug_assert!(args.len() <= ARGS_MAX);
    for (k, v) in args {
        debug_assert!(v.len() <= VALUE_MAX);
        out.push(b';');
        out.extend_from_slice(k.as_bytes());
        out.push(b'=');
        push_escaped(out, v);
    }
}

/// Open a paired op (or emit a point op via `point`).
pub fn open(out: &mut Vec<u8>, op: Op, args: &[(&str, &str)]) {
    debug_assert!(!op.is_point());
    push_prefix(out);
    out.extend_from_slice(op.as_str().as_bytes());
    push_args(out, args);
    push_st(out);
}

pub fn close(out: &mut Vec<u8>, op: Op) {
    debug_assert!(!op.is_point());
    push_prefix(out);
    out.push(b'/');
    out.extend_from_slice(op.as_str().as_bytes());
    push_st(out);
}

pub fn point(out: &mut Vec<u8>, op: Op, args: &[(&str, &str)]) {
    debug_assert!(op.is_point());
    push_prefix(out);
    out.extend_from_slice(op.as_str().as_bytes());
    push_args(out, args);
    push_st(out);
}

// ---------------------------------------------------------------------------
// Parse
// ---------------------------------------------------------------------------

/// Find the end of an OSC that starts at `i` (i points at ESC, input[i+1] is
/// `]`). Returns (payload_end, next_index): the OSC body is
/// input[i+2..payload_end], and next_index is the first byte after the
/// terminator. None if the stream ends before any terminator (12.1 rule 3:
/// an unterminated frame at end-of-stream is abandoned by the caller).
fn osc_end(input: &[u8], i: usize) -> Option<(usize, usize)> {
    let mut j = i + 2;
    while j < input.len() {
        match input[j] {
            BEL => return Some((j, j + 1)),
            ESC if j + 1 < input.len() && input[j + 1] == b'\\' => {
                return Some((j, j + 2));
            }
            _ => j += 1,
        }
    }
    None
}

fn pct_decode(raw: &[u8]) -> Option<String> {
    let mut out: Vec<u8> = Vec::with_capacity(raw.len());
    let mut i = 0;
    while i < raw.len() {
        let b = raw[i];
        if b == b'%' {
            if i + 2 >= raw.len() {
                return None; // truncated escape
            }
            let hi = (raw[i + 1] as char).to_digit(16)?;
            let lo = (raw[i + 2] as char).to_digit(16)?;
            out.push(((hi << 4) | lo) as u8);
            i += 3;
        } else {
            out.push(b);
            i += 1;
        }
    }
    String::from_utf8(out).ok()
}

/// Parse ONE beacon frame body (the bytes between `ESC ]` and the
/// terminator). None = not ours / malformed / over-cap -> the caller drops
/// the frame (keeping any payload outside it) or passes it through as text
/// (a foreign OSC).
fn parse_body(body: &[u8]) -> Option<Event> {
    // Fields are ;-separated: 1936 ; v1 ; [/]op [; k=v]*
    let mut fields = body.split(|&b| b == b';');
    if fields.next()? != b"1936" {
        return None; // foreign OSC -- the caller treats the whole thing as text
    }
    if fields.next()? != b"v1" {
        return Some(Event::Text(Vec::new())); // ours, unknown version: drop frame, keep nothing
    }
    let opf = fields.next()?;
    let (closing, opname) = if opf.first() == Some(&b'/') {
        (true, &opf[1..])
    } else {
        (false, &opf[..])
    };
    let op = Op::parse(core::str::from_utf8(opname).ok()?)?;
    if closing {
        if op.is_point() {
            return None; // a close of a point op is malformed
        }
        return Some(Event::Close(op));
    }
    let mut args: Vec<Arg> = Vec::new();
    for f in fields {
        if args.len() == ARGS_MAX {
            return None; // over the arg cap: the frame is dropped whole
        }
        let eq = f.iter().position(|&b| b == b'=')?; // no '=' -> malformed
        let key = core::str::from_utf8(&f[..eq]).ok()?;
        if key.is_empty() || !key.bytes().all(|b| b.is_ascii_lowercase()) {
            return None;
        }
        if f.len() - eq - 1 > VALUE_MAX {
            return None;
        }
        let value = pct_decode(&f[eq + 1..])?;
        args.push(Arg {
            key: String::from(key),
            value,
        });
    }
    if op.is_point() {
        Some(Event::Point(op, args))
    } else {
        Some(Event::Open(op, args))
    }
}

/// Parse a byte stream into events. Robustness contract (12.8 P3): never
/// panics, never buffers past a cap, and always yields every payload byte --
/// a malformed / unknown / oversized beacon frame is dropped WHOLE (its
/// bytes gone, its neighbors kept); a foreign escape of any kind is payload.
pub fn parse(input: &[u8]) -> Vec<Event> {
    let mut events: Vec<Event> = Vec::new();
    let mut text: Vec<u8> = Vec::new();
    let mut i = 0;
    while i < input.len() {
        let b = input[i];
        // Only `ESC ]` opens an OSC; every other byte (other escapes
        // included) is payload to this layer.
        if b != ESC || i + 1 >= input.len() || input[i + 1] != b']' {
            text.push(b);
            i += 1;
            continue;
        }
        match osc_end(input, i) {
            None => {
                // Unterminated OSC at end-of-stream. Ours -> abandoned
                // (dropped). Foreign -> payload passthrough (bytes we do not
                // own are never eaten).
                if !input[i + 2..].starts_with(b"1936;") {
                    text.extend_from_slice(&input[i..]);
                }
                i = input.len();
            }
            Some((body_end, next)) => {
                let frame_len = next - i;
                let body = &input[i + 2..body_end];
                if !body.starts_with(b"1936;") && body != b"1936" {
                    // A foreign OSC is payload, terminator included.
                    text.extend_from_slice(&input[i..next]);
                    i = next;
                    continue;
                }
                // Ours: over-cap or malformed drops the frame whole.
                if frame_len <= FRAME_MAX {
                    match parse_body(body) {
                        Some(Event::Text(_)) | None => {} // dropped
                        Some(ev) => {
                            if !text.is_empty() {
                                events.push(Event::Text(core::mem::take(&mut text)));
                            }
                            events.push(ev);
                        }
                    }
                }
                i = next;
            }
        }
    }
    if !text.is_empty() {
        events.push(Event::Text(text));
    }
    events
}

/// The P1 tool: every payload byte, no frames. `strip(realize(Rich)) ==
/// realize(None)` byte-identical is the property every emitter is held to.
pub fn strip(input: &[u8]) -> Vec<u8> {
    let mut out: Vec<u8> = Vec::new();
    for ev in parse(input) {
        if let Event::Text(t) = ev {
            out.extend_from_slice(&t);
        }
    }
    out
}

#[cfg(test)]
mod tests {
    use super::*;
    use alloc::vec;

    fn open_bytes(op: Op, args: &[(&str, &str)]) -> Vec<u8> {
        let mut v = Vec::new();
        open(&mut v, op, args);
        v
    }

    #[test]
    fn emit_shape_is_normative() {
        let mut v = Vec::new();
        open(&mut v, Op::Zone, &[("k", "prompt")]);
        assert_eq!(v, b"\x1b]1936;v1;zone;k=prompt\x1b\\");
        v.clear();
        close(&mut v, Op::Zone);
        assert_eq!(v, b"\x1b]1936;v1;/zone\x1b\\");
        v.clear();
        point(&mut v, Op::Mark, &[("k", "exit"), ("code", "0")]);
        assert_eq!(v, b"\x1b]1936;v1;mark;k=exit;code=0\x1b\\");
    }

    #[test]
    fn roundtrip_with_escaping() {
        // `;`, `%`, UTF-8, and a control byte all survive the wire.
        let hairy = "a;b%c\u{00e9}\t";
        let mut v = Vec::new();
        open(&mut v, Op::Obj, &[("type", "path"), ("ref", hairy)]);
        v.extend_from_slice(b"shown");
        close(&mut v, Op::Obj);
        let evs = parse(&v);
        assert_eq!(evs.len(), 3);
        match &evs[0] {
            Event::Open(Op::Obj, args) => {
                assert_eq!(args[0], Arg { key: String::from("type"), value: String::from("path") });
                assert_eq!(args[1].value, hairy);
            }
            other => panic!("not an open: {:?}", other),
        }
        assert_eq!(evs[1], Event::Text(b"shown".to_vec()));
        assert_eq!(evs[2], Event::Close(Op::Obj));
    }

    #[test]
    fn strip_is_the_payload_identity() {
        let mut v = Vec::new();
        v.extend_from_slice(b"plain ");
        open(&mut v, Op::Em, &[("class", "strong")]);
        v.extend_from_slice(b"bold");
        close(&mut v, Op::Em);
        v.extend_from_slice(b" tail");
        assert_eq!(strip(&v), b"plain bold tail".to_vec());
    }

    #[test]
    fn foreign_escapes_are_payload() {
        // SGR, aurora's OSC 7770 (BEL- and ST-terminated), and a lone ESC
        // all pass through strip untouched.
        let input = b"\x1b[1mX\x1b[0m\x1b]7770;aurora;theme;parchment\x07Y\x1b]0;title\x1b\\Z\x1b";
        assert_eq!(strip(input), input.to_vec());
    }

    #[test]
    fn bel_terminates_ours_too() {
        let mut v: Vec<u8> = b"\x1b]1936;v1;rule".to_vec();
        v.push(BEL);
        v.extend_from_slice(b"after");
        let evs = parse(&v);
        assert_eq!(evs[0], Event::Point(Op::Rule, vec![]));
        assert_eq!(evs[1], Event::Text(b"after".to_vec()));
    }

    #[test]
    fn malformed_and_unknown_drop_whole_keeping_neighbors() {
        // Unknown op; unknown version; bad key; missing '='; close-of-point.
        for bad in [
            &b"\x1b]1936;v1;blink\x1b\\"[..],
            &b"\x1b]1936;v2;zone;k=prompt\x1b\\"[..],
            &b"\x1b]1936;v1;em;Class=x\x1b\\"[..],
            &b"\x1b]1936;v1;em;noequals\x1b\\"[..],
            &b"\x1b]1936;v1;/mark\x1b\\"[..],
        ] {
            let mut v: Vec<u8> = b"A".to_vec();
            v.extend_from_slice(bad);
            v.extend_from_slice(b"B");
            assert_eq!(strip(&v), b"AB".to_vec(), "case: {:?}", bad);
            assert_eq!(parse(&v).len(), 1); // one coalesced Text("AB")
        }
    }

    #[test]
    fn caps_enforced() {
        // A frame at exactly FRAME_MAX parses; one byte over drops whole.
        // Raw-constructed: an at-cap frame cannot be built through open()
        // (its per-value cap is 1024, correctly asserted on the emit side),
        // so it uses several args, each under VALUE_MAX -- unknown keys are
        // ignored per rule 4 but exercise the length accounting.
        // 10 (prefix) + 3 ("hdr") + 8 (";level=1") + 2 (ST) = 23;
        // ";aa=" + 1000 = 1004; ";ab=" + 1017 = 1021; total 2048.
        let mut v: Vec<u8> = b"\x1b]1936;v1;hdr;level=1;aa=".to_vec();
        v.extend_from_slice("x".repeat(1000).as_bytes());
        v.extend_from_slice(b";ab=");
        v.extend_from_slice("y".repeat(1017).as_bytes());
        v.extend_from_slice(b"\x1b\\");
        assert_eq!(v.len(), FRAME_MAX);
        assert_eq!(parse(&v).len(), 1);
        // One byte over: pad the last value by one -> dropped whole.
        let mut over: Vec<u8> = b"\x1b]1936;v1;hdr;level=1;aa=".to_vec();
        over.extend_from_slice("x".repeat(1000).as_bytes());
        over.extend_from_slice(b";ab=");
        over.extend_from_slice("y".repeat(1018).as_bytes());
        over.extend_from_slice(b"\x1b\\");
        assert_eq!(over.len(), FRAME_MAX + 1);
        assert_eq!(parse(&over).len(), 0);
        // VALUE_MAX guards the DECODED value on parse.
        let too_long = "y".repeat(VALUE_MAX + 1);
        let mut w: Vec<u8> = b"\x1b]1936;v1;hdr;level=".to_vec();
        w.extend_from_slice(too_long.as_bytes());
        w.extend_from_slice(b"\x1b\\");
        assert_eq!(parse(&w).len(), 0);
        // The 9th arg drops the frame.
        let mut nine: Vec<u8> = b"\x1b]1936;v1;zone".to_vec();
        for _ in 0..9 {
            nine.extend_from_slice(b";k=v");
        }
        nine.extend_from_slice(b"\x1b\\");
        assert_eq!(parse(&nine).len(), 0);
    }

    #[test]
    fn truncation_never_panics_or_eats_foreign() {
        let mut full = Vec::new();
        open(&mut full, Op::Zone, &[("k", "output")]);
        full.extend_from_slice(b"body");
        close(&mut full, Op::Zone);
        // Every prefix parses without panic; payload bytes never vanish
        // except inside our own abandoned frame.
        for cut in 0..full.len() {
            let _ = parse(&full[..cut]);
        }
        // An unterminated FOREIGN osc passes through.
        let foreign = b"\x1b]0;half-title with no terminator";
        assert_eq!(strip(foreign), foreign.to_vec());
        // An unterminated OURS is abandoned (dropped), neighbors kept.
        let mut ours: Vec<u8> = b"kept".to_vec();
        ours.extend_from_slice(b"\x1b]1936;v1;zone;k=prompt");
        assert_eq!(strip(&ours), b"kept".to_vec());
    }

    #[test]
    fn empty_value_and_flag_args() {
        let v = open_bytes(Op::Obj, &[("ref", "")]);
        match &parse(&v)[0] {
            Event::Open(Op::Obj, args) => assert_eq!(args[0].value, ""),
            other => panic!("{:?}", other),
        }
    }
}
