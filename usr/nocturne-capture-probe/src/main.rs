// /bin/nocturne-capture-probe -- the N-3c-2 device-capture (`source`) authority
// witness (docs/NOCTURNE.md 6.4/6.8, I-46). Reading a device source is recording
// -- eavesdropping unless the reader holds the sink authority -- so `source` lives
// ONLY on the per-connection /srv/nocturne-ctl post (the peer is the reader) and
// never on joey's shared /dev/nocturne mount (peer=mounter=SYSTEM, the N-3a-2 F1
// lesson).
//
// The witness bar (operator-ratified 2026-09-07): DETERMINISTIC, so it runs under
// `audiodev=none` in CI with no host capture backend. That means the captured
// CONTENT is silence, so this probe must NOT assert non-silence -- a silence-
// content check would be satisfied by a BROKEN RX path AND by the working null
// backend, indistinguishably (the broken-fixture trap). The discriminating
// positive is the period COUNT: the driver's `periods-captured` in `info` must
// CLIMB once an authorized reader opens `source`. A broken RX path leaves it 0
// (=> a clean, bounded FAIL, never a hung boot -- we poll the non-blocking `info`,
// not a blocking `source` read, to detect delivery).
//
//   PARENT (SYSTEM):
//     - opening /srv/nocturne-ctl/source is ACCEPTED (the SYSTEM axis).
//     - the single-reader guard: a SECOND concurrent source open is EBUSY.
//     - RX delivers: with `source` held, `info`'s periods-captured climbs past a
//       threshold (the COUNT -- the deterministic positive).
//     - the byte path: one `source` read returns data (>0 bytes; silent under the
//       null backend, so no content assertion) -- the client-visible mirror works.
//     - the eavesdrop-via-mount regression: /dev/nocturne/source does NOT EXIST
//       (source is -ctl-only, never the shared mount).
//
//   DENY child (a USER principal; argv "deny") -- BOTH must be refused:
//     - opening the -ctl `source` is DENIED (EPERM): not SYSTEM, no CAP_AUDIO_GRAPH,
//       not the console-owner session.
//     - /dev/nocturne/source does NOT EXIST (the mount carries no source).
//
// The CAP_AUDIO_GRAPH-admit axis is covered by test_devcap.clearance_audio_graph
// and the console-owner axis by proc_identity.peer_snapshot_console_owner; a boot
// probe holds neither. The per-read fresh re-check (mid-recording revocation) is
// prose-argued + code-audited; a boot probe cannot mutate its own caps.

#![no_std]
#![no_main]

extern crate alloc;

use alloc::string::String;
use core::time::Duration;

#[global_allocator]
static GLOBAL_ALLOCATOR: libthyla_rs::alloc::ThylaAlloc = libthyla_rs::alloc::ThylaAlloc;

use libthyla_rs::env;
use libthyla_rs::io::Write;
use libthyla_rs::process::{Command, Stdio};
use libthyla_rs::time;
use libthyla_rs::{
    t_close, t_open, t_putstr, t_read, T_OREAD, T_WALK_OPEN_FROM_ROOT,
};

// The sink-authority post (per-connection; the gate reads THIS program as peer).
const NOC_CTL: &[u8] = b"/srv/nocturne-ctl";
// The playback mount's info file (world-readable, non-authority): reports the
// driver's capture counters.
const MOUNT_INFO: &[u8] = b"/dev/nocturne/info";
// `source` must NOT exist on the shared mount (it is -ctl-only).
const MOUNT_SOURCE: &[u8] = b"/dev/nocturne/source";
const SELF_BIN: &str = "/bin/nocturne-capture-probe";
const DENY_PRINCIPAL: u32 = 1;

// >= 2 captured periods proves the RX path clocks (a broken path stays at 0).
const CAPTURED_THRESHOLD: u64 = 2;
// Poll `info` up to this many times, sleeping between, before declaring the RX
// path dead. 200 x 5 ms = 1 s -- generous vs the ~10.7 ms capture period.
const POLL_TRIES: u32 = 200;
const POLL_SLEEP_MS: u64 = 5;

/// Write to BOTH the kernel console and fd 1 (joey's pouch_smoke checks fd 1).
fn say(s: &str) {
    let _ = t_putstr(s);
    let mut out = libthyla_rs::io::stdout();
    let _ = out.write_all(s.as_bytes());
}

fn fail(why: &str) -> i64 {
    let mut s = String::from("NOCTURNE-CAPTURE-PROBE FAIL: ");
    s.push_str(why);
    s.push('\n');
    say(&s);
    1
}

/// Connect DIRECTLY to /srv/nocturne-ctl -- a fresh per-client server conn, so
/// nocturned resolves the peer as US, not the /dev/nocturne mounter. <0 on fail.
fn connect_ctl() -> i64 {
    unsafe { t_open(T_WALK_OPEN_FROM_ROOT, NOC_CTL.as_ptr(), NOC_CTL.len(), T_OREAD) }
}

/// Open the device-capture `source` on a -ctl connection. Returns the fd (<0 on
/// refusal: EPERM unauthorized, EBUSY second reader, ENODEV no capture device).
fn open_source(ctl: i64) -> i64 {
    unsafe { t_open(ctl, b"source".as_ptr(), 6, T_OREAD) }
}

/// Read the driver's `periods-captured` counter from the mount `info` file. None
/// on any read/parse failure (treated as "not yet observed").
fn read_captured_count() -> Option<u64> {
    let f = unsafe { t_open(T_WALK_OPEN_FROM_ROOT, MOUNT_INFO.as_ptr(), MOUNT_INFO.len(), T_OREAD) };
    if f < 0 {
        return None;
    }
    let mut buf = [0u8; 1024];
    let n = unsafe { t_read(f, buf.as_mut_ptr(), buf.len()) };
    unsafe { t_close(f) };
    if n <= 0 {
        return None;
    }
    let text = core::str::from_utf8(&buf[..n as usize]).ok()?;
    for line in text.lines() {
        if let Some(rest) = line.strip_prefix("periods-captured ") {
            return rest.trim().parse::<u64>().ok();
        }
    }
    None
}

/// Poll `info` until periods-captured reaches the threshold (the RX path clocks)
/// or the bound elapses. True iff the count climbed -- the deterministic positive.
fn capture_count_climbs() -> bool {
    for _ in 0..POLL_TRIES {
        if let Some(c) = read_captured_count() {
            if c >= CAPTURED_THRESHOLD {
                return true;
            }
        }
        let _ = time::sleep(Duration::from_millis(POLL_SLEEP_MS));
    }
    false
}

/// /dev/nocturne/source must NOT exist -- capture is -ctl-only. True iff the mount
/// has no such file (the open is refused).
fn mount_source_absent() -> bool {
    let s = unsafe { t_open(T_WALK_OPEN_FROM_ROOT, MOUNT_SOURCE.as_ptr(), MOUNT_SOURCE.len(), T_OREAD) };
    if s >= 0 {
        unsafe { t_close(s) };
        return false;
    }
    true
}

/// The DENY child (a user principal): opening the -ctl `source` AND finding it on
/// the mount must BOTH be refused. Exit 0 iff both were refused.
fn deny_child() -> i64 {
    if !mount_source_absent() {
        say("NOCTURNE-CAPTURE-DENY FAIL: /dev/nocturne/source exists on the shared mount (mount eavesdrop)\n");
        return 1;
    }
    let ctl = connect_ctl();
    if ctl < 0 {
        say("NOCTURNE-CAPTURE-DENY FAIL: child could not connect /srv/nocturne-ctl\n");
        return 3;
    }
    let src = open_source(ctl);
    if src >= 0 {
        unsafe { t_close(src) };
        unsafe { t_close(ctl) };
        say("NOCTURNE-CAPTURE-DENY FAIL: user OPENED the device source -- gate bypassed\n");
        return 1;
    }
    unsafe { t_close(ctl) };
    say("NOCTURNE-CAPTURE-DENY OK: user source open + mount source both refused\n");
    0
}

fn parent() -> i64 {
    // 1. SYSTEM opens the source (the positive authority arm). Opening it turns
    //    capture ON (on-demand), so the RX stream starts here.
    let ctl = connect_ctl();
    if ctl < 0 {
        return fail("connect /srv/nocturne-ctl");
    }
    let src = open_source(ctl);
    if src < 0 {
        unsafe { t_close(ctl) };
        // A negative here on SYSTEM means either no capture device (ENODEV -- the
        // streams=1 config, which this witness's boot avoids) or a gate bug.
        return fail("SYSTEM source open was refused (no capture device, or the gate is wrong)");
    }

    // 2. Single-reader guard: a SECOND concurrent source open must be EBUSY. A
    //    fresh -ctl conn (also SYSTEM, so authority is not what refuses it).
    let ctl2 = connect_ctl();
    if ctl2 < 0 {
        unsafe { t_close(src) };
        unsafe { t_close(ctl) };
        return fail("second connect /srv/nocturne-ctl");
    }
    let src2 = open_source(ctl2);
    if src2 >= 0 {
        unsafe { t_close(src2) };
        unsafe { t_close(ctl2) };
        unsafe { t_close(src) };
        unsafe { t_close(ctl) };
        return fail("a SECOND concurrent source open was ACCEPTED (single-reader guard)");
    }
    unsafe { t_close(ctl2) };

    // 3. RX delivers (the deterministic COUNT): with `source` held, the driver's
    //    periods-captured must climb. This is the discriminating positive -- a
    //    broken RX path stays at 0. We poll the non-blocking `info`, so a dead RX
    //    fails cleanly here rather than hanging a blocking `source` read below.
    if !capture_count_climbs() {
        unsafe { t_close(src) };
        unsafe { t_close(ctl) };
        return fail("periods-captured did not climb (the RX path delivered no periods)");
    }

    // 4. The byte path: one `source` read returns data (the mirror the cycle fills
    //    is client-visible). Content is silence under audiodev=none, so assert only
    //    that bytes flowed -- NEVER non-silence (the broken-fixture trap).
    let mut buf = [0u8; 4096];
    let n = unsafe { t_read(src, buf.as_mut_ptr(), buf.len()) };
    unsafe { t_close(src) };
    unsafe { t_close(ctl) };
    if n <= 0 {
        return fail("a source read returned no bytes though periods-captured climbed");
    }

    // 5. The eavesdrop-via-mount regression: /dev/nocturne/source does not exist.
    if !mount_source_absent() {
        return fail("/dev/nocturne/source exists on the shared mount (source must be -ctl-only)");
    }

    // 6. Negative gate: a user-principal child -- source open AND mount source both
    //    refused. The parent holds CAP_SET_IDENTITY (joey's spawn mask) to stamp
    //    the child's principal; the child inherits this namespace (so it sees the
    //    mount + /srv) but not SYSTEM, not the console-owner session, no clearance.
    match Command::new(SELF_BIN)
        .arg("deny")
        .identity(DENY_PRINCIPAL, DENY_PRINCIPAL, &[])
        .caps(0)
        .stderr(Stdio::Piped)
        .spawn()
    {
        Ok(mut child) => match child.wait() {
            Ok(st) if st.success() => {}
            Ok(_) => return fail("deny child reported the source gate was bypassed"),
            Err(_) => return fail("deny child wait() failed"),
        },
        Err(_) => return fail("could not spawn the deny child (missing CAP_SET_IDENTITY?)"),
    }

    say("NOCTURNE-CAPTURE-PROBE PASS (SYSTEM source open + single-reader + periods-captured climbed + bytes flowed; mount absent + user source deny)\n");
    0
}

#[no_mangle]
pub extern "C" fn rs_main() -> i64 {
    let deny = env::args().operands().next().is_some_and(|a| a == b"deny");
    if deny {
        deny_child()
    } else {
        parent()
    }
}
