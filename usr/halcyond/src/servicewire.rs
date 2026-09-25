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

pub trait Handler {
    /// Replace the reply only here. The input is one bounded complete frame.
    fn dispatch(&mut self, request: &[u8]) -> Result<(), ()>;
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
}

impl Stream {
    pub fn new() -> Self {
        Self::default()
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
        self.reply_len == 0 && !matches!(self.frame_len(), Ok(None))
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
                    if handler.dispatch(&self.input[..len]).is_err() {
                        return false;
                    }
                    let reply_len = handler.reply().len();
                    if !(7..=MAX_FRAME).contains(&reply_len) {
                        return false;
                    }
                    self.reply_len = reply_len;
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
        fn dispatch(&mut self, req: &[u8]) -> Result<(), ()> {
            self.seen.push(req[4]);
            self.reply = req.to_vec();
            Ok(())
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
