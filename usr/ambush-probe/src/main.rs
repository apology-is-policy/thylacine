// /ambush-probe -- the Stage-8c-1 Ambush (Delve port) in-guest smoke.
//
// Iteration 0 of the 8c-1 runtime half: prove the cross-built Ambush debugger
// binary actually EXECUTES on Thylacine before exercising its debug-fs backend
// (attach/inspect, which the following sub-chunk builds on top of this). The
// scaffold's `GOOS=thylacine go build ./...` proved Ambush COMPILES; this proves
// it RUNS. `ambush version` drives the whole Go runtime (scheduler, mallocinit,
// sysmon OS-thread spawn) + the cobra command tree (710 vendored deps) + an fd-1
// write + exit_group -- a real "does the ported multi-MB binary come up" proof,
// deliberately isolated from the proc_thylacine backend so a first-boot failure
// pins "binary does not run" vs "backend is wrong".
//
// Ambush is OPTIONAL infra: build_ambush bakes /ambush only when BOTH the Go
// fork ($GOFORK) and the Ambush fork ($AMBUSHFORK) are present at build time. If
// /ambush is unbaked, the spawn fails and the probe SKIPs (exit 0) so a
// fork-absent checkout still boots. A present-but-broken Ambush (a Go runtime
// throw, a snare:* fault, a non-zero exit, or a missing version banner) FAILS
// the boot -- the 8c regression sentinel. joey spawns /ambush-probe boot-fatally.

#![no_std]
#![no_main]

extern crate alloc;

#[global_allocator]
static GLOBAL_ALLOCATOR: libthyla_rs::alloc::ThylaAlloc = libthyla_rs::alloc::ThylaAlloc;

use alloc::vec::Vec;
use libthyla_rs::io::Read;
use libthyla_rs::process::{Command, Stdio};
use libthyla_rs::{t_exits, t_putstr};

// Substring search over the captured banner bytes (no_std, no regex).
fn contains(hay: &[u8], needle: &[u8]) -> bool {
    if needle.is_empty() {
        return true;
    }
    if hay.len() < needle.len() {
        return false;
    }
    hay.windows(needle.len()).any(|w| w == needle)
}

fn probe() -> Result<(), &'static str> {
    // `/ambush version`. Root-anchored absolute path (pre-pivot cpio root, like
    // debug-probe). All three stdio Piped: Ambush writes its banner on fd 1
    // (unlike the fd-less native SYS_PUTS probes), and an Inherit child would try
    // to clone this fd-less probe's absent 0/1/2 (-> EIO), so it needs real pipe
    // handles. stdin/stderr stay empty for `version`.
    let mut child = match Command::new("/ambush")
        .arg("version")
        .stdin(Stdio::Piped)
        .stdout(Stdio::Piped)
        .stderr(Stdio::Piped)
        .spawn()
    {
        Ok(c) => c,
        Err(_) => {
            t_putstr("ambush-probe: SKIP (ambush not baked -- Go/Ambush fork absent at build)\n");
            unsafe { t_exits(0) }
        }
    };

    // Drain stdout to EOF FIRST (blocks until Ambush finishes printing + exits,
    // closing fd 1 -> EOF via the #68 last-thread-out fd close), THEN reap. The
    // version banner is a few short lines (well under the pipe buffer), so
    // draining before reap cannot deadlock. Cap the accumulation defensively.
    let mut out: Vec<u8> = Vec::new();
    let mut stdout = child
        .stdout
        .take()
        .ok_or("ambush-probe: FAIL -- no stdout pipe\n")?;
    let mut buf = [0u8; 512];
    loop {
        match stdout.read(&mut buf) {
            Ok(0) => break,
            Ok(n) => {
                if out.len() < 64 * 1024 {
                    out.extend_from_slice(&buf[..n]);
                }
            }
            Err(_) => return Err("ambush-probe: FAIL -- read ambush stdout\n"),
        }
    }

    let st = child
        .wait()
        .map_err(|_| "ambush-probe: FAIL -- reap ambush\n")?;

    // Echo the banner to the boot log for ground-truth (it is ASCII).
    if let Ok(s) = core::str::from_utf8(&out) {
        t_putstr("ambush-probe: ambush version output follows:\n");
        t_putstr(s);
        if !s.ends_with('\n') {
            t_putstr("\n");
        }
    }

    if !st.success() {
        return Err("ambush-probe: FAIL -- ambush version exited non-zero (Go throw / snare?)\n");
    }
    if out.is_empty() {
        return Err("ambush-probe: FAIL -- ambush version produced no output\n");
    }
    // A binary that runs + exits 0 but prints garbage must not pass: assert the
    // Delve version banner's "Version" marker is present.
    if !contains(&out, b"Version") {
        return Err("ambush-probe: FAIL -- ambush version banner missing 'Version'\n");
    }
    Ok(())
}

#[no_mangle]
pub extern "C" fn rs_main() -> i64 {
    t_putstr("ambush-probe: starting (8c-1 Ambush version smoke)\n");
    match probe() {
        Ok(()) => {
            t_putstr("ambush-probe: PASS (ambush runs; version banner ok)\n");
            unsafe { t_exits(0) }
        }
        Err(msg) => {
            t_putstr(msg);
            unsafe { t_exits(1) }
        }
    }
}
