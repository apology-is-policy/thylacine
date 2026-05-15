// /sbin/corvus — Thylacine key-agent daemon (skeleton; P5-corvus-bringup-a).
//
// This is the P5-corvus-bringup-a skeleton: startup hardening +
// readiness banner + clean exit. The Spoor server + binary frame wire
// codec + verb dispatch (AUTH / UNWRAP / etc.) land at successive
// sub-chunks; see `docs/CORVUS-DESIGN.md §10` for the full arc.
//
// The skeleton is testable end-to-end via the production-shape
// orchestration: joey spawns /sbin/corvus with CAP_LOCK_PAGES |
// CAP_CSPRNG_READ via SYS_SPAWN_WITH_CAPS, corvus applies the hardening
// syscalls in the spec-mandated order, verifies the kernel CSPRNG is
// seeded, prints "corvus: ready" and exits 0. A non-zero exit signals
// a hardening failure that joey reports.
//
// Hardening sequence (per CORVUS-DESIGN.md §4.1.1):
//
//   1. t_mlockall(0)            Pin all pages; PROC_FLAG_MLOCKED set.
//                               Requires CAP_LOCK_PAGES (gated at the
//                               syscall surface; joey provides it via
//                               spawn-with-caps).
//
//   2. t_set_dumpable(0)        Disable core-dump permission;
//                               PROC_FLAG_NODUMP set. One-way: a future
//                               t_set_dumpable(1) on this Proc is
//                               refused.
//
//   3. t_set_traceable(0)       Disable debug-Spoor attach permission;
//                               PROC_FLAG_NOTRACE set. Same one-way
//                               semantics.
//
//   4. t_getrandom(buf, 32, 0)  Verify the kernel CSPRNG is seeded by
//                               attempting a 32-byte read. The kernel's
//                               ARM RNDR is seeded at boot
//                               (kern_random_seeded() returns true); if
//                               the syscall returns -1, corvus exits
//                               non-zero because the hardening cannot
//                               proceed without a seeded CSPRNG.
//
//   5. t_explicit_bzero(buf, 32)  Wipe the scratch entropy buffer before
//                                 returning. The skeleton doesn't use
//                                 the bytes; the wipe demonstrates the
//                                 discipline that future verbs will
//                                 follow for every plaintext secret.
//
// Invariant correspondence (CORVUS-DESIGN.md §9):
//
//   C-2  corvus pages are mlock'd + DONTDUMP'd  ←  mlockall + set_dumpable.
//   C-5  in-RAM secrets are wiped before close  ←  explicit_bzero pattern.
//   C-15 corvus refuses to generate randomness  ←  getrandom-seeded probe.
//        until kernel CSPRNG is verified seeded
//
// The Spoor server lives at /srv/corvus/ in the production path; the
// skeleton doesn't open it yet (that lands at P5-corvus-bringup-b).
// joey rfork-execs /sbin/corvus, waits for it, reports status. The
// skeleton's clean-exit IS the test signal.

#![no_std]
#![no_main]

use libthyla_rs::{
    t_explicit_bzero, t_getrandom, t_mlockall, t_putstr, t_set_dumpable, t_set_traceable,
};

// Scratch buffer for the getrandom-seeded probe. 32 bytes is small
// enough to fit on the stack (default stack size from
// usr/scripts/aarch64-userspace.ld is several pages) yet large enough
// that a buggy CSPRNG returning all-zeros would be statistically
// detectable. The skeleton doesn't perform that check; the kernel-side
// kern_random_bytes either succeeds (real entropy) or returns -1
// (unseeded / RNDR-failure).
const PROBE_LEN: usize = 32;

// step_fail — emit a tagged failure marker and exit 1. The marker is
// machine-grep-friendly for tools/test.sh ("corvus: STEP=<n> FAIL=<rc>"),
// so a regression that breaks one step is immediately attributable.
//
// Inlined-cold so the success path stays compact; never returns.
#[cold]
#[inline(never)]
fn step_fail(step: u8, rc: i64) -> ! {
    t_putstr("corvus: STEP=");
    // Single-digit step in 1..=5; emit '0' + step. Avoids itoa.
    let digit = b'0' + step;
    let buf = [digit, 0];
    let _ = t_putstr(unsafe { core::str::from_utf8_unchecked(&buf[..1]) });
    t_putstr(" FAIL rc=");
    // rc is i64; emit hex for compact + grep-friendly. Two nibbles for
    // the low byte is enough — corvus's syscalls only return 0 / -1.
    let nibble = (rc as u8) & 0x0f;
    let hex_char = if nibble < 10 { b'0' + nibble } else { b'a' + (nibble - 10) };
    let hex_buf = [hex_char, 0];
    let _ = t_putstr(unsafe { core::str::from_utf8_unchecked(&hex_buf[..1]) });
    t_putstr("\n");
    unsafe { libthyla_rs::t_exits(1) }
}

#[no_mangle]
pub extern "C" fn rs_main() -> i64 {
    t_putstr("corvus: skeleton starting (P5-corvus-bringup-a)\n");

    // Step 1: mlockall — pin all pages so secrets never touch swap.
    let rc = unsafe { t_mlockall(0) };
    if rc != 0 {
        step_fail(1, rc);
    }

    // Step 2: set_dumpable(0) — disable core dumps for this Proc. The
    // kernel's PROC_FLAG_NODUMP gate is permanent; future dump paths
    // refuse to dump corvus.
    let rc = unsafe { t_set_dumpable(0) };
    if rc != 0 {
        step_fail(2, rc);
    }

    // Step 3: set_traceable(0) — disable debug-Spoor attach. Same
    // permanence as set_dumpable.
    let rc = unsafe { t_set_traceable(0) };
    if rc != 0 {
        step_fail(3, rc);
    }

    // Step 4: getrandom-seeded probe. A 32-byte read from the kernel
    // CSPRNG. The buf must be writable; we stack-allocate.
    let mut probe: [u8; PROBE_LEN] = [0; PROBE_LEN];
    let rc = unsafe { t_getrandom(probe.as_mut_ptr(), PROBE_LEN, 0) };
    if rc != PROBE_LEN as i64 {
        step_fail(4, rc);
    }

    // Step 5: wipe the scratch via explicit_bzero. The compiler-barrier
    // semantics matter for real secrets (passphrase + KEK + DEK); for
    // 32 bytes of probe entropy the discipline is exercised here so the
    // codepath is hot and validated end-to-end at boot.
    let rc = unsafe { t_explicit_bzero(probe.as_mut_ptr(), PROBE_LEN) };
    if rc != 0 {
        step_fail(5, rc);
    }

    t_putstr("corvus: ready (hardening applied; CSPRNG seeded; no verbs yet)\n");
    0
}
