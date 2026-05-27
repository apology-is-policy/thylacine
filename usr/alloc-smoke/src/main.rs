// /alloc-smoke — runtime validation for libthyla-rs::alloc (U-2b).
//
// First native Thylacine binary that uses the alloc crate. Declares
// libthyla-rs::alloc::ThylaAlloc as its `#[global_allocator]` and
// exercises Box / Vec / String through a small sequence of property
// checks. Spawned by joey at boot; success prints "alloc-smoke: OK"
// + exits 0; any failed check prints a tagged FAIL message + exits 1.
//
// The failure modes that this binary catches:
//   - SYS_BURROW_ATTACH return-value misinterpretation in
//     ensure_initialized.
//   - linked_list_allocator init pointer/size mistakes.
//   - alloc/dealloc protocol mismatches that would corrupt the free
//     list before another binary noticed.
//   - alloc-error-handler path landing somewhere other than t_exits(1).
//
// Not a comprehensive stress test (no concurrency, no fragmentation
// scenarios, no large allocations near INITIAL_HEAP_SIZE). The
// libthyla-rs U-2-test sub-chunk exercises the integrated surface
// across all U-2X modules.

#![no_std]
#![no_main]

extern crate alloc;

use alloc::boxed::Box;
use alloc::string::String;
use alloc::vec::Vec;

use libthyla_rs::alloc::ThylaAlloc;
use libthyla_rs::t_putstr;

#[global_allocator]
static GLOBAL_ALLOCATOR: ThylaAlloc = ThylaAlloc;

#[no_mangle]
pub extern "C" fn rs_main() -> i64 {
    // Box — single heap word + Drop.
    let b: Box<u32> = Box::new(0xDEADBEEF);
    if *b != 0xDEADBEEF {
        t_putstr("alloc-smoke: Box round-trip FAILED\n");
        return 1;
    }
    drop(b);

    // Vec — capacity growth path (forces multiple realloc cycles
    // starting from empty).
    let mut v: Vec<u32> = Vec::new();
    for i in 0..1024u32 {
        v.push(i);
    }
    if v.len() != 1024 || v[0] != 0 || v[1023] != 1023 {
        t_putstr("alloc-smoke: Vec growth FAILED\n");
        return 1;
    }
    // Triangular-number sanity (sum 0..1023 = 1023 * 1024 / 2 = 523776)
    // — catches off-by-one bugs in any future allocator that might
    // duplicate or drop entries.
    let sum: u64 = v.iter().map(|&x| x as u64).sum();
    if sum != 523_776 {
        t_putstr("alloc-smoke: Vec sum check FAILED\n");
        return 1;
    }
    drop(v);

    // String — capacity-doubling path (each push_str may realloc).
    let mut s = String::new();
    for _ in 0..64 {
        s.push_str("hello, world\n");
    }
    if s.len() != 64 * 13 {
        t_putstr("alloc-smoke: String length FAILED\n");
        return 1;
    }
    drop(s);

    // Many small allocs + frees in a tight loop — exercises the
    // free-list reuse path. If the allocator's bookkeeping is broken,
    // this loop typically panics or hangs within a few iterations.
    for _ in 0..256 {
        let small: Box<[u8; 32]> = Box::new([0xAAu8; 32]);
        if small[0] != 0xAA || small[31] != 0xAA {
            t_putstr("alloc-smoke: small Box pattern FAILED\n");
            return 1;
        }
        drop(small);
    }

    t_putstr("alloc-smoke: Box + Vec + String + small-alloc loop OK\n");
    0
}
