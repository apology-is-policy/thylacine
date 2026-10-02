//! One ordered control stream with independent, bounded read/write submissions.
use crate::admission::Error;
use libhalcyon::{
    interaction_control::Request,
    interaction_events::{Body, Receiver, Record, RECORD_BYTES, SELECT},
};
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Io {
    Read,
    Write(usize),
}
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Submission {
    pub tag: u64,
    pub io: Io,
}
pub struct Exchange {
    receiver: Receiver,
    next: u64,
    read: Option<Submission>,
    write: Option<Submission>,
    outbound: Option<usize>,
    selected: bool,
    record: Option<Record>,
    request: Option<Request>,
    failed: bool,
}
impl Default for Exchange {
    fn default() -> Self {
        Self::new()
    }
}
impl Exchange {
    pub const fn new() -> Self {
        Self {
            receiver: Receiver::new(),
            next: 1,
            read: None,
            write: None,
            outbound: Some(SELECT.len()),
            selected: false,
            record: None,
            request: None,
            failed: false,
        }
    }
    pub fn ready(&self) -> bool {
        !self.failed && self.selected && self.receiver.identity().is_some()
    }
    pub fn start(&mut self, r: Request) -> Result<[u8; 80], Error> {
        if self.failed {
            return Err(Error::Protocol);
        }
        if !self.ready()
            || self.request.is_some()
            || self.write.is_some()
            || self.outbound.is_some()
        {
            return Err(Error::Busy);
        }
        if Request::decode(&r.encode()) != Some(r) {
            return Err(Error::Invalid);
        }
        self.request = Some(r);
        self.outbound = Some(80);
        Ok(r.encode())
    }
    fn tag(&mut self, io: Io) -> Result<Submission, Error> {
        let tag = self.next;
        self.next = self.next.checked_add(1).ok_or(Error::Exhausted)?;
        Ok(Submission { tag, io })
    }
    pub fn submit_write(&mut self) -> Result<Option<Submission>, Error> {
        if self.failed {
            return Err(Error::Protocol);
        }
        if self.write.is_some() {
            return Ok(None);
        }
        let Some(n) = self.outbound else {
            return Ok(None);
        };
        let s = self.tag(Io::Write(n))?;
        self.write = Some(s);
        Ok(Some(s))
    }
    pub fn submit_read(&mut self) -> Result<Option<Submission>, Error> {
        if self.failed {
            return Err(Error::Protocol);
        }
        if !self.selected || self.read.is_some() || self.record.is_some() {
            return Ok(None);
        }
        let s = self.tag(Io::Read)?;
        self.read = Some(s);
        Ok(Some(s))
    }
    pub fn retry(&mut self, s: Submission) {
        if self.read == Some(s) {
            self.read = None;
        }
        if self.write == Some(s) {
            self.write = None;
        }
    }
    pub fn complete(&mut self, tag: u64, result: i32, bytes: &[u8]) -> Result<(), Error> {
        if self.failed {
            return Err(Error::Protocol);
        }
        let check = if self.write.is_some_and(|s| s.tag == tag) {
            let s = self.write.take().unwrap();
            let Io::Write(n) = s.io else { unreachable!() };
            if result == n as i32 {
                self.outbound = None;
                self.selected = true;
                Ok(())
            } else {
                Err(if result < 0 {
                    Error::Transport(result)
                } else {
                    Error::Protocol
                })
            }
        } else if self.read.is_some_and(|s| s.tag == tag) {
            self.read = None;
            if result != RECORD_BYTES as i32 || bytes.len() != RECORD_BYTES {
                Err(if result < 0 {
                    Error::Transport(result)
                } else {
                    Error::Protocol
                })
            } else {
                self.receiver
                    .accept(bytes)
                    .map_err(|_| Error::Protocol)
                    .and_then(|record| {
                        if let Body::Decision {
                            leaf,
                            binding,
                            request,
                            op,
                            ..
                        } = record.body
                        {
                            if !self.request.is_some_and(|r| {
                                (r.leaf, r.binding, r.request, r.op) == (leaf, binding, request, op)
                            }) {
                                return Err(Error::Protocol);
                            }
                        }
                        self.record = Some(record);
                        Ok(())
                    })
            }
        } else {
            return Ok(());
        };
        if check.is_err() {
            self.failed = true;
            self.record = None;
        }
        check
    }
    pub fn take(&mut self) -> Option<Record> {
        if self.failed {
            return None;
        }
        if matches!(self.record?.body, Body::Decision { .. }) {
            // Rread and Rwrite CQEs may arrive in either order.
            if self.write.is_some() || self.outbound.is_some() {
                return None;
            }
            self.request = None;
        }
        self.record.take()
    }
}
#[cfg(feature = "guest")]
mod native {
    use super::*;
    use crate::{EventRing, TapError};
    use libthyla_rs::{
        fs::File,
        loom::{RegisteredBuffer, Ring, Sqe, SETUP_SQPOLL},
    };
    pub struct Channel {
        ring: Ring,
        buffer: RegisteredBuffer,
        _file: File,
        exchange: Exchange,
    }
    impl Channel {
        pub fn preopen(session: &EventRing) -> Result<File, TapError> {
            crate::admission::Channel::preopen(session)
        }
        pub fn from_file(file: File) -> Result<Self, TapError> {
            let ring = Ring::setup(4, SETUP_SQPOLL).map_err(|_| TapError::Loom)?;
            let mut buffer = RegisteredBuffer::new(160).map_err(|_| TapError::Loom)?;
            buffer.as_mut_slice().fill(0);
            buffer.as_mut_slice()[..SELECT.len()].copy_from_slice(&SELECT);
            ring.register_buffers(&[buffer.buf_reg()])
                .map_err(|_| TapError::Loom)?;
            ring.register_handles(&[file.as_raw_fd()])
                .map_err(|_| TapError::Loom)?;
            Ok(Self {
                ring,
                buffer,
                _file: file,
                exchange: Exchange::new(),
            })
        }
        pub fn ready(&self) -> bool {
            self.exchange.ready()
        }
        pub fn poll_fd(&self) -> i32 {
            self.ring.raw_fd()
        }
        pub fn take(&mut self) -> Option<Record> {
            self.exchange.take()
        }
        pub fn start(&mut self, r: Request) -> Result<(), Error> {
            let b = self.exchange.start(r)?;
            self.buffer.as_mut_slice()[..80].copy_from_slice(&b);
            Ok(())
        }
        pub fn pump(&mut self) -> Result<(), Error> {
            // At most the read and write CQEs per pass, never a busy rearm loop.
            for _ in 0..2 {
                let Some(c) = self.ring.reap() else {
                    break;
                };
                self.exchange.complete(
                    c.user_data,
                    c.result,
                    &self.buffer.as_mut_slice()[80..160],
                )?;
            }
            let mut count = 0;
            for read in [false, true] {
                let Some(s) = (if read {
                    self.exchange.submit_read()?
                } else {
                    self.exchange.submit_write()?
                }) else {
                    continue;
                };
                let sqe = match s.io {
                    Io::Write(n) => Sqe::write(0, 0, n as u32, 0, 0, s.tag),
                    Io::Read => Sqe::read(0, 0, 80, 0, 80, s.tag),
                };
                if let Err(e) = self.ring.try_submit(&sqe) {
                    self.exchange.retry(s);
                    return Err(Error::Transport(-e.as_errno()));
                }
                count += 1;
            }
            if count != 0 {
                self.ring
                    .enter(count, 0, 0)
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
    use libhalcyon::interaction_control::{Op, Reply};
    fn q() -> Request {
        Request {
            op: Op::Check,
            request: 3,
            leaf: 2,
            binder_pid: 0,
            binding: 4,
            foreground: 5,
            subject: 6,
            controller: 7,
            context: 8,
            epoch: 9,
        }
    }
    fn ready() -> Exchange {
        let mut e = Exchange::new();
        let w = e.submit_write().unwrap().unwrap();
        assert_eq!(w.io, Io::Write(16));
        assert_eq!(e.submit_read(), Ok(None));
        e.complete(w.tag, 16, &[]).unwrap();
        let r = e.submit_read().unwrap().unwrap();
        e.complete(
            r.tag,
            80,
            &Record {
                sequence: 1,
                body: Body::Ready(10),
            }
            .encode(),
        )
        .unwrap();
        assert!(e.ready());
        e.take().unwrap();
        e
    }
    fn decision(sequence: u64) -> [u8; 80] {
        Record {
            sequence,
            body: Body::decision(
                q(),
                Ok(Reply {
                    op: Op::Check,
                    request: 3,
                    focus: 11,
                    seat: 12,
                    foreground: 5,
                }),
            ),
        }
        .encode()
    }
    #[test]
    fn parked_read_does_not_block_write_and_cqe_order_cannot_release_early() {
        for write_first in [true, false] {
            let mut e = ready();
            let r = e.submit_read().unwrap().unwrap();
            e.start(q()).unwrap();
            let w = e.submit_write().unwrap().unwrap();
            if write_first {
                e.complete(w.tag, 80, &[]).unwrap();
            }
            e.complete(r.tag, 80, &decision(2)).unwrap();
            if !write_first {
                assert_eq!(e.take(), None);
                assert_eq!(e.start(q()), Err(Error::Busy));
                e.complete(w.tag, 80, &[]).unwrap();
            }
            assert!(matches!(e.take().unwrap().body, Body::Decision { .. }));
            assert_eq!(e.take(), None);
        }
    }
    #[test]
    fn queued_notifications_precede_decision_and_duplicate_cqe_cannot_advance() {
        let mut e = ready();
        e.start(q()).unwrap();
        let w = e.submit_write().unwrap().unwrap();
        e.complete(w.tag, 80, &[]).unwrap();
        let r = e.submit_read().unwrap().unwrap();
        let lost = Record {
            sequence: 2,
            body: Body::FocusLost {
                leaf: 2,
                binding: 4,
                epoch: 12,
            },
        };
        e.complete(r.tag + 100, 80, &decision(2)).unwrap();
        assert_eq!(e.take(), None);
        e.complete(r.tag, 80, &lost.encode()).unwrap();
        assert_eq!(e.submit_read(), Ok(None));
        assert_eq!(e.take(), Some(lost));
        let r2 = e.submit_read().unwrap().unwrap();
        e.complete(r.tag, 80, &lost.encode()).unwrap();
        assert_eq!(e.take(), None);
        e.complete(r2.tag, 80, &decision(3)).unwrap();
        assert!(e.take().is_some());
    }
    #[test]
    fn short_eof_transport_and_wrong_decision_poison() {
        for n in [-5, 0, 1, 79, 81] {
            let mut e = ready();
            let r = e.submit_read().unwrap().unwrap();
            assert!(e.complete(r.tag, n, &decision(2)).is_err());
            assert!(!e.ready());
            assert_eq!(e.submit_write(), Err(Error::Protocol));
        }
        let mut e = ready();
        let r = e.submit_read().unwrap().unwrap();
        assert!(e.complete(r.tag, 80, &decision(2)).is_err());
        assert_eq!(e.take(), None);
    }
    #[test]
    fn failed_write_discards_early_decision() {
        let mut e = ready();
        e.start(q()).unwrap();
        let w = e.submit_write().unwrap().unwrap();
        let r = e.submit_read().unwrap().unwrap();
        e.complete(r.tag, 80, &decision(2)).unwrap();
        assert_eq!(e.take(), None);
        assert!(e.complete(w.tag, -5, &[]).is_err());
        assert_eq!(e.take(), None);
    }
    #[test]
    fn refused_enqueue_uses_new_tags_and_exhaustion_never_wraps() {
        let mut e = ready();
        let r = e.submit_read().unwrap().unwrap();
        e.retry(r);
        let r2 = e.submit_read().unwrap().unwrap();
        assert!(r2.tag > r.tag);
        e.complete(r.tag, 80, &decision(2)).unwrap();
        assert_eq!(e.take(), None);
        e.retry(r2);
        e.next = u64::MAX;
        assert_eq!(e.submit_read(), Err(Error::Exhausted));
    }
}
