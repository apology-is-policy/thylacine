//! The kernel calls the adapter is built from, and the owner of everything a
//! ring holds in the address space: its descriptor, its mapping, its buffers.
extern crate alloc;
use crate::{
    owned_mapping::{Mapping, Setup},
    ring_payload::RingBackend,
    service_abi::{
        BufferReceipt, BufferReturn, Control, PoolCreate, PoolMember, REGISTER_ABORT_SCOPE,
        REGISTER_QUERY_SERVICE_SLOT, REGISTER_REAP_SERVICE_SLOT, REGISTER_RETURN_SERVICE_BUFFER,
        REGISTER_SERVICE_POOL, REGISTER_SERVICE_SLOT, Reservation, SERVICE_RETIRED, SlotRef,
        Snapshot,
    },
};
use alloc::{boxed::Box, vec::Vec};
use core::{
    cell::{Cell, RefCell},
    ptr::NonNull,
};

pub const POLLIN: i16 = 0x001;
pub const POLLERR: i16 = 0x008;
pub const POLLHUP: i16 = 0x010;
pub const POLLNVAL: i16 = 0x020;

pub(crate) const EBADF: i32 = 9;
pub(crate) const EBUSY: i32 = 16;
pub(crate) const EINTR: i32 = 4;
pub(crate) const EINVAL: i32 = 22;
pub(crate) const EIO: i32 = 5;
pub(crate) const ENOENT: i32 = 2;
pub(crate) const ENOSPC: i32 = 28;
pub(crate) const ETIMEDOUT: i32 = 110;
pub(crate) const ECANCELED: i32 = 125;

pub(crate) const PAGE: usize = 4096;
pub(crate) const ZERO_REF: SlotRef = SlotRef {
    slot: 0,
    reserved: 0,
    incarnation: 0,
};

/// The raw kernel calls a private ring needs. Each method is one system call and
/// returns the kernel's answer unchanged: a value >= 0, or a negative errno.
///
/// # Safety
/// An implementation passes every argument to the kernel and every result back
/// unchanged. `attach` returns a fresh, eager, physically contiguous, zeroed,
/// read-write region (a Loom registered buffer), and `detach` removes exactly
/// the region it names. Only `poll` may block, and no call reenters the adapter.
pub unsafe trait Kernel {
    /// SYS_LOOM_SETUP: the kernel reads `params.flags` and fills the rest.
    fn setup(&self, entries: u32, params: &mut Setup) -> i64;
    /// SYS_LOOM_REGISTER.
    ///
    /// # Safety
    /// `arg` addresses `nargs` records of the kind `op` takes, writable for the
    /// call.
    unsafe fn register(&self, fd: i32, op: u32, arg: *mut u8, nargs: u32) -> i64;
    /// SYS_LOOM_ENTER.
    fn enter(&self, fd: i32, to_submit: u32, min_complete: u32, flags: u32) -> i64;
    /// poll(2) on one descriptor: its returned events (> 0), 0 on timeout, or a
    /// negative errno.
    fn poll(&self, fd: i32, events: i16, timeout_ms: i32) -> i64;
    fn close(&self, fd: i32) -> i64;
    /// A registered-buffer region of `len` bytes, a page multiple: its address,
    /// or a negative errno.
    fn attach(&self, len: usize) -> i64;
    fn detach(&self, va: usize, len: usize) -> i64;
    /// CLOCK_MONOTONIC in nanoseconds, the clock every Loom deadline is on.
    fn now(&self) -> u64;
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) enum SlotKind {
    Target,
    Scope,
    Fid,
}
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) enum SlotState {
    /// Owned by a handle or an operation.
    Live,
    /// A fid whose handle is gone: it is clunked when the ring has room.
    Clunk,
    /// Its CLUNK is in flight.
    Clunking,
    /// REAP is retried each turn until the kernel frees the slot.
    Reap,
}
#[derive(Clone, Copy, Debug)]
pub(crate) struct SlotEntry {
    pub r: SlotRef,
    pub kind: SlotKind,
    /// A fid's scope; a scope's own reference.
    pub scope: SlotRef,
    pub state: SlotState,
    pub aborted: bool,
}

/// The descriptor, the validated ring mapping and the buffer regions. Dropped
/// last, after every reader of the mapping and every lease on the buffers.
pub(crate) struct Shared<K: Kernel> {
    pub kernel: K,
    fd: Cell<i32>,
    ring: Cell<Option<(usize, usize)>>,
    regions: Box<[(usize, usize)]>,
    /// The regions the client cores may name; the last one is the name arena.
    pub buffers: usize,
    pub slots: RefCell<[Option<SlotEntry>; 64]>,
    pub admission: Cell<bool>,
}
impl<K: Kernel> Shared<K> {
    /// Attach every region, or none: a refusal detaches what was attached.
    pub(crate) fn attach(kernel: K, lengths: &[usize], buffers: usize) -> Result<Self, i32> {
        let mut regions = Vec::with_capacity(lengths.len());
        for &len in lengths {
            let va = kernel.attach(len);
            if va <= 0 || va as usize % PAGE != 0 {
                if va > 0 {
                    kernel.detach(va as usize, len);
                }
                for &(va, len) in &regions {
                    kernel.detach(va, len);
                }
                return Err(if va < 0 { va as i32 } else { -EIO });
            }
            regions.push((va as usize, len));
        }
        Ok(Self {
            kernel,
            fd: Cell::new(-1),
            ring: Cell::new(None),
            regions: regions.into_boxed_slice(),
            buffers,
            slots: RefCell::new([None; 64]),
            admission: Cell::new(true),
        })
    }
    pub(crate) fn set_fd(&self, fd: i32) {
        self.fd.set(fd);
    }
    /// Called only once the mapping's geometry has been validated.
    pub(crate) fn set_ring(&self, base: usize, len: usize) {
        self.ring.set(Some((base, len)));
    }
    pub(crate) fn fd(&self) -> i32 {
        self.fd.get()
    }
    pub(crate) fn region(&self, index: usize) -> (usize, usize) {
        self.regions[index]
    }
    pub(crate) fn regions(&self) -> &[(usize, usize)] {
        &self.regions
    }
    pub(crate) fn close_fd(&self) {
        let fd = self.fd.replace(-1);
        if fd >= 0 {
            self.kernel.close(fd);
        }
    }
    /// The ring was closed under the adapter. Its descriptor is the only one
    /// (a private ring is never duplicated or inherited, and exec latches it
    /// only past the point of no return), so another thread closed it and the
    /// number may already name another file: it is dropped, never closed.
    pub(crate) fn forget_fd(&self) {
        self.fd.set(-1);
    }
    /// One versioned record; after close the ring is gone and nothing is sent.
    pub(crate) fn register<T>(&self, op: u32, record: &mut T) -> i64 {
        let fd = self.fd.get();
        if fd < 0 {
            return -(ECANCELED as i64);
        }
        unsafe { self.kernel.register(fd, op, (record as *mut T).cast(), 1) }
    }
    pub(crate) fn control(&self, op: u32, object: SlotRef) -> i64 {
        let mut c = Control {
            size: 32,
            version: 1,
            flags: 0,
            object,
            reserved: 0,
        };
        self.register(op, &mut c)
    }
    pub(crate) fn reserve(&self, kind: u32, scope: SlotRef) -> Result<SlotRef, i32> {
        let mut r = Reservation {
            size: 48,
            version: 1,
            flags: 0,
            kind,
            reserved: 0,
            scope,
            result: ZERO_REF,
        };
        let rc = self.register(REGISTER_SERVICE_SLOT, &mut r);
        if rc < 0 {
            return Err(rc as i32);
        }
        if r.result.slot >= 64 || r.result.reserved != 0 || r.result.incarnation == 0 {
            return Err(-EIO);
        }
        Ok(r.result)
    }
    pub(crate) fn state(&self, object: SlotRef) -> Result<u32, i32> {
        let mut s = Snapshot {
            size: 64,
            version: 1,
            flags: 0,
            object,
            kind: 0,
            state: 0,
            scope: ZERO_REF,
            active_ops: 0,
            pending_terminals: 0,
            reason: 0,
            status_flags: 0,
        };
        let rc = self.register(REGISTER_QUERY_SERVICE_SLOT, &mut s);
        if rc < 0 { Err(rc as i32) } else { Ok(s.state) }
    }
    pub(crate) fn track(&self, entry: SlotEntry) {
        let mut slots = self.slots.borrow_mut();
        let cell = &mut slots[entry.r.slot as usize];
        assert!(cell.is_none(), "the kernel minted a slot the adapter still holds");
        *cell = Some(entry);
    }
    pub(crate) fn entry(&self, r: SlotRef) -> Option<SlotEntry> {
        self.slots.borrow()[r.slot as usize].filter(|e| e.r == r)
    }
    pub(crate) fn set_state(&self, r: SlotRef, state: SlotState) {
        if let Some(e) = self.slots.borrow_mut()[r.slot as usize].as_mut() {
            if e.r == r {
                e.state = state;
            }
        }
    }
    /// ABORT_SCOPE: the whole connection, every sibling operation included.
    pub(crate) fn abort(&self, scope: SlotRef) {
        let first = {
            let mut slots = self.slots.borrow_mut();
            match slots[scope.slot as usize].as_mut() {
                Some(e) if e.r == scope => !core::mem::replace(&mut e.aborted, true),
                _ => false,
            }
        };
        if first {
            self.control(REGISTER_ABORT_SCOPE, scope);
        }
    }
    /// One REAP attempt for every slot waiting on it. A busy slot is retried on a
    /// later pass; a refusal other than EBUSY leaves it for close to reclaim.
    pub(crate) fn reap_pass(&self) {
        for i in 0..64 {
            let due = self.slots.borrow()[i].filter(|e| e.state == SlotState::Reap);
            if let Some(e) = due {
                let rc = self.control(REGISTER_REAP_SERVICE_SLOT, e.r);
                if rc == 0 || rc == -(ENOENT as i64) {
                    self.slots.borrow_mut()[i] = None;
                }
            }
        }
    }
}
impl<K: Kernel> Drop for Shared<K> {
    fn drop(&mut self) {
        self.close_fd();
        if let Some((va, len)) = self.ring.take() {
            self.kernel.detach(va, len);
        }
        for &(va, len) in self.regions.iter() {
            self.kernel.detach(va, len);
        }
    }
}

/// The client cores' backend: buffers, pools and whole-ring retirement.
pub(crate) struct Backend<K: Kernel>(pub alloc::rc::Rc<Shared<K>>);
// SAFETY: the regions are eager and stay attached until Shared drops, after the
// last core reference. Pool records go to the kernel unchanged, and a refused
// creation leaves the output zero. retired() reports true only once admission is
// closed and the kernel reports every scope RETIRED, and no other writer exists:
// every fixed or pooled operation belongs to a scope.
unsafe impl<K: Kernel> RingBackend for Backend<K> {
    fn register_pool(&self, ranges: &[PoolMember], out: &mut SlotRef) -> i64 {
        let empty = PoolMember {
            buffer_index: 0,
            reserved: 0,
            offset: 0,
            length: 0,
        };
        let mut record = PoolCreate {
            size: 1568,
            version: 1,
            flags: 0,
            count: ranges.len() as u32,
            reserved: 0,
            result: ZERO_REF,
            members: [empty; 64],
        };
        record.members[..ranges.len()].copy_from_slice(ranges);
        let rc = self.0.register(REGISTER_SERVICE_POOL, &mut record);
        *out = record.result;
        rc
    }
    fn buffer_count(&self) -> usize {
        self.0.buffers
    }
    fn buffer_len(&self, index: usize) -> usize {
        self.0.region(index).1
    }
    fn buffer(&self, index: usize) -> NonNull<u8> {
        NonNull::new(self.0.region(index).0 as *mut u8).expect("attached region")
    }
    fn return_buffer(&self, receipt: BufferReceipt) -> i64 {
        let mut r = BufferReturn {
            size: 40,
            version: 1,
            flags: 0,
            receipt,
        };
        self.0.register(REGISTER_RETURN_SERVICE_BUFFER, &mut r)
    }
    /// The kernel has no pool retirement; the core's own latch stops new reads,
    /// and REAP (destroy_pool) frees the slot once its streams have ended.
    fn retire_pool(&self, _pool: SlotRef) {}
    fn destroy_pool(&self, pool: SlotRef) -> i64 {
        self.0.control(REGISTER_REAP_SERVICE_SLOT, pool)
    }
    fn abort_scope(&self, scope: SlotRef) {
        self.0.abort(scope);
    }
    fn begin_retire(&self) {
        self.0.admission.set(false);
        let scopes: [Option<SlotRef>; 64] = core::array::from_fn(|i| {
            self.0.slots.borrow()[i]
                .filter(|e| e.kind == SlotKind::Scope)
                .map(|e| e.r)
        });
        for scope in scopes.into_iter().flatten() {
            self.0.abort(scope);
        }
    }
    fn retired(&self) -> bool {
        if self.0.admission.get() || self.0.fd() < 0 {
            return false;
        }
        (0..64).all(|i| {
            let scope = self.0.slots.borrow()[i]
                .filter(|e| e.kind == SlotKind::Scope)
                .map(|e| e.r);
            match scope {
                None => true,
                Some(r) => match self.0.state(r) {
                    Ok(state) => state == SERVICE_RETIRED,
                    Err(e) => e == -ENOENT,
                },
            }
        })
    }
    fn close(&self) {
        self.0.close_fd();
    }
}

/// The ring mapping as the paired CQ reader's owner sees it. Closing closes the
/// descriptor; Shared detaches the mapping once nothing reads it.
pub(crate) struct Map<K: Kernel> {
    pub shared: alloc::rc::Rc<Shared<K>>,
    pub base: NonNull<u8>,
    pub len: usize,
}
// SAFETY: base/len are the kernel's setup output for this descriptor, which
// OwnedMapping validates before reading; the mapping is detached only by Shared,
// only after validation, and only once this owner and every reader are gone.
unsafe impl<K: Kernel> Mapping for Map<K> {
    fn base(&self) -> NonNull<u8> {
        self.base
    }
    fn len(&self) -> usize {
        self.len
    }
    fn close(&mut self) {
        self.shared.close_fd();
    }
}
