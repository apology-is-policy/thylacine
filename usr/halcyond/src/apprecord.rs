//! One HIN1 transaction fid: contiguous assembly, exact replay and cancellation.
//!
//! The connection owner supplies a non-reused fid incarnation and allowances
//! after subtracting ALL other fids and transport allocations. Retain one exact
//! request, compare a duplicate in place, and never allocate a second body for
//! replay. A new header retires the old cache before allocating its body. This
//! module dispatches no authority and performs no mutation of clipboard data.
//! Replies may be small semantic values or references to pinned snapshots; the
//! owner must count their allocation, retire them before HSC ACK, and revalidate
//! authority before delivering data. A wire request ID is never a local ticket.
use alloc::vec::Vec;
use libhalcyon::{
    interaction_body::Request,
    interaction_frame::{ReceiveError, Receiver},
    interaction_wire::{Error, Failure, Header, HEADER_BYTES, MAX_RECORD},
};

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Ticket { incarnation: u64, request: u64 }
impl Ticket {
    pub fn incarnation(self) -> u64 { self.incarnation }
    pub fn request(self) -> u64 { self.request }
}
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Progress { Partial, Dispatch(Ticket), Replay }
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
enum Phase { Idle, Prefix, Body, Replay, Pending, Answered, Poisoned }

/// Implementations count allocated capacity, not merely live response length.
/// Snapshot references must be charged to the session payload ledger as well.
pub trait CachedReply { fn reserved_bytes(&self) -> usize; }
impl CachedReply for Vec<u8> { fn reserved_bytes(&self) -> usize { self.capacity() } }
impl<R: CachedReply> CachedReply for Result<R, Failure> {
    fn reserved_bytes(&self) -> usize { self.as_ref().map_or(0, CachedReply::reserved_bytes) }
}

pub struct Record<R> {
    incarnation: u64,
    last: u64,
    phase: Phase,
    prefix: [u8; HEADER_BYTES],
    received: usize,
    record: Receiver,
    reply: Option<R>,
}
impl<R: CachedReply> Record<R> {
    /// The owner, not the client, allocates this incarnation monotonically.
    pub fn new(incarnation: u64) -> Result<Self, Failure> {
        if incarnation == 0 { return Err(Failure::Invalid); }
        Ok(Self { incarnation, last: 0, phase: Phase::Idle,
            prefix: [0; HEADER_BYTES], received: 0, record: Receiver::default(), reply: None })
    }
    pub fn input_reserved(&self) -> usize { HEADER_BYTES + self.record.reserved_bytes() }
    pub fn output_reserved(&self) -> usize { self.reply.as_ref().map_or(0, CachedReply::reserved_bytes) }
    fn ticket(&self) -> Ticket { Ticket { incarnation: self.incarnation, request: self.last } }
    /// Available only for the one completed, not-yet-answered request.
    pub fn request(&self, ticket: Ticket) -> Option<Request<'_>> {
        if self.phase != Phase::Pending || ticket != self.ticket() { return None; }
        let (header, body) = self.record.complete()?;
        Request::decode_body(header, body).ok().map(|(_, request)| request)
    }
    pub fn reply(&self) -> Option<&R> {
        (self.phase == Phase::Answered).then_some(self.reply.as_ref()).flatten()
    }
    /// Caller prepares the semantic result within its output budget BEFORE any
    /// externally visible mutation. A stale receipt cannot overwrite a cache.
    pub fn finish(&mut self, ticket: Ticket, result: R, output_allowance: usize) -> Result<(), Failure> {
        if self.phase != Phase::Pending || ticket != self.ticket() { return Err(Failure::Gone); }
        if result.reserved_bytes() > output_allowance.min(MAX_RECORD) { return Err(Failure::TooLarge); }
        self.reply = Some(result);
        self.phase = Phase::Answered;
        Ok(())
    }
    /// Retire every retained byte but keep the ID high-water mark. Cancelled
    /// requests can never be redispatched with their old ID, even after reset.
    /// The returned ticket identifies any application admission to cancel.
    pub fn cancel(&mut self) -> Option<Ticket> {
        let pending = (self.phase == Phase::Pending).then(|| self.ticket());
        self.record.reset();
        self.reply = None;
        self.prefix.fill(0);
        self.received = 0;
        self.phase = Phase::Idle;
        pending
    }
    pub fn write(&mut self, offset: u64, bytes: &[u8], input_allowance: usize) -> Result<Progress, Failure> {
        // Refuse concurrent writes without destroying the pending admission.
        if self.phase == Phase::Pending { return Err(Failure::Busy); }
        if self.phase == Phase::Poisoned { return Err(Failure::Invalid); }
        let result = self.append(offset, bytes, input_allowance.min(MAX_RECORD));
        if result.is_err() { self.phase = Phase::Poisoned; }
        result
    }
    fn append(&mut self, offset: u64, mut bytes: &[u8], allowance: usize) -> Result<Progress, Failure> {
        if bytes.is_empty() { return Err(Failure::Invalid); }
        if self.input_reserved() > allowance { return Err(Failure::TooLarge); }
        if matches!(self.phase, Phase::Idle | Phase::Answered) {
            if offset != 0 { return Err(Failure::Invalid); }
            self.phase = Phase::Prefix;
            self.received = 0;
        }
        if offset != self.received as u64 { return Err(Failure::Invalid); }
        if self.phase == Phase::Prefix {
            let n = bytes.len().min(HEADER_BYTES - self.received);
            self.prefix[self.received..self.received + n].copy_from_slice(&bytes[..n]);
            self.received += n;
            bytes = &bytes[n..];
            if self.received < HEADER_BYTES { return Ok(Progress::Partial); }
            let header = Header::decode(&self.prefix).map_err(wire_failure)?;
            if header.response || header.request_id < self.last { return Err(Failure::Invalid); }
            if header.request_id == self.last {
                // Only the immediate completed record is eligible. Comparing
                // just the ID, operation, length or body hash is insufficient.
                let (old, _) = self.record.complete().ok_or(Failure::Invalid)?;
                if self.reply.is_none() || old != header { return Err(Failure::Invalid); }
                self.phase = Phase::Replay;
            } else {
                self.last = header.request_id;
                self.record.reset();
                self.reply = None;
                self.phase = Phase::Body;
                self.record.push(0, &self.prefix, allowance - HEADER_BYTES).map_err(receive_failure)?;
            }
        }
        match self.phase {
            Phase::Replay => {
                let (header, body) = self.record.complete().ok_or(Failure::Invalid)?;
                let start = self.received - HEADER_BYTES;
                let end = start.checked_add(bytes.len()).ok_or(Failure::TooLarge)?;
                if body.get(start..end) != Some(bytes) { return Err(Failure::Invalid); }
                self.received += bytes.len();
                if self.received == header.length {
                    self.phase = Phase::Answered;
                    Ok(Progress::Replay)
                } else { Ok(Progress::Partial) }
            }
            Phase::Body => {
                if !bytes.is_empty() {
                    self.record.push(self.received as u64, bytes, allowance - HEADER_BYTES).map_err(receive_failure)?;
                    self.received += bytes.len();
                }
                if let Some((header, body)) = self.record.complete() {
                    Request::decode_body(header, body).map_err(wire_failure)?;
                    self.phase = Phase::Pending;
                    Ok(Progress::Dispatch(self.ticket()))
                } else { Ok(Progress::Partial) }
            }
            _ => Err(Failure::Invalid),
        }
    }
}
fn wire_failure(e: Error) -> Failure {
    match e {
        Error::Unsupported => Failure::Unsupported,
        Error::TooLarge => Failure::TooLarge,
        Error::Malformed => Failure::Invalid,
    }
}
fn receive_failure(e: ReceiveError) -> Failure {
    match e {
        ReceiveError::Wire(error) => wire_failure(error),
        ReceiveError::Budget => Failure::TooLarge,
        ReceiveError::Allocation => Failure::NoMemory,
        _ => Failure::Invalid,
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use libhalcyon::interaction_body::Scope;
    type TestRecord = Record<Result<Vec<u8>, Failure>>;
    fn scope() -> Scope { Scope { session: 1, controller: 2, context: 3, epoch: 4 } }
    fn request(id: u64) -> Vec<u8> { Request::Begin { scope: scope(), length: 3 }.encode(id).unwrap() }
    fn send(r: &mut TestRecord, bytes: &[u8]) -> Ticket {
        match r.write(0, bytes, MAX_RECORD).unwrap() {
            Progress::Dispatch(t) => t, other => panic!("expected dispatch, got {other:?}")
        }
    }
    #[test]
    fn protocol_failures_preserve_the_reserved_error_codes() {
        let mut r = TestRecord::new(1).unwrap();
        let mut unknown = Request::Hello.encode(1).unwrap(); unknown[6] = 250;
        assert_eq!(r.write(0, &unknown, MAX_RECORD), Err(Failure::Unsupported));
        r.cancel();
        let mut huge = Request::Hello.encode(2).unwrap();
        huge[8..12].copy_from_slice(&32769u32.to_le_bytes());
        assert_eq!(r.write(0, &huge, MAX_RECORD), Err(Failure::TooLarge));
        r.cancel();
        assert_eq!(r.write(0, &[], MAX_RECORD), Err(Failure::Invalid));
        r.cancel();
        let mut bad = request(3); bad[56..60].copy_from_slice(&1048577u32.to_le_bytes());
        assert_eq!(r.write(0, &bad, MAX_RECORD), Err(Failure::TooLarge));
    }
    #[test]
    fn every_fragment_boundary_and_exact_replay_dispatch_only_once() {
        let bytes = request(1);
        for split in 1..bytes.len() {
            let mut r = TestRecord::new(1).unwrap();
            assert_eq!(r.write(0, &bytes[..split], MAX_RECORD), Ok(Progress::Partial));
            assert!(r.reply().is_none());
            let t = match r.write(split as u64, &bytes[split..], MAX_RECORD).unwrap() {
                Progress::Dispatch(t) => t, other => panic!("{other:?}")
            };
            assert_eq!(r.request(t), Some(Request::Begin { scope: scope(), length: 3 }));
            r.finish(t, Ok(alloc::vec![4, 5]), MAX_RECORD).unwrap();
            let reserved = r.input_reserved();
            assert_eq!(r.write(0, &bytes[..split], reserved), Ok(Progress::Partial));
            assert_eq!(r.reply().is_none(), true, "partial duplicate exposed reply");
            assert_eq!(r.write(split as u64, &bytes[split..], reserved), Ok(Progress::Replay));
            assert_eq!(r.input_reserved(), reserved, "duplicate allocated another body");
            assert_eq!(r.reply(), Some(&Ok(alloc::vec![4, 5])));
            assert!(r.request(t).is_none(), "completed mutation became dispatchable");
        }
    }
    #[test]
    fn same_id_changed_header_or_body_never_replays() {
        let bytes = request(2);
        for at in 0..bytes.len() {
            // Every byte is significant, even one in an application-owned scope.
            let mut r = TestRecord::new(1).unwrap();
            let t = send(&mut r, &bytes); r.finish(t, Err(Failure::Denied), 0).unwrap();
            let mut bad = bytes.clone(); bad[at] ^= 128;
            // Altering the ID upward names a NEW request, so test ID bytes separately.
            if (16..24).contains(&at) { continue; }
            assert_eq!(r.write(0, &bad, MAX_RECORD).is_err(), true, "byte {at} was ignored");
            assert!(r.reply().is_none());
            assert!(r.write(0, &bytes, MAX_RECORD).is_err(), "poisoned fid recovered silently");
        }
    }
    #[test]
    fn failures_are_replayed_and_old_ids_cannot_replace_newer_results() {
        let mut r = TestRecord::new(1).unwrap();
        let t = send(&mut r, &request(3)); r.finish(t, Err(Failure::Conflict), 0).unwrap();
        assert_eq!(r.write(0, &request(3), MAX_RECORD), Ok(Progress::Replay));
        assert_eq!(r.reply(), Some(&Err(Failure::Conflict)));
        let t = send(&mut r, &request(9)); r.finish(t, Ok(Vec::new()), 0).unwrap();
        assert_eq!(r.write(0, &request(3), MAX_RECORD), Err(Failure::Invalid));
    }
    #[test]
    fn pending_write_refusal_does_not_lose_admission_and_cancel_burns_id() {
        let mut r = TestRecord::new(1).unwrap(); let t = send(&mut r, &request(1));
        assert_eq!(r.write(0, &request(2), MAX_RECORD), Err(Failure::Busy));
        assert!(r.request(t).is_some());
        assert_eq!(r.cancel(), Some(t));
        assert_eq!(r.input_reserved(), 2 * HEADER_BYTES);
        assert_eq!(r.output_reserved(), 0);
        assert_eq!(r.finish(t, Ok(Vec::new()), MAX_RECORD), Err(Failure::Gone));
        assert_eq!(r.write(0, &request(1), MAX_RECORD), Err(Failure::Invalid));
        r.cancel();
        let fresh = send(&mut r, &request(2));
        assert_eq!(r.finish(t, Ok(Vec::new()), MAX_RECORD), Err(Failure::Gone));
        r.finish(fresh, Ok(Vec::new()), 0).unwrap();
    }
    #[test]
    fn clunk_and_fid_reuse_cannot_accept_an_old_completion() {
        let mut old = TestRecord::new(100).unwrap(); let t = send(&mut old, &request(1));
        drop(old);
        let mut new = TestRecord::new(101).unwrap(); let fresh = send(&mut new, &request(1));
        assert_eq!(new.finish(t, Ok(Vec::new()), 0), Err(Failure::Gone));
        assert!(new.request(fresh).is_some());
        assert_ne!(t.incarnation(), fresh.incarnation());
    }
    #[test]
    fn surplus_wrong_offsets_and_invalid_bodies_never_dispatch() {
        let bytes = request(1);
        for split in 1..bytes.len() {
            let mut r = TestRecord::new(1).unwrap();
            assert_eq!(r.write(0, &bytes[..split], MAX_RECORD), Ok(Progress::Partial));
            assert!(r.write(split as u64 + 1, &bytes[split..], MAX_RECORD).is_err());
        }
        let mut r = TestRecord::new(1).unwrap(); let mut extra = bytes.clone(); extra.push(0);
        assert!(r.write(0, &extra, MAX_RECORD).is_err());
        r.cancel(); let mut bad = request(2); bad[24..32].fill(0);
        assert!(r.write(0, &bad, MAX_RECORD).is_err());
        r.cancel(); let mut response = Request::Hello.encode(3).unwrap(); response[12] = 1;
        assert!(r.write(0, &response, MAX_RECORD).is_err());
    }
    #[test]
    fn all_fids_share_input_and_output_budgets_including_cache_capacity() {
        let mut records: [TestRecord; 8] = core::array::from_fn(|i| TestRecord::new(i as u64 + 1).unwrap());
        let bytes = Request::Write { transfer: 1, offset: 0, data: &alloc::vec![b'x'; 16384] }.encode(1).unwrap();
        let budget = |rs: &[TestRecord], own: usize| MAX_RECORD - rs.iter().enumerate()
            .filter(|(i,_)| *i != own).map(|(_,r)| r.input_reserved()).sum::<usize>();
        let t = match records[0].write(0, &bytes, budget(&records, 0)).unwrap() {
            Progress::Dispatch(t) => t, _ => unreachable!()
        };
        let mut response = Vec::with_capacity(16384); response.push(1);
        records[0].finish(t, Ok(response), MAX_RECORD).unwrap();
        assert_eq!(records[0].output_reserved(), 16384, "count capacity, not length");
        let allowance = budget(&records, 1);
        assert_eq!(records[1].write(0, &bytes, allowance), Err(Failure::TooLarge));
        assert!(records.iter().map(Record::input_reserved).sum::<usize>() <= MAX_RECORD);
        let t = send(&mut records[2], &request(1));
        assert_eq!(records[2].finish(t, Ok(Vec::with_capacity(16385)),
            MAX_RECORD - records[0].output_reserved()), Err(Failure::TooLarge));
        assert!(records[2].request(t).is_some());
        for r in &mut records { r.cancel(); }
        assert_eq!(records.iter().map(Record::input_reserved).sum::<usize>(), 8 * 48);
        assert_eq!(records.iter().map(Record::output_reserved).sum::<usize>(), 0);
    }
    #[test]
    fn new_header_reuses_request_budget_and_cancel_clears_cached_output() {
        let mut r = TestRecord::new(1).unwrap();
        let t = send(&mut r, &request(1)); r.finish(t, Ok(alloc::vec![5; 10]), 10).unwrap();
        let bytes = request(2); let exact = r.input_reserved();
        assert!(matches!(r.write(0, &bytes, exact), Ok(Progress::Dispatch(_))));
        assert_eq!(r.input_reserved(), exact); assert_eq!(r.output_reserved(), 0);
        r.cancel(); assert!(r.reply().is_none());
        let hello = Request::Hello.encode(u64::MAX).unwrap();
        let t = send(&mut r, &hello); r.finish(t, Ok(Vec::new()), 0).unwrap();
        assert_eq!(r.write(0, &hello, MAX_RECORD), Ok(Progress::Replay));
        r.cancel(); assert_eq!(r.write(0, &hello, MAX_RECORD), Err(Failure::Invalid));
    }
}
