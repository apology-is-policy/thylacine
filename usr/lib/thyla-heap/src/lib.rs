//! thyla-heap -- the native heap's policy (B-1c; ARCH 6.5 "Capacity";
//! `dec-2026-09-24-native-heap-large-blocks`).
//!
//! A block smaller than [`DIRECT_MIN`] and aligned to less than it comes from
//! dlmalloc (dlmalloc-rs, the copy the Rust standard library vendors) over
//! [`Reservations`]. Any other block is a lazy reservation of its own, detached
//! when it is freed. Both halves grow until the system refuses, and both give
//! memory back: dlmalloc's trim decommits the top segment's tail and releases a
//! reservation it has emptied, and a direct block takes its pages with it.
//!
//! The kernel calls belong to a [`Backend`]. libthyla-rs supplies the
//! syscalls; a host test supplies its own, so what it measures is the guest's
//! policy.

#![no_std]

use core::alloc::{GlobalAlloc, Layout};
use core::cell::RefCell;
use core::cmp;
use core::ptr;

use dlmalloc::{Allocator, Dlmalloc};
use spinning_top::{const_spinlock, Spinlock};

/// The page the kernel maps at.
pub const PAGE: usize = 4096;

/// A block this size or larger is mapped on its own. This is C dlmalloc's
/// DEFAULT_MMAP_THRESHOLD, which the Rust port dropped: there every block lives
/// in a segment, and one freed below a live block keeps its pages.
pub const DIRECT_MIN: usize = 256 * 1024;

/// The first reservation's length. Each reservation opened while others are
/// live doubles it, up to [`RESERVATION_DOUBLINGS`] times, so a heap of any
/// size stays a handful of segments. A reservation costs address space only:
/// its pages are charged when they are touched.
pub const RESERVATION_LEN: usize = 256 << 20;

/// 256 MiB doubled seventeen times is 32 TiB: seventeen reservations reach
/// half the 64 TiB burrow window, and an eighteenth of that size cannot fit.
const RESERVATION_DOUBLINGS: u32 = 17;

/// At most this many reservations are live. A length the kernel refuses is
/// asked for again at half, down to the carve's own size, so past the doubled
/// ones the table holds the smaller reservations a full or fragmented window
/// still grants; it fills only once the window is all but exhausted.
const RESERVATIONS_MAX: usize = 64;

/// The kernel calls the heap is built from. A refusal (`None` or `false`)
/// changes nothing.
pub trait Backend {
    /// A lazy, read-write, demand-zero reservation of `len` bytes (a page
    /// multiple), its base aligned to `2^align_log2` (0 means a page).
    fn reserve(&self, len: usize, align_log2: u32) -> Option<usize>;
    /// Return the pages of `[va, va + len)`. The range stays reserved and reads
    /// as zero when it is next touched.
    fn decommit(&self, va: usize, len: usize) -> bool;
    /// Unmap `[va, va + len)`: its pages and its address space.
    fn detach(&self, va: usize, len: usize) -> bool;
}

impl<T: Backend + ?Sized> Backend for &T {
    fn reserve(&self, len: usize, align_log2: u32) -> Option<usize> {
        (**self).reserve(len, align_log2)
    }
    fn decommit(&self, va: usize, len: usize) -> bool {
        (**self).decommit(va, len)
    }
    fn detach(&self, va: usize, len: usize) -> bool {
        (**self).detach(va, len)
    }
}

/// `size` rounded up to a page, or `None` if that overflows.
pub fn page_round(size: usize) -> Option<usize> {
    size.checked_add(PAGE - 1).map(|s| s & !(PAGE - 1))
}

/// Whether a block of this shape is mapped on its own rather than taken from
/// dlmalloc. A small block aligned past a page is dlmalloc's too (memalign pads
/// it inside a segment); a mapping of its own would spend a VMA on each one, and
/// the kernel's VMA cap would refuse memory that is free. Only an alignment of
/// `DIRECT_MIN` or more, which dlmalloc would pad by as much, is the kernel's to
/// place.
pub fn is_direct(size: usize, align: usize) -> bool {
    size >= DIRECT_MIN || align >= DIRECT_MIN
}

#[derive(Clone, Copy)]
struct Reservation {
    base: usize,
    len: usize,
    /// dlmalloc's segment in this reservation is `[base, bump)`.
    bump: usize,
}

const UNUSED: Reservation = Reservation { base: 0, len: 0, bump: 0 };

struct Table {
    /// `live[..n]`, oldest first; only the last one is carved from.
    live: [Reservation; RESERVATIONS_MAX],
    n: usize,
    /// The carved part of every reservation: dlmalloc's segments.
    carved: usize,
    /// Page-rounded bytes of the direct blocks, each counted from before it is
    /// reserved until after it is detached. They are the heap's, not
    /// dlmalloc's, and are counted here so one lock covers the footprint.
    direct: usize,
    /// The most `carved + direct` has been since the last reset.
    peak: usize,
}

impl Table {
    fn grew(&mut self) {
        self.peak = cmp::max(self.peak, self.carved + self.direct);
    }
}

/// dlmalloc's platform: the lazy reservations its segments are carved from.
///
/// dlmalloc takes [`Allocator::free_part`] to shrink a segment for good, so
/// the tail must not stay mapped behind its back. Mapped with a fresh attach
/// per `alloc` call and a decommit per trim, every decommitted tail would stay
/// a live VMA that dlmalloc had forgotten, and the segment's later release would
/// orphan it: one VMA per trim-and-release cycle, until the kernel's cap refused
/// memory that was free. So every segment is carved at a bump pointer inside a
/// reservation this type owns. A trim decommits the tail and moves the bump back
/// over it, and only a whole reservation is ever detached.
///
/// Reservations never merge in dlmalloc's view. A carve never fills its
/// reservation, so each reservation's tail stays mapped and no new reservation
/// can land at a segment's end (dlmalloc would extend). A first carve always
/// ends inside its own reservation, so it can never meet a segment's base
/// (dlmalloc would prepend). One reservation is therefore one segment and one
/// VMA, and the segment dlmalloc trims is always the latest reservation's.
pub struct Reservations<B> {
    backend: B,
    first_len: usize,
    table: RefCell<Table>,
}

impl<B> Reservations<B> {
    const fn new(backend: B, first_len: usize) -> Self {
        Reservations {
            backend,
            first_len,
            table: RefCell::new(Table {
                live: [UNUSED; RESERVATIONS_MAX],
                n: 0,
                carved: 0,
                direct: 0,
                peak: 0,
            }),
        }
    }

    /// The bytes dlmalloc holds: the carved part of every reservation.
    pub fn carved(&self) -> usize {
        self.table.borrow().carved
    }

    /// How many reservations are live.
    pub fn count(&self) -> usize {
        self.table.borrow().n
    }

    fn footprint(&self) -> usize {
        let t = self.table.borrow();
        t.carved + t.direct
    }

    fn peak(&self) -> usize {
        self.table.borrow().peak
    }

    fn reset_peak(&self) {
        let mut t = self.table.borrow_mut();
        t.peak = t.carved + t.direct;
    }

    /// Count a direct block, unless the footprint would pass `isize::MAX`: no
    /// system holds that much, and past it the counts could overflow.
    fn add_direct(&self, len: usize) -> bool {
        let mut t = self.table.borrow_mut();
        match t.carved.checked_add(t.direct).and_then(|f| f.checked_add(len)) {
            Some(f) if f <= isize::MAX as usize => {
                t.direct += len;
                t.grew();
                true
            }
            _ => false,
        }
    }

    fn sub_direct(&self, len: usize) {
        self.table.borrow_mut().direct -= len;
    }
}

impl<B: Backend> Reservations<B> {
    fn carve(&self, size: usize) -> Option<usize> {
        if size == 0 || !size.is_multiple_of(PAGE) {
            return None;
        }
        let mut t = self.table.borrow_mut();
        if t.n > 0 {
            let latest = t.n - 1;
            let r = &mut t.live[latest];
            // Strictly more room than asked: see the type's comment.
            if r.base + r.len - r.bump > size {
                let va = r.bump;
                r.bump += size;
                t.carved += size;
                t.grew();
                return Some(va);
            }
        }
        if t.n == RESERVATIONS_MAX {
            return None;
        }
        let least = size.checked_add(PAGE)?;
        let doublings = cmp::min(t.n as u32, RESERVATION_DOUBLINGS);
        let mut len = cmp::max(self.first_len.checked_mul(1usize << doublings)?, least);
        let base = loop {
            if let Some(base) = self.backend.reserve(len, 0) {
                break base;
            }
            if len == least {
                return None;
            }
            len = cmp::max(len / 2 / PAGE * PAGE, least);
        };
        let n = t.n;
        t.live[n] = Reservation { base, len, bump: base + size };
        t.n += 1;
        t.carved += size;
        t.grew();
        Some(base)
    }

    fn shrink(&self, ptr: *mut u8, old: usize, new: usize) -> bool {
        let mut t = self.table.borrow_mut();
        if t.n == 0 {
            return false;
        }
        let latest = t.n - 1;
        let r = &mut t.live[latest];
        let base = ptr as usize;
        if base != r.base || r.bump - r.base != old || new > old || !(old - new).is_multiple_of(PAGE) {
            return false;
        }
        if new == old {
            return true;
        }
        if !self.backend.decommit(base + new, old - new) {
            return false;
        }
        r.bump = base + new;
        t.carved -= old - new;
        true
    }

    fn release(&self, ptr: *mut u8, size: usize) -> bool {
        let mut t = self.table.borrow_mut();
        let base = ptr as usize;
        let n = t.n;
        let i = match t.live[..n].iter().position(|r| r.base == base) {
            Some(i) => i,
            None => return false,
        };
        let r = t.live[i];
        if r.bump - r.base != size {
            return false;
        }
        if !self.backend.detach(r.base, r.len) {
            return false;
        }
        t.live.copy_within(i + 1..n, i);
        t.n -= 1;
        t.carved -= size;
        true
    }
}

unsafe impl<B: Backend + Send> Allocator for Reservations<B> {
    fn alloc(&self, size: usize) -> (*mut u8, usize, u32) {
        match self.carve(size) {
            Some(va) => (va as *mut u8, size, 0),
            None => (ptr::null_mut(), 0, 0),
        }
    }

    // dlmalloc 0.2.14 makes no mmapped chunk, the only kind it remaps.
    fn remap(&self, _ptr: *mut u8, _oldsize: usize, _newsize: usize, _can_move: bool) -> *mut u8 {
        ptr::null_mut()
    }

    fn free_part(&self, ptr: *mut u8, oldsize: usize, newsize: usize) -> bool {
        self.shrink(ptr, oldsize, newsize)
    }

    fn free(&self, ptr: *mut u8, size: usize) -> bool {
        self.release(ptr, size)
    }

    fn can_release_part(&self, _flags: u32) -> bool {
        true
    }

    // A carve is either never touched or decommitted since it was last used.
    fn allocates_zeros(&self) -> bool {
        true
    }

    fn page_size(&self) -> usize {
        PAGE
    }
}

/// The heap: small blocks from dlmalloc over [`Reservations`], the rest
/// mapped on their own. It implements `GlobalAlloc` for any backend.
pub struct Heap<B: Backend + Copy + Send + Sync> {
    /// Also the lock over the footprint, the direct blocks' part included.
    small: Spinlock<Dlmalloc<Reservations<B>>>,
    backend: B,
}

impl<B: Backend + Copy + Send + Sync> Heap<B> {
    pub const fn new(backend: B) -> Self {
        Self::with_reservation_len(backend, RESERVATION_LEN)
    }

    /// A heap whose first reservation is `first_len` bytes (a page multiple);
    /// a test uses a small one to cross reservations cheaply.
    pub const fn with_reservation_len(backend: B, first_len: usize) -> Self {
        Heap {
            small: const_spinlock(Dlmalloc::new_with_allocator(Reservations::new(backend, first_len))),
            backend,
        }
    }

    /// The bytes the heap holds from the system: dlmalloc's segments plus the
    /// direct blocks. A reader on any thread never sees less than the heap
    /// holds, and the kernel's count of its touched pages is at most this.
    pub fn footprint(&self) -> usize {
        self.small.lock().allocator().footprint()
    }

    /// The most the footprint has been since [`Heap::reset_peak`]: counted where
    /// it grows, so a realloc's moment holding both blocks counts, as does a
    /// direct block the system then refused.
    pub fn peak(&self) -> usize {
        self.small.lock().allocator().peak()
    }

    /// Start [`Heap::peak`] again from the footprint now.
    pub fn reset_peak(&self) {
        self.small.lock().allocator().reset_peak()
    }

    /// How many reservations dlmalloc's segments occupy.
    pub fn reservations(&self) -> usize {
        self.small.lock().allocator().count()
    }

    /// Return what dlmalloc can: the top segment's free tail and every
    /// reservation it has emptied. A free already does this once the top chunk
    /// passes dlmalloc's trim threshold (2 MiB); this is the explicit form.
    pub fn trim(&self) -> bool {
        unsafe { self.small.lock().trim(0) }
    }

    fn direct_alloc(&self, size: usize, align: usize) -> *mut u8 {
        let len = match page_round(size) {
            Some(len) => len,
            None => return ptr::null_mut(),
        };
        let align_log2 = if align > PAGE { align.trailing_zeros() } else { 0 };
        // Counted before the reservation exists, as a free uncounts only after
        // the detach, so the footprint is never below what the heap holds.
        if !self.small.lock().allocator().add_direct(len) {
            return ptr::null_mut();
        }
        match self.backend.reserve(len, align_log2) {
            Some(va) => va as *mut u8,
            None => {
                self.small.lock().allocator().sub_direct(len);
                ptr::null_mut()
            }
        }
    }

    fn direct_free(&self, p: *mut u8, size: usize) {
        // The block was allocated, so its size rounds.
        let len = page_round(size).unwrap_or(0);
        if self.backend.detach(p as usize, len) {
            self.small.lock().allocator().sub_direct(len);
        }
    }

    unsafe fn direct_realloc(&self, p: *mut u8, old: usize, align: usize, new: usize) -> *mut u8 {
        let old_len = page_round(old).unwrap_or(0);
        let new_len = match page_round(new) {
            Some(len) => len,
            None => return ptr::null_mut(),
        };
        if new_len == old_len {
            return p;
        }
        if new_len < old_len && self.backend.detach(p as usize + new_len, old_len - new_len) {
            self.small.lock().allocator().sub_direct(old_len - new_len);
            return p;
        }
        let q = self.direct_alloc(new, align);
        if !q.is_null() {
            ptr::copy_nonoverlapping(p, q, cmp::min(old, new));
            self.direct_free(p, old);
        }
        q
    }
}

unsafe impl<B: Backend + Copy + Send + Sync> GlobalAlloc for Heap<B> {
    unsafe fn alloc(&self, layout: Layout) -> *mut u8 {
        if is_direct(layout.size(), layout.align()) {
            self.direct_alloc(layout.size(), layout.align())
        } else {
            self.small.lock().malloc(layout.size(), layout.align())
        }
    }

    // A direct block is a fresh reservation, so it is zero already.
    unsafe fn alloc_zeroed(&self, layout: Layout) -> *mut u8 {
        if is_direct(layout.size(), layout.align()) {
            self.direct_alloc(layout.size(), layout.align())
        } else {
            self.small.lock().calloc(layout.size(), layout.align())
        }
    }

    unsafe fn dealloc(&self, p: *mut u8, layout: Layout) {
        if is_direct(layout.size(), layout.align()) {
            self.direct_free(p, layout.size())
        } else {
            self.small.lock().free(p, layout.size(), layout.align())
        }
    }

    unsafe fn realloc(&self, p: *mut u8, layout: Layout, new_size: usize) -> *mut u8 {
        let align = layout.align();
        match (is_direct(layout.size(), align), is_direct(new_size, align)) {
            (false, false) => self.small.lock().realloc(p, layout.size(), align, new_size),
            (true, true) => self.direct_realloc(p, layout.size(), align, new_size),
            _ => {
                let q = self.alloc(Layout::from_size_align_unchecked(new_size, align));
                if !q.is_null() {
                    ptr::copy_nonoverlapping(p, q, cmp::min(layout.size(), new_size));
                    self.dealloc(p, layout);
                }
                q
            }
        }
    }
}

#[cfg(test)]
mod tests;
