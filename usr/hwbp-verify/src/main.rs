// /hwbp-verify -- the Go IDE Stage 8a-2a empirical hardware-debug verify.
//
// The design's one open empirical question (DEBUG-FS-DESIGN §2, §10): does a
// GUEST-programmed EL0 hardware breakpoint deliver its debug exception (EC
// 0x30) to guest EL1 on this substrate? TCG fully emulates it; macOS HVF on
// Apple Silicon is the young path the design requires PROVEN before the 8a-2b
// HW-debug tier builds on it. This probe answers it end to end:
//
//   1. arm a real hardware breakpoint (DBGBCR0=0x1E5 / DBGBVR0 / MDSCR.MDE) on a
//      VA it is about to execute -- via the self-scoped `hwverify` ctl verb on
//      its OWN /proc/<pid>/ctl (I-39 owner-gated; kernel/devproc.c),
//   2. execute the armed instruction (call `hwbp_target`),
//   3. read the verdict back from the same ctl file: "hwverify fired=<0|1> ...".
//
// A `fired=1` is the empirical proof the EC delivered. A `fired=0` (never trapped)
// is a LEGITIMATE verdict -- "HW debug does not deliver under this accel, so the
// 8a-2 tests run TCG-only" -- NOT a failure. Only a malfunction (the kernel
// terminating the probe on a stray EC, or the ctl surface misbehaving) is a real
// error. joey logs the verdict and gates the boot ONLY on a malfunction.
//
// Exit codes: 0 = delivered (PASS); 3 = not delivered (clean TCG-only verdict);
// 1 = a probe-side error (open/write/read failure -- a malfunction).

#![no_std]
#![no_main]

extern crate alloc;

#[global_allocator]
static GLOBAL_ALLOCATOR: libthyla_rs::alloc::ThylaAlloc = libthyla_rs::alloc::ThylaAlloc;

use alloc::format;
use libthyla_rs::fs::OpenOptions;
use libthyla_rs::io::Write;
use libthyla_rs::{t_exits, t_getpid, t_pread, t_putstr};

// The instruction the breakpoint is armed on. #[inline(never)] + the volatile
// asm keep it a real, called-into function with a stable entry address; the bp
// fires on its first instruction, the kernel records + disarms, and it returns
// normally. `hwbp_target as u64` is the runtime VA (PC-relative, load-base
// independent) -- exactly what DBGBVR0 wants.
#[no_mangle]
#[inline(never)]
extern "C" fn hwbp_target() {
    unsafe { core::arch::asm!("nop", options(nostack, preserves_flags)); }
}

fn err_exit(msg: &str) -> ! {
    t_putstr(msg);
    unsafe { t_exits(1) } // malfunction (probe-side error) -> boot-fatal at joey
}

// Scan a ctl read for "fired=1" / "fired=0"; returns Some(true|false) or None
// (no verify result present -- an empty or unexpected read).
fn parse_fired(buf: &[u8]) -> Option<bool> {
    const TOK: &[u8] = b"fired=";
    if buf.len() < TOK.len() + 1 {
        return None;
    }
    let mut i = 0;
    while i + TOK.len() < buf.len() {
        if &buf[i..i + TOK.len()] == TOK {
            return match buf[i + TOK.len()] {
                b'1' => Some(true),
                b'0' => Some(false),
                _ => None,
            };
        }
        i += 1;
    }
    None
}

#[no_mangle]
pub extern "C" fn rs_main() -> i64 {
    t_putstr("hwbp-verify: starting (8a-2a HW-debug delivery check)\n");

    let pid = unsafe { t_getpid() };
    if pid <= 0 {
        err_exit("hwbp-verify: FAIL -- getpid\n");
    }

    // ORDWR: write arms/disarms; read pulls the verdict back.
    let mut ctl = match OpenOptions::new()
        .read(true)
        .write(true)
        .open(&format!("/proc/{}/ctl", pid))
    {
        Ok(f) => f,
        Err(_) => err_exit("hwbp-verify: FAIL -- open /proc/self/ctl\n"),
    };

    let va = (hwbp_target as extern "C" fn()) as usize as u64;
    let arm_cmd = format!("hwverify 0x{:x}", va);

    // A few attempts: the arm+trap window is a single EL0 instruction on one CPU,
    // so a genuine "not delivered" is deterministic (fired=0 every time), while a
    // freak arm-CPU != execute-CPU migration (essentially impossible during serial
    // boot) is caught by a retry. Each attempt disarms first (a prior un-fired
    // arm leaves bp0 set), then arms fresh.
    let mut delivered: Option<bool> = None;
    for _ in 0..4 {
        let _ = ctl.write_all(b"hwverify off"); // idempotent disarm (clears a stale arm)
        if ctl.write_all(arm_cmd.as_bytes()).is_err() {
            err_exit("hwbp-verify: FAIL -- write hwverify arm\n");
        }

        hwbp_target(); // the armed instruction: if HW debug delivers, EC 0x30 fires here

        let mut buf = [0u8; 64];
        let n = unsafe { t_pread(ctl.as_raw_fd() as i64, buf.as_mut_ptr(), buf.len(), 0) };
        if n <= 0 {
            err_exit("hwbp-verify: FAIL -- read hwverify result (empty)\n");
        }
        match parse_fired(&buf[..n as usize]) {
            Some(true) => {
                delivered = Some(true);
                break;
            }
            Some(false) => delivered = Some(false), // keep trying (migration retry)
            None => err_exit("hwbp-verify: FAIL -- malformed hwverify result\n"),
        }
    }
    let _ = ctl.write_all(b"hwverify off"); // leave bp0 disarmed regardless

    match delivered {
        Some(true) => {
            t_putstr("hwbp-verify: PASS -- guest EL0 hardware breakpoint delivered EC 0x30 to EL1\n");
            unsafe { t_exits(0) }
        }
        _ => {
            // A clean "not delivered" verdict: the 8a-2 HW-debug tests run
            // TCG-only. NOT a failure; joey logs it and boot continues.
            t_putstr("hwbp-verify: not delivered under this accel (8a-2 HW-debug tests are TCG-only)\n");
            unsafe { t_exits(3) }
        }
    }
}
