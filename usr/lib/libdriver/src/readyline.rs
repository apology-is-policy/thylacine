// The readiness-line accumulator -- the pure half of the warden's
// driver-readiness read (MENAGERIE.md section 5). A freshly-spawned driver
// signals it is up by writing one newline-terminated line ("READY") to its
// stdout pipe; the warden reads it to tell a long-lived service from a one-shot
// proof.
//
// PURE (no libthyla-rs), like `manifest` / `resource` / `source` / `supervise`:
// the line assembly + bounds are exercised on the host. The warden owns the
// impure half -- ONE bounded `read(up to READY_LINE_MAX)` per poll-readable
// event -- and feeds each chunk here.
//
// THE F1 FIX (5e-4 audit). The original read the pipe ONE BYTE AT A TIME with
// blocking reads, looping until '\n'. A non-TCB driver that wrote a PARTIAL line
// (e.g. "READ", no newline) and then held -- alive, write end open, writing
// nothing more -- stalled the warden on the next byte's blocking read FOREVER,
// escaping the give-up budget (which lived in the warden's outer poll loop, not
// the inner per-byte read). A hang on the TCB warden is a boot DoS by a
// misbehaving driver -- exactly the untrusted-3rd-party-driver model the
// framework EXISTS to sandbox (NOVEL #11). A single bounded read returns the
// AVAILABLE bytes without blocking for a full buffer, so feeding chunks into an
// accumulator that persists across poll iterations never blocks mid-line.

use alloc::string::{String, ToString};
use alloc::vec::Vec;

/// The maximum length of a driver's readiness line. A longer line without a
/// newline is a garbled/hostile signal -- the driver is non-TCB. (The "READY"
/// token is 5 bytes; real readiness lines are tiny.)
pub const READY_LINE_MAX: usize = 64;

/// The result of feeding one freshly-read chunk into the readiness accumulator.
#[derive(Clone, Debug, PartialEq, Eq)]
pub enum ReadyLine {
    /// A complete line (the bytes before the first '\n') is assembled.
    Line(String),
    /// No newline yet; `acc` holds the partial line. Poll + read again.
    NeedMore,
    /// The accumulator reached `READY_LINE_MAX` without a newline, or the line
    /// was not valid UTF-8 -- a garbled/hostile signal; give up on this pipe.
    Garbled,
}

/// Feed one freshly-read `chunk` into the readiness-line accumulator `acc`,
/// scanning for the first '\n'.
///
/// The warden calls this after a single bounded `read` returns `chunk` (the
/// bytes available without blocking). On `NeedMore` the warden polls + reads
/// again, feeding the next chunk into the same `acc`, so a line split across
/// reads still assembles -- bounded by `READY_LINE_MAX`. Bytes after the first
/// '\n' in `chunk` are discarded: a readiness signal is exactly one line, and
/// the warden stops reading the pipe once it has the line.
pub fn feed_ready_line(acc: &mut Vec<u8>, chunk: &[u8]) -> ReadyLine {
    for &b in chunk {
        if b == b'\n' {
            // `acc` already holds the prior partial plus this chunk's bytes up
            // to (not including) the newline -- i.e. the full line.
            return match core::str::from_utf8(acc) {
                Ok(s) => ReadyLine::Line(s.to_string()),
                Err(_) => ReadyLine::Garbled,
            };
        }
        if acc.len() >= READY_LINE_MAX {
            return ReadyLine::Garbled; // over-long without a newline
        }
        acc.push(b);
    }
    ReadyLine::NeedMore
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn whole_line_in_one_chunk() {
        let mut acc = Vec::new();
        assert_eq!(
            feed_ready_line(&mut acc, b"READY\n"),
            ReadyLine::Line("READY".to_string())
        );
    }

    #[test]
    fn split_line_assembles_across_chunks() {
        // The exact F1 scenario: a partial line ("READ") arrives first, the rest
        // ("Y\n") later. The per-byte blocking read stalled here; the accumulator
        // assembles it without ever blocking mid-line.
        let mut acc = Vec::new();
        assert_eq!(feed_ready_line(&mut acc, b"READ"), ReadyLine::NeedMore);
        assert_eq!(acc.as_slice(), b"READ"); // partial retained
        assert_eq!(
            feed_ready_line(&mut acc, b"Y\n"),
            ReadyLine::Line("READY".to_string())
        );
    }

    #[test]
    fn one_byte_at_a_time_assembles() {
        // A driver dribbling bytes one at a time still assembles -- and crucially
        // each call returns NeedMore (the warden re-polls) rather than blocking.
        let mut acc = Vec::new();
        for &b in b"READY" {
            assert_eq!(feed_ready_line(&mut acc, &[b]), ReadyLine::NeedMore);
        }
        assert_eq!(
            feed_ready_line(&mut acc, b"\n"),
            ReadyLine::Line("READY".to_string())
        );
    }

    #[test]
    fn bytes_after_newline_are_discarded() {
        // Only the first line matters; trailing bytes in the same chunk are not
        // part of the readiness signal.
        let mut acc = Vec::new();
        assert_eq!(
            feed_ready_line(&mut acc, b"READY\nextra noise"),
            ReadyLine::Line("READY".to_string())
        );
    }

    #[test]
    fn overlong_without_newline_is_garbled() {
        // A driver that floods the pipe with no newline cannot grow the
        // accumulator without bound -- it is capped and rejected.
        let mut acc = Vec::new();
        let flood = [b'x'; READY_LINE_MAX + 8];
        assert_eq!(feed_ready_line(&mut acc, &flood), ReadyLine::Garbled);
        assert!(acc.len() <= READY_LINE_MAX);
    }

    #[test]
    fn exact_max_length_line_is_accepted() {
        // A line of exactly READY_LINE_MAX bytes plus the newline is the boundary
        // -- accepted (the bound is on a newline-less accumulator).
        let mut acc = Vec::new();
        let mut line = [b'a'; READY_LINE_MAX + 1];
        line[READY_LINE_MAX] = b'\n';
        match feed_ready_line(&mut acc, &line) {
            ReadyLine::Line(s) => assert_eq!(s.len(), READY_LINE_MAX),
            other => panic!("expected the boundary line, got {:?}", other),
        }
    }

    #[test]
    fn overlong_split_is_garbled_not_unbounded() {
        // Even dribbled in pieces, a newline-less stream is capped -- the
        // accumulator never exceeds READY_LINE_MAX across many NeedMore calls.
        let mut acc = Vec::new();
        let mut result = ReadyLine::NeedMore;
        for _ in 0..32 {
            result = feed_ready_line(&mut acc, b"zzzz"); // 4 bytes per call, no newline
            if result == ReadyLine::Garbled {
                break;
            }
            assert!(acc.len() <= READY_LINE_MAX);
        }
        assert_eq!(result, ReadyLine::Garbled);
    }

    #[test]
    fn non_utf8_line_is_garbled() {
        let mut acc = Vec::new();
        assert_eq!(
            feed_ready_line(&mut acc, &[0xff, 0xfe, b'\n']),
            ReadyLine::Garbled
        );
    }

    #[test]
    fn empty_chunk_is_needmore() {
        let mut acc = Vec::new();
        assert_eq!(feed_ready_line(&mut acc, b""), ReadyLine::NeedMore);
        assert!(acc.is_empty());
    }

    #[test]
    fn empty_line_is_an_empty_string() {
        let mut acc = Vec::new();
        assert_eq!(
            feed_ready_line(&mut acc, b"\n"),
            ReadyLine::Line(String::new())
        );
    }
}
