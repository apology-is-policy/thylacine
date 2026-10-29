//! Private client CQ reader. Owns no mapping or payload storage.
use super::service_abi::BufferReceipt;
use core::{
    ptr,
    sync::atomic::{AtomicU32, Ordering},
};
#[repr(C)]
#[derive(Clone, Copy, Debug)]
pub(crate) struct Completion {
    pub user_data: u64,
    pub result: i32,
    pub flags: u32,
}
const _: () = assert!(core::mem::size_of::<Completion>() == 16);
const _: () = assert!(core::mem::offset_of!(Completion, result) == 8);
const _: () = assert!(core::mem::offset_of!(Completion, flags) == 12);
#[derive(Debug)]
pub(crate) struct Record {
    pub cqe: Completion,
    pub receipt: BufferReceipt,
}
#[derive(Debug, PartialEq, Eq)]
pub(crate) enum CqError {
    Geometry,
    Corrupt,
}
/// Neither Clone nor Send/Sync. Exclusive creator-side consumer only.
/// The owned client must retain the mapping and never expose legacy raw reap.
pub(crate) struct PairedCq {
    head: *const AtomicU32,
    tail: *const AtomicU32,
    cq: *const Completion,
    receipts: *const BufferReceipt,
    entries: u32,
    next: u32,
}
impl PairedCq {
    /// # Safety
    /// base must own a live, writable, aligned shared mapping of len bytes until
    /// this reader is dropped. Only the kernel producer may write CQ/receipts,
    /// publishing both before a release-tail; it must not reuse an entry before
    /// acquire-head. This is the sole head writer; offsets are setup outputs.
    /// The outer owner must also validate SQ regions before publishing any SQE.
    pub(crate) unsafe fn new(
        base: *mut u8,
        len: usize,
        hdr: usize,
        cq: usize,
        receipts: usize,
        entries: u32,
    ) -> Result<Self, CqError> {
        if base.is_null()
            || base as usize % 8 != 0
            || len > isize::MAX as usize
            || (base as usize).checked_add(len).is_none()
            || entries == 0
            || !entries.is_power_of_two()
            || entries > 8192
        {
            return Err(CqError::Geometry);
        }
        let regions = [
            (hdr, 64usize),
            (cq, entries as usize * 16),
            (receipts, entries as usize * 32),
        ];
        for (i, &(start, size)) in regions.iter().enumerate() {
            let end = start.checked_add(size).ok_or(CqError::Geometry)?;
            if start % 8 != 0 || end > len {
                return Err(CqError::Geometry);
            }
            for &(prior, bytes) in &regions[..i] {
                if start < prior + bytes && prior < end {
                    return Err(CqError::Geometry);
                }
            }
        }
        let head = unsafe { base.add(hdr + 16).cast::<AtomicU32>() };
        let tail = unsafe { base.add(hdr + 20).cast::<AtomicU32>() };
        let next = unsafe { (*head).load(Ordering::Relaxed) };
        Ok(Self {
            head,
            tail,
            cq: unsafe { base.add(cq).cast() },
            receipts: unsafe { base.add(receipts).cast() },
            entries,
            next,
        })
    }
    pub(crate) fn reap(&mut self) -> Result<Option<Record>, CqError> {
        // Local cursor cannot be raced through a safe shared reference.
        if unsafe { (*self.head).load(Ordering::Relaxed) } != self.next {
            return Err(CqError::Corrupt);
        }
        let tail = unsafe { (*self.tail).load(Ordering::Acquire) };
        let pending = tail.wrapping_sub(self.next);
        if pending > self.entries {
            return Err(CqError::Corrupt);
        }
        if pending == 0 {
            return Ok(None);
        }
        let slot = (self.next & (self.entries - 1)) as usize;
        // Producer owns no access to this slot until the single release below.
        let cqe = unsafe { ptr::read(self.cq.add(slot)) };
        let receipt = unsafe { ptr::read(self.receipts.add(slot)) };
        let record = Record { cqe, receipt };
        self.next = self.next.wrapping_add(1);
        unsafe {
            (*self.head).store(self.next, Ordering::Release);
        }
        Ok(Some(record))
    }
}
