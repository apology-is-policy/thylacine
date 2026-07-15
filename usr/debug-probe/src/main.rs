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
use libthyla_rs::io::Write;
use libthyla_rs::process::{Child, Command, Stdio};
use libthyla_rs::time::{sleep, Duration};
use libthyla_rs::{t_exits, t_pread, t_putstr, t_pwrite, t_wait_pid_for};

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

    // Disarm the bp (stopped-only), set the exit flag (region[2]), resume -> the
    // child runs bp_landmark clean (now un-armed) and exit(0)s. The un-armed
    // resume also proves hwrmbreak actually cleared the bp (else it would re-trap
    // forever and never exit).
    ctl.write_all(format!("hwrmbreak 0x{:x}", bp_va).as_bytes())
        .map_err(|_| "debug-probe: FAIL -- hwrmbreak\n")?;
    let two: u64 = 1;
    if unsafe { t_pwrite(mem_f.as_raw_fd() as i64, (&two as *const u64) as *const u8, 8, (x21 + 16) as i64) } != 8 {
        return Err("debug-probe: FAIL -- write exit flag via mem\n");
    }
    ctl.write_all(b"start").map_err(|_| "debug-probe: FAIL -- start(exit)\n")?;

    // The child exits loop2 and exit(0)s. Reap it.
    match child.wait() {
        Ok(st) if st.success() => Ok(()),
        Ok(_) => Err("debug-probe: FAIL -- child exit status != 0 after bp cycle\n"),
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
