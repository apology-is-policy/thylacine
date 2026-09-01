// Input policy: the held-feed queue + key translation + the modal key
// map. The feed machinery is aurora's #129/#135/#136 discipline carried
// over WITH its policy selftest arms -- here they are real host tests
// (the lib+bin split's whole point; aurora could only self-test in-guest).
//
// The contract (see aurora main.rs for the full case law): consfeed
// returns a SHORT count under back-pressure (the bytes are still ours);
// ONE write attempt per pass (a retry spin would wedge the renderer
// against a legitimately-stalled reader); drop the NEWEST over the bound
// (n_tty's rule -- a dropped prefix leaves a complete but DIFFERENT
// command); a negative return is an ERROR (clear + report), not
// back-pressure; and a non-empty queue must couple to a BOUNDED wait
// (a hidden surface receives no frame ticks).

use alloc::vec::Vec;

/// The held-input bound: ~8x the kernel ring; far past any human burst.
pub const FEED_PENDING_MAX: usize = 4096;
/// The bounded-wait nap while input is held (paces the pass rate, not a
/// per-pass retry).
pub const FEED_RETRY_MS: u64 = 50;

#[derive(PartialEq, Eq, Clone, Copy, Debug)]
pub enum FeedLoss {
    None,
    OverBound,
    WriteErr,
}

/// One drain attempt against an injected writer. Returns what was lost
/// and why (the #136 F5 split: an I/O failure must never read "backlog").
pub fn feed_drain_with(
    write: &mut dyn FnMut(&[u8]) -> i64,
    pending: &mut Vec<u8>,
    cap: usize,
    dropped: &mut u64,
) -> FeedLoss {
    if pending.is_empty() {
        return FeedLoss::None;
    }
    let n = write(pending.as_slice());
    if n < 0 {
        *dropped += pending.len() as u64;
        pending.clear();
        return FeedLoss::WriteErr;
    }
    let took = n as usize;
    if took >= pending.len() {
        pending.clear();
        return FeedLoss::None;
    }
    if took > 0 {
        pending.drain(..took);
    }
    if pending.len() > cap {
        let excess = pending.len() - cap;
        pending.truncate(cap);
        *dropped += excess as u64;
        return FeedLoss::OverBound;
    }
    FeedLoss::None
}

/// Must this pass bound its wait? (Non-empty queue <=> bounded wait.)
pub fn wait_is_bounded(held: usize) -> bool {
    held != 0
}

/// Translate one KEY event into terminal bytes (press + autorepeat feed;
/// release silent; runes as UTF-8; non-rune keys to the classic CSI
/// sequences ut's editor and Kaua parse). Aurora's table, shared intent.
pub fn key_bytes(code: u16, value: u32, rune: u32, out: &mut Vec<u8>) {
    if value == 0 {
        return;
    }
    if rune != 0 {
        if let Some(ch) = char::from_u32(rune) {
            let mut b = [0u8; 4];
            out.extend_from_slice(ch.encode_utf8(&mut b).as_bytes());
        }
        return;
    }
    let seq: &[u8] = match code {
        103 => b"\x1b[A",
        108 => b"\x1b[B",
        106 => b"\x1b[C",
        105 => b"\x1b[D",
        102 => b"\x1b[H",
        107 => b"\x1b[F",
        104 => b"\x1b[5~",
        109 => b"\x1b[6~",
        111 => b"\x1b[3~",
        110 => b"\x1b[2~",
        _ => return,
    };
    out.extend_from_slice(seq);
}

/// The transcript's two keyboard modes (HALCYON.md section 4: the
/// Helix-modal transcript). Esc leaves Insert; `i` returns to the
/// writable prompt (jumping the view to the bottom).
#[derive(PartialEq, Eq, Clone, Copy, Debug)]
pub enum Mode {
    Insert,
    Normal,
}

/// What a Normal-mode key asks of the view. Selection verbs arrive at
/// H-2d-4; v0 is navigation.
#[derive(PartialEq, Eq, Clone, Copy, Debug)]
pub enum NormalAct {
    None,
    ScrollLines(i32),
    ScrollHalfPage(i32),
    Top,
    Bottom,
    ToInsert,
}

/// Map a Normal-mode key (rune-first, then code) to its action.
/// Positive scroll = toward older content (up).
pub fn normal_key(code: u16, rune: u32) -> NormalAct {
    match rune {
        0x6a => NormalAct::ScrollLines(-1), // j: down (newer)
        0x6b => NormalAct::ScrollLines(1),  // k: up (older)
        0x64 | 0x04 => NormalAct::ScrollHalfPage(-1), // d / ctrl-d
        0x75 | 0x15 => NormalAct::ScrollHalfPage(1),  // u / ctrl-u
        0x67 => NormalAct::Top,    // g (gg collapsed to one press in v0)
        0x47 => NormalAct::Bottom, // G
        0x69 => NormalAct::ToInsert, // i
        _ => match code {
            103 => NormalAct::ScrollLines(1),      // Up
            108 => NormalAct::ScrollLines(-1),     // Down
            104 => NormalAct::ScrollHalfPage(1),   // PgUp
            109 => NormalAct::ScrollHalfPage(-1),  // PgDn
            102 => NormalAct::Top,                 // Home
            107 => NormalAct::Bottom,              // End
            _ => NormalAct::None,
        },
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use alloc::vec::Vec;

    // Aurora's #129/#135/#136 selftest arms (a)-(f), as host tests.

    #[test]
    fn full_accept_empties() {
        let mut q: Vec<u8> = Vec::new();
        let mut dropped = 0u64;
        q.extend_from_slice(b"abcd");
        let mut seen: Vec<Vec<u8>> = Vec::new();
        let mut w = |b: &[u8]| {
            seen.push(b.to_vec());
            4i64
        };
        assert_eq!(feed_drain_with(&mut w, &mut q, 8, &mut dropped), FeedLoss::None);
        assert!(q.is_empty());
        assert_eq!(seen[0].as_slice(), b"abcd");
        assert_eq!(dropped, 0);
    }

    #[test]
    fn zero_accept_loses_nothing() {
        let mut q: Vec<u8> = Vec::new();
        let mut dropped = 0u64;
        q.extend_from_slice(b"abcd");
        let mut w = |_: &[u8]| 0i64;
        assert_eq!(feed_drain_with(&mut w, &mut q, 8, &mut dropped), FeedLoss::None);
        assert_eq!(q.as_slice(), b"abcd");
        assert_eq!(dropped, 0);
    }

    #[test]
    fn partial_accept_keeps_order() {
        let mut q: Vec<u8> = Vec::new();
        let mut dropped = 0u64;
        q.extend_from_slice(b"abcd");
        let mut w = |_: &[u8]| 2i64;
        assert_eq!(feed_drain_with(&mut w, &mut q, 8, &mut dropped), FeedLoss::None);
        assert_eq!(q.as_slice(), b"cd");
    }

    #[test]
    fn over_bound_drops_newest_and_reports() {
        let mut q: Vec<u8> = Vec::new();
        let mut dropped = 0u64;
        q.extend_from_slice(b"0123456789AB");
        let mut w = |_: &[u8]| 0i64;
        assert_eq!(feed_drain_with(&mut w, &mut q, 8, &mut dropped), FeedLoss::OverBound);
        assert_eq!(q.as_slice(), b"01234567", "the newest went, the oldest survive");
        assert_eq!(dropped, 4);
    }

    #[test]
    fn write_error_clears_and_reports_as_error() {
        let mut q: Vec<u8> = Vec::new();
        let mut dropped = 0u64;
        q.extend_from_slice(b"abcd");
        let mut w = |_: &[u8]| -1i64;
        assert_eq!(feed_drain_with(&mut w, &mut q, 8, &mut dropped), FeedLoss::WriteErr);
        assert!(q.is_empty());
        assert_eq!(dropped, 4);
    }

    #[test]
    fn pacing_couples_to_held() {
        assert!(!wait_is_bounded(0));
        assert!(wait_is_bounded(1));
        assert!(wait_is_bounded(FEED_PENDING_MAX));
    }

    #[test]
    fn key_translation() {
        let mut out = Vec::new();
        key_bytes(30, 0, 0x61, &mut out);
        assert!(out.is_empty(), "release is silent");
        key_bytes(30, 1, 0x61, &mut out);
        assert_eq!(out.as_slice(), b"a");
        out.clear();
        key_bytes(103, 1, 0, &mut out);
        assert_eq!(out.as_slice(), b"\x1b[A");
        out.clear();
        key_bytes(30, 1, 0xe9, &mut out);
        assert_eq!(out.as_slice(), "\u{e9}".as_bytes(), "runes feed as UTF-8");
    }

    #[test]
    fn normal_mode_map() {
        assert_eq!(normal_key(0, 0x6b), NormalAct::ScrollLines(1));
        assert_eq!(normal_key(0, 0x6a), NormalAct::ScrollLines(-1));
        assert_eq!(normal_key(0, 0x69), NormalAct::ToInsert);
        assert_eq!(normal_key(0, 0x47), NormalAct::Bottom);
        assert_eq!(normal_key(104, 0), NormalAct::ScrollHalfPage(1));
        assert_eq!(normal_key(0, 0x7a), NormalAct::None);
    }
}
