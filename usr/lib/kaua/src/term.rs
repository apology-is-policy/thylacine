// kaua::term -- the device/capability OUTPUT backend (audit-bearing).
//
// `Terminal` owns the console SCREEN: it writes one batched escape frame to
// fd 1 per flush (the diff -> kaua::encode) and manages the alt-screen
// lifecycle. Input is NOT here -- it lives in kaua::source (`PollSource`),
// SEPARATE so the Loom seam is real: a future LoomSource swaps the input half
// without touching this diff->fd1 output half (KAUA.md 4.4). The Terminal is
// thin glue over the host-tested pure layers (buffer/encode); only the fd-1 I/O
// + the alt-screen lifecycle live here, behind the `backend` feature (the only
// part that needs libthyla-rs, which builds for aarch64-thylacine only).
//
// CAPABILITY DISCIPLINE (KAUA.md sections 3.5 + 5): the Terminal acquires the
// SCREEN (alt-screen + cursor) on fd 1, NOT the line discipline. Raw termios is
// set by `ut` via its private consctl fd BEFORE the app is spawned (the dance,
// T-4); the input source (kaua::source) reads fd 0 assuming bytes already arrive
// raw. The Terminal never touches consctl, is never console-attached -> I-27
// untouched. It works equally for a trusted or untrusted caller (it only ever
// writes fd 1), which keeps the API honest for the future SAK-overlay consumer.
//
// CRASH BACKSTOP: on a clean return the `Drop` impl restores the screen
// (leave-alt-screen, show-cursor, reset-SGR). `no_std` apps run `panic = abort`,
// so `Drop` does NOT run on a panic/kill -> `ut`'s post-reap restore (T-4) is the
// authoritative backstop. Both restores are idempotent.

use alloc::vec::Vec;

use libthyla_rs::err::Result;
use libthyla_rs::io::{stdout, Stdout, Write};

use crate::buffer::{Buffer, Cell};
use crate::encode;
use crate::rect::Rect;
use crate::style::Style;

/// A double-buffered console SCREEN over fd 1. Pair it with a `PollSource`
/// (kaua::source) for input.
///
/// The console size is fixed at `enter()` time -- there is no winsize-query
/// syscall at v1.0 (a CPR round-trip + a resize note is a documented seam,
/// KAUA.md section 3.4 / 12). A caller that knows a better size passes it; a
/// resize is handled by `resize()` (for a future winsize signal).
pub struct Terminal {
    out: Stdout,
    /// On-screen state (what the terminal currently shows).
    front: Buffer,
    /// The frame under construction (the app draws into this).
    back: Buffer,
    /// App-requested cursor: `Some((x,y))` shows it there after a flush; `None`
    /// keeps it hidden.
    cursor: Option<(u16, u16)>,
    /// When set, the next flush repaints every cell (after `clear()`/`resize()`).
    repaint: bool,
    /// Reused output staging buffer (one allocation, cleared per flush).
    scratch: Vec<u8>,
    /// True between a successful `enter()` and `leave()`/`Drop` -- guards the
    /// restore so it runs exactly once.
    entered: bool,
}

impl Terminal {
    /// Enter the alternate screen over `area`: emit alt-screen + hide-cursor +
    /// disable-autowrap + clear to fd 1. The screen starts blank; the first
    /// `draw`/`flush` paints the app's content.
    pub fn enter(area: Rect) -> Result<Terminal> {
        let mut term = Terminal {
            out: stdout(),
            front: Buffer::empty(area),
            back: Buffer::empty(area),
            cursor: None,
            repaint: false,
            scratch: Vec::with_capacity(8 * 1024),
            entered: false,
        };
        let mut init = Vec::with_capacity(64);
        init.extend_from_slice(encode::ENTER_ALT_SCREEN);
        init.extend_from_slice(encode::HIDE_CURSOR);
        init.extend_from_slice(encode::DISABLE_AUTOWRAP);
        init.extend_from_slice(encode::CLEAR_SCREEN);
        term.out.write_all(&init)?;
        term.out.flush()?;
        term.entered = true;
        Ok(term)
    }

    /// The drawable area.
    #[inline]
    pub fn area(&self) -> Rect {
        self.back.area
    }

    /// The back buffer to draw into. Typically used via `draw`.
    #[inline]
    pub fn back_mut(&mut self) -> &mut Buffer {
        &mut self.back
    }

    /// Set the app cursor: `Some((x,y))` shows it there after the next flush;
    /// `None` hides it. Applied on `flush`.
    #[inline]
    pub fn set_cursor(&mut self, pos: Option<(u16, u16)>) {
        self.cursor = pos;
    }

    /// Clear the back buffer and force a full repaint on the next flush.
    pub fn clear(&mut self) {
        self.back.reset();
        self.repaint = true;
    }

    /// Resize to `area` (front + back) and force a full repaint. For a future
    /// winsize signal; v1.0 has no source for it (a documented seam).
    pub fn resize(&mut self, area: Rect) {
        self.front.resize(area);
        self.back.resize(area);
        self.repaint = true;
    }

    /// Reset the back buffer, let `f` draw into it, then flush the diff. The
    /// canonical per-frame call.
    pub fn draw<F: FnOnce(&mut Buffer)>(&mut self, f: F) -> Result<()> {
        self.back.reset();
        f(&mut self.back);
        self.flush()
    }

    /// Emit the changed cells (or every cell on a repaint) as one batched escape
    /// frame to fd 1, then position the real cursor and swap front <- back.
    pub fn flush(&mut self) -> Result<()> {
        // Collect the updates as owned (x,y,Cell) so no borrow of `back`/`front`
        // is held while writing `scratch` (disjoint-field clarity; the clone is
        // cheap -- a char + a small Style -- and a repaint is rare).
        let updates: Vec<(u16, u16, Cell)> = if self.repaint {
            let area = self.back.area;
            let w = area.width as usize;
            self.back
                .content
                .iter()
                .enumerate()
                .map(|(i, c)| {
                    let x = area.x + (i % w.max(1)) as u16;
                    let y = area.y + (i / w.max(1)) as u16;
                    (x, y, c.clone())
                })
                .collect()
        } else {
            self.back
                .diff(&self.front)
                .into_iter()
                .map(|(x, y, c)| (x, y, c.clone()))
                .collect()
        };

        self.scratch.clear();
        let mut pen: Option<(u16, u16)> = None;
        let mut last_style: Option<Style> = None;
        encode::render_cells(
            &mut self.scratch,
            updates.iter().map(|(x, y, c)| (*x, *y, c)),
            &mut pen,
            &mut last_style,
        );

        // Place the real cursor.
        match self.cursor {
            Some((x, y)) => {
                encode::push_move_to(&mut self.scratch, x, y);
                self.scratch.extend_from_slice(encode::SHOW_CURSOR);
            }
            None => self.scratch.extend_from_slice(encode::HIDE_CURSOR),
        }

        self.out.write_all(&self.scratch)?;
        self.out.flush()?;

        self.front = self.back.clone();
        self.repaint = false;
        Ok(())
    }

    /// Restore the screen: reset SGR, re-enable autowrap, show the cursor, leave
    /// the alternate screen. Idempotent + also run by `Drop`; `ut`'s post-reap
    /// restore is the crash backstop (see the module header).
    pub fn leave(&mut self) -> Result<()> {
        if !self.entered {
            return Ok(());
        }
        self.entered = false;
        let mut out = Vec::with_capacity(32);
        out.extend_from_slice(encode::RESET_SGR);
        out.extend_from_slice(encode::ENABLE_AUTOWRAP);
        out.extend_from_slice(encode::SHOW_CURSOR);
        out.extend_from_slice(encode::LEAVE_ALT_SCREEN);
        self.out.write_all(&out)?;
        self.out.flush()
    }
}

impl Drop for Terminal {
    fn drop(&mut self) {
        // Best-effort clean-exit restore; errors are unrecoverable here and `ut`
        // is the authoritative backstop on a crash (Drop does not run then).
        let _ = self.leave();
    }
}
