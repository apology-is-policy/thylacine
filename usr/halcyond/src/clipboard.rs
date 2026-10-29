//! Bounded clipboard storage; the adapter must authenticate owners and focus.
//!
//! Prepare pins/validates before an ordered focus check. Only its matching live
//! reply may call admit_read/publish. Tickets carry no application authority.
//! Publication moves a preallocated immutable buffer and never allocates. The
//! adapter must serialize focus replies, revocation and these calls, cancel on
//! peer/context loss, and cancel_all_transfers at trusted-seat takeover.
use alloc::{rc::Rc, vec::Vec};
use libhalcyon::interaction_body::{Scope, IDLE_MS, LIFETIME_MS, READ_SLOTS, WRITE_SLOTS};
use libhalcyon::interaction_wire::{clipboard_text, Failure, MAX_CHUNK, MAX_TEXT};

pub const PAYLOAD_CEILING: usize = (1 + READ_SLOTS as usize + WRITE_SLOTS as usize) * MAX_TEXT;

/// Identity supplied by the authenticated broker, never decoded from a peer body.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Owner {
    pub connection: u64,
    pub scope: Scope,
}
impl Owner {
    fn valid(self) -> bool {
        self.connection != 0
            && self.scope.session != 0
            && self.scope.controller != 0
            && self.scope.context != 0
            && self.scope.epoch != 0
    }
}

#[derive(Clone, Copy)]
struct Meta {
    owner: Owner,
    id: u64,
    started: u64,
    touched: u64,
}
impl Meta {
    fn expired(self, now: u64) -> bool {
        now.checked_sub(self.started)
            .is_none_or(|d| d >= LIFETIME_MS as u64)
            || now
                .checked_sub(self.touched)
                .is_none_or(|d| d >= IDLE_MS as u64)
    }
}
struct Write {
    meta: Meta,
    data: Rc<Vec<u8>>,
    declared: usize,
    prepared: bool,
}
struct Read {
    meta: Meta,
    data: Rc<Vec<u8>>,
    generation: u64,
    admitted: bool,
}

/// A one-use pending operation; the broker binds it to one focus-check request.
#[derive(Debug, PartialEq, Eq)]
pub struct ReadTicket {
    owner: Owner,
    id: u64,
}
#[derive(Debug, PartialEq, Eq)]
pub struct CommitTicket {
    owner: Owner,
    id: u64,
    next: u64,
}
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Snapshot {
    pub transfer: u64,
    pub generation: u64,
    pub length: usize,
}

pub struct Clipboard {
    session: u64,
    current: Rc<Vec<u8>>,
    generation: u64,
    next_id: u64,
    writes: [Option<Write>; WRITE_SLOTS as usize],
    reads: [Option<Read>; READ_SLOTS as usize],
    pending_commit: Option<u64>,
}
impl Clipboard {
    pub fn new(session: u64) -> Result<Self, Failure> {
        if session == 0 {
            return Err(Failure::Invalid);
        }
        Ok(Self {
            session,
            current: Rc::new(Vec::new()),
            generation: 0,
            next_id: 1,
            writes: [None, None],
            reads: [None, None],
            pending_commit: None,
        })
    }
    pub fn generation(&self) -> u64 {
        self.generation
    }

    /// Integrate expiry into the event loop's next wakeup, without periodic polling.
    pub fn next_deadline(&self) -> Option<u64> {
        let deadline = |m: Meta| {
            m.started
                .saturating_add(LIFETIME_MS as u64)
                .min(m.touched.saturating_add(IDLE_MS as u64))
        };
        self.writes
            .iter()
            .flatten()
            .map(|w| deadline(w.meta))
            .chain(self.reads.iter().flatten().map(|r| deadline(r.meta)))
            .min()
    }

    /// Conservative capacity accounting; shared snapshots may be counted twice.
    pub fn payload_reservation(&self) -> usize {
        self.current.capacity()
            + self
                .writes
                .iter()
                .flatten()
                .map(|w| w.data.capacity())
                .sum::<usize>()
            + self
                .reads
                .iter()
                .flatten()
                .map(|r| r.data.capacity())
                .sum::<usize>()
    }
    fn allocate_id(&mut self) -> Result<u64, Failure> {
        let id = self.next_id;
        self.next_id = id.checked_add(1).ok_or(Failure::Busy)?;
        Ok(id)
    }
    fn available(&self, owner: Owner) -> Result<(), Failure> {
        if !owner.valid() {
            return Err(Failure::Invalid);
        }
        if owner.scope.session != self.session {
            return Err(Failure::Denied);
        }
        // Context changes cannot multiply a controller's staging allowance.
        let same = |m: &Meta| {
            m.owner.scope.session == owner.scope.session
                && m.owner.scope.controller == owner.scope.controller
        };
        if self.writes.iter().flatten().any(|w| same(&w.meta))
            || self.reads.iter().flatten().any(|r| same(&r.meta))
        {
            return Err(Failure::Busy);
        }
        Ok(())
    }
    fn write_index(&mut self, owner: Owner, id: u64, now: u64) -> Result<usize, Failure> {
        let i = self
            .writes
            .iter()
            .position(|w| w.as_ref().is_some_and(|w| w.meta.id == id))
            .ok_or(Failure::Gone)?;
        let m = self.writes[i].as_ref().unwrap().meta;
        if m.owner != owner {
            return Err(Failure::Denied);
        }
        if m.expired(now) {
            self.remove_write(i);
            return Err(Failure::Timeout);
        }
        Ok(i)
    }
    fn read_index(&mut self, owner: Owner, id: u64, now: u64) -> Result<usize, Failure> {
        let i = self
            .reads
            .iter()
            .position(|r| r.as_ref().is_some_and(|r| r.meta.id == id))
            .ok_or(Failure::Gone)?;
        let m = self.reads[i].as_ref().unwrap().meta;
        if m.owner != owner {
            return Err(Failure::Denied);
        }
        if m.expired(now) {
            self.reads[i] = None;
            return Err(Failure::Timeout);
        }
        Ok(i)
    }
    fn remove_write(&mut self, i: usize) {
        if let Some(w) = self.writes[i].take() {
            if self.pending_commit == Some(w.meta.id) {
                self.pending_commit = None;
            }
        }
    }

    /// Call only after the broker admits this BeginCopy through authoritative focus.
    pub fn begin_admitted(
        &mut self,
        owner: Owner,
        length: usize,
        now: u64,
    ) -> Result<u64, Failure> {
        self.expire(now);
        self.available(owner)?;
        if length > MAX_TEXT {
            return Err(Failure::TooLarge);
        }
        let i = self
            .writes
            .iter()
            .position(Option::is_none)
            .ok_or(Failure::Busy)?;
        let mut data = Vec::new();
        data.try_reserve_exact(length)
            .map_err(|_| Failure::NoMemory)?;
        if data.capacity() > MAX_TEXT {
            return Err(Failure::NoMemory);
        }
        let id = self.allocate_id()?;
        self.writes[i] = Some(Write {
            meta: Meta {
                owner,
                id,
                started: now,
                touched: now,
            },
            data: Rc::new(data),
            declared: length,
            prepared: false,
        });
        Ok(id)
    }
    pub fn write(
        &mut self,
        owner: Owner,
        id: u64,
        offset: usize,
        bytes: &[u8],
        now: u64,
    ) -> Result<usize, Failure> {
        let i = self.write_index(owner, id, now)?;
        let w = self.writes[i].as_mut().unwrap();
        if w.prepared {
            return Err(Failure::Busy);
        }
        if offset != w.data.len() || bytes.is_empty() {
            return Err(Failure::Invalid);
        }
        if bytes.len() > MAX_CHUNK || bytes.len() > w.declared - w.data.len() {
            return Err(Failure::TooLarge);
        }
        // The full declared capacity was reserved before this transfer was returned.
        Rc::get_mut(&mut w.data)
            .ok_or(Failure::Busy)?
            .extend_from_slice(bytes);
        w.meta.touched = now;
        Ok(bytes.len())
    }
    pub fn prepare_commit(
        &mut self,
        owner: Owner,
        id: u64,
        expected: u64,
        now: u64,
    ) -> Result<CommitTicket, Failure> {
        let i = self.write_index(owner, id, now)?;
        if self.pending_commit.is_some() {
            return Err(Failure::Busy);
        }
        if expected != self.generation {
            return Err(Failure::Conflict);
        }
        let next = self.generation.checked_add(1).ok_or(Failure::Busy)?;
        let w = self.writes[i].as_mut().unwrap();
        if w.data.len() != w.declared || clipboard_text(&w.data).is_err() {
            return Err(Failure::Invalid);
        }
        w.prepared = true;
        w.meta.touched = now;
        self.pending_commit = Some(id);
        Ok(CommitTicket { owner, id, next })
    }
    /// Matching live focus success only; no allocation or text scan after admission.
    pub fn publish(&mut self, ticket: CommitTicket, now: u64) -> Result<u64, Failure> {
        let i = self.write_index(ticket.owner, ticket.id, now)?;
        if self.pending_commit != Some(ticket.id)
            || ticket.next.checked_sub(1) != Some(self.generation)
        {
            return Err(Failure::Gone);
        }
        let w = self.writes[i].take().unwrap();
        self.pending_commit = None;
        self.current = w.data;
        self.generation = ticket.next;
        Ok(self.generation)
    }
    pub fn reject_commit(&mut self, ticket: CommitTicket) {
        let _ = self.cancel(ticket.owner, ticket.id);
    }

    /// Pin the candidate generation before asking Tapestry to admit this Get.
    pub fn prepare_read(&mut self, owner: Owner, now: u64) -> Result<ReadTicket, Failure> {
        self.expire(now);
        self.available(owner)?;
        let i = self
            .reads
            .iter()
            .position(Option::is_none)
            .ok_or(Failure::Busy)?;
        let id = self.allocate_id()?;
        self.reads[i] = Some(Read {
            meta: Meta {
                owner,
                id,
                started: now,
                touched: now,
            },
            data: self.current.clone(),
            generation: self.generation,
            admitted: false,
        });
        Ok(ReadTicket { owner, id })
    }
    /// The ticket is consumed even on refusal; the adapter never retries admissions.
    pub fn admit_read(&mut self, ticket: ReadTicket, now: u64) -> Result<Snapshot, Failure> {
        let i = self.read_index(ticket.owner, ticket.id, now)?;
        let r = self.reads[i].as_mut().unwrap();
        r.admitted = true;
        r.meta.touched = now;
        Ok(Snapshot {
            transfer: r.meta.id,
            generation: r.generation,
            length: r.data.len(),
        })
    }
    pub fn reject_read(&mut self, ticket: ReadTicket) {
        let _ = self.cancel(ticket.owner, ticket.id);
    }
    pub fn read(
        &mut self,
        owner: Owner,
        id: u64,
        offset: usize,
        count: usize,
        now: u64,
    ) -> Result<&[u8], Failure> {
        let i = self.read_index(owner, id, now)?;
        let r = self.reads[i].as_mut().unwrap();
        if !r.admitted {
            return Err(Failure::Denied);
        }
        if count == 0 || offset > r.data.len() {
            return Err(Failure::Invalid);
        }
        if count > MAX_CHUNK {
            return Err(Failure::TooLarge);
        }
        r.meta.touched = now;
        let end = offset + count.min(r.data.len() - offset);
        Ok(&r.data[offset..end])
    }
    pub fn cancel(&mut self, owner: Owner, id: u64) -> Result<bool, Failure> {
        for i in 0..self.writes.len() {
            if let Some(w) = &self.writes[i] {
                if w.meta.id == id {
                    if w.meta.owner != owner {
                        return Err(Failure::Denied);
                    }
                    self.remove_write(i);
                    return Ok(true);
                }
            }
        }
        for r in &mut self.reads {
            if let Some(v) = r {
                if v.meta.id == id {
                    if v.meta.owner != owner {
                        return Err(Failure::Denied);
                    }
                    *r = None;
                    return Ok(true);
                }
            }
        }
        Ok(false)
    }
    fn discard(&mut self, predicate: impl Fn(Meta) -> bool, reads: bool) {
        for i in 0..self.writes.len() {
            if self.writes[i].as_ref().is_some_and(|w| predicate(w.meta)) {
                self.remove_write(i);
            }
        }
        if reads {
            for r in &mut self.reads {
                if r.as_ref().is_some_and(|r| predicate(r.meta)) {
                    *r = None;
                }
            }
        }
    }
    pub fn expire(&mut self, now: u64) {
        self.discard(|m| m.expired(now), true);
    }
    pub fn drop_connection(&mut self, connection: u64) {
        self.discard(|m| m.owner.connection == connection, true);
    }
    pub fn drop_owner(&mut self, owner: Owner) {
        self.discard(|m| m.owner == owner, true);
    }
    /// Ordered focus loss cancels writes; an already admitted read may finish.
    pub fn lose_focus(&mut self, owner: Owner) {
        self.discard(|m| m.owner == owner, false);
    }
    pub fn cancel_all_transfers(&mut self) {
        self.discard(|_| true, true);
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use alloc::vec;
    fn owner(n: u64) -> Owner {
        Owner {
            connection: n,
            scope: Scope {
                session: 7,
                controller: n,
                context: n,
                epoch: 1,
            },
        }
    }
    fn clipboard() -> Clipboard {
        Clipboard::new(7).unwrap()
    }
    fn copy(c: &mut Clipboard, o: Owner, text: &[u8], now: u64) -> u64 {
        let id = c.begin_admitted(o, text.len(), now).unwrap();
        for (i, b) in text.chunks(MAX_CHUNK).enumerate() {
            c.write(o, id, i * MAX_CHUNK, b, now).unwrap();
        }
        let ticket = c.prepare_commit(o, id, c.generation(), now).unwrap();
        c.publish(ticket, now).unwrap()
    }
    fn get(c: &mut Clipboard, o: Owner, now: u64) -> Snapshot {
        let t = c.prepare_read(o, now).unwrap();
        c.admit_read(t, now).unwrap()
    }
    #[test]
    fn publication_is_atomic_and_pins_before_admission() {
        let mut c = clipboard();
        assert_eq!(c.generation(), 0);
        assert_eq!(copy(&mut c, owner(1), b"first", 0), 1);
        let pending = c.prepare_read(owner(2), 0).unwrap();
        let id = pending.id;
        assert_eq!(c.read(owner(2), id, 0, 16, 0), Err(Failure::Denied));
        copy(&mut c, owner(1), b"second", 0);
        let s = c.admit_read(pending, 0).unwrap();
        assert_eq!((s.generation, s.length), (1, 5));
        assert_eq!(c.read(owner(2), id, 0, 16, 0).unwrap(), b"first");
        c.lose_focus(owner(2));
        assert_eq!(c.read(owner(2), id, 2, 16, 0).unwrap(), b"rst");
        assert_eq!(c.read(owner(2), id, 5, 16, 0).unwrap(), b"");
        assert_eq!(c.read(owner(2), id, 6, 16, 0), Err(Failure::Invalid));
        let latest = get(&mut c, owner(3), 0);
        assert_eq!(
            c.read(owner(3), latest.transfer, 0, 16, 0).unwrap(),
            b"second"
        );
        c.drop_connection(1); // Successful source exit cannot destroy clipboard.
        assert_eq!(c.generation(), 2);
    }
    #[test]
    fn incomplete_invalid_and_denied_copy_preserve_the_current_value() {
        let mut c = clipboard();
        copy(&mut c, owner(1), b"retained", 0);
        let id = c.begin_admitted(owner(1), 3, 0).unwrap();
        c.write(owner(1), id, 0, &[0xc3], 0).unwrap();
        assert_eq!(c.prepare_commit(owner(1), id, 1, 0), Err(Failure::Invalid));
        // UTF-8 can span chunks; framing cannot require each chunk to be text.
        c.write(owner(1), id, 1, &[0xa9, b'\n'], 0).unwrap();
        let t = c.prepare_commit(owner(1), id, 1, 0).unwrap();
        assert_eq!(c.write(owner(1), id, 3, b"x", 0), Err(Failure::Busy));
        c.reject_commit(t);
        assert_eq!(c.generation(), 1);
        for invalid in [&b"a\0"[..], &b"a\r"[..], &b"\xff"[..], &b"\x1b"[..]] {
            let id = c.begin_admitted(owner(1), invalid.len(), 0).unwrap();
            c.write(owner(1), id, 0, invalid, 0).unwrap();
            assert_eq!(c.prepare_commit(owner(1), id, 1, 0), Err(Failure::Invalid));
            c.cancel(owner(1), id).unwrap();
        }
        let s = get(&mut c, owner(2), 0);
        assert_eq!(c.read(owner(2), s.transfer, 0, 16, 0).unwrap(), b"retained");
        assert_eq!(copy(&mut c, owner(1), b"", 0), 2);
        c.cancel(owner(2), s.transfer).unwrap();
        let empty = get(&mut c, owner(2), 0);
        assert_eq!((empty.generation, empty.length), (2, 0));
    }
    #[test]
    fn commit_serialization_and_generation_conflict_never_overwrite_newer_copy() {
        let mut c = clipboard();
        let a = c.begin_admitted(owner(1), 0, 0).unwrap();
        let b = c.begin_admitted(owner(2), 0, 0).unwrap();
        let t = c.prepare_commit(owner(1), a, 0, 0).unwrap();
        assert_eq!(c.prepare_commit(owner(2), b, 0, 0), Err(Failure::Busy));
        assert_eq!(c.generation(), 0);
        c.publish(t, 0).unwrap();
        assert_eq!(c.prepare_commit(owner(2), b, 0, 0), Err(Failure::Conflict));
        let t = c.prepare_commit(owner(2), b, 1, 0).unwrap();
        c.publish(t, 0).unwrap();
        assert_eq!(c.generation(), 2);
        assert_eq!(c.cancel(owner(1), a), Ok(false));
    }
    #[test]
    fn owner_connection_epoch_and_session_are_not_interchangeable() {
        let mut c = clipboard();
        let id = c.begin_admitted(owner(1), 1, 0).unwrap();
        let mut wrong = [owner(1); 5];
        wrong[0].connection += 1;
        wrong[1].scope.session += 1;
        wrong[2].scope.controller += 1;
        wrong[3].scope.context += 1;
        wrong[4].scope.epoch += 1;
        for o in wrong {
            assert_eq!(c.write(o, id, 0, b"x", 0), Err(Failure::Denied));
            assert_eq!(c.cancel(o, id), Err(Failure::Denied));
            c.drop_owner(o);
        }
        assert_eq!(c.begin_admitted(wrong[1], 0, 0), Err(Failure::Denied));
        assert_eq!(c.begin_admitted(wrong[4], 0, 0), Err(Failure::Busy));
        assert!(c.prepare_read(owner(1), 0).is_err());
        c.write(owner(1), id, 0, b"x", 0).unwrap();
        let t = c.prepare_commit(owner(1), id, 0, 0).unwrap();
        c.drop_owner(owner(1));
        assert_eq!(c.publish(t, 0), Err(Failure::Gone));
        let id2 = c.begin_admitted(owner(1), 0, 0).unwrap();
        assert_ne!(id2, id);
    }
    #[test]
    fn stale_admission_cannot_resurrect_cancelled_work_or_cross_trusted_takeover() {
        let mut c = clipboard();
        copy(&mut c, owner(1), b"kept", 0);
        let r = c.prepare_read(owner(2), 0).unwrap();
        let w = c.begin_admitted(owner(1), 0, 0).unwrap();
        let t = c.prepare_commit(owner(1), w, 1, 0).unwrap();
        c.cancel_all_transfers();
        assert_eq!(c.admit_read(r, 0), Err(Failure::Gone));
        assert_eq!(c.publish(t, 0), Err(Failure::Gone));
        assert_eq!(c.generation(), 1);
        assert_eq!(c.payload_reservation(), 4);
        let r = c.prepare_read(owner(2), 0).unwrap();
        c.drop_connection(2);
        assert_eq!(c.admit_read(r, 0), Err(Failure::Gone));
        let w = c.begin_admitted(owner(1), 0, 0).unwrap();
        let t = c.prepare_commit(owner(1), w, 1, 0).unwrap();
        c.lose_focus(owner(1));
        assert_eq!(c.publish(t, 0), Err(Failure::Gone));
    }
    #[test]
    fn idle_total_and_regressing_clock_expire_without_publication() {
        let mut c = clipboard();
        let id = c.begin_admitted(owner(1), 6, 0).unwrap();
        c.write(owner(1), id, 0, b"a", 29_999).unwrap();
        c.write(owner(1), id, 1, b"b", 59_998).unwrap();
        c.write(owner(1), id, 2, b"c", 89_997).unwrap();
        c.write(owner(1), id, 3, b"d", 119_996).unwrap();
        assert_eq!(
            c.write(owner(1), id, 4, b"e", 120_000),
            Err(Failure::Timeout)
        );
        let id = c.begin_admitted(owner(1), 0, 120_000).unwrap();
        let t = c.prepare_commit(owner(1), id, 0, 120_000).unwrap();
        assert_eq!(c.publish(t, 150_000), Err(Failure::Timeout));
        let r = c.prepare_read(owner(2), 150_000).unwrap();
        assert_eq!(c.admit_read(r, 180_000), Err(Failure::Timeout));
        let id = c.begin_admitted(owner(1), 1, 180_000).unwrap();
        assert_eq!(
            c.write(owner(1), id, 0, b"x", 179_999),
            Err(Failure::Timeout)
        );
        assert_eq!(c.generation(), 0);
        assert_eq!(c.payload_reservation(), 0);
    }
    #[test]
    fn five_payload_slots_are_the_complete_storage_ceiling() {
        let mut c = clipboard();
        let value = vec![b'x'; MAX_TEXT];
        copy(&mut c, owner(1), &value, 0);
        let old = get(&mut c, owner(2), 0);
        copy(&mut c, owner(1), &value, 0);
        let newer = get(&mut c, owner(3), 0);
        copy(&mut c, owner(1), &value, 0);
        assert_eq!(c.prepare_read(owner(4), 0), Err(Failure::Busy));
        let a = c.begin_admitted(owner(4), MAX_TEXT, 0).unwrap();
        c.begin_admitted(owner(5), MAX_TEXT, 0).unwrap();
        assert_eq!(c.begin_admitted(owner(6), 0, 0), Err(Failure::Busy));
        assert_eq!(c.payload_reservation(), PAYLOAD_CEILING);
        assert_eq!(PAYLOAD_CEILING, 5 * 1024 * 1024);
        assert_ne!(old.generation, newer.generation);
        assert_eq!(c.write(owner(4), a, 1, b"x", 0), Err(Failure::Invalid));
        assert_eq!(
            c.write(owner(4), a, 0, &value[..MAX_CHUNK + 1], 0),
            Err(Failure::TooLarge)
        );
        c.expire(30_000);
        assert_eq!(c.payload_reservation(), MAX_TEXT);
        assert_eq!(
            c.begin_admitted(owner(4), MAX_TEXT + 1, 30_000),
            Err(Failure::TooLarge)
        );
    }
    #[test]
    fn monotonic_identifiers_refuse_exhaustion_without_wrap_or_mutation() {
        let mut c = clipboard();
        c.next_id = u64::MAX;
        assert_eq!(c.begin_admitted(owner(1), 0, 0), Err(Failure::Busy));
        assert_eq!(c.prepare_read(owner(2), 0), Err(Failure::Busy));
        assert_eq!(c.payload_reservation(), 0);
        c.next_id = 1;
        c.generation = u64::MAX;
        let id = c.begin_admitted(owner(1), 0, 0).unwrap();
        assert_eq!(
            c.prepare_commit(owner(1), id, u64::MAX, 0),
            Err(Failure::Busy)
        );
        assert_eq!(c.generation(), u64::MAX);
    }

    #[test]
    fn deadlines_and_rejected_reads_release_their_slots() {
        let mut c = clipboard();
        assert_eq!(c.next_deadline(), None);
        let id = c.begin_admitted(owner(1), 2, 100).unwrap();
        assert_eq!(c.next_deadline(), Some(30_100));
        let pending = c.prepare_read(owner(2), 150).unwrap();
        c.write(owner(1), id, 0, b"x", 200).unwrap();
        assert_eq!(c.next_deadline(), Some(30_150));
        c.reject_read(pending);
        assert_eq!(c.next_deadline(), Some(30_200));
        c.cancel_all_transfers();
        let s = get(&mut c, owner(2), 200);
        assert_eq!(
            c.read(owner(1), s.transfer, 0, 1, 200),
            Err(Failure::Denied)
        );
        assert_eq!(
            c.read(owner(2), s.transfer, 0, MAX_CHUNK + 1, 200),
            Err(Failure::TooLarge)
        );
        assert_eq!(
            c.read(owner(2), s.transfer, 0, 0, 200),
            Err(Failure::Invalid)
        );
        c.expire(30_200);
        assert_eq!(c.next_deadline(), None);
    }
}
