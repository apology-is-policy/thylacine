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
//   driver sends ^Z       ->  ticks CONTINUE  (POSIX pending: the fan posts,
//                                              proc_job_stop_pgrp does NOT stop
//                                              us -- this is the arm assertion,
//                                              see jc-probe's maskstop rung)
//   UNMASK  ->  set_mask(NONE)             (the EL0 return from THIS syscall is
//                                           the delivery point: the peek now
//                                           yields tty:susp, handler_va == 0,
//                                           default action STOP -> the tail's
//                                           arm dequeues and stops us)
//   driver `fg`  ->  ticks resume  ->  DONE
//
// The tick IS the clock. The driver counts ticks rather than sleeping, and the
// unmask fires at a fixed TICK NUMBER -- so a dilated host stretches the
// driver's window and the child's schedule by the same factor and the ordering
// (^Z lands well before UNMASK_AT_TICK) holds by construction rather than by a
// wall-clock margin that a loaded host could eat.
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
use libthyla_rs::{t_exits, t_write};

/// Tick cadence. Fast enough that the driver's per-leg waits are short, slow
/// enough that a tick is never mistaken for the driver's own round-trip.
const TICK: Duration = Duration::from_millis(250);

/// The tick at which we unmask -- i.e. the tick at which the deferred stop
/// lands. The driver sends `^Z` around tick 3-4 (it waits for two ticks, then
/// settles), so the margin is ~8 ticks of the CHILD's own clock, not of the
/// host's.
const UNMASK_AT_TICK: u32 = 12;

/// Total ticks before a clean exit. The 8 after the unmask are the post-resume
/// window the driver's `fg` leg matches in.
///
/// Both numbers are kept as small as the proof allows: this probe is boot-fatal
/// via joey, so every second here is a second on EVERY boot of every gate --
/// 40 of them in one SMP gate run.
const TOTAL_TICKS: u32 = 20;

/// Markers. MUST match jc-probe's maskstop rung. Uppercase for the LS-CI/
/// jc-probe idiom: the shell's line editor echoes the (lowercase) command name,
/// so an uppercase token can only have come from this program's output.
const M_READY: &[u8] = b"MASKREADY\n";
const M_TICK: &[u8] = b"MTICK\n";
const M_UNMASK: &[u8] = b"MASKOFF\n";
const M_DONE: &[u8] = b"MASKDONE\n";
const M_FAIL_MASK: &[u8] = b"MASKFAIL\n";

fn emit(b: &[u8]) {
    // SAFETY: SVC wrapper; `b` is a valid slice and fd 1 is our stdout (the
    // pts slave jc-probe's hosted ut handed us).
    let _ = unsafe { t_write(1, b.as_ptr(), b.len()) };
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

    for n in 1..=TOTAL_TICKS {
        emit(M_TICK);
        let _ = libthyla_rs::time::sleep(TICK);
        if n == UNMASK_AT_TICK {
            // Announce BEFORE the syscall: the stop lands on that syscall's
            // EL0 return, so anything printed after it would only appear once
            // the driver had already resumed us -- too late to be the marker
            // the driver waits on before expecting `Stopped`.
            emit(M_UNMASK);
            if notes::set_mask(NoteMask::NONE).is_err() {
                emit(M_FAIL_MASK);
                // SAFETY: `!`-returning SVC.
                unsafe { t_exits(2) }
            }
            // <- the deferred tty:susp is delivered on the return from the
            //    set_mask SVC above; execution resumes here only after `fg`.
        }
    }

    emit(M_DONE);
    // SAFETY: `!`-returning SVC.
    unsafe { t_exits(0) }
}
