//! Internal bounded receipt ledger for the owned private-service client.
//! This owns metadata only, not storage, syscalls, or CQ consumption. The outer
//! client must copy paired CQE/receipt before acknowledging and own all backing.
use super::service_abi::{BufferReceipt, PoolMember, SlotRef, CQE_SERVICE_BUFFER};

use core::sync::atomic::{AtomicU64, Ordering};
static NEXT_OWNER: AtomicU64 = AtomicU64::new(1);
const MEMBERS: usize = 64;
const MORE: u32 = 1;
const EMPTY_REF: SlotRef = SlotRef { slot: 0, reserved: 0, incarnation: 0 };

#[derive(Debug, PartialEq, Eq)]
pub(crate) enum LeaseError { Geometry, Receipt, Busy, Stale, State, Exhausted }
#[derive(Clone, Copy, PartialEq, Eq)]
enum State { Available, Leased, Returning }
#[derive(Clone, Copy)]
struct Member { range: PoolMember, last: u64, state: State }
const EMPTY_MEMBER: Member = Member {
    range: PoolMember { buffer_index: 0, reserved: 0, offset: 0, length: 0 },
    last: 0, state: State::Available,
};

/// Not Clone or Copy. The eventual PayloadLease owns this token alongside its
/// storage/owner reference. A token alone never grants access to a pointer.
#[derive(Debug)]
pub(crate) struct LeaseToken { owner: u64, receipt: BufferReceipt, length: usize }
impl LeaseToken {
    pub(crate) fn receipt(&self) -> BufferReceipt { self.receipt }
    pub(crate) fn length(&self) -> usize { self.length }
}

pub(crate) struct LeaseLedger {
    owner: u64,
    pool: SlotRef,
    members: [Member; MEMBERS],
    count: usize,
    outstanding: usize,
}
impl LeaseLedger {
    pub(crate) fn new(pool: SlotRef, ranges: &[PoolMember], buffer_lengths: &[u64]) -> Result<Self, LeaseError> {
        if pool.reserved != 0 || pool.incarnation == 0 || pool.slot >= 64 ||
            ranges.is_empty() || ranges.len() > MEMBERS || buffer_lengths.len() > MEMBERS {
            return Err(LeaseError::Geometry);
        }
        let mut members = [EMPTY_MEMBER; MEMBERS];
        for (i, range) in ranges.iter().enumerate() {
            let end = range.offset.checked_add(range.length).ok_or(LeaseError::Geometry)?;
            let size = buffer_lengths.get(range.buffer_index as usize).ok_or(LeaseError::Geometry)?;
            if range.reserved != 0 || range.length == 0 || end > *size {
                return Err(LeaseError::Geometry);
            }
            // The outer owner guarantees different buffer indices are distinct
            // owned allocations; ranges within one allocation must not overlap.
            for prior in &ranges[..i] {
                if false && prior.buffer_index == range.buffer_index && range.offset < prior.offset + prior.length && prior.offset < end {
                    return Err(LeaseError::Geometry);
                }
            }
            members[i].range = *range;
        }
        let owner = NEXT_OWNER.fetch_update(Ordering::Relaxed, Ordering::Relaxed, |n| n.checked_add(1)).map_err(|_| LeaseError::Exhausted)?;
        Ok(Self { owner, pool, members, count: ranges.len(), outstanding: 0 })
    }

    /// For a pooled READ only. Non-payload terminal/error records must have a
    /// completely zero paired receipt. EOF/final never releases earlier leases.
    pub(crate) fn accept_read(&mut self, result: i32, flags: u32, receipt: BufferReceipt) -> Result<Option<LeaseToken>, LeaseError> {
        if flags & !(MORE | CQE_SERVICE_BUFFER) != 0 { return Err(LeaseError::Receipt); }
        if flags & CQE_SERVICE_BUFFER == 0 {
            if flags != 0 || result > 0 || receipt.pool != EMPTY_REF || receipt.member != 0 || receipt.reserved != 0 || receipt.lease != 0 {
                return Err(LeaseError::Receipt);
            }
            return Ok(None);
        }
        if result <= 0 || receipt.pool != self.pool || receipt.reserved != 0 || receipt.lease == 0 || receipt.member as usize >= self.count {
            return Err(LeaseError::Receipt);
        }
        let member = &mut self.members[receipt.member as usize];
        if result as u64 > member.range.length { return Err(LeaseError::Receipt); }
        if member.state != State::Available { return Err(LeaseError::Busy); }
        if receipt.lease <= member.last { return Err(LeaseError::Stale); }
        member.last = receipt.lease;
        member.state = State::Leased;
        self.outstanding += 1;
        Ok(Some(LeaseToken { owner: self.owner, receipt, length: result as usize }))
    }

    fn exact(&mut self, token: &LeaseToken) -> Result<&mut Member, LeaseError> {
        let r = token.receipt;
        if token.owner != self.owner || r.pool != self.pool || r.reserved != 0 || r.member as usize >= self.count || r.lease == 0 { return Err(LeaseError::Stale); }
        let member = &mut self.members[r.member as usize];
        if member.last != r.lease { return Err(LeaseError::Stale); }
        Ok(member)
    }
    /// The outer PayloadLease must end every payload borrow before calling.
    /// Retrying a failed syscall keeps the same Returning token and charge.
    pub(crate) fn begin_return(&mut self, token: &LeaseToken) -> Result<BufferReceipt, LeaseError> {
        let member = self.exact(token)?;
        if member.state != State::Leased && member.state != State::Returning { return Err(LeaseError::State); }
        member.state = State::Returning;
        Ok(token.receipt)
    }
    pub(crate) fn finish_return(&mut self, token: &LeaseToken, syscall_result: i64) -> Result<(), LeaseError> {
        let member = self.exact(token)?;
        if member.state != State::Returning { return Err(LeaseError::State); }
        // Only confirmed success releases the local receipt. Any error, even
        // NOENT, retains ownership until retry or outer-owner proven close.
        if syscall_result == 0 {
            member.state = State::Available;
            self.outstanding -= 1;
        }
        Ok(())
    }
    pub(crate) fn outstanding(&self) -> usize { self.outstanding }
}
