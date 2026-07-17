// /stack-child -- the target for the Stage-8b settled-thread kstack inspect E2E
// (stack-probe drives it). It blocks FOREVER in torpor_wait -- a futex wait on a
// word that is never woken -- parking DEEP in the kernel (sleep() on the torpor
// rendez): on_cpu==false, settled, but NOT debug-stopped and NOT at the EL0-return
// tail. This is exactly the case an 8a debug-stop can never freeze: a debug-stop
// parks only at the EL0-return tail (a syscall complete), which this thread --
// blocked mid-syscall forever -- never reaches. stack-probe reads /proc/<pid>/kstack
// (owner-authorized, NO debug-stop) and asserts a non-empty symbolized KERNEL
// backtrace, then killgrp's us (a kill wakes the torpor sleeper via the #811
// death-interruptible path -> we unwind + die). The parent owns our lifetime, so
// the wait loop is deliberately unbounded.
//
// Silent by design (no fd I/O): a concurrent print would byte-race the console.

#![no_std]
#![no_main]

extern crate alloc;

#[global_allocator]
static GLOBAL_ALLOCATOR: libthyla_rs::alloc::ThylaAlloc = libthyla_rs::alloc::ThylaAlloc;

use core::sync::atomic::AtomicU32;

#[no_mangle]
pub extern "C" fn rs_main() -> i64 {
    // WORD stays 0, so torpor::wait(expected=0) sleeps (futex semantics: wait iff
    // *addr == expected). Nobody wakes it -> we block indefinitely in sleep().
    // `None` == T_TORPOR_TIMEOUT_INDEFINITE. A spurious wake just re-blocks (we
    // never exit on our own -- the parent kills us while blocked).
    static WORD: AtomicU32 = AtomicU32::new(0);
    loop {
        let _ = libthyla_rs::torpor::wait(&WORD, 0, None);
    }
}
