//! The driver's end of the line to the Mac: the rules of docs/MAC-RUNNER.md 4.4 over guest
//! connections the caller names by a key (the connection's handle). No I/O: the caller says
//! what the device and the connections did, sends the frames it is given, writes each
//! connection's pending bytes, and hangs up the connections it is told to.

use alloc::collections::{BTreeMap, VecDeque};
use alloc::vec::Vec;

use macline::{put, put_hello, End, Event, Kind, Line, Malformed, Parser, MIN_MAXFRAME};

/// A guest connection's handle.
pub type Key = i64;

/// The most bytes held for one guest connection that is not reading: past it the channel
/// closes, so one connection never stalls the others.
pub const PENDING_MAX: usize = 256 * 1024;

/// Guest connections at once.
pub const MAX_CONNS: usize = 32;

#[derive(Copy, Clone, Debug, PartialEq, Eq)]
pub enum State {
    /// No app on the port.
    Down,
    /// The app is on the port; this end's HELLO is queued and the app's has not come.
    Hello,
    /// Both HELLOs: guest connections are taken.
    Up,
    /// This end found the line malformed and sent RESET; input is discarded until the app
    /// goes away.
    Reset,
}

struct Conn {
    chan: u32,
    /// The app's bytes not yet written to the guest connection.
    pending: VecDeque<u8>,
}

pub struct Relay {
    state: State,
    line: Line,
    parser: Parser,
    /// This end's `maxframe`: the largest frame the app may send.
    max: u32,
    /// The largest frame this end makes: what one transmit buffer holds.
    frame_max: u32,
    conns: BTreeMap<Key, Conn>,
    chans: BTreeMap<u32, Key>,
    /// Whole frames toward the app, in order.
    out: VecDeque<Vec<u8>>,
    out_bytes: usize,
    hangups: Vec<Key>,
}

impl Relay {
    pub fn new(max: u32, frame_max: u32) -> Relay {
        assert!(max >= MIN_MAXFRAME && frame_max >= MIN_MAXFRAME);
        Relay {
            state: State::Down,
            line: Line::new(End::Driver),
            parser: Parser::new(max),
            max,
            frame_max,
            conns: BTreeMap::new(),
            chans: BTreeMap::new(),
            out: VecDeque::new(),
            out_bytes: 0,
            hangups: Vec::new(),
        }
    }

    pub fn state(&self) -> State {
        self.state
    }

    pub fn conns(&self) -> usize {
        self.conns.len()
    }

    /// The app connected (PORT_OPEN 1): a fresh line, and this end's HELLO before anything.
    pub fn host_open(&mut self) {
        self.drop_line();
        self.state = State::Hello;
        let mut f = Vec::new();
        put_hello(&mut f, self.max);
        self.push(f);
    }

    /// The app went away (PORT_OPEN 0): every channel closes. The caller discards what the
    /// receive queue had delivered before the event.
    pub fn host_closed(&mut self) {
        self.drop_line();
        self.state = State::Down;
    }

    /// Bytes the device received from the app. A malformed line is answered with RESET and
    /// returned, for the caller to say.
    pub fn rx(&mut self, bytes: &[u8]) -> Option<Malformed> {
        if matches!(self.state, State::Down | State::Reset) {
            return None;
        }
        self.parser.push(bytes);
        loop {
            let why = match self.parser.next() {
                Ok(None) => return None,
                Ok(Some((kind, chan, data))) => match self.line.receive(kind, chan, data) {
                    Ok(ev) => {
                        self.event(ev);
                        continue;
                    }
                    Err(why) => why,
                },
                Err(why) => why,
            };
            self.drop_line();
            self.state = State::Reset;
            let mut f = Vec::new();
            put(&mut f, Kind::Reset, 0, &[]);
            self.push(f);
            return Some(why);
        }
    }

    fn event(&mut self, ev: Event) {
        match ev {
            Event::Hello(_) => self.state = State::Up,
            Event::Data(chan, data) => {
                // No key: the guest's end hung up and this end sent CLOSE; the bytes have
                // nowhere to go.
                if let Some(&key) = self.chans.get(&chan) {
                    let c = self.conns.get_mut(&key).expect("a channel's key names a connection");
                    if c.pending.len() + data.len() > PENDING_MAX {
                        self.close(key);
                    } else {
                        c.pending.extend(data);
                    }
                }
            }
            Event::Closed { chan, .. } => {
                // The app closed first: the guest's connection hangs up, and this end owes
                // its CLOSE.
                if let Some(key) = self.chans.remove(&chan) {
                    self.conns.remove(&key);
                    self.hangups.push(key);
                    if self.line.close(chan) {
                        self.frame(Kind::Close, chan, &[]);
                    }
                }
            }
            // The app's end only; Line refuses them at the driver's.
            Event::Opened(_) | Event::Reset | Event::Discarded => {}
        }
    }

    /// A guest connection was accepted: true when it has a channel. False while the line is
    /// not up or at MAX_CONNS; the caller hangs it up.
    pub fn accept(&mut self, key: Key) -> bool {
        if self.state != State::Up || self.conns.len() >= MAX_CONNS || self.conns.contains_key(&key) {
            return false;
        }
        let chan = self.line.open();
        self.conns.insert(key, Conn { chan, pending: VecDeque::new() });
        self.chans.insert(chan, key);
        self.frame(Kind::Open, chan, &[]);
        true
    }

    /// Bytes a guest connection wrote, as DATA frames on its channel.
    pub fn guest_data(&mut self, key: Key, bytes: &[u8]) {
        let Some(chan) = self.conns.get(&key).map(|c| c.chan) else { return };
        let peer = self.line.peer_max().unwrap_or(MIN_MAXFRAME);
        let room = (peer.min(self.frame_max) as usize) - macline::HEADER;
        for piece in bytes.chunks(room) {
            self.frame(Kind::Data, chan, piece);
        }
    }

    /// A guest connection hung up (or failed): its channel closes and the caller closes the
    /// handle.
    pub fn guest_hangup(&mut self, key: Key) {
        self.close(key);
    }

    fn close(&mut self, key: Key) {
        if let Some(c) = self.conns.remove(&key) {
            self.chans.remove(&c.chan);
            self.hangups.push(key);
            if self.line.close(c.chan) {
                self.frame(Kind::Close, c.chan, &[]);
            }
        }
    }

    /// The app's bytes waiting for a guest connection.
    pub fn pending(&mut self, key: Key) -> &[u8] {
        match self.conns.get_mut(&key) {
            Some(c) => c.pending.make_contiguous(),
            None => &[],
        }
    }

    /// `n` of a connection's pending bytes were written.
    pub fn wrote(&mut self, key: Key, n: usize) {
        if let Some(c) = self.conns.get_mut(&key) {
            let n = n.min(c.pending.len());
            c.pending.drain(..n);
        }
    }

    /// The connections to hang up, each once.
    pub fn take_hangups(&mut self) -> Vec<Key> {
        core::mem::take(&mut self.hangups)
    }

    /// Bytes of frames queued toward the app.
    pub fn out_bytes(&self) -> usize {
        self.out_bytes
    }

    /// The next frame toward the app, if it fits in `room` bytes. Frames are never split, so
    /// each transmit buffer holds whole frames.
    pub fn next_frame(&mut self, room: usize) -> Option<Vec<u8>> {
        if self.out.front()?.len() > room {
            return None;
        }
        let f = self.out.pop_front()?;
        self.out_bytes -= f.len();
        Some(f)
    }

    fn frame(&mut self, kind: Kind, chan: u32, data: &[u8]) {
        let mut f = Vec::with_capacity(macline::HEADER + data.len());
        put(&mut f, kind, chan, data);
        self.push(f);
    }

    fn push(&mut self, f: Vec<u8>) {
        self.out_bytes += f.len();
        self.out.push_back(f);
    }

    fn drop_line(&mut self) {
        self.hangups.extend(self.conns.keys().copied());
        self.conns.clear();
        self.chans.clear();
        self.line.reset();
        self.parser.clear();
        self.out.clear();
        self.out_bytes = 0;
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    const MAX: u32 = 16384;
    const FRAME: u32 = 4096;

    fn app(kind: Kind, chan: u32, data: &[u8]) -> Vec<u8> {
        let mut f = Vec::new();
        put(&mut f, kind, chan, data);
        f
    }

    fn app_hello(max: u32) -> Vec<u8> {
        let mut f = Vec::new();
        put_hello(&mut f, max);
        f
    }

    fn frames(r: &mut Relay) -> Vec<(u8, u32, Vec<u8>)> {
        let mut out = Vec::new();
        while let Some(f) = r.next_frame(usize::MAX) {
            let size = u32::from_le_bytes([f[0], f[1], f[2], f[3]]) as usize;
            assert_eq!(size, f.len(), "one whole frame per item");
            out.push((f[4], u32::from_le_bytes([f[5], f[6], f[7], f[8]]), f[9..].to_vec()));
        }
        out
    }

    /// The app connected and both HELLOs went; the frames so far are taken.
    fn up() -> Relay {
        let mut r = Relay::new(MAX, FRAME);
        r.host_open();
        assert_eq!(r.rx(&app_hello(65536)), None);
        assert_eq!(r.state(), State::Up);
        frames(&mut r);
        r
    }

    #[test]
    fn a_fresh_line_starts_with_this_ends_hello_and_takes_no_connection_before_the_apps() {
        let mut r = Relay::new(MAX, FRAME);
        assert!(!r.accept(5), "no app");
        r.host_open();
        let f = frames(&mut r);
        assert_eq!(f.len(), 1);
        assert_eq!(f[0].0, Kind::Hello as u8);
        assert_eq!(macline::read_hello(&f[0].2), Ok(MAX));
        assert!(!r.accept(5), "the app's HELLO has not come");
        r.rx(&app_hello(4096));
        assert!(r.accept(5));
        assert_eq!(frames(&mut r), [(Kind::Open as u8, 1, Vec::new())]);
    }

    #[test]
    fn a_guests_bytes_go_as_data_no_larger_than_either_end_allows() {
        let mut r = up();
        r.accept(7);
        frames(&mut r);
        let bytes: Vec<u8> = (0..10_000u32).map(|i| i as u8).collect();
        r.guest_data(7, &bytes);
        let f = frames(&mut r);
        assert_eq!(f.len(), 3, "the app takes 65536 but this end's buffer is 4096");
        assert!(f.iter().all(|(k, c, d)| *k == Kind::Data as u8 && *c == 1 && d.len() <= 4096 - 9));
        assert_eq!(f.into_iter().flat_map(|(_, _, d)| d).collect::<Vec<u8>>(), bytes);
        // An app that takes less than this end's buffer bounds the frames instead.
        let mut r = Relay::new(MAX, 65536);
        r.host_open();
        r.rx(&app_hello(4096));
        r.accept(1);
        frames(&mut r);
        r.guest_data(1, &[0; 5000]);
        assert!(frames(&mut r).iter().all(|(_, _, d)| d.len() <= 4096 - 9));
    }

    #[test]
    fn the_apps_bytes_wait_for_their_connection() {
        let mut r = up();
        r.accept(7);
        r.accept(8);
        frames(&mut r);
        r.rx(&app(Kind::Data, 2, b"Rversion"));
        r.rx(&app(Kind::Data, 1, b"Rattach"));
        assert_eq!(r.pending(7), b"Rattach");
        assert_eq!(r.pending(8), b"Rversion");
        r.wrote(8, 4);
        assert_eq!(r.pending(8), b"sion");
        r.wrote(8, 99);
        assert_eq!(r.pending(8), b"");
        assert_eq!(r.pending(99), b"", "no such connection");
    }

    #[test]
    fn a_guest_hangup_closes_the_channel_and_the_id_frees_at_the_apps_close() {
        let mut r = up();
        r.accept(7);
        frames(&mut r);
        r.guest_hangup(7);
        assert_eq!(r.take_hangups(), [7]);
        assert_eq!(frames(&mut r), [(Kind::Close as u8, 1, Vec::new())]);
        // The app may still send until its CLOSE; the bytes have nowhere to go.
        assert_eq!(r.rx(&app(Kind::Data, 1, b"late")), None);
        assert_eq!(r.rx(&app(Kind::Close, 1, &[])), None);
        assert_eq!(frames(&mut r), [], "this end's CLOSE went already");
        assert!(r.take_hangups().is_empty());
        r.accept(9);
        assert_eq!(frames(&mut r), [(Kind::Open as u8, 2, Vec::new())]);
    }

    #[test]
    fn the_apps_close_hangs_up_the_connection_and_this_end_answers_it() {
        let mut r = up();
        r.accept(7);
        frames(&mut r);
        r.rx(&app(Kind::Close, 1, &[]));
        assert_eq!(r.take_hangups(), [7]);
        assert_eq!(frames(&mut r), [(Kind::Close as u8, 1, Vec::new())]);
        assert_eq!(r.conns(), 0);
        r.guest_hangup(7);
        assert!(r.take_hangups().is_empty(), "hung up once");
        assert_eq!(frames(&mut r), []);
    }

    #[test]
    fn half_a_frame_from_the_app_before_it_went_is_not_read_as_the_next_ones() {
        let mut r = up();
        let f = app(Kind::Data, 1, b"cut short");
        assert_eq!(r.rx(&f[..5]), None);
        r.host_closed();
        r.host_open();
        assert_eq!(r.rx(&app_hello(4096)), None, "the next app's HELLO parses from its first byte");
        assert_eq!(r.state(), State::Up);
    }

    #[test]
    fn a_connection_that_stops_reading_is_closed_and_the_others_go_on() {
        let mut r = up();
        r.accept(7);
        r.accept(8);
        frames(&mut r);
        let piece = [0u8; 8192];
        for _ in 0..PENDING_MAX / piece.len() {
            assert_eq!(r.rx(&app(Kind::Data, 1, &piece)), None);
        }
        assert_eq!(r.pending(7).len(), PENDING_MAX);
        assert!(r.take_hangups().is_empty(), "exactly the bound is held");
        assert_eq!(r.rx(&app(Kind::Data, 1, b"x")), None);
        assert_eq!(r.take_hangups(), [7]);
        assert_eq!(frames(&mut r), [(Kind::Close as u8, 1, Vec::new())]);
        r.rx(&app(Kind::Data, 2, b"still here"));
        assert_eq!(r.pending(8), b"still here");
        assert_eq!(r.state(), State::Up);
    }

    #[test]
    fn the_app_going_away_hangs_up_every_connection_and_forgets_the_line() {
        let mut r = up();
        r.accept(7);
        r.accept(8);
        r.guest_data(7, b"Tclunk");
        r.host_closed();
        let mut h = r.take_hangups();
        h.sort();
        assert_eq!(h, [7, 8]);
        assert_eq!(r.out_bytes(), 0);
        assert_eq!(frames(&mut r), []);
        assert_eq!(r.state(), State::Down);
        assert_eq!(r.rx(&app(Kind::Data, 1, b"stale")), None, "nothing is read while no app is there");
        assert!(!r.accept(9));
        // The next app gets a fresh line: HELLO first, and the parser holds nothing stale.
        r.rx(&[1, 2, 3]);
        r.host_open();
        assert_eq!(frames(&mut r)[0].0, Kind::Hello as u8);
        r.rx(&app_hello(4096));
        assert_eq!(r.state(), State::Up);
    }

    #[test]
    fn a_second_open_without_a_close_is_a_fresh_line() {
        let mut r = up();
        r.accept(7);
        r.host_open();
        assert_eq!(r.take_hangups(), [7]);
        assert_eq!(r.state(), State::Hello);
        assert_eq!(frames(&mut r).len(), 1);
    }

    #[test]
    fn a_malformed_line_is_reset_and_input_is_discarded_until_the_app_goes() {
        for bad in [
            app(Kind::Data, 1, b"before the hello"),
            app(Kind::Open, 3, &[]),
            {
                let mut f = app(Kind::Data, 1, b"x");
                f[4] = 9;
                f
            },
            8u32.to_le_bytes().to_vec(),
            (MAX + 1).to_le_bytes().to_vec(),
        ] {
            let mut r = Relay::new(MAX, FRAME);
            r.host_open();
            frames(&mut r);
            assert!(r.rx(&bad).is_some(), "{:?}", bad);
            assert_eq!(r.state(), State::Reset);
            assert_eq!(frames(&mut r), [(Kind::Reset as u8, 0, Vec::new())]);
            assert_eq!(r.rx(&app_hello(4096)), None, "discarded");
            assert_eq!(r.state(), State::Reset);
            assert!(!r.accept(1));
            r.host_closed();
            r.host_open();
            r.rx(&app_hello(4096));
            assert_eq!(r.state(), State::Up, "the next connection starts clean");
        }
        // Up, then a frame on a channel never opened.
        let mut r = up();
        r.accept(7);
        assert_eq!(r.rx(&app(Kind::Data, 5, b"x")), Some(Malformed::Channel(Kind::Data, 5)));
        assert_eq!(r.take_hangups(), [7]);
        assert_eq!(frames(&mut r), [(Kind::Reset as u8, 0, Vec::new())]);
    }

    #[test]
    fn frames_are_handed_out_whole_or_not_at_all() {
        let mut r = up();
        r.accept(7);
        r.accept(8);
        assert_eq!(r.out_bytes(), 18);
        assert_eq!(r.next_frame(8), None, "a 9-byte frame does not fit in 8");
        assert_eq!(r.next_frame(9).map(|f| f.len()), Some(9));
        assert_eq!(r.out_bytes(), 9);
        assert!(r.next_frame(9).is_some());
        assert_eq!(r.next_frame(9), None);
        assert_eq!(r.out_bytes(), 0);
    }

    #[test]
    fn no_more_than_max_conns_and_no_key_twice() {
        let mut r = up();
        for k in 0..MAX_CONNS as Key {
            assert!(r.accept(k));
        }
        assert!(!r.accept(MAX_CONNS as Key));
        r.guest_hangup(0);
        assert!(!r.accept(1), "a key held is not taken twice");
        assert!(r.accept(100));
    }

    #[test]
    fn bytes_for_a_connection_that_is_gone_are_dropped() {
        let mut r = up();
        r.guest_data(42, b"no such connection");
        assert_eq!(frames(&mut r), []);
    }
}
