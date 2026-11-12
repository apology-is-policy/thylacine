// /susp-mask-child -- the foreground job for jc-probe's `maskstop` rung: a
// program that MASKS the tty family, absorbs a `^Z` without stopping, and then
// takes the deferred stop the moment it unmasks.
//
// WHY THIS EXISTS. The kernel has two spellings of "apply the default stop".
// The one an ordinary `^Z` uses is the POST-time decider: pts ldisc ->
// tty:susp -> proc_job_stop_pgrp, whose per-member arm calls
// proc_tty_susp_would_stop_locked and stops the Proc right there. That path is
// well covered (jc-probe's stop / restop rungs, pty-4, pty-susp-pouch).
//
// The OTHER spelling is the EL0-return tail's NOTE_DFL_STOP arm
// (notes_deliver_at_el0_return -> notes_stop_note_name_locked -> dequeue ->
// proc_job_stop_self), and NOTHING reached it. Not because it is dead code and
// not because the tests were wrong, but because its precondition is a state no
// test CONSTRUCTED. Read the decider's own contract: reaching a queued
// stop-class note at all means the post-time decider declined, and there is
// exactly one way that happens -- EVERY thread had NOTE_BIT_TTY masked, so the
// fan POSTED the note instead of stopping. An unmasked `^Z` never gets there.
// Three green E2Es sat over that hole; they were not wrong, they were
// irrelevant, which from a green is indistinguishable.
//
// So this program constructs the state on purpose:
//
//   mask tty  ->  READY  ->  ticks         (the pgrp is stoppable-in-principle
//                                           but every thread defers)
//   driver sends ^Z       ->  HELD          (POSIX pending: the fan posts,
//                                           proc_job_stop_pgrp does NOT stop
//                                           us. We read the queued tty:susp
//                                           back from the kernel and say so --
//                                           the arm assertion, see jc-probe's
//                                           maskstop rung)
//   HOLD_TICKS more ticks ->  UNMASK  ->  set_mask(NONE)
//                                          (the EL0 return from THIS syscall is
//                                           the delivery point: the peek now
//                                           yields tty:susp, handler_va == 0,
//                                           default action STOP -> the tail's
//                                           arm dequeues and stops us)
//   driver `fg`  ->  ticks resume  ->  DONE
//
// Nothing here waits on a clock for the driver. The unmask follows the `^Z` by
// a count of our own ticks, and we learn that the `^Z` has come by asking the
// kernel what is queued behind our mask (SYS_NOTE_WAIT's pending form, POSIX
// sigpending), which takes nothing off the queue. A driver held up for any
// time short of its own watchdog finds us still masked and still ticking.
//
// Every write is one t_write to fd 1: no stdio buffer exists that could flush
// on resume and forge the exact output that reads as "it never stopped".

#![no_std]
#![no_main]

extern crate alloc;

#[global_allocator]
static GLOBAL_ALLOCATOR: libthyla_rs::alloc::ThylaAlloc = libthyla_rs::alloc::ThylaAlloc;

use core::time::Duration;
use libthyla_rs::notes::{self, NoteClass, NoteMask};
use libthyla_rs::{t_exits, t_note_wait, t_write, T_NOTE_TAKE_TTY_SUSP, T_NOTE_WAIT_PENDING};

/// Tick cadence. Fast enough that the driver's per-leg waits are short, slow
/// enough that a tick is never mistaken for the driver's own round-trip.
const TICK: Duration = Duration::from_millis(250);

/// Ticks we keep running with the `^Z` queued before we unmask. The driver
/// counts them between MASKHELD and MASKOFF. MUST match jc-probe.
const HOLD_TICKS: u32 = 3;

/// Ticks after the resume, before a clean exit: the window the driver's `fg`
/// leg matches in.
///
/// Both numbers are kept as small as the proof allows: this probe is boot-fatal
/// via joey, so every second here is a second on EVERY boot of every gate --
/// 40 of them in one SMP gate run.
const AFTER_TICKS: u32 = 4;

/// Ticks we wait for the `^Z`: a minute, past the driver's watchdog, so the
/// limit never ends us inside the ladder and an abandoned child does not
/// linger.
const WAIT_LIMIT_TICKS: u32 = 240;

/// Markers. MUST match jc-probe's maskstop rung. Uppercase for the LS-CI/
/// jc-probe idiom: the shell's line editor echoes the (lowercase) command name,
/// so an uppercase token can only have come from this program's output.
const M_READY: &[u8] = b"MASKREADY\n";
const M_TICK: &[u8] = b"MTICK\n";
const M_HELD: &[u8] = b"MASKHELD\n";
const M_UNMASK: &[u8] = b"MASKOFF\n";
const M_DONE: &[u8] = b"MASKDONE\n";
const M_FAIL_MASK: &[u8] = b"MASKFAIL\n";

fn emit(b: &[u8]) {
    // SAFETY: SVC wrapper; `b` is a valid slice and fd 1 is our stdout (the
    // pts slave jc-probe's hosted ut handed us).
    let _ = unsafe { t_write(1, b.as_ptr(), b.len()) };
}

/// One tick: the marker, then the cadence. False when the terminal took
/// nothing, which is a terminal whose master is gone.
fn tick() -> bool {
    // SAFETY: SVC wrapper; M_TICK is a valid static buffer.
    if unsafe { t_write(1, M_TICK.as_ptr(), M_TICK.len()) } <= 0 {
        return false;
    }
    let _ = libthyla_rs::time::sleep(TICK);
    true
}

/// Whether a tty:susp is queued behind our mask. The pending form reads the
/// queue and takes nothing from it.
fn susp_held() -> bool {
    // SAFETY: SVC wrapper; the pending form takes no pointer.
    let word = unsafe { t_note_wait(T_NOTE_WAIT_PENDING, 0, core::ptr::null_mut(), 0) };
    word > 0 && ((word as u64) & T_NOTE_TAKE_TTY_SUSP) != 0
}

#[no_mangle]
pub extern "C" fn rs_main() -> i64 {
    // Install the mask, then READ IT BACK FROM THE KERNEL. A bare "set_mask
    // returned Ok" would only prove the syscall was accepted -- and this whole
    // scenario is a negative assertion (the ^Z does NOT stop us), which a mask
    // that silently failed to install would satisfy for the wrong reason: the
    // ^Z would stop us, jc-probe's arm assertion would fire, and the diagnosis
    // would point at the kernel instead of at us. The second set_mask returns
    // the PREVIOUS mask, so a Tty bit in it is the kernel's own testimony that
    // the first call took effect.
    let want = NoteMask::just(NoteClass::Tty);
    if notes::set_mask(want).is_err() {
        emit(M_FAIL_MASK);
        // SAFETY: `!`-returning SVC.
        unsafe { t_exits(2) }
    }
    match notes::set_mask(want) {
        Ok(prev) if prev.contains(NoteClass::Tty) => {}
        _ => {
            emit(M_FAIL_MASK);
            // SAFETY: `!`-returning SVC.
            unsafe { t_exits(2) }
        }
    }
    emit(M_READY);

    // Tick until the `^Z` is queued. The kernel is the one that says so: the
    // fan found every thread masked and posted the note where a stop would
    // have parked us.
    let mut waited = 0;
    while !susp_held() {
        if waited == WAIT_LIMIT_TICKS || !tick() {
            // SAFETY: `!`-returning SVC.
            unsafe { t_exits(3) }
        }
        waited += 1;
    }
    emit(M_HELD);
    for _ in 0..HOLD_TICKS {
        if !tick() {
            // SAFETY: `!`-returning SVC.
            unsafe { t_exits(3) }
        }
    }

    // Announce BEFORE the syscall: the stop lands on that syscall's EL0
    // return, so anything printed after it would only appear once the driver
    // had already resumed us -- too late to be the marker the driver waits on
    // before expecting `Stopped`.
    emit(M_UNMASK);
    if notes::set_mask(NoteMask::NONE).is_err() {
        emit(M_FAIL_MASK);
        // SAFETY: `!`-returning SVC.
        unsafe { t_exits(2) }
    }
    // <- the deferred tty:susp is delivered on the return from the set_mask
    //    SVC above; execution resumes here only after `fg`.

    for _ in 0..AFTER_TICKS {
        if !tick() {
            // SAFETY: `!`-returning SVC.
            unsafe { t_exits(3) }
        }
    }

    emit(M_DONE);
    // SAFETY: `!`-returning SVC.
    unsafe { t_exits(0) }
}
