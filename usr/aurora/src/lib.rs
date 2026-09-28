// aurora (lib) -- the renderer's pure half: the cell painter (`render`), the
// F10 settings panel's state machine and drawing (`osd`), and the config
// file's grammar (`config`). None of it takes a syscall except the config
// file's load and save, which the `backend` feature gates; the bin half
// (main.rs) owns the console drain/feed pair, the tapestry surface and the
// loop.
//
// The split exists for the tests. aurora was a bin-only `no_std` crate, so
// `cargo test` could not build it for any host, and the nine unit tests these
// modules carried were written "DORMANT" -- pinned contracts that nothing ran.
// With the bin's dependencies behind `backend`, `--no-default-features` builds
// the three modules for the host and `tools/test-rust.sh` runs them.

#![no_std]

extern crate alloc;

pub mod config;
pub mod osd;
pub mod render;

/// AURORA.md 3: follow the console program's synchronized frame (DEC mode
/// 2026) for one pass. Aurora captures no VT events, so it reads the VT's
/// state: a change in `frames`, the count of frames opened, is a new frame
/// and opens the hold (the flag alone cannot tell a new frame from an
/// abandoned one still open), and the flag down closes it.
pub fn follow_frame(hold: &mut vt::FrameHold, seen: &mut u32, frames: u32, open: bool) {
    if frames != *seen {
        *seen = frames;
        hold.open();
    }
    if !open {
        hold.close();
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    const T: u64 = 5_000_000_000;

    // A frame that opened and closed between two passes is whole: no hold.
    #[test]
    fn a_frame_that_closed_between_passes_is_painted_at_once() {
        let (mut h, mut seen) = (vt::FrameHold::default(), 0);
        follow_frame(&mut h, &mut seen, 1, false);
        assert!(!h.holds(T));
    }

    // A frame the bound abandoned is not held again while it stays open,
    // but the next one is, even when the flag never dropped between passes.
    #[test]
    fn an_abandoned_frame_is_not_held_again_but_the_next_one_is() {
        let (mut h, mut seen) = (vt::FrameHold::default(), 0);
        follow_frame(&mut h, &mut seen, 1, true);
        assert!(h.holds(T));
        assert!(!h.holds(T + vt::SYNC_HOLD_NS), "the bound abandons it");
        h.painted();
        follow_frame(&mut h, &mut seen, 1, true);
        assert!(!h.holds(T + vt::SYNC_HOLD_NS + 1), "the same frame");
        follow_frame(&mut h, &mut seen, 2, true);
        assert!(h.holds(T + vt::SYNC_HOLD_NS + 2), "a new frame");
    }
}
