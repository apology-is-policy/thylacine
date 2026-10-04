//! Serialized HIA1 exchanges. One preopened ctl, one operation and one SQE in
//! flight: no per-action open, thread, allocation or blocking RPC. The native
//! shell below uses Loom SQPOLL on the SAME authenticated compositor connection.
//! Cancellation removes broker authority; it does not reuse the DMA buffer before
//! its CQE. A cancelled exchange must still be drained, or the channel dropped.
use libhalcyon::interaction_control::{Reply, Request, REPLY_BYTES, REQUEST_BYTES};

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Error {
    Busy,
    Invalid,
    Protocol,
    Transport(i32),
    Exhausted,
}
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Io {
    Write,
    Read { offset: usize },
}
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Submission {
    pub tag: u64,
    pub io: Io,
}
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Completion {
    pub request: Request,
    pub result: Result<Reply, Error>,
}

/// Pure single-owner state. Tags are never recycled, even across requests.
/// Bytes are stable from submission through completion; only an exact matching
/// tag may change state. A transport/protocol fault poisons the channel.
pub struct Exchange {
    request: Option<Request>,
    phase: Io,
    inflight: Option<Submission>,
    completed: Option<Completion>,
    last_request: u64,
    next_tag: u64,
    failed: bool,
    reply: [u8; REPLY_BYTES],
}
const _: () = assert!(core::mem::size_of::<Exchange>() <= 512);

impl Default for Exchange {
    fn default() -> Self {
        Self::new()
    }
}
impl Exchange {
    pub const fn new() -> Self {
        Self {
            request: None,
            phase: Io::Write,
            inflight: None,
            completed: None,
            last_request: 0,
            next_tag: 1,
            failed: false,
            reply: [0; REPLY_BYTES],
        }
    }
    pub fn start(&mut self, request: Request) -> Result<[u8; REQUEST_BYTES], Error> {
        if self.failed {
            return Err(Error::Protocol);
        }
        if self.request.is_some() || self.completed.is_some() {
            return Err(Error::Busy);
        }
        let bytes = request.encode();
        if request.request <= self.last_request || Request::decode(&bytes) != Some(request) {
            return Err(Error::Invalid);
        }
        self.last_request = request.request;
        self.request = Some(request);
        self.phase = Io::Write;
        self.reply.fill(0);
        Ok(bytes)
    }
    /// Called only when the transport can enqueue. `retry_submission` returns
    /// ownership on a refused enqueue; a submitted SQE must never use it.
    pub fn submission(&mut self) -> Result<Option<Submission>, Error> {
        if self.inflight.is_some() || self.request.is_none() {
            return Ok(None);
        }
        let tag = self.next_tag;
        self.next_tag = tag.checked_add(1).ok_or(Error::Exhausted)?;
        let s = Submission {
            tag,
            io: self.phase,
        };
        self.inflight = Some(s);
        Ok(Some(s))
    }
    pub fn retry_submission(&mut self, s: Submission) {
        if self.inflight == Some(s) {
            self.inflight = None;
        }
    }
    pub fn completed_read_len(&self, tag: u64, result: i32) -> Option<usize> {
        let s = self.inflight?;
        if self.failed || s.tag != tag || result <= 0 { return None; }
        match s.io {
            Io::Read { offset } if result as usize <= REPLY_BYTES - offset => Some(result as usize),
            _ => None,
        }
    }
    pub fn complete(&mut self, tag: u64, result: i32, bytes: &[u8]) {
        let Some(s) = self.inflight else { return };
        if s.tag != tag {
            return;
        }
        self.inflight = None;
        let request = self.request.unwrap();
        // Rlerror is a completed decision on WRITE (e.g. denied focus), not a
        // broken transport. The next higher request can use the same ctl.
        if result < 0 {
            if matches!(s.io, Io::Read { .. }) {
                self.failed = true;
            }
            self.finish(Err(Error::Transport(result)));
            return;
        }
        match s.io {
            Io::Write if result as usize == REQUEST_BYTES => {
                self.phase = Io::Read { offset: 0 };
            }
            Io::Read { offset }
                if result > 0
                    && result as usize <= REPLY_BYTES - offset
                    && bytes.len() == result as usize =>
            {
                let end = offset + bytes.len();
                self.reply[offset..end].copy_from_slice(bytes);
                if end == REPLY_BYTES {
                    match Reply::decode(&self.reply, request) {
                        Some(r) => self.finish(Ok(r)),
                        None => {
                            self.failed = true;
                            self.finish(Err(Error::Protocol));
                        }
                    }
                } else {
                    self.phase = Io::Read { offset: end };
                }
            }
            _ => {
                self.failed = true;
                self.finish(Err(Error::Protocol));
            }
        }
    }
    fn finish(&mut self, result: Result<Reply, Error>) {
        self.completed = Some(Completion {
            request: self.request.take().unwrap(),
            result,
        });
    }
    pub fn take(&mut self) -> Option<Completion> {
        self.completed.take()
    }
}

#[cfg(feature = "guest")]
mod native {
    use super::*;
    use crate::{EventRing, TapError};
    use libthyla_rs::loom::{RegisteredBuffer, Ring, Sqe, SETUP_SQPOLL};
    use libthyla_rs::{fs::File, handle::Rights};
    /// Drop order matters: destroy/join the ring before releasing its buffer or
    /// fid. The retained EventRing keeps the authenticated session alive.
    pub struct Channel {
        ring: Ring,
        buffer: RegisteredBuffer,
        _ctl: File,
        _session: Option<EventRing>,
        exchange: Exchange,
    }
    impl Channel {
        /// Setup only: opening/registering this channel may block. All later
        /// start/pump/take operations are bounded and never wait for a reply.
        pub fn open(session: &EventRing) -> Result<Self, TapError> {
            let mut channel = Self::from_file(Self::preopen(session)?)?;
            channel._session = Some(session.clone());
            Ok(channel)
        }
        /// Open on the UI's authenticated connection during setup. File is
        /// movable; EventRing/Ring are not. The executor builds its own ring.
        pub fn preopen(session: &EventRing) -> Result<File, TapError> {
            let fd = unsafe {
                libthyla_rs::t_open(session.root(), b"ctl".as_ptr(), 3, libthyla_rs::T_ORDWR)
            };
            if fd < 0 {
                return Err(TapError::Connect);
            }
            Ok(unsafe { File::from_raw_fd(fd as i32, Rights::READ | Rights::WRITE) })
        }
        pub fn from_file(ctl: File) -> Result<Self, TapError> {
            let fd = ctl.as_raw_fd();
            let ring = Ring::setup(4, SETUP_SQPOLL).map_err(|_| TapError::Loom)?;
            let mut buffer =
                RegisteredBuffer::new(REQUEST_BYTES + REPLY_BYTES).map_err(|_| TapError::Loom)?;
            buffer.as_mut_slice().fill(0);
            // SAFETY: owned byte storage; this client tracks submitted ranges and
            // borrows them only before submission or after their matching completion.
            unsafe { ring.register_buffers(&[buffer.buf_reg()]) }
                .map_err(|_| TapError::Loom)?;
            ring.register_handles(&[fd as i32])
                .map_err(|_| TapError::Loom)?;
            Ok(Self {
                ring,
                buffer,
                _ctl: ctl,
                _session: None,
                exchange: Exchange::new(),
            })
        }
        pub fn poll_fd(&self) -> i32 {
            self.ring.raw_fd()
        }
        pub fn start(&mut self, request: Request) -> Result<(), Error> {
            let bytes = self.exchange.start(request)?;
            self.buffer.as_mut_range(0..REQUEST_BYTES).ok_or(Error::Invalid)?.copy_from_slice(&bytes);
            Ok(())
        }
        pub fn take(&mut self) -> Option<Completion> {
            self.exchange.take()
        }
        pub fn pump(&mut self) -> Result<(), Error> {
            // At most one CQE can belong to this channel. Never run an
            // unbounded reap/rearm loop, even for a fast compositor.
            if let Some(cqe) = self.ring.reap() {
                let bytes = if let Some(n) = self.exchange.completed_read_len(cqe.user_data, cqe.result) {
                    self.buffer.as_slice_range(REQUEST_BYTES..REQUEST_BYTES+n).ok_or(Error::Invalid)?
                } else { &[] };
                self.exchange.complete(cqe.user_data, cqe.result, bytes);
            }
            if let Some(s) = self.exchange.submission()? {
                let sqe = match s.io {
                    Io::Write => Sqe::write(0, 0, REQUEST_BYTES as u32, 0, 0, s.tag),
                    Io::Read { offset } => Sqe::read(
                        0,
                        offset as u64,
                        (REPLY_BYTES - offset) as u32,
                        0,
                        REQUEST_BYTES as u64,
                        s.tag,
                    ),
                };
                if let Err(e) = self.ring.try_submit(&sqe) {
                    self.exchange.retry_submission(s);
                    return Err(Error::Transport(-e.as_errno()));
                }
                // SQPOLL handles the protocol and wakes the existing poll loop.
                self.ring
                    .enter(1, 0, 0)
                    .map_err(|e| Error::Transport(-e.as_errno()))?;
            }
            Ok(())
        }
    }
}
#[cfg(feature = "guest")]
pub use native::Channel;

#[cfg(test)]
mod tests {
    use super::*;
    use libhalcyon::interaction_control::Op;
    fn request(id: u64) -> Request {
        Request {
            op: Op::Check,
            request: id,
            leaf: 1,
            binder_pid: 0,
            binding: 2,
            foreground: 3,
            subject: 4,
            controller: 5,
            context: 6,
            epoch: 7,
        }
    }
    fn reply(r: Request) -> [u8; REPLY_BYTES] {
        Reply {
            op: r.op,
            request: r.request,
            focus: 8,
            seat: 9,
            foreground: r.foreground,
        }
        .encode()
    }
    fn writing(e: &mut Exchange, id: u64) -> Submission {
        e.start(request(id)).unwrap();
        e.submission().unwrap().unwrap()
    }
    #[test]
    fn delayed_partial_and_duplicate_completions() {
        let mut e = Exchange::new();
        let w = writing(&mut e, 1);
        assert_eq!(e.start(request(2)), Err(Error::Busy));
        assert_eq!(e.submission(), Ok(None));
        e.complete(w.tag + 1, 80, &[]);
        assert_eq!(e.take(), None);
        e.complete(w.tag, 80, &[]);
        let r = e.submission().unwrap().unwrap();
        e.complete(w.tag, 80, &[]);
        assert_eq!(e.submission(), Ok(None));
        let bytes = reply(request(1));
        e.complete(r.tag, 13, &bytes[..13]);
        let r = e.submission().unwrap().unwrap();
        assert_eq!(r.io, Io::Read { offset: 13 });
        e.complete(r.tag, 27, &bytes[13..]);
        assert_eq!(e.start(request(2)), Err(Error::Busy));
        assert_eq!(e.take().unwrap().result.unwrap().request, 1);
        assert_eq!(e.start(request(1)), Err(Error::Invalid));
        let next = writing(&mut e, 2);
        assert!(next.tag > r.tag);
        e.complete(r.tag, 27, &bytes[13..]);
        assert_eq!(e.take(), None);
    }
    #[test]
    fn denial_is_not_an_empty_success_and_can_be_followed_by_new_request() {
        let mut e = Exchange::new();
        let w = writing(&mut e, 1);
        e.complete(w.tag, -1, &[]);
        assert_eq!(e.take().unwrap().result, Err(Error::Transport(-1)));
        writing(&mut e, 2);
    }
    #[test]
    fn truncation_eof_overrun_and_mismatched_receipts_poison() {
        for n in [0, 1, 79, 81] {
            let mut e = Exchange::new();
            let w = writing(&mut e, 1);
            e.complete(w.tag, n, &[]);
            assert_eq!(e.take().unwrap().result, Err(Error::Protocol));
            assert_eq!(e.start(request(2)), Err(Error::Protocol));
        }
        for n in [0, 41, -5, 40] {
            let mut e = Exchange::new();
            let w = writing(&mut e, 1);
            e.complete(w.tag, 80, &[]);
            let r = e.submission().unwrap().unwrap();
            e.complete(r.tag, n, &reply(request(2)));
            assert!(e.take().unwrap().result.is_err());
            assert_eq!(e.start(request(2)), Err(Error::Protocol));
        }
    }
    #[test]
    fn refused_enqueue_and_tag_exhaustion_never_reuse() {
        let mut e = Exchange::new();
        let a = writing(&mut e, 1);
        e.retry_submission(a);
        let b = e.submission().unwrap().unwrap();
        assert!(b.tag > a.tag);
        e.complete(a.tag, 80, &[]);
        assert_eq!(e.take(), None);
        e.retry_submission(b);
        e.next_tag = u64::MAX;
        assert_eq!(e.submission(), Err(Error::Exhausted));
    }

    #[test]
    fn payload_borrow_requires_matching_read_completion() {
        let mut e=Exchange::new();
        e.start(request(1)).unwrap();
        let w=e.submission().unwrap().unwrap();
        assert_eq!(e.completed_read_len(w.tag, REQUEST_BYTES as i32), None);
        e.complete(w.tag, REQUEST_BYTES as i32, &[]);
        let r=e.submission().unwrap().unwrap();
        assert_eq!(e.completed_read_len(w.tag, 1), None);
        assert_eq!(e.completed_read_len(r.tag+1, 1), None);
        assert_eq!(e.completed_read_len(r.tag, -1), None);
        assert_eq!(e.completed_read_len(r.tag, REPLY_BYTES as i32+1), None);
        assert_eq!(e.completed_read_len(r.tag, 1), Some(1));
        e.complete(r.tag, -1, &[]);
        assert_eq!(e.completed_read_len(r.tag, 1), None);
    }
}
