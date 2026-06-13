// kaua::term -- the device/capability backend (the only audit-bearing layer).
//
// `Terminal` owns the console wire: it reads raw bytes from fd 0 (the pollable
// cons, LS-8a) through the VT parser (kaua::input) and writes one batched escape
// frame to fd 1 per flush (the diff -> kaua::encode). It is thin glue over the
// host-tested pure layers (buffer/input/encode); only the fd 0/1 I/O + the
// alt-screen lifecycle live here, behind the `backend` feature (it is the only
// part that needs libthyla-rs, which builds for aarch64-thylacine only).
//
// CAPABILITY DISCIPLINE (KAUA.md sections 3.5 + 5): the Terminal acquires the
// SCREEN (alt-screen + cursor) on fd 1, NOT the line discipline. Raw termios is
// set by `ut` via its private consctl fd BEFORE the app is spawned (the dance,
// T-4); the Terminal reads fd 0 assuming bytes already arrive raw. It never
// touches consctl, is never console-attached -> I-27 untouched. It works equally
// for a trusted or untrusted caller (it only ever reads fd 0 / writes fd 1),
// which keeps the API honest for the future SAK-overlay consumer (section 7).
//
// CRASH BACKSTOP: on a clean return the `Drop` impl restores the screen
// (leave-alt-screen, show-cursor, reset-SGR). `no_std` apps run `panic = abort`,
// so `Drop` does NOT run on a panic/kill -> `ut`'s post-reap restore (T-4) is the
// authoritative backstop. Both restores are idempotent.

use alloc::vec::Vec;

use libthyla_rs::err::Result;
use libthyla_rs::io::{stdin, stdout, Read, Stdin, Stdout, Write};
use libthyla_rs::poll::{PollEvents, PollSet, PollTimeout};

use crate::buffer::{Buffer, Cell};
use crate::encode;
use crate::event::KeyEvent;
use crate::input::Parser;
use crate::rect::Rect;
use crate::style::Style;

/// The reused fd-0 read chunk. Sized to comfortably hold a burst of pasted
/// input or a long escape sequence within one read.
const READ_CHUNK: usize = 1024;

/// A double-buffered console terminal over fd 0 (input) + fd 1 (output).
///
/// The console size is fixed at `enter()` time -- there is no winsize-query
/// syscall at v1.0 (a CPR round-trip + a resize note is a documented seam,
/// KAUA.md section 3.4 / 12). A caller that knows a better size passes it; a
/// resize is handled by `resize()` (for a future winsize signal).
pub struct Terminal {
    out: Stdout,
    inp: Stdin,
    poll: PollSet,
    parser: Parser,
    /// On-screen state (what the terminal currently shows).
    front: Buffer,
    /// The frame under construction (the app draws into this).
    back: Buffer,
    /// App-requested cursor: `Some((x,y))` shows it there after a flush; `None`
    /// keeps it hidden.
    cursor: Option<(u16, u16)>,
    /// When set, the next flush repaints every cell (after `clear()`/`resize()`).
    repaint: bool,
    /// Latched once fd 0 reports EOF / HUP -- the loop's "quit" signal.
    eof: bool,
    /// Reused output staging buffer (one allocation, cleared per flush).
    scratch: Vec<u8>,
    inbuf: [u8; READ_CHUNK],
    /// True between a successful `enter()` and `leave()`/`Drop` -- guards the
    /// restore so it runs exactly once.
    entered: bool,
}

impl Terminal {
    /// Enter the alternate screen over `area`: emit alt-screen + hide-cursor +
    /// disable-autowrap + clear to fd 1, and register fd 0 for poll. The screen
    /// starts blank; the first `draw`/`flush` paints the app's content.
    pub fn enter(area: Rect) -> Result<Terminal> {
        let mut poll = PollSet::new();
        poll.add(&stdin(), PollEvents::READ);
        let mut term = Terminal {
            out: stdout(),
            inp: stdin(),
            poll,
            parser: Parser::new(),
            front: Buffer::empty(area),
            back: Buffer::empty(area),
            cursor: None,
            repaint: false,
            eof: false,
            scratch: Vec::with_capacity(8 * 1024),
            inbuf: [0; READ_CHUNK],
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

    /// `true` once fd 0 has reported EOF / HUP.
    #[inline]
    pub fn eof(&self) -> bool {
        self.eof
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

    /// Poll fd 0 (subject to `timeout`) and return every key the available bytes
    /// decode to. An empty result means the timeout elapsed with no input (or
    /// only non-key bytes). EOF/HUP on fd 0 latches `eof()`. The console read is
    /// #811 death-interruptible, so a dying app unwinds cleanly.
    pub fn read_keys(&mut self, timeout: PollTimeout) -> Result<Vec<KeyEvent>> {
        let mut events = Vec::new();
        let mut readable = false;
        for ev in self.poll.poll(timeout)? {
            if ev.fd == 0 {
                if ev.is_readable() {
                    readable = true;
                }
                if ev.is_hup() || ev.is_err() {
                    self.eof = true;
                }
            }
        }
        if readable {
            let n = self.inp.read(&mut self.inbuf)?;
            if n == 0 {
                self.eof = true;
            }
            for &b in &self.inbuf[..n] {
                if let Some(e) = self.parser.feed(b) {
                    events.push(e);
                }
            }
            if let Some(e) = self.parser.flush() {
                events.push(e);
            }
        }
        Ok(events)
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
