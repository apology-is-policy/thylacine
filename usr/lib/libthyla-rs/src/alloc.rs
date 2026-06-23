// libthyla-rs::alloc — Thylacine native heap allocator.
//
// Provides `ThylaAlloc`, a `core::alloc::GlobalAlloc` implementation
// backed by a single SYS_BURROW_ATTACH_LAZY region subdivided by
// `linked_list_allocator`. Binaries opt in by declaring:
//
//     #[global_allocator]
//     static ALLOC: libthyla_rs::alloc::ThylaAlloc =
//         libthyla_rs::alloc::ThylaAlloc;
//     extern crate alloc;
//
// After that, the `alloc` crate (Box, Vec, String, BTreeMap, ...) is
// usable throughout the binary. Lazy init: the first allocation
// triggers one SYS_BURROW_ATTACH_LAZY (which reserves but commits no
// physical pages); subsequent allocations subdivide that region
// locally, faulting in pages as blocks are written. Binaries that
// never call alloc never pay the syscall.
//
// Foundation chunk: U-2b per docs/UTOPIA-SHELL-DESIGN.md §15.
//
// WHY libthyla-rs DOES NOT DECLARE `#[global_allocator]` ITSELF:
// Rust requires exactly one `#[global_allocator]` per binary's
// dependency tree. corvus already declares its own static-BSS-backed
// allocator (usr/corvus/src/main.rs); a duplicate declaration here
// would break corvus's link. Future native binaries that want the
// canonical heap declare the line shown above explicitly. The minor
// boilerplate buys per-binary choice (most pick ThylaAlloc;
// resource-constrained or pre-burrow_attach binaries can pick their
// own).
//
// SIZING:
//   - INITIAL_HEAP_SIZE = 4 MiB. Fits the shell + a few coreutils +
//     parser ASTs + tab-completion caches comfortably; far below
//     BURROW_RESERVE_MAX (= 1 GiB) so the lazy attach never bumps the
//     upper bound. Because the region is reserved (not committed), the
//     4 MiB costs nothing until blocks are written -- a binary that
//     allocates a few KiB has a few KiB of RSS. Growable heap is a v1.x
//     consideration; v1 binaries that need more allocate explicit
//     Burrows themselves.
//
// THREAD SAFETY:
//   - `LockedHeap` from `linked_list_allocator` uses an internal
//     spin::Mutex (the crate's `use_spin` feature, enabled in
//     Cargo.toml). Multi-thread allocations on multi-Thread Procs
//     (Phase 6 sub-chunk 9a's SYS_THREAD_SPAWN) serialize through
//     the spinlock. Low contention is expected (allocations are
//     short; the lock is held briefly).
//   - The initialization state machine uses a separate AtomicU8 with
//     acquire/release ordering; the loser of the init CAS spins
//     until the winner's STATE_READY release publishes the heap
//     bounds.
//
// FAULT POLICY:
//   - If the initial SYS_BURROW_ATTACH_LAZY fails (no VA gap, VMA cap,
//     or invalid request), the program calls t_exits(1). LockedHeap would
//     otherwise return null pointers, which alloc-aware code mostly
//     treats as "abort with OOM" via the global alloc_error_handler
//     — but the panic message would be opaque. Failing fast at init
//     with a clear cause is better for debugging.
//   - We do NOT install a custom alloc_error_handler. The default
//     no_std handler panics; libthyla-rs's `#[panic_handler]` routes
//     panics to t_exits(1). So an OOM during normal operation
//     terminates the program with exit status 1.

use crate::{t_burrow_attach_lazy, t_exits};
use core::alloc::{GlobalAlloc, Layout};
use core::sync::atomic::{AtomicU8, Ordering};

use linked_list_allocator::LockedHeap;

/// The initial heap size requested from the kernel on first
/// allocation. 4 MiB. Sized to comfortably host the shell + coreutils
/// + parser ASTs. Below `BURROW_ATTACH_MAX` (256 MiB) by ~64x.
///
/// v1.x growable-heap support would re-attach additional burrows on
/// demand; v1 fixes the initial allocation.
pub const INITIAL_HEAP_SIZE: usize = 4 * 1024 * 1024;

// Lazy-init state machine. Three states, single-byte atomic.
//
// Transitions are:
//   UNINIT --(CAS by first caller)--> INITIALIZING --(store-Release after init)--> READY
//   UNINIT --(CAS loser)--> still UNINIT, spin loop on STATE until READY
//
// Acquire/Release on STATE pairs with the linked_list_allocator's
// LockedHeap so the heap-bottom + heap-size are visible cross-CPU.
const STATE_UNINIT:       u8 = 0;
const STATE_INITIALIZING: u8 = 1;
const STATE_READY:        u8 = 2;

static STATE: AtomicU8 = AtomicU8::new(STATE_UNINIT);
static HEAP: LockedHeap = LockedHeap::empty();

/// The Thylacine global allocator.
///
/// Zero-sized; the actual heap state is in the module-level `HEAP`
/// static. Binaries instantiate this in their `#[global_allocator]`
/// declaration:
///
/// ```ignore
/// #[global_allocator]
/// static ALLOC: libthyla_rs::alloc::ThylaAlloc =
///     libthyla_rs::alloc::ThylaAlloc;
/// extern crate alloc;
/// ```
pub struct ThylaAlloc;

unsafe impl GlobalAlloc for ThylaAlloc {
    unsafe fn alloc(&self, layout: Layout) -> *mut u8 {
        ensure_initialized();
        HEAP.alloc(layout)
    }

    unsafe fn dealloc(&self, ptr: *mut u8, layout: Layout) {
        // Reaching dealloc means alloc succeeded, which means
        // ensure_initialized completed at least once. Skip the
        // check on the hot path.
        HEAP.dealloc(ptr, layout)
    }
}

// ensure_initialized — single-init dispatch. First caller wins the
// CAS and performs SYS_BURROW_ATTACH + heap init; subsequent callers
// (or losers of a concurrent first-call race) observe STATE_READY
// and skip the work.
//
// Inlining is left to the compiler; the fast-path is a single
// atomic load + compare. Realistic call sites are deeply inlined
// inside Box::new / Vec::push / etc., so even hot allocation loops
// pay only the load.
unsafe fn ensure_initialized() {
    if STATE.load(Ordering::Acquire) == STATE_READY {
        return;
    }

    match STATE.compare_exchange(
        STATE_UNINIT,
        STATE_INITIALIZING,
        Ordering::AcqRel,
        Ordering::Relaxed,
    ) {
        Ok(_) => {
            // Won the race. Perform the one-time init. The heap region is
            // RESERVED lazily (overcommit): the 4 MiB span costs nothing until
            // the linked-list allocator hands out blocks that get written,
            // which fault pages in one at a time. A binary that allocates
            // little commits little.
            let rc = t_burrow_attach_lazy(INITIAL_HEAP_SIZE as u64);
            if rc <= 0 {
                // SYS_BURROW_ATTACH_LAZY returns a positive VA on success
                // and -1 on failure. Treat both 0 and negative as
                // failure (0 would be the null VA, which the kernel
                // never returns from this syscall but we defend
                // against anyway).
                //
                // No path to recover at heap init time. Fail fast.
                t_exits(1);
            }
            HEAP.lock().init(rc as usize as *mut u8, INITIAL_HEAP_SIZE);

            // Publish the initialized heap bounds. Release pairs
            // with the Acquire load above (and with the spin loop
            // in the CAS-loser branch).
            STATE.store(STATE_READY, Ordering::Release);
        }
        Err(_) => {
            // Lost the race (some other thread is initializing).
            // Spin until the initializer publishes STATE_READY.
            // Bounded by one SYS_BURROW_ATTACH + a small amount of
            // heap-init work — microseconds at most.
            while STATE.load(Ordering::Acquire) != STATE_READY {
                core::hint::spin_loop();
            }
        }
    }
}
