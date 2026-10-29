//! Uninstalled request/CQ correlation metadata, not an I/O or future owner.
extern crate alloc;
use alloc::{boxed::Box, rc::Rc};
use core::{marker::PhantomData, sync::atomic::{AtomicU64, Ordering}};
use super::{service_abi::{SlotRef, BufferReceipt}, paired_cq::{Record, Completion}};
static NEXT_COOKIE: AtomicU64 = AtomicU64::new(1);
fn cookie(counter: &AtomicU64) -> Result<u64, Error> {
    counter.fetch_update(Ordering::Relaxed, Ordering::Relaxed, |n| n.checked_add(1)).map_err(|_| Error::Exhausted)
}
fn valid(r: SlotRef) -> bool { r.slot < 64 && r.reserved == 0 && r.incarnation != 0 }
fn zero(r: BufferReceipt) -> bool {
    r.pool.slot == 0 && r.pool.reserved == 0 && r.pool.incarnation == 0 && r.member == 0 && r.reserved == 0 && r.lease == 0
}
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) enum Kind { Control, FixedRead(u32), FixedWrite(u32), PooledRead { pool: SlotRef, limit: u32, multi: bool } }
#[derive(Debug, PartialEq, Eq)]
pub(crate) enum Error { Geometry, Full, Exhausted, Stale, State, Record }
#[derive(Clone, Copy, PartialEq, Eq)]
enum Phase { Prepared, Active, Terminal }
struct Entry { cookie: u64, scope: SlotRef, kind: Kind, phase: Phase, abandoned: bool, terminal: Option<Completion> }
#[derive(Debug)]
pub(crate) struct Delivery { pub cookie: u64, pub scope: SlotRef, pub kind: Kind, pub terminal: bool, pub abandoned: bool, pub record: Record }
/// Allocated once at checked CQ capacity; at most 8192 records. Unique global
/// cookies prevent another library ring's coincident slot/request from matching.
/// No raw user-selected user_data, Clone, Send or Sync. Exclusive creator only.
pub(crate) struct RequestBook { entries: Box<[Option<Entry>]>, _creator: PhantomData<Rc<()>> }
impl RequestBook {
    pub(crate) fn new(cq_entries: usize) -> Result<Self, Error> {
        if cq_entries == 0 || cq_entries > 8192 || !cq_entries.is_power_of_two() { return Err(Error::Geometry); }
        Ok(Self { entries: (0..cq_entries).map(|_| None).collect::<alloc::vec::Vec<_>>().into_boxed_slice(), _creator: PhantomData })
    }
    /// Caller retains scope, pool and fixed buffer owners independently. Reserve
    /// before publishing an SQE; use this exact cookie as user_data.
    pub(crate) fn reserve(&mut self, scope: SlotRef, kind: Kind) -> Result<u64, Error> {
        if !valid(scope) { return Err(Error::Geometry); }
        if let Kind::PooledRead { pool, limit, .. } = kind {
            if !valid(pool) || limit == 0 { return Err(Error::Geometry); }
        }
        let slot = self.entries.iter().position(Option::is_none).ok_or(Error::Full)?;
        let id = cookie(&NEXT_COOKIE)?;
        self.entries[slot] = Some(Entry { cookie: id, scope, kind, phase: Phase::Prepared, abandoned: false, terminal: None });
        Ok(id)
    }
    fn index(&self, id: u64) -> Result<usize, Error> {
        self.entries.iter().position(|e| e.as_ref().is_some_and(|e| e.cookie == id)).ok_or(Error::Stale)
    }
    /// Only before release-SQ-tail: a failed ENTER is NOT proof of nonadmission.
    pub(crate) fn cancel_unpublished(&mut self, id: u64) -> Result<(), Error> {
        let i = self.index(id)?;
        if self.entries[i].as_ref().unwrap().phase != Phase::Prepared { return Err(Error::State); }
        self.entries[i] = None; Ok(())
    }
    /// Immediately before release-SQ-tail, with no subsequent fallible local work.
    pub(crate) fn publish(&mut self, id: u64) -> Result<(), Error> {
        let i = self.index(id)?; let e = self.entries[i].as_mut().unwrap();
        if e.phase != Phase::Prepared { return Err(Error::State); }
        e.phase = Phase::Active; Ok(())
    }
    /// Called with the copied paired record from this owned mapping only. All
    /// rejection paths leave ownership intact; outer owner must retire on corrupt
    /// input. Pool ledger must still validate member geometry/lease monotonicity.
    pub(crate) fn accept(&mut self, record: Record) -> Result<Delivery, Error> {
        let i = self.index(record.cqe.user_data)?; let e = self.entries[i].as_mut().unwrap();
        if e.phase != Phase::Active { return Err(Error::State); }
        let Completion { result, flags, .. } = record.cqe;
        match e.kind {
            Kind::PooledRead { pool, limit, multi } if result > 0 => {
                if flags & !5 != 0 || flags & 4 == 0 || (!multi && flags & 1 != 0) || result as u32 > limit ||
                    record.receipt.pool != pool || record.receipt.reserved != 0 || record.receipt.member >= 64 || record.receipt.lease == 0 { return Err(Error::Record); }
            }
            Kind::PooledRead { .. } => { if flags != 0 || !zero(record.receipt) { return Err(Error::Record); } }
            Kind::Control => { if flags != 0 || result > 0 || !zero(record.receipt) { return Err(Error::Record); } }
            Kind::FixedRead(limit) | Kind::FixedWrite(limit) => {
                if flags != 0 || (result > 0 && result as u32 > limit) || !zero(record.receipt) { return Err(Error::Record); }
            }
        }
        let terminal = flags & 1 == 0;
        let delivery = Delivery { cookie: e.cookie, scope: e.scope, kind: e.kind, terminal, abandoned: e.abandoned, record };
        if terminal { e.phase = Phase::Terminal; e.terminal = Some(delivery.record.cqe); }
        Ok(delivery)
    }
    /// Returns scope only on first pending abandonment: executor must request
    /// scope-wide ABORT, including sibling requests. No storage/refund occurs.
    pub(crate) fn abandon(&mut self, id: u64) -> Result<Option<SlotRef>, Error> {
        let i = self.index(id)?; let e = self.entries[i].as_mut().unwrap();
        if e.phase == Phase::Prepared { return Err(Error::State); }
        let abort = e.phase == Phase::Active; e.abandoned = true;
        Ok(if abort { Some(e.scope) } else { None })
    }
    /// Executor consumes once after transferring a payload receipt to its lease
    /// owner and settling fixed-I/O ownership. Terminal storage remains charged
    /// until this call; payload leases remain independent after it.
    pub(crate) fn take_terminal(&mut self, id: u64) -> Result<Completion, Error> {
        let i = self.index(id)?;
        if self.entries[i].as_ref().unwrap().phase != Phase::Terminal { return Err(Error::State); }
        Ok(self.entries[i].take().unwrap().terminal.unwrap())
    }
    pub(crate) fn occupied(&self) -> usize { self.entries.iter().flatten().count() }
}
#[cfg(test)]
pub(crate) fn exhausted_cookie_test() {
    let counter=AtomicU64::new(u64::MAX-1);
    assert_eq!(cookie(&counter),Ok(u64::MAX-1));
    assert_eq!(cookie(&counter),Err(Error::Exhausted));
    assert_eq!(cookie(&counter),Err(Error::Exhausted));
}
