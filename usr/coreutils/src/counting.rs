// The host tests' allocator: the system's, counting what each thread asks
// for, so a test can show a loop holds nothing however long its input.

extern crate std;

use core::alloc::{GlobalAlloc, Layout};
use core::cell::Cell;
use std::alloc::System;

struct Counting;

std::thread_local! {
    static ASKED: Cell<usize> = const { Cell::new(0) };
}

unsafe impl GlobalAlloc for Counting {
    unsafe fn alloc(&self, layout: Layout) -> *mut u8 {
        ASKED.with(|a| a.set(a.get() + layout.size()));
        unsafe { System.alloc(layout) }
    }

    unsafe fn dealloc(&self, p: *mut u8, layout: Layout) {
        unsafe { System.dealloc(p, layout) }
    }

    unsafe fn realloc(&self, p: *mut u8, layout: Layout, new_size: usize) -> *mut u8 {
        ASKED.with(|a| a.set(a.get() + new_size));
        unsafe { System.realloc(p, layout, new_size) }
    }
}

#[global_allocator]
static COUNTING: Counting = Counting;

/// The bytes this thread allocated while `f` ran.
pub fn allocated(f: impl FnOnce()) -> usize {
    let before = ASKED.with(|a| a.get());
    f();
    ASKED.with(|a| a.get()) - before
}

#[test]
fn an_allocation_is_counted() {
    let asked = allocated(|| {
        let v = alloc::vec![7u8; 4096];
        assert_eq!(v[4095], 7);
    });
    assert!(asked >= 4096, "the counter saw {} bytes", asked);
}
