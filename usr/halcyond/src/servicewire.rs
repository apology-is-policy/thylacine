//! Bounded nonblocking 9P stream pump, shared by the media service adapters.
//!
//! The handler owns ONE reply buffer, never changed until its last byte is
//! accepted. The pump owns ONE input buffer (32 KiB ceiling), and dispatches
//! directly from it: no second full-frame copy. Already-buffered frames are
//! runnable work even when the descriptor is no longer readable. Each turn
//! has frame, byte and monotonic deadline limits; WouldBlock always returns
//! to the event loop. These are transport facts, not admission decisions.

use alloc::vec::Vec;

pub const MAX_FRAME: usize = 32768;
pub const FRAMES_PER_TURN: usize = 8;
pub const BYTES_PER_TURN: usize = 64 * 1024;
const READ_CHUNK: usize = 4096;

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum IoError {
    Again,
    Closed,
}

pub trait Endpoint {
    /// Always nonblocking. Zero read is EOF; zero write is an error.
    fn read(&mut self, dst: &mut [u8]) -> Result<usize, IoError>;
    fn write(&mut self, src: &[u8]) -> Result<usize, IoError>;
    fn now_ns(&self) -> u64;
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Dispatch {
    Reply,
    Park(u64),
    Cancel(u64),
}

pub trait Handler {
    /// Dispatch one bounded frame; a parked request leaves the reply empty.
    fn dispatch(&mut self, request: &[u8]) -> Result<Dispatch, ()>;
    fn reply(&self) -> &[u8];
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Interest {
    Read,
    Write,
}

#[derive(Default)]
pub struct Stream {
    input: Vec<u8>,
    sent: usize,
    reply_len: usize,
    parked: u64,
    last_park: u64,
    closed: bool,
}

impl Stream {
    pub fn new() -> Self {
        Self::default()
    }

    /// Build output only after its ticket and slot are known to be current.
    pub fn resume_reply<H: Handler>(
        &mut self,
        ticket: u64,
        handler: &mut H,
        build: impl FnOnce(&mut H) -> Result<(), ()>,
    ) -> Result<(), ()> {
        if self.closed || ticket == 0 || self.parked != ticket || self.reply_len != 0 {
            return Err(());
        }
        if build(handler).is_err() || !(7..=MAX_FRAME).contains(&handler.reply().len()) {
            self.closed = true;
            return Err(());
        }
        self.reply_len = handler.reply().len();
        self.parked = 0;
        Ok(())
    }

    /// A partial frame poisons the connection; its suffix must never be reused.
    pub fn cancel_output(&mut self) -> bool {
        self.closed |= self.sent != 0;
        self.input.clear();
        self.reply_len = 0;
        self.parked = 0;
        self.sent = 0;
        !self.closed
    }

    pub fn interest(&self) -> Interest {
        if self.reply_len != 0 {
            Interest::Write
        } else {
            Interest::Read
        }
    }

    /// A complete (or malformed) buffered header needs a UI turn. Partial
    /// frames wait for READ; pending replies wait for WRITE. No timer polling.
    pub fn runnable(&self) -> bool {
        self.closed || (self.reply_len == 0 && !matches!(self.frame_len(), Ok(None)))
    }

    fn frame_len(&self) -> Result<Option<usize>, ()> {
        if self.input.len() < 4 {
            return Ok(None);
        }
        let len = u32::from_le_bytes(self.input[..4].try_into().unwrap()) as usize;
        if !(7..=MAX_FRAME).contains(&len) {
            return Err(());
        }
        Ok((self.input.len() >= len).then_some(len))
    }

    /// The caller supplies a shared service-pass deadline. No I/O or dispatch
    /// starts after it; an individual handler must itself have bounded work.
    /// false means close this connection and discard its uncommitted state.
    pub fn service(
        &mut self,
        io: &mut impl Endpoint,
        handler: &mut impl Handler,
        deadline_ns: u64,
    ) -> bool {
        if self.closed {
            return false;
        }
        let mut frames = 0;
        let mut bytes = 0;
        loop {
            if bytes == BYTES_PER_TURN || io.now_ns() >= deadline_ns {
                return true;
            }
            if self.reply_len != 0 {
                let reply = handler.reply();
                if reply.len() != self.reply_len {
                    return false;
                }
                let count = (self.reply_len - self.sent).min(BYTES_PER_TURN - bytes);
                match io.write(&reply[self.sent..self.sent + count]) {
                    Ok(n) if n > 0 && n <= count => {
                        self.sent += n;
                        bytes += n;
                        if self.sent == self.reply_len {
                            self.sent = 0;
                            self.reply_len = 0;
                        }
                    }
                    Err(IoError::Again) => return true,
                    _ => return false,
                }
                continue;
            }
            if frames == FRAMES_PER_TURN {
                return true;
            }
            match self.frame_len() {
                Err(()) => return false,
                Ok(Some(len)) => {
                    let Ok(action) = handler.dispatch(&self.input[..len]) else {
                        return false;
                    };
                    let reply_len = handler.reply().len();
                    match action {
                        Dispatch::Park(ticket) => {
                            if ticket == 0
                                || ticket <= self.last_park
                                || self.parked != 0
                                || reply_len != 0
                            {
                                return false;
                            }
                            self.parked = ticket;
                            self.last_park = ticket;
                        }
                        Dispatch::Reply | Dispatch::Cancel(_) => {
                            if !(7..=MAX_FRAME).contains(&reply_len) {
                                return false;
                            }
                            if let Dispatch::Cancel(ticket) = action {
                                if ticket == 0 || self.parked != ticket {
                                    return false;
                                }
                                self.parked = 0;
                            }
                            self.reply_len = reply_len;
                        }
                    }
                    self.input.drain(..len);
                    frames += 1;
                }
                Ok(None) => {
                    let len = self.input.len();
                    let count = (MAX_FRAME - len)
                        .min(READ_CHUNK)
                        .min(BYTES_PER_TURN - bytes);
                    if count == 0 || self.input.try_reserve_exact(count).is_err() {
                        return false;
                    }
                    self.input.resize(len + count, 0);
                    let result = io.read(&mut self.input[len..]);
                    match result {
                        Ok(n) if n > 0 && n <= count => {
                            self.input.truncate(len + n);
                            bytes += n;
                        }
                        Err(IoError::Again) => {
                            self.input.truncate(len);
                            return true;
                        }
                        _ => {
                            self.input.truncate(len);
                            return false;
                        }
                    }
                }
            }
        }
    }
}

// Payload allocations are bounded independently of this fixed metadata.
const _: () = assert!(core::mem::size_of::<Stream>() <= 64);

#[cfg(test)]
mod tests {
    use super::*;
    use alloc::collections::VecDeque;
    use alloc::vec;
    use core::cell::Cell;

    #[derive(Default)]
    struct Peer {
        input: VecDeque<u8>,
        output: Vec<u8>,
        credit: usize,
        read_chunk: usize,
        write_chunk: usize,
        eof: bool,
        clock: Cell<u64>,
        tick: u64,
        calls: usize,
    }
    impl Peer {
        fn new(input: Vec<u8>) -> Self {
            Self {
                input: input.into(),
                credit: usize::MAX,
                read_chunk: MAX_FRAME,
                write_chunk: MAX_FRAME,
                ..Self::default()
            }
        }
    }
    impl Endpoint for Peer {
        fn read(&mut self, dst: &mut [u8]) -> Result<usize, IoError> {
            self.calls += 1;
            if self.input.is_empty() {
                return if self.eof { Ok(0) } else { Err(IoError::Again) };
            }
            let n = dst.len().min(self.read_chunk).min(self.input.len());
            for d in &mut dst[..n] {
                *d = self.input.pop_front().unwrap();
            }
            Ok(n)
        }
        fn write(&mut self, src: &[u8]) -> Result<usize, IoError> {
            self.calls += 1;
            let n = src.len().min(self.credit).min(self.write_chunk);
            if n == 0 {
                return Err(IoError::Again);
            }
            self.output.extend_from_slice(&src[..n]);
            self.credit -= n;
            Ok(n)
        }
        fn now_ns(&self) -> u64 {
            let t = self.clock.get();
            self.clock.set(t + self.tick);
            t
        }
    }
    #[derive(Default)]
    struct Echo {
        reply: Vec<u8>,
        seen: Vec<u8>,
    }
    impl Handler for Echo {
        fn dispatch(&mut self, req: &[u8]) -> Result<Dispatch, ()> {
            self.seen.push(req[4]);
            self.reply = req.to_vec();
            Ok(Dispatch::Reply)
        }
        fn reply(&self) -> &[u8] {
            &self.reply
        }
    }
    fn frame(id: u8, n: usize) -> Vec<u8> {
        let mut v = vec![id; n];
        v[..4].copy_from_slice(&(n as u32).to_le_bytes());
        v
    }
    struct Deferred {
        actions: VecDeque<Dispatch>,
        bytes: Vec<u8>,
        seen: Vec<u8>,
    }
    impl Handler for Deferred {
        fn dispatch(&mut self, req: &[u8]) -> Result<Dispatch, ()> {
            self.seen.push(req[4]);
            let action = self.actions.pop_front().ok_or(())?;
            self.bytes = if matches!(action, Dispatch::Park(_)) {
                Vec::new()
            } else {
                req.to_vec()
            };
            Ok(action)
        }
        fn reply(&self) -> &[u8] {
            &self.bytes
        }
    }
    fn deferred(actions: &[Dispatch]) -> Deferred {
        Deferred {
            actions: actions.iter().copied().collect(),
            bytes: Vec::new(),
            seen: Vec::new(),
        }
    }
    #[test]
    fn parked_read_allows_flush_and_stale_resumption_cannot_replace_output() {
        let mut s = Stream::new();
        let mut h = deferred(&[Dispatch::Park(1), Dispatch::Cancel(1)]);
        let mut p = Peer::new([frame(1, 7), frame(2, 7)].concat());
        p.credit = 0;
        assert!(s.service(&mut p, &mut h, u64::MAX));
        assert_eq!(h.seen, [1, 2]);
        assert_eq!(s.interest(), Interest::Write);
        let original = h.bytes.clone();
        let mut built = false;
        assert!(s
            .resume_reply(1, &mut h, |_| {
                built = true;
                Ok(())
            })
            .is_err());
        assert!(!built);
        assert_eq!(h.bytes, original);
        p.credit = usize::MAX;
        assert!(s.service(&mut p, &mut h, u64::MAX));
        assert_eq!(p.output, frame(2, 7));
    }
    #[test]
    fn exact_park_resumes_only_after_immediate_reply_drains() {
        let mut s = Stream::new();
        let mut h = deferred(&[Dispatch::Park(1), Dispatch::Reply]);
        let mut p = Peer::new([frame(1, 7), frame(2, 7)].concat());
        p.credit = 0;
        assert!(s.service(&mut p, &mut h, u64::MAX));
        let mut built = false;
        assert!(s
            .resume_reply(1, &mut h, |_| {
                built = true;
                Ok(())
            })
            .is_err());
        assert!(!built);
        p.credit = usize::MAX;
        assert!(s.service(&mut p, &mut h, u64::MAX));
        assert!(!s.runnable());
        assert!(s
            .resume_reply(2, &mut h, |_| {
                built = true;
                Ok(())
            })
            .is_err());
        assert!(!built);
        assert_eq!(
            s.resume_reply(1, &mut h, |h| {
                h.bytes = frame(9, 9);
                Ok(())
            }),
            Ok(())
        );
        assert_eq!(s.interest(), Interest::Write);
        assert!(s
            .resume_reply(1, &mut h, |_| {
                built = true;
                Ok(())
            })
            .is_err());
        assert!(!built);
        assert!(s.service(&mut p, &mut h, u64::MAX));
        assert_eq!(p.output, [frame(2, 7), frame(9, 9)].concat());
    }
    #[test]
    fn cancelled_output_discards_buffered_input_and_partial_frames_poison() {
        for credit in 0..9 {
            let mut s = Stream::new();
            let mut h = Echo::default();
            let mut p = Peer::new([frame(1, 9), frame(2, 7)].concat());
            p.credit = credit;
            assert!(s.service(&mut p, &mut h, u64::MAX));
            assert_eq!(h.seen, [1]);
            assert_eq!(s.cancel_output(), credit == 0);
            p.credit = usize::MAX;
            assert_eq!(s.service(&mut p, &mut h, u64::MAX), credit == 0);
            assert_eq!(h.seen, [1]);
            assert_eq!(p.output, frame(1, 9)[..credit]);
        }
        let mut s = Stream::new();
        let mut h = Echo::default();
        let mut p = Peer::new(frame(1, 9));
        assert!(s.service(&mut p, &mut h, u64::MAX));
        assert!(s.cancel_output()); // A fully delivered frame needs no suffix.
        assert_eq!(p.output, frame(1, 9));
    }
    #[test]
    fn cancellation_preserves_ticket_monotonicity_and_drops_old_park() {
        let mut s = Stream::new();
        let mut h = deferred(&[Dispatch::Park(7), Dispatch::Park(7)]);
        let mut p = Peer::new(frame(1, 7));
        assert!(s.service(&mut p, &mut h, u64::MAX));
        assert!(s.cancel_output());
        let mut built = false;
        assert!(s
            .resume_reply(7, &mut h, |_| {
                built = true;
                Ok(())
            })
            .is_err());
        assert!(!built);
        p.input.extend(frame(2, 7));
        assert!(!s.service(&mut p, &mut h, u64::MAX));
    }
    #[test]
    fn malformed_deferred_transitions_and_failed_builders_refuse() {
        for actions in [
            vec![Dispatch::Park(0)],
            vec![Dispatch::Park(1), Dispatch::Park(2)],
            vec![Dispatch::Park(1), Dispatch::Cancel(2)],
            vec![Dispatch::Cancel(0)],
        ] {
            let mut s = Stream::new();
            let mut p = Peer::new((0..actions.len()).flat_map(|i| frame(i as u8, 7)).collect());
            let mut h = deferred(&actions);
            assert!(!s.service(&mut p, &mut h, u64::MAX));
        }
        for bytes in [0, 6, MAX_FRAME + 1] {
            let mut s = Stream::new();
            let mut h = deferred(&[Dispatch::Park(1)]);
            let mut p = Peer::new(frame(1, 7));
            assert!(s.service(&mut p, &mut h, u64::MAX));
            assert!(s
                .resume_reply(1, &mut h, |h| {
                    h.bytes = vec![0; bytes];
                    Ok(())
                })
                .is_err());
            assert!(!s.cancel_output());
            assert!(!s.service(&mut p, &mut h, u64::MAX));
            assert!(p.output.is_empty());
        }
    }
    #[test]
    fn short_writes_do_not_repeat_dispatch_or_lose_a_byte() {
        let input = [frame(1, 27), frame(2, 101)].concat();
        let mut p = Peer::new(input.clone());
        p.write_chunk = 3;
        p.credit = 5;
        let mut s = Stream::new();
        let mut h = Echo::default();
        assert!(s.service(&mut p, &mut h, u64::MAX));
        assert_eq!(p.output, input[..5]);
        assert_eq!(h.seen, [1]);
        assert_eq!(s.interest(), Interest::Write);
        assert!(!s.runnable());
        let calls = p.calls;
        assert!(s.service(&mut p, &mut h, u64::MAX));
        assert_eq!(p.calls, calls + 1);
        assert_eq!(h.seen, [1]);
        p.credit = usize::MAX;
        assert!(s.service(&mut p, &mut h, u64::MAX));
        assert_eq!(p.output, input);
        assert_eq!(h.seen, [1, 2]);
        assert_eq!(s.interest(), Interest::Read);
        assert!(!s.runnable());
    }
    #[test]
    fn buffered_frames_continue_without_another_read_edge() {
        let input: Vec<u8> = (0..19).flat_map(|i| frame(i, 7)).collect();
        let mut p = Peer::new(input.clone());
        let mut s = Stream::new();
        let mut h = Echo::default();
        for expected in [8, 16, 19] {
            assert!(s.service(&mut p, &mut h, u64::MAX));
            assert!(p.input.is_empty());
            assert_eq!(h.seen.len(), expected);
            assert_eq!(s.runnable(), expected != 19);
        }
        assert_eq!(p.output, input);
    }
    #[test]
    fn every_split_and_full_size_frame_survives_backpressure() {
        for chunk in [1, 2, 3, 4, 7, 31, 4095, 4096, MAX_FRAME] {
            let input = frame(9, MAX_FRAME);
            let mut p = Peer::new(input.clone());
            p.read_chunk = chunk;
            p.write_chunk = chunk;
            let mut s = Stream::new();
            let mut h = Echo::default();
            for _ in 0..3 {
                assert!(s.service(&mut p, &mut h, u64::MAX));
            }
            assert_eq!(p.output, input);
            assert_eq!(h.seen, [9]);
            assert!(s.input.capacity() <= MAX_FRAME);
        }
    }
    #[test]
    fn deadline_and_byte_limit_yield_without_losing_progress() {
        let input = [frame(1, MAX_FRAME), frame(2, MAX_FRAME)].concat();
        let mut p = Peer::new(input.clone());
        let mut s = Stream::new();
        let mut h = Echo::default();
        p.tick = 1;
        assert!(s.service(&mut p, &mut h, 2));
        assert_eq!(p.calls, 2);
        assert!(h.seen.is_empty());
        p.tick = 0;
        assert!(s.service(&mut p, &mut h, u64::MAX));
        assert!(p.output.len() < input.len());
        assert!(s.service(&mut p, &mut h, u64::MAX));
        assert_eq!(p.output, input);
    }
    #[test]
    fn malformed_size_and_truncated_disconnect_never_dispatch() {
        for n in [0u32, 6, MAX_FRAME as u32 + 1, u32::MAX] {
            let mut p = Peer::new(n.to_le_bytes().to_vec());
            let mut s = Stream::new();
            let mut h = Echo::default();
            assert!(!s.service(&mut p, &mut h, u64::MAX));
            assert!(h.seen.is_empty());
        }
        let mut p = Peer::new(frame(1, 100)[..51].to_vec());
        p.eof = true;
        let mut s = Stream::new();
        let mut h = Echo::default();
        assert!(!s.service(&mut p, &mut h, u64::MAX));
        assert!(h.seen.is_empty());
    }
    #[test]
    fn stalled_peer_cannot_consume_another_peers_turn() {
        let mut stalled = Peer::new(frame(1, MAX_FRAME));
        stalled.credit = 0;
        let mut slow = Stream::new();
        let mut sh = Echo::default();
        assert!(slow.service(&mut stalled, &mut sh, u64::MAX));
        let mut live = Peer::new(frame(2, 7));
        let mut fast = Stream::new();
        let mut fh = Echo::default();
        assert!(fast.service(&mut live, &mut fh, u64::MAX));
        assert_eq!(live.output, frame(2, 7));
        assert_eq!(sh.seen, [1]);
    }
}
