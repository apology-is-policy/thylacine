// /debug-probe -- the Stage-8a debug-fs in-guest E2E (the 8a-1c owed proof).
//
// A kernel unit test can drive the devproc debug files against a SYNTHETIC
// thread-less / hand-parked target, but it cannot produce the real thing: a
// genuinely-parked EL0 thread (a kthread never EL0-returns, so it never reaches
// the stop checkpoint and carries no EL0 trapframe). This probe closes that gap
// end to end:
//
//   1. spawn /debug-child (the target -- a yield loop with SENTINEL_REG pinned in
//      x20 and a 2-word stack region addressed by x21),
//   2. open /proc/<pid>/{ctl,regs,mem,kregs,kstack,wait},
//   3. `attach` then `stop` (retrying until x20 == SENTINEL_REG confirms the
//      child is in its park loop -- the stopped frame we want),
//   4. INSPECT the stopped target:
//        - regs:   x20 == SENTINEL_REG (exact register proof), x21/sp/pc are EL0
//                  VAs, pstate M[3:0] == 0 (EL0t) -- the SPSR privilege bits,
//        - mem:    read 8 bytes at the x21 VA == SENTINEL_MEM (cross-Proc read of
//                  a genuinely-resident EL0 page against a foreign pgtable_root),
//        - kregs:  fp/lr/sp are kernel (TTBR1, bit-63) VAs,
//        - kstack: a non-empty symbolized kernel backtrace starting at "#0",
//        - wait:   returns "stopped" (the notification channel, already parked),
//   5. MODIFY + RESUME: write 1 to the child's resume flag via /proc/<pid>/mem
//      (cross-Proc WRITE), `start`, and reap the child == 0 -- proving the write
//      landed in the target's address space AND the full stop/inspect/resume
//      cycle.
//
// joey spawns + reaps + asserts exit 0 + the "debug-probe: PASS" marker, so any
// failure gates the boot. On any failure path the child is `killgrp`'d so it can
// never outlive the probe (the child loop is intentionally unbounded).

#![no_std]
#![no_main]

extern crate alloc;

#[global_allocator]
static GLOBAL_ALLOCATOR: libthyla_rs::alloc::ThylaAlloc = libthyla_rs::alloc::ThylaAlloc;

use alloc::format;
use libthyla_rs::fs::{File, OpenOptions};
use libthyla_rs::io::{Read, Write};
use libthyla_rs::process::{Child, Command, Stdio};
use libthyla_rs::time::{sleep, Duration};
use libthyla_rs::{t_exits, t_pread, t_putstr, t_pwrite, t_wait_pid_for, T_WAIT_WNOHANG};

// MUST match debug-child.
const SENTINEL_REG: u64 = 0xDEB0_DEB0;
const SENTINEL_MEM: u64 = 0xDEB0_0001_CAFE_0001;

// t_user_regs byte offsets (the /proc/<pid>/regs ABI, syscall.h).
const R_X20: usize = 20 * 8;
const R_X21: usize = 21 * 8;
const R_X22: usize = 22 * 8; // 8a-2b: the child pins &bp_landmark here
const R_SP: usize = 248;
const R_PC: usize = 256;
const R_PSTATE: usize = 264;
const REGS_LEN: usize = 272;

// t_kernel_regs byte offsets (the /proc/<pid>/kregs ABI, syscall.h).
const K_FP: usize = 80;
const K_LR: usize = 88;
const K_SP: usize = 96;
const KREGS_LEN: usize = 112;

// EL0 VAs are TTBR0 (< 2^48, bit 63 clear); kernel VAs are TTBR1 (bit 63 set).
const USER_VA_LIMIT: u64 = 1u64 << 47;

// Bound on the post-`start` wait for the watchpoint stop (task #70). A working
// substrate re-stops within one instruction of the resume, so ~20 s is orders of
// magnitude of slack even under full emulation -- it exists only to convert a
// non-delivering substrate's guest wedge from a boot-long hang into a report.
const WP_POLL_TRIES: u32 = 800;
const WP_POLL_MS: u64 = 25;

// Bound on the post-SKIP reap sweep (~2 s). A child that merely ran to completion
// exits at once; one wedged inside the emulator never will, and waiting cannot
// change that -- see the SKIP path.
const REAP_POLL_TRIES: u32 = 80;

fn fail(msg: &str) -> ! {
    t_putstr(msg);
    unsafe { t_exits(1) }
}

// Print the tagged reason, best-effort terminate AND REAP the target (its loop
// is unbounded -- the probe owns its lifetime), and gate the boot. Reaping (not
// just killing) is load-bearing: an unreaped killed child orphans to joey and
// its later reap returns a "wrong pid" that extincts kproc's joey reaper,
// masking THIS message.
fn die(child: &Child, msg: &str) -> ! {
    t_putstr(msg);
    let _ = child.kill();
    let mut st: i32 = 0;
    unsafe {
        t_wait_pid_for(child.pid(), 0, &mut st as *mut i32);
    }
    unsafe { t_exits(1) }
}

fn u64_le(buf: &[u8], off: usize) -> u64 {
    let mut v: u64 = 0;
    let mut i = 0;
    while i < 8 {
        v |= (buf[off + i] as u64) << (8 * i);
        i += 1;
    }
    v
}

// task #70: does this substrate deliver EL0 data watchpoints?
//
// Not something the guest can safely probe for itself: *arming* a watchpoint is
// what wedges a non-delivering substrate, and the wedged thread cannot be killed
// (it never takes a timer IRQ, so it never reaches the EL0-return die-check), so
// under TCG's round-robin it starves the whole guest and the boot never finishes.
// A self-test would therefore destroy the machine it was testing.
//
// So the answer comes from outside: run-vm.sh passes `-append
// thylacine.nowatchpoint` on an accel it knows cannot deliver, QEMU turns that
// into /chosen/bootargs, and we read it back through the /hw FDT mount. Absent or
// unreadable -> assume delivery (fail toward the hard assertion, so a substrate
// that simply lacks the /hw mount still gets the real test rather than a silent
// skip).
fn substrate_delivers_watchpoints() -> bool {
    // NB: devhw is `.seekable = false` (kernel/devhw.c), so the #37 ESPIPE gate
    // rejects a positioned pread on it up front -- this MUST be a sequential read.
    let mut f = match File::open("/hw/chosen/bootargs") {
        Ok(f) => f,
        Err(_) => return true, // no bootargs property -> nothing opted out
    };
    let mut buf = [0u8; 256];
    let n = match f.read(&mut buf) {
        Ok(n) if n > 0 => n,
        _ => return true,
    };
    let hay = &buf[..n];
    let needle = b"nowatchpoint";
    !hay.windows(needle.len()).any(|w| w == needle)
}

// Fill `buf` fully via positioned reads (pread does not advance a cursor); a
// short return before the buffer fills is a failure (EOF/hole/error).
fn read_exact_at(fd: i64, off: i64, buf: &mut [u8]) -> Result<(), ()> {
    let mut done = 0usize;
    while done < buf.len() {
        let r = unsafe {
            t_pread(
                fd,
                buf[done..].as_mut_ptr(),
                buf.len() - done,
                off + done as i64,
            )
        };
        if r <= 0 {
            return Err(());
        }
        done += r as usize;
    }
    Ok(())
}

// The full debug cycle. Returns Err(reason) on any failed assertion; the child
// stays owned by rs_main so the caller can terminate it.
fn debug_flow(child: &mut Child) -> Result<(), &'static str> {
    let pid = child.pid();

    let mut ctl = OpenOptions::new()
        .write(true)
        .open(&format!("/proc/{}/ctl", pid))
        .map_err(|_| "debug-probe: FAIL -- open ctl\n")?;
    let regs_f = File::open(&format!("/proc/{}/regs", pid)).map_err(|_| "debug-probe: FAIL -- open regs\n")?;
    let mem_f = OpenOptions::new()
        .read(true)
        .write(true)
        .open(&format!("/proc/{}/mem", pid))
        .map_err(|_| "debug-probe: FAIL -- open mem\n")?;
    let kregs_f = File::open(&format!("/proc/{}/kregs", pid)).map_err(|_| "debug-probe: FAIL -- open kregs\n")?;
    let kstack_f = File::open(&format!("/proc/{}/kstack", pid)).map_err(|_| "debug-probe: FAIL -- open kstack\n")?;

    ctl.write_all(b"attach").map_err(|_| "debug-probe: FAIL -- attach\n")?;

    // Stop, then confirm the child is in its park loop (x20 == SENTINEL_REG). If
    // it hasn't reached the loop yet (still in _start), resume and retry -- the
    // sentinel-in-x20 is a reliable "in loop" signal, so this is deterministic,
    // not a fixed-sleep race.
    let mut regs = [0u8; REGS_LEN];
    let mut in_loop = false;
    for _ in 0..80 {
        ctl.write_all(b"stop").map_err(|_| "debug-probe: FAIL -- stop\n")?; // blocks until stopped
        if read_exact_at(regs_f.as_raw_fd() as i64, 0, &mut regs).is_err() {
            return Err("debug-probe: FAIL -- read regs\n");
        }
        if u64_le(&regs, R_X20) == SENTINEL_REG {
            in_loop = true;
            break;
        }
        ctl.write_all(b"start").map_err(|_| "debug-probe: FAIL -- start(retry)\n")?;
        let _ = sleep(Duration::from_millis(10));
    }
    if !in_loop {
        return Err("debug-probe: FAIL -- child never reached the park loop (x20 sentinel)\n");
    }
    // The child is now STOPPED in its loop (last verb was `stop`). Inspect it.

    // --- regs: the register frame of a real parked EL0 thread ---
    let x21 = u64_le(&regs, R_X21);
    let sp = u64_le(&regs, R_SP);
    let pc = u64_le(&regs, R_PC);
    let pstate = u64_le(&regs, R_PSTATE);
    if x21 == 0 || x21 >= USER_VA_LIMIT {
        return Err("debug-probe: FAIL -- regs x21 not an EL0 VA\n");
    }
    if sp == 0 || sp >= USER_VA_LIMIT {
        return Err("debug-probe: FAIL -- regs sp not an EL0 VA\n");
    }
    if pc == 0 || pc >= USER_VA_LIMIT {
        return Err("debug-probe: FAIL -- regs pc not an EL0 VA\n");
    }
    if pstate & 0xf != 0 {
        // SPSR M[3:0] == 0b0000 -> EL0t (the target is an EL0 thread).
        return Err("debug-probe: FAIL -- regs pstate not EL0t\n");
    }
    t_putstr("debug-probe: regs ok (x20 sentinel, EL0t frame)\n");

    // --- mem: cross-Proc READ of the target's resident EL0 stack region ---
    let mut mem8 = [0u8; 8];
    if read_exact_at(mem_f.as_raw_fd() as i64, x21 as i64, &mut mem8).is_err() {
        return Err("debug-probe: FAIL -- read mem@x21\n");
    }
    if u64_le(&mem8, 0) != SENTINEL_MEM {
        return Err("debug-probe: FAIL -- mem sentinel mismatch\n");
    }
    t_putstr("debug-probe: mem read ok (cross-Proc SENTINEL_MEM)\n");

    // --- kregs: the kernel-side frame (fp/lr/sp are TTBR1 VAs) ---
    let mut kregs = [0u8; KREGS_LEN];
    if read_exact_at(kregs_f.as_raw_fd() as i64, 0, &mut kregs).is_err() {
        return Err("debug-probe: FAIL -- read kregs\n");
    }
    let kfp = u64_le(&kregs, K_FP);
    let klr = u64_le(&kregs, K_LR);
    let ksp = u64_le(&kregs, K_SP);
    if (kfp >> 63) != 1 || (klr >> 63) != 1 || (ksp >> 63) != 1 {
        return Err("debug-probe: FAIL -- kregs fp/lr/sp not kernel VAs\n");
    }
    t_putstr("debug-probe: kregs ok (kernel-side frame)\n");

    // --- kstack: a non-empty symbolized kernel backtrace starting at "#0" ---
    let mut kbuf = [0u8; 256];
    let kn = unsafe { t_pread(kstack_f.as_raw_fd() as i64, kbuf.as_mut_ptr(), kbuf.len(), 0) };
    if kn <= 0 {
        return Err("debug-probe: FAIL -- read kstack\n");
    }
    if kbuf[0] != b'#' || kbuf[1] != b'0' {
        return Err("debug-probe: FAIL -- kstack does not start at frame #0\n");
    }
    t_putstr("debug-probe: kstack ok (unified-stack kernel half)\n");

    // --- wait: the stop-notification channel (already parked -> returns now) ---
    {
        let wait_f = File::open(&format!("/proc/{}/wait", pid)).map_err(|_| "debug-probe: FAIL -- open wait\n")?;
        let mut wbuf = [0u8; 16];
        let wn = unsafe { t_pread(wait_f.as_raw_fd() as i64, wbuf.as_mut_ptr(), wbuf.len(), 0) };
        if wn < 7 || &wbuf[..7] != b"stopped" {
            return Err("debug-probe: FAIL -- wait did not report stopped\n");
        }
    }
    t_putstr("debug-probe: wait ok (stopped)\n");

    // The child pinned &bp_landmark in x22 -- capture it from the phase-1 frame
    // (before we overwrite `regs` at the bp stop below).
    let bp_va = u64_le(&regs, R_X22);
    if bp_va == 0 || bp_va >= USER_VA_LIMIT {
        return Err("debug-probe: FAIL -- regs x22 not an EL0 VA (bp landmark)\n");
    }

    // --- MODIFY + RESUME into loop2: cross-Proc WRITE the resume flag, `start` ---
    // (region[1] releases loop1; the child then loops calling bp_landmark).
    let one: u64 = 1;
    if unsafe { t_pwrite(mem_f.as_raw_fd() as i64, (&one as *const u64) as *const u8, 8, (x21 + 8) as i64) } != 8 {
        return Err("debug-probe: FAIL -- write resume flag via mem\n");
    }

    // --- 8a-2b: arm a real cross-Proc HARDWARE BREAKPOINT at bp_landmark ---
    // Arm while the target is still stopped (hwbreak is stopped-only), then resume
    // it into loop2. The first bp_landmark() call traps (EC 0x30 -> the whole-Proc
    // stop). This is the headline b-1 proof: a debugger-armed HW breakpoint fires
    // for a cross-Proc EL0 target and delivers a stop -- what only a real
    // scheduled EL0 execution can exercise (the kernel tests are structurally
    // blind to the install + EC route, exactly as they were to 8a-1's F1/F2).
    ctl.write_all(format!("hwbreak 0x{:x}", bp_va).as_bytes())
        .map_err(|_| "debug-probe: FAIL -- hwbreak\n")?;
    ctl.write_all(b"start").map_err(|_| "debug-probe: FAIL -- start(into loop2)\n")?;

    // Wait for the breakpoint to fire. `wait` blocks until fully-stopped; after
    // `start` cleared debug_stop_req, a return of "stopped" means the bp re-set it
    // (the child parked at bp_landmark). Not a fixed-sleep race -- the wait file
    // polls the stop state.
    {
        let wait_f = File::open(&format!("/proc/{}/wait", pid)).map_err(|_| "debug-probe: FAIL -- open wait(bp)\n")?;
        let mut wbuf = [0u8; 16];
        let wn = unsafe { t_pread(wait_f.as_raw_fd() as i64, wbuf.as_mut_ptr(), wbuf.len(), 0) };
        if wn < 7 || &wbuf[..7] != b"stopped" {
            return Err("debug-probe: FAIL -- bp did not deliver a stop\n");
        }
    }
    // The frozen frame's PC must be the breakpoint VA -- a HW bp fires with
    // ELR = the bp'd instruction (not yet executed). This is the end-to-end proof.
    if read_exact_at(regs_f.as_raw_fd() as i64, 0, &mut regs).is_err() {
        return Err("debug-probe: FAIL -- read regs(bp)\n");
    }
    if u64_le(&regs, R_PC) != bp_va {
        return Err("debug-probe: FAIL -- bp stop pc != landmark VA\n");
    }
    t_putstr("debug-probe: hwbreak ok (cross-Proc bp fired; pc == landmark)\n");

    // --- regression: >4 concurrent HW breakpoints through the ctl path ---
    // The table holds min(num_brps, DEBUG_HWBP_SLOTS) breakpoints. The software cap
    // was 4, which starved Delve's `next` (it arms one temp HW breakpoint per
    // successor PC + the return address, so a small step-over needs 4-5 concurrent
    // with the user's breakpoint -> the overflow `hwbreak` returned -1). With the
    // table sized to the arm64 max, the usable count is num_brps (6 on QEMU -cpu
    // max + Apple M2). bp_va is already armed (1); arm four more distinct 4-aligned
    // VAs (5 total) -- each ctl write must succeed (on the old cap of 4 the 5th
    // returned -1 -> Err). Remove the four extras so bp_va stays the sole armed bp
    // for the step-over dance below. (The VAs need not be mapped code: the arm only
    // records the table entry; nothing executes them -- they are removed before the
    // child resumes.)
    for i in 1..=4u64 {
        ctl.write_all(format!("hwbreak 0x{:x}", bp_va + i * 4).as_bytes())
            .map_err(|_| "debug-probe: FAIL -- 5-concurrent-bp arm (HW breakpoint ceiling < 5?)\n")?;
    }
    for i in 1..=4u64 {
        ctl.write_all(format!("hwrmbreak 0x{:x}", bp_va + i * 4).as_bytes())
            .map_err(|_| "debug-probe: FAIL -- 5-concurrent-bp disarm\n")?;
    }
    t_putstr("debug-probe: multi-bp ok (5 concurrent HW breakpoints via ctl)\n");

    // --- 8a-2b-2: single-step FROM the breakpointed PC (the step-over dance) ---
    // The target is stopped AT bp_landmark (pc == bp_va, the bp still armed). Each
    // `step` executes exactly one A64 instruction (4 bytes) and re-stops. The
    // FIRST step is from the breakpointed entry, so the step-over-breakpoint dance
    // must disable the bp for that step (else the resume re-traps on it instead of
    // advancing). Verify pc advances by exactly 4 for three steps -- staying
    // within the linear prologue+nops of bp_landmark (before any ret/branch).
    let mut prev_pc = bp_va;
    for _ in 0..3 {
        ctl.write_all(b"step").map_err(|_| "debug-probe: FAIL -- step\n")?; // blocks until re-stopped
        if read_exact_at(regs_f.as_raw_fd() as i64, 0, &mut regs).is_err() {
            return Err("debug-probe: FAIL -- read regs(step)\n");
        }
        let pc = u64_le(&regs, R_PC);
        if pc != prev_pc + 4 {
            return Err("debug-probe: FAIL -- step did not advance pc by one instruction\n");
        }
        prev_pc = pc;
    }
    t_putstr("debug-probe: step ok (single-step + step-over; pc advanced 4B/instr x3)\n");

    // Disarm the bp (stopped-only). The child is still stopped mid-bp_landmark.
    ctl.write_all(format!("hwrmbreak 0x{:x}", bp_va).as_bytes())
        .map_err(|_| "debug-probe: FAIL -- hwrmbreak\n")?;

    // --- 8a-2b-3: arm a real cross-Proc HARDWARE WATCHPOINT on region[3] ---
    // The watch target is at x21 + 24 (region[3], the 4th stack word). Arm a WRITE
    // watchpoint (8 bytes) while the target is still stopped (hwwatch is stopped-
    // only), set region[2]=1 so the child breaks loop2 and proceeds to the watched
    // store, then resume. The child's `write_volatile(&region[3], ...)` STR traps
    // (EC 0x34 -> the whole-Proc stop) -- the headline b-3 proof: a debugger-armed
    // HW watchpoint fires on a cross-Proc EL0 data access.
    let watch_va = x21 + 24;
    // task #70: on a substrate that cannot deliver EC 0x34, arming the watchpoint
    // wedges the child UNRECOVERABLY (see substrate_delivers_watchpoints), so the
    // only safe handling is not to arm at all. Release the child untrapped instead
    // and let the rest of the boot proceed.
    let wp_supported = substrate_delivers_watchpoints();
    if wp_supported {
        ctl.write_all(format!("hwwatch w 0x{:x} 8", watch_va).as_bytes())
            .map_err(|_| "debug-probe: FAIL -- hwwatch\n")?;
    } else {
        t_putstr("debug-probe: hwwatch SKIPPED -- substrate cannot deliver EC 0x34 (task #70)\n");
    }
    let proceed: u64 = 1;
    if unsafe { t_pwrite(mem_f.as_raw_fd() as i64, (&proceed as *const u64) as *const u8, 8, (x21 + 16) as i64) } != 8 {
        return Err("debug-probe: FAIL -- write proceed flag via mem\n");
    }
    ctl.write_all(b"start").map_err(|_| "debug-probe: FAIL -- start(into watch)\n")?;

    if !wp_supported {
        // Nothing armed: the child performs the store untrapped and exit(0)s, so
        // the ordinary reap is the whole remaining cycle.
        return match child.wait() {
            Ok(st) if st.success() => Ok(()),
            Ok(_) => Err("debug-probe: FAIL -- child exit status != 0 (wp-skipped path)\n"),
            Err(_) => Err("debug-probe: FAIL -- reap child (wp-skipped path)\n"),
        };
    }

    // Wait for the watchpoint to fire -- BOUNDED, deliberately not the blocking
    // /proc/<pid>/wait read. A successful regs read IS a stopped-ness test (the
    // debug reads are stopped-only, and `start` cleared debug_stop_req, so a read
    // only succeeds once something re-stopped the child -- and the bp is disarmed,
    // so that something is the watchpoint). Polling it therefore needs no blocking
    // channel and cannot hang.
    //
    // The bound is load-bearing: a substrate that does not implement EL0 data
    // watchpoints leaves the child stuck at the watched access, so a blocking wait
    // here never returns and takes the entire boot with it (task #70 -- QEMU TCG
    // programs DBGWVR/DBGWCR, never raises EC 0x34, and wedges the guest at the
    // access; the same kernel + encoding fires on real silicon under HVF). We
    // report the gap and skip the assertions instead of hanging. This cannot
    // silently mask a regression: tools/test.sh REQUIRES the "hwwatch ok" line on
    // any accel that can deliver, so the assertion stays hard where it can run.
    let mut fired = false;
    for _ in 0..WP_POLL_TRIES {
        if read_exact_at(regs_f.as_raw_fd() as i64, 0, &mut regs).is_ok() {
            fired = true;
            break;
        }
        let _ = sleep(Duration::from_millis(WP_POLL_MS));
    }
    if !fired {
        t_putstr("debug-probe: hwwatch SKIPPED -- no EC 0x34 delivered by this substrate (task #70)\n");
        // The child is stuck at the watched access: it cannot be stopped (it never
        // reaches the EL0-return checkpoint) and the wp cannot be lifted (hwrmwatch
        // is stopped-only). Ask it to die, but do NOT block on the reap: measured on
        // QEMU TCG, such a child is UNKILLABLE. The kill only takes effect at the
        // EL0-return tail's #809 die-check, which a thread reaches via the timer
        // IRQ -- and a thread spinning inside the emulator's own retry of a single
        // instruction never returns to the CPU loop where interrupts are polled, so
        // no tick is ever taken. Nothing in the guest can reclaim it; a blocking
        // reap here just moves the hang from the wait to the reap (measured).
        //
        // So: best-effort kill (it DOES work on a substrate that merely fails to
        // deliver EC 0x34 without wedging -- there the child runs to completion and
        // exits), a bounded WNOHANG sweep, then continue regardless. The stuck Proc
        // stays ALIVE rather than becoming a zombie, so it does not orphan a zombie
        // onto joey's reaper; it costs one wedged vCPU for the rest of the boot.
        let _ = child.kill();
        let mut st: i32 = 0;
        for _ in 0..REAP_POLL_TRIES {
            if unsafe { t_wait_pid_for(child.pid(), T_WAIT_WNOHANG, &mut st as *mut i32) } != 0 {
                break;
            }
            let _ = sleep(Duration::from_millis(WP_POLL_MS));
        }
        return Ok(());
    }
    // The frozen frame's PC is the store instruction (an EL0 VA, past the bp region).
    // A wp fires with ELR = the accessing instruction; we assert an EL0t frame (the
    // wp stopped a real user access), not an exact VA (the store's address is
    // compiler-placed in rs_main, unlike the pinned bp landmark).
    if read_exact_at(regs_f.as_raw_fd() as i64, 0, &mut regs).is_err() {
        return Err("debug-probe: FAIL -- read regs(wp)\n");
    }
    let wpc = u64_le(&regs, R_PC);
    let wps = u64_le(&regs, R_PSTATE);
    if wpc == 0 || wpc >= USER_VA_LIMIT || (wps & 0xf) != 0 {
        return Err("debug-probe: FAIL -- wp stop frame not a real EL0t access\n");
    }
    t_putstr("debug-probe: hwwatch ok (cross-Proc wp fired on store)\n");

    // Remove the wp (stopped-only), resume -> the store re-executes untrapped and
    // the child exit(0)s. The un-watched resume also proves hwrmwatch actually
    // cleared the wp (else the store would re-trap forever and never exit).
    ctl.write_all(format!("hwrmwatch 0x{:x}", watch_va).as_bytes())
        .map_err(|_| "debug-probe: FAIL -- hwrmwatch\n")?;
    ctl.write_all(b"start").map_err(|_| "debug-probe: FAIL -- start(exit)\n")?;

    // The child completes the store and exit(0)s. Reap it.
    match child.wait() {
        Ok(st) if st.success() => Ok(()),
        Ok(_) => Err("debug-probe: FAIL -- child exit status != 0 after wp cycle\n"),
        Err(_) => Err("debug-probe: FAIL -- reap child\n"),
    }
    // ctl / mem_f / regs_f / kregs_f / kstack_f drop here -> ctl close detaches.
}

#[no_mangle]
pub extern "C" fn rs_main() -> i64 {
    t_putstr("debug-probe: starting (8a-1c debug-fs E2E)\n");

    // Root-anchored (pre-pivot root_spoor = the cpio root); cwd-independent.
    // Piped stdio, not Inherit: debug-probe is itself spawned fd-less (joey's
    // SYS_PUTS probes carry no stdin/out/err), so an Inherit child would try to
    // clone absent fds (-> EIO). debug-child prints via SYS_PUTS and never
    // touches fd 0/1/2, so the pipes stay empty -> no drain deadlock.
    let mut child = match Command::new("/debug-child")
        .stdin(Stdio::Piped)
        .stdout(Stdio::Piped)
        .stderr(Stdio::Piped)
        .spawn()
    {
        Ok(c) => c,
        Err(e) => {
            t_putstr(&format!("debug-probe: FAIL -- spawn debug-child (errno {})\n", e.as_errno()));
            unsafe { t_exits(1) }
        }
    };

    match debug_flow(&mut child) {
        Ok(()) => {
            t_putstr("debug-probe: PASS\n");
            unsafe { t_exits(0) }
        }
        Err(msg) => die(&child, msg),
    }
}
