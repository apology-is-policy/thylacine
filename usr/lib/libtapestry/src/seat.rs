//! Independent HSC1 lane. Uses a separate registered control handle; it never
//! borrows the renderer EventRing or waits for presentation. The HIA1 channel
//! remains separate because its completions have different authority meaning.
use libhalcyon::seat_control::{Reply, Request, REPLY_BYTES, REQUEST_BYTES};

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
    use libhalcyon::seat_control::{Op, Snapshot};
    use libthyla_rs::{
        fs::File,
        handle::Rights,
        loom::{RegisteredBuffer, Ring, Sqe, SETUP_SQPOLL},
        *,
    };
    fn open_at(root: i64, path: &[u8], mode: u32) -> Result<File, TapError> {
        let fd = unsafe { t_open(root, path.as_ptr(), path.len(), mode) };
        if fd < 0 {
            return Err(TapError::Connect);
        }
        Ok(unsafe { File::from_raw_fd(fd as i32, Rights::READ | Rights::WRITE) })
    }
    /// Normal-connection setup only, before the UI starts presenting. The ctl
    /// pins this connection in the kernel; the reservation is not a credential.
    pub fn reserve(session: &EventRing) -> Result<(File, Snapshot), TapError> {
        let ctl = open_at(session.root(), b"ctl", T_ORDWR)?;
        let q = Request {
            op: Op::Reserve,
            request: 1,
            registration: 0,
            generation: 0,
            revision: 0,
        };
        let bytes = q.encode();
        if unsafe { t_write(ctl.as_raw_fd() as i64, bytes.as_ptr(), bytes.len()) }
            != bytes.len() as i64
        {
            return Err(TapError::Connect);
        }
        if unsafe { t_lseek(ctl.as_raw_fd() as i64, 0, T_SEEK_SET) } != 0 {
            return Err(TapError::Connect);
        }
        let mut bytes = [0u8; REPLY_BYTES];
        let mut at = 0;
        while at < bytes.len() {
            let n = unsafe {
                t_read(
                    ctl.as_raw_fd() as i64,
                    bytes[at..].as_mut_ptr(),
                    bytes.len() - at,
                )
            };
            if n <= 0 || n as usize > bytes.len() - at {
                return Err(TapError::Connect);
            }
            at += n as usize;
        }
        let r = Reply::decode(&bytes, q).ok_or(TapError::Connect)?;
        if r.state.phase != 0 || r.state.enabled {
            return Err(TapError::Connect);
        }
        Ok((ctl, r.state))
    }
    /// Drop order retires the SQPOLL ring before releasing registered storage
    /// and the control fid. Construct on its owner thread; no unsafe Send cast.
    pub struct Channel {
        ring: Ring,
        buffer: RegisteredBuffer,
        _ctl: File,
        exchange: Exchange,
    }
    impl Channel {
        pub fn open() -> Result<Self, TapError> {
            let root = open_at(T_WALK_OPEN_FROM_ROOT, b"/srv/tapestry-interaction", T_OREAD)?;
            let ctl = open_at(root.as_raw_fd() as i64, b"ctl", T_ORDWR)?;
            let ring = Ring::setup(4, SETUP_SQPOLL).map_err(|_| TapError::Loom)?;
            let mut buffer =
                RegisteredBuffer::new(REQUEST_BYTES + REPLY_BYTES).map_err(|_| TapError::Loom)?;
            buffer.as_mut_slice().fill(0);
            // SAFETY: owned byte storage; this client tracks submitted ranges and
            // borrows them only before submission or after their matching completion.
            unsafe { ring.register_buffers(&[buffer.buf_reg()]) }
                .map_err(|_| TapError::Loom)?;
            ring.register_handles(&[ctl.as_raw_fd()])
                .map_err(|_| TapError::Loom)?;
            Ok(Self {
                ring,
                buffer,
                _ctl: ctl,
                exchange: Exchange::new(),
            })
        }
        pub fn poll_fd(&self) -> i32 {
            self.ring.raw_fd()
        }
        pub fn start(&mut self, q: Request) -> Result<(), Error> {
            let b = self.exchange.start(q)?;
            self.buffer.as_mut_range(0..REQUEST_BYTES).ok_or(Error::Invalid)?.copy_from_slice(&b);
            Ok(())
        }
        pub fn take(&mut self) -> Option<Completion> {
            self.exchange.take()
        }
        pub fn pump(&mut self) -> Result<(), Error> {
            if let Some(c) = self.ring.reap() {
                let bytes = if let Some(n) = self.exchange.completed_read_len(c.user_data, c.result) {
                    self.buffer.as_slice_range(REQUEST_BYTES..REQUEST_BYTES+n).ok_or(Error::Invalid)?
                } else { &[] };
                self.exchange.complete(c.user_data, c.result, bytes);
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
                self.ring
                    .enter(1, 0, 0)
                    .map_err(|e| Error::Transport(-e.as_errno()))?;
            }
            Ok(())
        }
    }
}
#[cfg(feature = "guest")]
pub use native::{reserve, Channel};

#[cfg(test)]
mod tests {
    use super::*;
    use libhalcyon::seat_control::{Op, Snapshot};
    fn request(n: u64) -> Request {
        Request {
            op: Op::State,
            request: n,
            registration: 9,
            generation: 0,
            revision: 2,
        }
    }
    fn receipt(q: Request) -> [u8; REPLY_BYTES] {
        Reply {
            op: q.op,
            request: q.request,
            state: Snapshot {
                registration: 9,
                generation: 1,
                revision: 3,
                phase: 1,
                enabled: false,
            },
        }
        .encode()
    }
    #[test]
    fn parked_state_partial_reply_and_wrong_cqe() {
        let mut e = Exchange::new();
        let q = request(1);
        e.start(q).unwrap();
        let w = e.submission().unwrap().unwrap();
        e.complete(w.tag + 1, 40, &[]);
        assert_eq!(e.submission(), Ok(None));
        e.complete(w.tag, 40, &[]);
        let r = e.submission().unwrap().unwrap();
        assert_eq!(e.start(request(2)), Err(Error::Busy));
        let b = receipt(q);
        e.complete(r.tag, 17, &b[..17]);
        let r = e.submission().unwrap().unwrap();
        e.complete(r.tag, 39, &b[17..]);
        assert_eq!(e.take().unwrap().result.unwrap().state.phase, 1);
        assert_eq!(e.start(q), Err(Error::Invalid));
        assert!(e.start(request(2)).is_ok());
    }
    #[test]
    fn denied_join_and_corrupt_receipt_are_distinct() {
        let mut e = Exchange::new();
        e.start(request(1)).unwrap();
        let w = e.submission().unwrap().unwrap();
        e.complete(w.tag, -1, &[]);
        assert_eq!(e.take().unwrap().result, Err(Error::Transport(-1)));
        e.start(request(2)).unwrap();
        let w = e.submission().unwrap().unwrap();
        e.complete(w.tag, 40, &[]);
        let r = e.submission().unwrap().unwrap();
        e.complete(r.tag, 56, &receipt(request(1)));
        assert_eq!(e.take().unwrap().result, Err(Error::Protocol));
        assert_eq!(e.start(request(3)), Err(Error::Protocol));
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
