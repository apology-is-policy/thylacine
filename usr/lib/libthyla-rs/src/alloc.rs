// libthyla-rs::alloc -- the native heap (B-1c; ARCH 6.5 "Capacity").
//
// `ThylaAlloc` is the `core::alloc::GlobalAlloc` a native binary opts into:
//
//     #[global_allocator]
//     static ALLOC: libthyla_rs::alloc::ThylaAlloc =
//         libthyla_rs::alloc::ThylaAlloc;
//     extern crate alloc;
//
// after which the `alloc` crate (Box, Vec, String, BTreeMap, ...) is usable
// throughout the binary.
//
// The heap has no fixed size: it grows until the system is out of memory and
// gives memory back as it is freed. The policy is thyla-heap's -- dlmalloc
// over lazy reservations for blocks under 256 KiB whose alignment is under
// 256 KiB too, a reservation of its own for a larger or more aligned block,
// detached when it is freed. This module supplies the
// three kernel calls it is built from: SYS_BURROW_RESERVE (124) at RW,
// SYS_BURROW_DECOMMIT (84) and SYS_BURROW_DETACH (38, the range form). Nothing
// is reserved before the first allocation, and a reservation costs address
// space only: its pages are charged to the I-32 budget as they are touched.
//
// WHY libthyla-rs DOES NOT DECLARE `#[global_allocator]` ITSELF:
// Rust requires exactly one `#[global_allocator]` per binary's dependency
// tree, and corvus declares its own static-BSS-backed allocator
// (usr/corvus/src/main.rs); a declaration here would break corvus's link.
//
// THREAD SAFETY: dlmalloc runs under a spinlock (spinning_top, the lock this
// heap has always taken), so peer threads' allocations serialize. A direct
// block's reserve and detach run outside it.
//
// FAULT POLICY: an allocation the kernel refuses returns null. The default
// no_std alloc_error_handler panics on it, and libthyla-rs's #[panic_handler]
// exits 1; the fallible forms (`Vec::try_reserve` and the like) see the
// refusal instead. The kernel refuses a reservation only when address space
// runs out: memory runs out at a page, when it is first touched, and the
// kernel ends whichever program touched it (exit status 1, docs/ERRORS.md) --
// not necessarily the one that grew, since nothing picks a victim (ARCH 6.5).

use core::alloc::{GlobalAlloc, Layout};

use thyla_heap::{Backend, Heap};

use crate::{t_burrow_decommit, t_burrow_detach, t_burrow_reserve, T_BURROW_PROT_READ, T_BURROW_PROT_WRITE};

#[derive(Clone, Copy)]
struct Svc;

impl Backend for Svc {
    fn reserve(&self, len: usize, align_log2: u32) -> Option<usize> {
        let rw = T_BURROW_PROT_READ | T_BURROW_PROT_WRITE;
        let va = unsafe { t_burrow_reserve(len as u64, rw, align_log2 as u64) };
        if va > 0 {
            Some(va as usize)
        } else {
            None
        }
    }

    fn decommit(&self, va: usize, len: usize) -> bool {
        unsafe { t_burrow_decommit(va as u64, len as u64) == 0 }
    }

    fn detach(&self, va: usize, len: usize) -> bool {
        unsafe { t_burrow_detach(va as u64, len as u64) == 0 }
    }
}

static HEAP: Heap<Svc> = Heap::new(Svc);

/// The Thylacine global allocator. Zero-sized; the heap is this module's.
pub struct ThylaAlloc;

unsafe impl GlobalAlloc for ThylaAlloc {
    unsafe fn alloc(&self, layout: Layout) -> *mut u8 {
        HEAP.alloc(layout)
    }

    unsafe fn alloc_zeroed(&self, layout: Layout) -> *mut u8 {
        HEAP.alloc_zeroed(layout)
    }

    unsafe fn dealloc(&self, ptr: *mut u8, layout: Layout) {
        HEAP.dealloc(ptr, layout)
    }

    unsafe fn realloc(&self, ptr: *mut u8, layout: Layout, new_size: usize) -> *mut u8 {
        HEAP.realloc(ptr, layout, new_size)
    }
}

/// Bytes the heap holds from the system: dlmalloc's segments plus the direct
/// blocks. The data pages the kernel charges for them are at most this; its
/// count adds the page tables and pagemap nodes that map them, about one page
/// in 256.
pub fn footprint() -> usize {
    HEAP.footprint()
}

/// Give back what the heap can now, rather than at the next trim a free
/// triggers (once dlmalloc's top chunk passes 2 MiB).
pub fn trim() -> bool {
    HEAP.trim()
}

/// How many reservations dlmalloc's segments occupy.
pub fn reservations() -> usize {
    HEAP.reservations()
}
