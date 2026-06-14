// libthyla-rs::thread — raw kernel-thread spawn/exit.
//
// Foundation chunk: U-2g per docs/UTOPIA-SHELL-DESIGN.md section 15.6.13.
//
// At v1.0 this module is the LOW-LEVEL surface: `spawn_raw` mirrors
// the kernel's SYS_THREAD_SPAWN one-to-one, leaving stack allocation,
// closure marshalling, and join-protocol to the caller. The pouch
// boundary-line patch (sub-chunk 9b) demonstrates the high-level wrap
// over musl pthreads; native libthyla-rs callers either drive the
// raw API or build their own high-level wrapper.
//
// WHY NOT std::thread::spawn(FnOnce):
//   A `Thread::spawn(closure)` that hides stack allocation needs:
//     1. Allocate a user-VA stack (via SYS_BURROW_ATTACH or BSS).
//     2. Pack the closure into a Box<dyn FnOnce> and leak the
//        pointer through the kernel's `arg` slot.
//     3. Provide a trampoline that unpacks + invokes + SYS_THREAD_EXITs.
//     4. Wire a join mechanism (kernel has no per-thread join syscall
//        at v1.0; pthread emulates via the `clear_child_tid` torpor
//        wake-on-store path).
//
//   That's ~200-300 LOC of careful unsafe Rust. Worth doing -- but
//   only when the first real consumer surfaces. The shell `ut` is
//   single-threaded (poll-driven main loop, not async); corvus and
//   stratumd use pouch's pthread (musl on top of these primitives).
//   So at v1.0 the raw API is the right surface.
//
//   When the high-level API lands (likely a v1.x chunk co-located with
//   the joiner infrastructure), it composes on top of `spawn_raw`.
//
// SHARED-EVERYTHING SEMANTICS:
//   The new Thread shares pgtable_root + ASID + handle table +
//   Territory + cap_mask with the calling Proc. Only entry / sp / x0 /
//   TPIDR_EL0 are fresh. Files opened in the parent are visible to
//   the child; mounts done in the child affect the parent's
//   namespace. This is the kernel-level model; pthread sits on top.
//
// LIFETIME:
//   `spawn_raw` doesn't track the returned Tid. The kernel keeps the
//   Thread descriptor + kstack live until the Proc dies; v1.0 accepts
//   that bound. Per-Thread reaping is a v1.x extension.

use core::sync::atomic::AtomicU32;
use core::time::Duration;

use crate::err::{Error, Result};
use crate::torpor;
use crate::{t_set_tid_address, t_thread_exit, t_thread_spawn};

/// Per-Thread identifier. Mirrors the kernel's `tid_t` (i32).
pub type Tid = i32;

/// Spawn a new Thread in the calling Proc's address space.
///
/// The kernel installs `entry_va` as the new Thread's EL0 PC, `sp_va`
/// as SP_EL0, `arg` as x0, and `tls_va` into TPIDR_EL0; then the
/// Thread is enqueued on the run-tree. The caller is responsible for
/// the lifetime of every memory region pointed at by these arguments
/// (stack, TLS, anything the entry function reads).
///
/// AAPCS64 requires the stack TOP to be 16-byte aligned at function
/// entry; the kernel rejects misaligned `sp_va`.
///
/// Returns the new Tid on success.
///
/// Errors:
///   - `Error::InvalidArgument`: misaligned sp / out-of-bound entry /
///     out-of-bound sp / out-of-bound tls / caller is kproc.
///   - `Error::NoMemory`: kstack alloc fail / Thread cache alloc fail.
///
/// # Safety
///
/// - `entry_va` must point to a function with C ABI accepting one u64
///   argument (the value of `arg`).
/// - `sp_va` must be the TOP of a writable region the caller owns for
///   the lifetime of the new thread.
/// - `tls_va` must point to a region the entry function can use as
///   its TLS base, OR be 0 if the entry function does its own TLS
///   bring-up.
/// - The entry function MUST eventually call `exit_self()` (or
///   otherwise terminate; falling off the end is undefined).
/// - The new thread shares the address space + handle table; the
///   caller is responsible for any synchronization between parent and
///   child.
pub unsafe fn spawn_raw(entry_va: u64, sp_va: u64, arg: u64, tls_va: u64) -> Result<Tid> {
    // SAFETY: forwarded to the kernel; correctness preconditions are
    // the caller's responsibility (see Safety above).
    //
    // ptid = 0: native callers read the new Tid from THIS call's return
    // value, not from a child-visible shared word, so the CLONE_PARENT_-
    // SETTID publish (#112) is unnecessary here. The native join protocol
    // (see set_tid_address below) is set_tid_address + torpor, which the
    // child wires for itself -- there is no racy parent-side tid store to
    // close. The pouch pthread layer, which DOES have a child-read new->tid,
    // passes &new->tid instead.
    let rc = unsafe { t_thread_spawn(entry_va, sp_va, arg, tls_va, 0) };
    if rc < 0 {
        return Err(Error::from_syscall_return(rc).err().unwrap_or(Error::Io));
    }
    Ok(rc as Tid)
}

/// Terminate the calling Thread. NEVER returns.
///
/// If this is the LAST non-EXITING thread in the Proc, the kernel
/// also transitions the Proc to ZOMBIE with exit_status = 0 (mirrors
/// `t::process::exit_self` with status 0). The pouch boundary-line
/// patch's `clear_child_tid` mechanism (set by pthread_create) does
/// the cross-thread wake; native libthyla-rs callers that want
/// joining must wire their own atomic + torpor::wait protocol.
pub fn exit_self() -> ! {
    // SAFETY: t_thread_exit is `!`-returning by SVC contract.
    unsafe { t_thread_exit() }
}

/// Install `tid_word` as the calling thread's clear-child-tid
/// address. On `exit_self`, the kernel:
///   1. stores 0 to *`tid_word` (best-effort uaccess);
///   2. wakes every waiter on `tid_word` (torpor_wake u32::MAX).
///
/// Returns the calling thread's tid (positive int). Never fails for
/// userspace callers.
///
/// THE JOIN PROTOCOL:
///   - Parent: store some non-zero sentinel into `tid_word` BEFORE
///     spawning the child.
///   - Child (in its entry function, FIRST thing): call
///     `set_tid_address(&tid_word)`.
///   - Child: do work.
///   - Child: call `exit_self()`.
///   - Parent: `torpor::wait` on `tid_word` with `expected = sentinel`.
///     On wake, parent observes `tid_word == 0` -- the child is
///     THREAD_EXITING. Safe to drop the child's stack OR `exit_self`
///     the parent (the kernel's `exits` check counts EXITING threads
///     as not-live).
///
/// MULTI-THREAD EXIT SAFETY:
///   `exits()` on a Proc with live peers (#811, ARCH 8.8.1) CASCADES --
///   it routes through `proc_group_terminate` (flag + wake every sleeping
///   peer + IPI the running ones) and self-exits; the last Thread out
///   reaps the Proc. It does NOT extinct the kernel. So `t_exits` from any
///   Thread (incl. the `#[panic_handler]` / OOM path) terminates the whole
///   Proc cleanly, and `exit_self` on the LAST Thread transitions it to
///   ZOMBIE. Joining via this protocol before exiting just keeps the
///   teardown orderly (the child is already EXITING). The cascade syscall
///   `SYS_EXIT_GROUP` has landed (`t_exit_group`, =60).
pub fn set_tid_address(tid_word: &AtomicU32) -> Tid {
    // SAFETY: &AtomicU32 is 4-byte aligned + writable in user-VA for
    // the duration of the call.
    let rc = unsafe { t_set_tid_address(tid_word as *const AtomicU32 as *mut u32) };
    // Never fails per the kernel contract; cast at the type boundary.
    rc as Tid
}

/// Clear the calling thread's clear-child-tid registration. Useful if
/// the thread no longer wants its exit to wake a join target.
pub fn clear_tid_address() -> Tid {
    // SAFETY: passing null is the documented "no registration" form.
    let rc = unsafe { t_set_tid_address(core::ptr::null_mut()) };
    rc as Tid
}

/// Wait for `tid_word` to be cleared by the corresponding child
/// thread's `exit_self`. Returns once `tid_word` reads 0 OR `timeout`
/// lapses.
///
/// `expected` is the sentinel value the parent stored before spawning
/// (typically non-zero); the kernel clears to 0 on child exit. Reads
/// proceed under Acquire ordering.
///
/// Errors:
///   - `Error::TimedOut`: `timeout` lapsed before the child cleared.
///   - other errors propagated from `torpor::wait`.
pub fn join_tid(tid_word: &AtomicU32, expected: u32, timeout: Option<Duration>) -> Result<()> {
    loop {
        if tid_word.load(core::sync::atomic::Ordering::Acquire) == 0 {
            return Ok(());
        }
        match torpor::wait(tid_word, expected, timeout)? {
            torpor::WaitResult::Woken | torpor::WaitResult::ValueMismatch => {
                // Re-check; the kernel's wake doesn't guarantee the
                // value has reached 0 (just that SOME store happened).
                if tid_word.load(core::sync::atomic::Ordering::Acquire) == 0 {
                    return Ok(());
                }
                // Spurious or expected-value-changed: loop and re-wait.
            }
            torpor::WaitResult::TimedOut => return Err(Error::TimedOut),
        }
    }
}
