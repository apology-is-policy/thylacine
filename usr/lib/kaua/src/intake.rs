// kaua::intake -- one Event at a time, one byte per read. Pure; host-tested.
//
// A kaua app is a guest on its terminal: when it quits, or hands the terminal
// to a child (quarry's games), whatever was typed behind its last key belongs
// to whoever reads fd 0 next -- the shell's next command, the game's first key.
// A byte read into this process cannot be given back (there is no pushback
// into a terminal, and a TIOCSTI-like one is a forgery primitive), so the only
// way to leave it in the kernel is never to read it. The intake therefore reads
// fd 0 ONE byte per read(2) and stops at the first byte that completes an
// Event; the app acts on that event before the next byte is asked for. That is
// less(1)'s trade: a readiness check and a read per byte, against an editor's
// bulk read, which loses the type-ahead behind `:q`.
//
// The app still paints once per burst, not once per key: a `Burst` waits the
// app's timeout for its first event and takes only what is already queued for
// the rest, so a paste is handled as one run of events and then drawn.
//
// ESC holdoff (#173): a bare ESC is the head of a split arrow/function-key
// sequence as often as it is a lone Escape press, so when the parser holds one
// the next readiness wait is `Wait::Holdoff` (ESC_HOLDOFF_MS) instead of the
// caller's. Nothing within it -> `Parser::flush` resolves the ESC to an Escape
// key. A half-collected CSI/SS3/UTF-8 is never flushed; it waits for its next
// byte across calls, as the parser documents.

use alloc::collections::VecDeque;
use alloc::vec::Vec;

use crate::event::Event;
use crate::input::Parser;

/// How long a bare ESC waits for its continuation before it is an Escape key --
/// the standard terminal ESC timeout (cf. vim ttimeoutlen). A true lone Escape
/// press pays it once.
pub const ESC_HOLDOFF_MS: u32 = 50;

/// The most events one burst hands the app before it paints: a bound against an
/// unbounded writer, so a flood still repaints. Four console rings' worth of
/// single-byte keys; one pts ring (4 KiB) takes a few bursts.
pub const BURST_MAX: usize = 1024;

/// The most bytes one `next` reads without completing an event before it gives
/// up with the parser state kept: bytes that decode to nothing (NULs, an
/// over-long CSI) are counted here, since `BURST_MAX` counts only events. One
/// pts ring.
pub const QUIET_BYTES_MAX: usize = 4096;

/// Which wait `Fd0::ready` should apply.
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum Wait {
    /// The app's timeout: the first event of a burst.
    Caller,
    /// None: the rest of a burst takes only what is already queued.
    Zero,
    /// `ESC_HOLDOFF_MS`: a bare ESC is held, waiting for a continuation byte.
    Holdoff,
}

/// What a readiness wait found on fd 0.
#[derive(Clone, Copy, PartialEq, Eq, Debug, Default)]
pub struct Readiness {
    pub readable: bool,
    pub hup: bool,
}

/// The terminal side of the intake: a readiness wait and a one-byte read.
/// `kaua::source` implements it over fd 0; the tests script it.
pub trait Fd0 {
    type Error;
    fn ready(&mut self, wait: Wait) -> Result<Readiness, Self::Error>;
    /// Exactly one byte, or `None` at end of file.
    fn read_byte(&mut self) -> Result<Option<u8>, Self::Error>;
}

impl<T: Fd0 + ?Sized> Fd0 for &mut T {
    type Error = T::Error;
    fn ready(&mut self, wait: Wait) -> Result<Readiness, T::Error> {
        (**self).ready(wait)
    }
    fn read_byte(&mut self) -> Result<Option<u8>, T::Error> {
        (**self).read_byte()
    }
}

/// The decode state that outlives one call: the parser (a sequence may span
/// calls) and the launch type-ahead not yet replayed.
pub struct Intake {
    parser: Parser,
    pending: VecDeque<u8>,
    eof: bool,
}

impl Intake {
    /// `pending`: bytes read from fd 0 before the loop began (the launch size
    /// probe's type-ahead, kaua::query), replayed before fd 0 is read.
    pub fn new(pending: Vec<u8>) -> Self {
        Intake {
            parser: Parser::new(),
            pending: pending.into(),
            eof: false,
        }
    }

    /// True once fd 0 reported end of file or hang-up with nothing left to read.
    pub fn is_eof(&self) -> bool {
        self.eof
    }

    /// The next Event, or `None` when fd 0 offered nothing within `wait`
    /// (`Caller` or `Zero`) or `QUIET_BYTES_MAX` bytes completed nothing. Reads
    /// up to the byte that completes the event returned, and not one byte past
    /// it.
    pub fn next<F: Fd0>(&mut self, fd: &mut F, wait: Wait) -> Result<Option<Event>, F::Error> {
        while let Some(b) = self.pending.pop_front() {
            if let Some(e) = self.feed(b) {
                return Ok(Some(e));
            }
        }
        let mut quiet = 0usize;
        loop {
            let w = if self.parser.pending_escape() {
                Wait::Holdoff
            } else {
                wait
            };
            let r = fd.ready(w)?;
            if !r.readable {
                // A hang-up that is still readable (a pts whose master is gone
                // reads as EOF) is read below; only an unreadable one ends here.
                if r.hup {
                    self.eof = true;
                }
                return Ok(self.flush());
            }
            match fd.read_byte()? {
                None => {
                    self.eof = true;
                    return Ok(self.flush());
                }
                Some(b) => {
                    if let Some(e) = self.feed(b) {
                        return Ok(Some(e));
                    }
                    quiet += 1;
                    if quiet >= QUIET_BYTES_MAX {
                        return Ok(None);
                    }
                }
            }
        }
    }

    /// A byte completes at most one event: the parser surfaces a cursor-position
    /// report -- the console's answer to a size query -- as a resize and no key
    /// (bug_nora_hvf_cpr_handshake).
    fn feed(&mut self, b: u8) -> Option<Event> {
        let key = self.parser.feed(b);
        if let Some((c, r)) = self.parser.take_resize() {
            return Some(Event::Resize(c, r));
        }
        key.map(Event::Key)
    }

    fn flush(&mut self) -> Option<Event> {
        self.parser.flush().map(Event::Key)
    }
}

/// One run of events for the app to handle before it paints: the first waits
/// the app's timeout, the rest take only what fd 0 already holds, at most
/// `BURST_MAX`. The app stops pulling the moment an event may end it or give
/// the terminal away, and nothing behind that event has been read.
pub struct Burst<'a, F: Fd0> {
    intake: &'a mut Intake,
    fd: F,
    taken: usize,
    done: bool,
}

impl<'a, F: Fd0> Burst<'a, F> {
    pub fn new(intake: &'a mut Intake, fd: F) -> Self {
        Burst {
            intake,
            fd,
            taken: 0,
            done: false,
        }
    }

    pub fn next(&mut self) -> Result<Option<Event>, F::Error> {
        if self.done || self.taken >= BURST_MAX {
            return Ok(None);
        }
        let wait = if self.taken == 0 {
            Wait::Caller
        } else {
            Wait::Zero
        };
        let e = self.intake.next(&mut self.fd, wait)?;
        match e {
            Some(_) => self.taken += 1,
            None => self.done = true,
        }
        Ok(e)
    }

    /// Events handed out so far; 0 after the burst ends means the app's
    /// timeout lapsed with nothing typed.
    pub fn taken(&self) -> usize {
        self.taken
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::event::{KeyCode, KeyEvent, Mods};

    /// A scripted terminal: `script` is what fd 0 holds, in order; a `None`
    /// step is a readiness wait that finds nothing (the typist paused).
    struct Term {
        script: VecDeque<Option<u8>>,
        waits: Vec<Wait>,
        reads: usize,
        hup_when_empty: bool,
    }

    impl Term {
        fn new(steps: &[Option<u8>]) -> Self {
            Term {
                script: steps.iter().copied().collect(),
                waits: Vec::new(),
                reads: 0,
                hup_when_empty: false,
            }
        }
        fn bytes(s: &[u8]) -> Self {
            Self::new(&s.iter().map(|&b| Some(b)).collect::<Vec<_>>())
        }
        fn left(&self) -> Vec<u8> {
            self.script.iter().flatten().copied().collect()
        }
    }

    impl Fd0 for Term {
        type Error = ();
        fn ready(&mut self, wait: Wait) -> Result<Readiness, ()> {
            self.waits.push(wait);
            match self.script.front() {
                Some(Some(_)) => Ok(Readiness {
                    readable: true,
                    hup: false,
                }),
                Some(None) => {
                    self.script.pop_front();
                    Ok(Readiness::default())
                }
                None => Ok(Readiness {
                    readable: false,
                    hup: self.hup_when_empty,
                }),
            }
        }
        fn read_byte(&mut self) -> Result<Option<u8>, ()> {
            self.reads += 1;
            Ok(self.script.pop_front().flatten())
        }
    }

    fn key(c: char) -> Option<Event> {
        Some(Event::Key(KeyEvent::char(c)))
    }

    #[test]
    fn the_bytes_behind_a_quit_key_are_never_read() {
        let mut t = Term::bytes(b"jq echo next\r");
        let mut i = Intake::new(Vec::new());
        assert_eq!(i.next(&mut t, Wait::Caller), Ok(key('j')));
        assert_eq!(i.next(&mut t, Wait::Caller), Ok(key('q')));
        // The app quits here: everything typed behind q is still in the terminal.
        assert_eq!(t.left(), b" echo next\r");
        assert_eq!(t.reads, 2);
    }

    #[test]
    fn a_multibyte_key_reads_exactly_its_own_bytes() {
        let mut t = Term::bytes(b"\x1b[Aq");
        let mut i = Intake::new(Vec::new());
        assert_eq!(
            i.next(&mut t, Wait::Caller),
            Ok(Some(Event::Key(KeyEvent::new(KeyCode::Up))))
        );
        assert_eq!(t.left(), b"q");
    }

    #[test]
    fn a_lone_escape_waits_the_holdoff_then_is_an_escape_key() {
        // ESC, the typist pauses, then SPACE: Escape, then Space -- not Alt-Space.
        let mut t = Term::new(&[Some(0x1b), None, Some(b' ')]);
        let mut i = Intake::new(Vec::new());
        assert_eq!(
            i.next(&mut t, Wait::Caller),
            Ok(Some(Event::Key(KeyEvent::new(KeyCode::Esc))))
        );
        assert_eq!(t.waits, [Wait::Caller, Wait::Holdoff]);
        assert_eq!(i.next(&mut t, Wait::Caller), Ok(key(' ')));
    }

    #[test]
    fn a_split_arrow_assembles_across_the_holdoff() {
        // The ESC head arrives alone; its tail is readable by the holdoff wait.
        let mut t = Term::bytes(b"\x1b[B");
        let mut i = Intake::new(Vec::new());
        assert_eq!(
            i.next(&mut t, Wait::Caller),
            Ok(Some(Event::Key(KeyEvent::new(KeyCode::Down))))
        );
        assert_eq!(t.waits, [Wait::Caller, Wait::Holdoff, Wait::Caller]);
    }

    #[test]
    fn esc_then_a_key_at_once_is_still_alt() {
        let mut t = Term::bytes(b"\x1bx");
        let mut i = Intake::new(Vec::new());
        assert_eq!(
            i.next(&mut t, Wait::Caller),
            Ok(Some(Event::Key(KeyEvent::with(
                KeyCode::Char('x'),
                Mods::ALT
            ))))
        );
    }

    #[test]
    fn a_half_sequence_is_kept_not_flushed_when_input_pauses() {
        let mut t = Term::new(&[Some(0x1b), Some(b'['), None, Some(b'C')]);
        let mut i = Intake::new(Vec::new());
        assert_eq!(i.next(&mut t, Wait::Caller), Ok(None));
        assert_eq!(
            i.next(&mut t, Wait::Caller),
            Ok(Some(Event::Key(KeyEvent::new(KeyCode::Right))))
        );
    }

    #[test]
    fn nothing_ready_is_none_and_reads_nothing() {
        let mut t = Term::new(&[None]);
        let mut i = Intake::new(Vec::new());
        assert_eq!(i.next(&mut t, Wait::Caller), Ok(None));
        assert_eq!(t.reads, 0);
        assert!(!i.is_eof());
    }

    #[test]
    fn launch_type_ahead_replays_before_fd0_is_asked() {
        let mut t = Term::bytes(b"z");
        let mut i = Intake::new(b"ab".to_vec());
        assert_eq!(i.next(&mut t, Wait::Caller), Ok(key('a')));
        assert_eq!(i.next(&mut t, Wait::Caller), Ok(key('b')));
        assert!(t.waits.is_empty());
        assert_eq!(i.next(&mut t, Wait::Caller), Ok(key('z')));
    }

    #[test]
    fn a_sequence_split_between_type_ahead_and_fd0_assembles() {
        let mut t = Term::bytes(b"[D");
        let mut i = Intake::new(b"\x1b".to_vec());
        assert_eq!(
            i.next(&mut t, Wait::Caller),
            Ok(Some(Event::Key(KeyEvent::new(KeyCode::Left))))
        );
    }

    #[test]
    fn a_cursor_report_is_a_resize_and_ends_at_its_final_byte() {
        let mut t = Term::bytes(b"\x1b[40;120Rq");
        let mut i = Intake::new(Vec::new());
        assert_eq!(
            i.next(&mut t, Wait::Caller),
            Ok(Some(Event::Resize(120, 40)))
        );
        assert_eq!(t.left(), b"q");
    }

    #[test]
    fn a_held_escape_at_the_end_of_input_is_escape_and_a_hangup_is_eof() {
        let mut t = Term::bytes(b"\x1b");
        let mut i = Intake::new(Vec::new());
        assert_eq!(
            i.next(&mut t, Wait::Caller),
            Ok(Some(Event::Key(KeyEvent::new(KeyCode::Esc))))
        );
        assert!(!i.is_eof());
        assert_eq!(i.next(&mut t, Wait::Caller), Ok(None));
        assert!(!i.is_eof());
        t.hup_when_empty = true;
        assert_eq!(i.next(&mut t, Wait::Caller), Ok(None));
        assert!(i.is_eof());
    }

    #[test]
    fn a_zero_length_read_is_eof() {
        struct Closed;
        impl Fd0 for Closed {
            type Error = ();
            fn ready(&mut self, _: Wait) -> Result<Readiness, ()> {
                Ok(Readiness {
                    readable: true,
                    hup: true,
                })
            }
            fn read_byte(&mut self) -> Result<Option<u8>, ()> {
                Ok(None)
            }
        }
        let mut i = Intake::new(Vec::new());
        assert_eq!(i.next(&mut Closed, Wait::Caller), Ok(None));
        assert!(i.is_eof());
    }

    #[test]
    fn bytes_that_complete_nothing_cannot_hold_the_app() {
        // NULs decode to no event; a writer of nothing but NULs gets one pts
        // ring of them read, then the app has its None and paints.
        let mut t = Term::bytes(&[0u8; QUIET_BYTES_MAX + 5]);
        let mut i = Intake::new(Vec::new());
        assert_eq!(i.next(&mut t, Wait::Caller), Ok(None));
        assert_eq!(t.reads, QUIET_BYTES_MAX);
        assert_eq!(t.left().len(), 5);
    }

    #[test]
    fn a_burst_waits_only_for_its_first_event() {
        let mut t = Term::bytes(b"ab");
        let mut i = Intake::new(Vec::new());
        let mut b = Burst::new(&mut i, &mut t);
        assert_eq!(b.next(), Ok(key('a')));
        assert_eq!(b.next(), Ok(key('b')));
        assert_eq!(b.next(), Ok(None));
        assert_eq!(b.taken(), 2);
        drop(b);
        assert_eq!(t.waits, [Wait::Caller, Wait::Zero, Wait::Zero]);
    }

    #[test]
    fn an_app_that_stops_at_its_quit_key_leaves_the_rest_typed() {
        let mut t = Term::bytes(b"jjqls\r");
        let mut i = Intake::new(Vec::new());
        let mut b = Burst::new(&mut i, &mut t);
        while let Ok(Some(e)) = b.next() {
            if e == Event::Key(KeyEvent::char('q')) {
                break;
            }
        }
        drop(b);
        assert_eq!(t.left(), b"ls\r");
    }

    #[test]
    fn a_flood_is_cut_at_burst_max_and_the_rest_stays_queued() {
        let mut t = Term::bytes(&[b'x'; BURST_MAX + 5]);
        let mut i = Intake::new(Vec::new());
        let mut b = Burst::new(&mut i, &mut t);
        let mut n = 0;
        while let Ok(Some(_)) = b.next() {
            n += 1;
        }
        assert_eq!(n, BURST_MAX);
        drop(b);
        assert_eq!(t.left().len(), 5);
    }

    #[test]
    fn a_burst_that_timed_out_stays_ended() {
        let mut t = Term::new(&[None, Some(b'a')]);
        let mut i = Intake::new(Vec::new());
        let mut b = Burst::new(&mut i, &mut t);
        assert_eq!(b.next(), Ok(None));
        assert_eq!(b.next(), Ok(None));
        assert_eq!(b.taken(), 0);
        drop(b);
        assert_eq!(t.left(), b"a");
    }
}
