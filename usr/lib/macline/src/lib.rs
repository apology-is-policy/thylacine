//! macline -- the channel framing of the line to the Mac (version `mac1`, docs/MAC-RUNNER.md 4.4).
//!
//! The virtio console port `org.thylacine.mac` is one byte stream. It carries one 9P2000.L
//! session per guest connection to `/srv/mac`, each on its own channel. This module is the
//! format and the channel rules, nothing else: no I/O, no device, no 9P. The guest's
//! virtio-serial driver and the Mac app both build it, so the two ends cannot read the format
//! differently, and its host tests cover both.
//!
//! Every frame is `size[4] type[1] chan[4] data[size-9]`, little-endian, `size` counting the
//! whole frame. HELLO opens a fresh line from each side; OPEN (the driver's) starts a channel;
//! DATA carries the next bytes of a channel's stream; CLOSE says its sender sends no more on
//! the channel, and an id is free again only once both sides have sent it; RESET (the
//! driver's) says the driver found the line malformed.
#![cfg_attr(not(test), no_std)]
extern crate alloc;

use alloc::collections::BTreeMap;
use alloc::vec::Vec;

pub const VERSION: &[u8] = b"mac1";
/// `size[4] type[1] chan[4]`.
pub const HEADER: usize = 9;
/// No side may say it accepts frames smaller than this.
pub const MIN_MAXFRAME: u32 = 4096;

#[derive(Copy, Clone, Debug, PartialEq, Eq)]
pub enum Kind {
    Hello = 0,
    Open = 1,
    Data = 2,
    Close = 3,
    Reset = 4,
}

impl Kind {
    fn from(t: u8) -> Option<Kind> {
        Some(match t {
            0 => Kind::Hello,
            1 => Kind::Open,
            2 => Kind::Data,
            3 => Kind::Close,
            4 => Kind::Reset,
            _ => return None,
        })
    }
}

/// Which end of the line this is. OPEN and RESET are sent only by the driver.
#[derive(Copy, Clone, Debug, PartialEq, Eq)]
pub enum End {
    Driver,
    App,
}

/// Why the line is malformed. Any of these ends the line: the app closes its connection and
/// connects again; the driver sends RESET and discards input until the app goes away.
#[derive(Copy, Clone, Debug, PartialEq, Eq)]
pub enum Malformed {
    /// A type byte that is no kind.
    UnknownType(u8),
    /// A size below the header or above this end's `maxframe`.
    Size(u32),
    /// A kind this end's peer never sends.
    NotThePeers(Kind),
    /// The driver's peer sent something before its HELLO, or a second HELLO came.
    HelloOrder,
    /// A HELLO that does not parse, or not on channel 0.
    Hello,
    /// The peer speaks another version.
    Version,
    /// The peer accepts frames smaller than `MIN_MAXFRAME`.
    MaxFrame(u32),
    /// DATA or CLOSE on a channel that is not open, OPEN on an id in use, channel 0 where a
    /// channel is meant, a body on a frame that has none, or RESET off channel 0.
    Channel(Kind, u32),
}

/// One frame, encoded at the end of `out`. The caller keeps `data` within the peer's
/// `maxframe` (`data_frames` splits a stream).
pub fn put(out: &mut Vec<u8>, kind: Kind, chan: u32, data: &[u8]) {
    let size = (HEADER + data.len()) as u32;
    out.extend_from_slice(&size.to_le_bytes());
    out.push(kind as u8);
    out.extend_from_slice(&chan.to_le_bytes());
    out.extend_from_slice(data);
}

/// This end's HELLO: `version[s] maxframe[4]`, the version a 9P string.
pub fn put_hello(out: &mut Vec<u8>, maxframe: u32) {
    let mut body = Vec::with_capacity(2 + VERSION.len() + 4);
    body.extend_from_slice(&(VERSION.len() as u16).to_le_bytes());
    body.extend_from_slice(VERSION);
    body.extend_from_slice(&maxframe.to_le_bytes());
    put(out, Kind::Hello, 0, &body);
}

/// A channel's bytes as DATA frames no larger than the peer's `maxframe`.
pub fn data_frames(out: &mut Vec<u8>, chan: u32, bytes: &[u8], peer_max: u32) {
    let room = (peer_max as usize).saturating_sub(HEADER).max(1);
    for piece in bytes.chunks(room) {
        put(out, Kind::Data, chan, piece);
    }
}

/// The peer's `maxframe` from a HELLO's body.
pub fn read_hello(body: &[u8]) -> Result<u32, Malformed> {
    if body.len() < 2 {
        return Err(Malformed::Hello);
    }
    let len = u16::from_le_bytes([body[0], body[1]]) as usize;
    if body.len() != 2 + len + 4 {
        return Err(Malformed::Hello);
    }
    if &body[2..2 + len] != VERSION {
        return Err(Malformed::Version);
    }
    let m = &body[2 + len..];
    let maxframe = u32::from_le_bytes([m[0], m[1], m[2], m[3]]);
    if maxframe < MIN_MAXFRAME {
        return Err(Malformed::MaxFrame(maxframe));
    }
    Ok(maxframe)
}

/// Whole frames out of a byte stream that may arrive in any pieces.
pub struct Parser {
    buf: Vec<u8>,
    at: usize,
    max: u32,
}

impl Parser {
    /// `max` is this end's `maxframe`: a larger frame is malformed.
    pub fn new(max: u32) -> Parser {
        Parser { buf: Vec::new(), at: 0, max }
    }

    pub fn push(&mut self, bytes: &[u8]) {
        if self.at > 0 && self.at == self.buf.len() {
            self.buf.clear();
            self.at = 0;
        }
        self.buf.extend_from_slice(bytes);
    }

    /// The next whole frame, or `None` until more bytes come. After an error the parser has
    /// no position in the stream; the caller ends the line and calls `clear`.
    pub fn next(&mut self) -> Result<Option<(Kind, u32, Vec<u8>)>, Malformed> {
        let have = &self.buf[self.at..];
        if have.len() < 4 {
            return Ok(None);
        }
        let size = u32::from_le_bytes([have[0], have[1], have[2], have[3]]);
        if (size as usize) < HEADER || size > self.max {
            return Err(Malformed::Size(size));
        }
        if have.len() < 5 {
            return Ok(None);
        }
        let kind = Kind::from(have[4]).ok_or(Malformed::UnknownType(have[4]))?;
        if have.len() < size as usize {
            return Ok(None);
        }
        let chan = u32::from_le_bytes([have[5], have[6], have[7], have[8]]);
        let data = have[HEADER..size as usize].to_vec();
        self.at += size as usize;
        if self.at == self.buf.len() {
            self.buf.clear();
            self.at = 0;
        }
        Ok(Some((kind, chan, data)))
    }

    /// A fresh line: whatever was buffered belongs to the one before.
    pub fn clear(&mut self) {
        self.buf.clear();
        self.at = 0;
    }

    /// Bytes held that make no whole frame yet.
    pub fn pending(&self) -> usize {
        self.buf.len() - self.at
    }
}

/// A channel as one end sees it. An id stays taken until both sides have sent CLOSE.
#[derive(Copy, Clone, Debug, PartialEq, Eq)]
pub enum Chan {
    Open,
    /// This end sent CLOSE; the peer may still send until its own.
    SentClose,
    /// The peer sent CLOSE; this end owes its own.
    GotClose,
}

/// What a valid frame from the peer means.
#[derive(Debug, PartialEq, Eq)]
pub enum Event {
    /// The peer's HELLO, with the largest frame it accepts.
    Hello(u32),
    /// The app's end only: a frame that came before the driver's HELLO, discarded (a
    /// connection's first bytes may be the end of the last one's).
    Discarded,
    /// The app's end only: a new channel.
    Opened(u32),
    Data(u32, Vec<u8>),
    /// The peer sends no more on the channel. `freed` when this end had sent CLOSE already.
    Closed { chan: u32, freed: bool },
    /// The app's end only: the driver found the line malformed.
    Reset,
}

/// One end's view of the line: the peer's HELLO and the channels.
pub struct Line {
    end: End,
    peer_max: Option<u32>,
    chans: BTreeMap<u32, Chan>,
    next: u32,
}

impl Line {
    pub fn new(end: End) -> Line {
        Line { end, peer_max: None, chans: BTreeMap::new(), next: 1 }
    }

    /// A fresh line (the app connected, or went away): no channels, no HELLO heard.
    pub fn reset(&mut self) {
        self.peer_max = None;
        self.chans.clear();
    }

    /// The peer's `maxframe`, once its HELLO came.
    pub fn peer_max(&self) -> Option<u32> {
        self.peer_max
    }

    pub fn chan(&self, chan: u32) -> Option<Chan> {
        self.chans.get(&chan).copied()
    }

    /// Ids taken: open, or closed by one side only.
    pub fn taken(&self) -> usize {
        self.chans.len()
    }

    /// The driver's end: a new channel's id, never 0 and never one still taken. The driver
    /// bounds its connections far below the id space, so the search ends.
    pub fn open(&mut self) -> u32 {
        loop {
            let id = self.next;
            self.next = self.next.wrapping_add(1);
            if id != 0 && !self.chans.contains_key(&id) {
                self.chans.insert(id, Chan::Open);
                return id;
            }
        }
    }

    /// This end sends CLOSE for `chan`: true when a CLOSE is owed (false once sent, or for an
    /// id not taken). The id is free once both sides have sent it.
    pub fn close(&mut self, chan: u32) -> bool {
        match self.chans.get(&chan).copied() {
            Some(Chan::Open) => {
                self.chans.insert(chan, Chan::SentClose);
                true
            }
            Some(Chan::GotClose) => {
                self.chans.remove(&chan);
                true
            }
            Some(Chan::SentClose) | None => false,
        }
    }

    /// May this end still send DATA on `chan`?
    pub fn sendable(&self, chan: u32) -> bool {
        matches!(self.chans.get(&chan), Some(Chan::Open) | Some(Chan::GotClose))
    }

    /// A frame from the peer, checked against the rules of `mac1`.
    pub fn receive(&mut self, kind: Kind, chan: u32, data: Vec<u8>) -> Result<Event, Malformed> {
        let peers = match self.end {
            End::Driver => !matches!(kind, Kind::Open | Kind::Reset),
            End::App => true,
        };
        if !peers {
            return Err(Malformed::NotThePeers(kind));
        }
        if self.peer_max.is_none() {
            if kind != Kind::Hello {
                return match self.end {
                    End::App => Ok(Event::Discarded),
                    End::Driver => Err(Malformed::HelloOrder),
                };
            }
            if chan != 0 {
                return Err(Malformed::Hello);
            }
            let max = read_hello(&data)?;
            self.peer_max = Some(max);
            return Ok(Event::Hello(max));
        }
        match kind {
            Kind::Hello => Err(Malformed::HelloOrder),
            Kind::Reset => {
                if chan != 0 || !data.is_empty() {
                    return Err(Malformed::Channel(kind, chan));
                }
                Ok(Event::Reset)
            }
            Kind::Open => {
                if chan == 0 || !data.is_empty() || self.chans.contains_key(&chan) {
                    return Err(Malformed::Channel(kind, chan));
                }
                self.chans.insert(chan, Chan::Open);
                Ok(Event::Opened(chan))
            }
            Kind::Data => match self.chans.get(&chan) {
                Some(Chan::Open) | Some(Chan::SentClose) => Ok(Event::Data(chan, data)),
                _ => Err(Malformed::Channel(kind, chan)),
            },
            Kind::Close => {
                if !data.is_empty() {
                    return Err(Malformed::Channel(kind, chan));
                }
                match self.chans.get(&chan).copied() {
                    Some(Chan::Open) => {
                        self.chans.insert(chan, Chan::GotClose);
                        Ok(Event::Closed { chan, freed: false })
                    }
                    Some(Chan::SentClose) => {
                        self.chans.remove(&chan);
                        Ok(Event::Closed { chan, freed: true })
                    }
                    _ => Err(Malformed::Channel(kind, chan)),
                }
            }
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn frames(bytes: &[u8], max: u32) -> Vec<(Kind, u32, Vec<u8>)> {
        let mut p = Parser::new(max);
        p.push(bytes);
        let mut out = Vec::new();
        while let Some(f) = p.next().unwrap() {
            out.push(f);
        }
        assert_eq!(p.pending(), 0);
        out
    }

    fn hello_body(version: &[u8], max: u32) -> Vec<u8> {
        let mut b = (version.len() as u16).to_le_bytes().to_vec();
        b.extend_from_slice(version);
        b.extend_from_slice(&max.to_le_bytes());
        b
    }

    #[test]
    fn a_frame_is_size_type_chan_data_little_endian() {
        let mut out = Vec::new();
        put(&mut out, Kind::Data, 0x0403_0201, b"ab");
        assert_eq!(out, [11, 0, 0, 0, 2, 1, 2, 3, 4, b'a', b'b']);
        let mut h = Vec::new();
        put_hello(&mut h, 0x0001_0000);
        assert_eq!(h, [19, 0, 0, 0, 0, 0, 0, 0, 0, 4, 0, b'm', b'a', b'c', b'1', 0, 0, 1, 0]);
        assert_eq!(read_hello(&h[HEADER..]), Ok(0x0001_0000));
    }

    #[test]
    fn frames_come_whole_from_any_pieces() {
        let mut out = Vec::new();
        put_hello(&mut out, 8192);
        put(&mut out, Kind::Open, 7, b"");
        put(&mut out, Kind::Data, 7, b"Tversion");
        put(&mut out, Kind::Close, 7, b"");
        let want = frames(&out, 8192);
        assert_eq!(want.len(), 4);
        let mut p = Parser::new(8192);
        let mut got = Vec::new();
        for b in &out {
            p.push(core::slice::from_ref(b));
            while let Some(f) = p.next().unwrap() {
                got.push(f);
            }
        }
        assert_eq!(got, want);
        assert_eq!(got[2], (Kind::Data, 7, b"Tversion".to_vec()));
    }

    #[test]
    fn a_size_below_the_header_or_above_maxframe_is_malformed() {
        let mut p = Parser::new(4096);
        p.push(&8u32.to_le_bytes());
        assert_eq!(p.next(), Err(Malformed::Size(8)));
        let mut p = Parser::new(4096);
        p.push(&4097u32.to_le_bytes());
        assert_eq!(p.next(), Err(Malformed::Size(4097)));
        let mut ok = Vec::new();
        put(&mut ok, Kind::Data, 1, &[0; 4096 - HEADER]);
        assert_eq!(frames(&ok, 4096).len(), 1);
    }

    #[test]
    fn an_unknown_type_is_malformed_before_its_body_arrives() {
        let mut p = Parser::new(4096);
        p.push(&[100, 0, 0, 0, 5]);
        assert_eq!(p.next(), Err(Malformed::UnknownType(5)));
    }

    #[test]
    fn a_hello_must_say_mac1_and_accept_4096() {
        assert_eq!(read_hello(&hello_body(b"mac1", 4096)), Ok(4096));
        assert_eq!(read_hello(&hello_body(b"mac2", 4096)), Err(Malformed::Version));
        assert_eq!(read_hello(&hello_body(b"mac1", 4095)), Err(Malformed::MaxFrame(4095)));
        let mut long = hello_body(b"mac1", 4096);
        long.push(0);
        assert_eq!(read_hello(&long), Err(Malformed::Hello));
        assert_eq!(read_hello(&[4]), Err(Malformed::Hello));
        let mut line = Line::new(End::Driver);
        assert_eq!(line.receive(Kind::Hello, 1, hello_body(b"mac1", 4096)), Err(Malformed::Hello));
    }

    #[test]
    fn the_driver_takes_nothing_before_the_apps_hello_and_one_hello_only() {
        let mut d = Line::new(End::Driver);
        assert_eq!(d.receive(Kind::Data, 1, b"x".to_vec()), Err(Malformed::HelloOrder));
        let mut d = Line::new(End::Driver);
        assert_eq!(d.receive(Kind::Hello, 0, hello_body(b"mac1", 65536)), Ok(Event::Hello(65536)));
        assert_eq!(d.peer_max(), Some(65536));
        assert_eq!(d.receive(Kind::Hello, 0, hello_body(b"mac1", 65536)), Err(Malformed::HelloOrder));
    }

    #[test]
    fn the_app_discards_frames_until_the_drivers_hello() {
        let mut a = Line::new(End::App);
        assert_eq!(a.receive(Kind::Data, 9, b"stale".to_vec()), Ok(Event::Discarded));
        assert_eq!(a.receive(Kind::Close, 9, Vec::new()), Ok(Event::Discarded));
        assert_eq!(a.receive(Kind::Hello, 0, hello_body(b"mac1", 4096)), Ok(Event::Hello(4096)));
        assert_eq!(a.receive(Kind::Open, 1, Vec::new()), Ok(Event::Opened(1)));
    }

    #[test]
    fn open_and_reset_are_the_drivers_alone() {
        let mut d = Line::new(End::Driver);
        d.receive(Kind::Hello, 0, hello_body(b"mac1", 4096)).unwrap();
        assert_eq!(d.receive(Kind::Open, 3, Vec::new()), Err(Malformed::NotThePeers(Kind::Open)));
        assert_eq!(d.receive(Kind::Reset, 0, Vec::new()), Err(Malformed::NotThePeers(Kind::Reset)));
        let mut a = Line::new(End::App);
        a.receive(Kind::Hello, 0, hello_body(b"mac1", 4096)).unwrap();
        assert_eq!(a.receive(Kind::Reset, 0, Vec::new()), Ok(Event::Reset));
        assert_eq!(a.receive(Kind::Reset, 1, Vec::new()), Err(Malformed::Channel(Kind::Reset, 1)));
        assert_eq!(a.receive(Kind::Reset, 0, b"x".to_vec()), Err(Malformed::Channel(Kind::Reset, 0)));
        let mut r = Vec::new();
        put(&mut r, Kind::Reset, 0, b"");
        assert_eq!(frames(&r, 4096), [(Kind::Reset, 0, Vec::new())], "type 4 is RESET on the wire");
    }

    #[test]
    fn an_id_is_free_only_once_both_sides_closed_it() {
        // The guest hangs up first: the driver closes, the app may still send, then closes.
        let mut d = Line::new(End::Driver);
        d.receive(Kind::Hello, 0, hello_body(b"mac1", 4096)).unwrap();
        let c = d.open();
        assert_eq!(c, 1);
        assert!(d.close(c));
        assert!(!d.close(c), "one CLOSE per side");
        assert!(!d.sendable(c));
        assert_eq!(d.receive(Kind::Data, c, b"late".to_vec()), Ok(Event::Data(c, b"late".to_vec())));
        assert_eq!(d.receive(Kind::Close, c, Vec::new()), Ok(Event::Closed { chan: c, freed: true }));
        assert_eq!(d.taken(), 0);
        // The app closes first: no more data from it, and the driver owes its own CLOSE.
        let c = d.open();
        assert_eq!(d.receive(Kind::Close, c, Vec::new()), Ok(Event::Closed { chan: c, freed: false }));
        assert_eq!(d.receive(Kind::Data, c, b"x".to_vec()), Err(Malformed::Channel(Kind::Data, c)));
        assert!(d.sendable(c));
        assert!(d.close(c));
        assert_eq!(d.taken(), 0);
    }

    #[test]
    fn data_or_close_on_a_channel_not_open_and_open_on_a_taken_id_are_malformed() {
        let mut a = Line::new(End::App);
        a.receive(Kind::Hello, 0, hello_body(b"mac1", 4096)).unwrap();
        assert_eq!(a.receive(Kind::Data, 5, b"x".to_vec()), Err(Malformed::Channel(Kind::Data, 5)));
        assert_eq!(a.receive(Kind::Close, 5, Vec::new()), Err(Malformed::Channel(Kind::Close, 5)));
        assert_eq!(a.receive(Kind::Open, 0, Vec::new()), Err(Malformed::Channel(Kind::Open, 0)));
        assert_eq!(a.receive(Kind::Open, 5, b"x".to_vec()), Err(Malformed::Channel(Kind::Open, 5)));
        a.receive(Kind::Open, 5, Vec::new()).unwrap();
        assert_eq!(a.receive(Kind::Open, 5, Vec::new()), Err(Malformed::Channel(Kind::Open, 5)));
        assert!(a.close(5));
        // Closed by the app only: the id is still taken until the driver's CLOSE.
        assert_eq!(a.receive(Kind::Open, 5, Vec::new()), Err(Malformed::Channel(Kind::Open, 5)));
        assert_eq!(a.receive(Kind::Close, 5, b"x".to_vec()), Err(Malformed::Channel(Kind::Close, 5)));
        assert_eq!(a.receive(Kind::Close, 5, Vec::new()), Ok(Event::Closed { chan: 5, freed: true }));
        assert_eq!(a.receive(Kind::Close, 5, Vec::new()), Err(Malformed::Channel(Kind::Close, 5)));
    }

    #[test]
    fn the_driver_never_hands_out_0_or_a_taken_id() {
        let mut d = Line::new(End::Driver);
        d.next = u32::MAX;
        assert_eq!(d.open(), u32::MAX);
        assert_eq!(d.open(), 1, "0 is skipped at the wrap");
        d.next = 1;
        assert_eq!(d.open(), 2, "1 is still taken");
    }

    #[test]
    fn a_fresh_line_forgets_channels_and_the_peers_hello() {
        let mut d = Line::new(End::Driver);
        d.receive(Kind::Hello, 0, hello_body(b"mac1", 4096)).unwrap();
        d.open();
        d.reset();
        assert_eq!(d.taken(), 0);
        assert_eq!(d.peer_max(), None);
        assert_eq!(d.receive(Kind::Data, 1, b"x".to_vec()), Err(Malformed::HelloOrder));
        let mut p = Parser::new(4096);
        p.push(&[20, 0, 0]);
        p.clear();
        assert_eq!(p.pending(), 0);
    }

    #[test]
    fn a_stream_is_split_to_the_peers_maxframe() {
        let mut out = Vec::new();
        let bytes: Vec<u8> = (0..10_000u32).map(|i| i as u8).collect();
        data_frames(&mut out, 3, &bytes, 4096);
        let got = frames(&out, 4096);
        assert_eq!(got.len(), 3);
        assert!(got.iter().all(|(k, c, d)| *k == Kind::Data && *c == 3 && d.len() <= 4096 - HEADER));
        let joined: Vec<u8> = got.into_iter().flat_map(|(_, _, d)| d).collect();
        assert_eq!(joined, bytes);
    }
}
